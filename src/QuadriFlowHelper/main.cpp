// qtmesh_quadriflow — out-of-process QuadriFlow quad remesher.
//
// QuadriFlow (BSD-3, vendored in src/dependencies/quadriflow) calls exit()
// from deep inside its solvers on failure, so the editor never links it: it
// runs this helper through QProcess, which turns a crash or an exit into an
// error message instead of a lost session, and lets Cancel kill the process.
//
// Protocol (stable — QuadRemesh.cpp parses it):
//   qtmesh_quadriflow -i in.obj -o out.obj [-f faces] [-sharp] [-boundary]
//                     [-adaptive] [-mcf] [-seed N]
//   stdout: "QF_STAGE <k> <n> <name>" per stage, then
//           "QF_DONE <vertices> <quads>" once out.obj is fully written.
//   A run that ends WITHOUT QF_DONE failed, whatever the exit code (upstream
//   exits 0 on several internal failures).
//
// The OBJ reader is our own: upstream's loader uses std::unary_function,
// which C++17 standard libraries have removed, and only positions + faces
// are needed (n-gons are fan-triangulated).

#include "config.hpp"
#include "field-math.hpp"
#include "optimizer.hpp"
#include "parametrizer.hpp"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace qflow;

namespace {

void stage(int k, const char* name)
{
    std::printf("QF_STAGE %d 6 %s\n", k, name);
    std::fflush(stdout);
}

int fail(int code, const std::string& msg)
{
    std::printf("QF_ERROR %s\n", msg.c_str());
    std::fflush(stdout);
    return code;
}

bool readObj(const char* path, MatrixXd& V, MatrixXi& F, std::string& err)
{
    std::ifstream in(path);
    if (!in) {
        err = "cannot open input";
        return false;
    }
    std::vector<double> pos;
    std::vector<int> tri;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2)
            continue;
        if (line[0] == 'v' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            double x = 0, y = 0, z = 0;
            if (!(ss >> x >> y >> z)) {
                err = "bad vertex line";
                return false;
            }
            pos.insert(pos.end(), {x, y, z});
        } else if (line[0] == 'f' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            std::vector<int> poly;
            std::string tok;
            const long nv = static_cast<long>(pos.size() / 3);
            while (ss >> tok) {
                long idx = std::strtol(tok.c_str(), nullptr, 10);   // stops at '/'
                if (idx < 0)
                    idx = nv + idx + 1;                               // relative index
                if (idx < 1 || idx > nv) {
                    err = "face index out of range";
                    return false;
                }
                poly.push_back(static_cast<int>(idx - 1));
            }
            for (size_t i = 2; i < poly.size(); ++i)
                tri.insert(tri.end(), {poly[0], poly[i - 1], poly[i]});
        }
    }
    if (pos.size() < 9 || tri.empty()) {
        err = "input has no triangles";
        return false;
    }
    V.resize(3, static_cast<Eigen::Index>(pos.size() / 3));
    for (Eigen::Index i = 0; i < V.cols(); ++i)
        V.col(i) = Vector3d(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
    F.resize(3, static_cast<Eigen::Index>(tri.size() / 3));
    for (Eigen::Index i = 0; i < F.cols(); ++i)
        F.col(i) = Vector3i(tri[3 * i], tri[3 * i + 1], tri[3 * i + 2]);
    return true;
}

} // namespace

// Global like upstream's main.cpp: the Parametrizer holds several large
// hierarchies and is too big for a comfortable stack frame.
static Parametrizer field;

int main(int argc, char** argv)
{
    // Numbers are exchanged as text: always the C locale ('.' decimals).
    std::setlocale(LC_ALL, "C");
    std::string input, output;
    int faces = -1;
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(argv[i], "-i") && hasValue) input = argv[++i];
        else if (!std::strcmp(argv[i], "-o") && hasValue) output = argv[++i];
        else if (!std::strcmp(argv[i], "-f") && hasValue) faces = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-seed") && hasValue)
            field.hierarchy.rng_seed = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-sharp")) field.flag_preserve_sharp = 1;
        else if (!std::strcmp(argv[i], "-boundary")) field.flag_preserve_boundary = 1;
        else if (!std::strcmp(argv[i], "-adaptive")) field.flag_adaptive_scale = 1;
        else if (!std::strcmp(argv[i], "-mcf")) field.flag_minimum_cost_flow = 1;
        else return fail(2, std::string("unknown argument: ") + argv[i]);
    }
    if (input.empty() || output.empty())
        return fail(2, "usage: qtmesh_quadriflow -i in.obj -o out.obj [-f faces] "
                       "[-sharp] [-boundary] [-adaptive] [-mcf] [-seed N]");

    stage(1, "load");
    std::string err;
    if (!readObj(input.c_str(), field.V, field.F, err))
        return fail(3, err);
    field.NormalizeMesh();

    stage(2, "initialize");
    field.Initialize(faces);

    if (field.flag_preserve_boundary) {
        // Same boundary constraints as upstream's main.cpp.
        Hierarchy& mRes = field.hierarchy;
        mRes.clearConstraints();
        for (uint32_t i = 0; i < 3 * mRes.mF.cols(); ++i) {
            if (mRes.mE2E[i] != -1)
                continue;
            const uint32_t i0 = mRes.mF(i % 3, i / 3);
            const uint32_t i1 = mRes.mF((i + 1) % 3, i / 3);
            const Vector3d p0 = mRes.mV[0].col(i0), p1 = mRes.mV[0].col(i1);
            Vector3d edge = p1 - p0;
            if (edge.squaredNorm() > 0) {
                edge.normalize();
                mRes.mCO[0].col(i0) = p0;
                mRes.mCO[0].col(i1) = p1;
                mRes.mCQ[0].col(i0) = mRes.mCQ[0].col(i1) = edge;
                mRes.mCQw[0][i0] = mRes.mCQw[0][i1] = mRes.mCOw[0][i0] = mRes.mCOw[0][i1] = 1.0;
            }
        }
        mRes.propagateConstraints();
    }

    stage(3, "orientation");
    Optimizer::optimize_orientations(field.hierarchy);
    field.ComputeOrientationSingularities();

    stage(4, "scale");
    if (field.flag_adaptive_scale == 1)
        field.EstimateSlope();
    Optimizer::optimize_scale(field.hierarchy, field.rho, field.flag_adaptive_scale);
    field.flag_adaptive_scale = 1;

    stage(5, "position");
    Optimizer::optimize_positions(field.hierarchy, field.flag_adaptive_scale);
    field.ComputePositionSingularities();

    stage(6, "extract");
    field.ComputeIndexMap();
    if (field.O_compact.empty() || field.F_compact.empty())
        return fail(4, "remesh produced no faces");

    // Write to a temp name first so a partially written file is never
    // mistaken for a result.
    const std::string tmp = output + ".part";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f)
        return fail(5, "cannot write output");
    const int nv = static_cast<int>(field.O_compact.size());
    for (const Vector3d& o : field.O_compact) {
        const Vector3d t = o * field.normalize_scale + field.normalize_offset;
        std::fprintf(f, "v %.9g %.9g %.9g\n", t[0], t[1], t[2]);
    }
    int quads = 0;
    for (const Vector4i& q : field.F_compact) {
        bool ok = true;
        for (int k = 0; k < 4; ++k)
            ok = ok && q[k] >= 0 && q[k] < nv;
        if (!ok)
            continue;
        std::fprintf(f, "f %d %d %d %d\n", q[0] + 1, q[1] + 1, q[2] + 1, q[3] + 1);
        ++quads;
    }
    const bool closed = std::fclose(f) == 0;
    std::remove(output.c_str());
    if (!closed || std::rename(tmp.c_str(), output.c_str()) != 0)
        return fail(5, "cannot finalize output");

    std::printf("QF_DONE %d %d\n", nv, quads);
    std::fflush(stdout);
    return 0;
}
