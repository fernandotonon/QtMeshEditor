#include "ShapeGuide.h"

#include "AppStorage.h"
#include "ModelDownloader.h"
#include "ModelFetch.h"
#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <thread>

namespace {

constexpr const char* kModelFile = "ss_encoder.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/trellis2/";
// Digest of the graph scripts/export-trellis-ss-encoder-onnx.py writes (it is
// deterministic); verified on download and for an existing file when the
// default hosting is used.
constexpr const char* kModelSha256 =
    "904d66260f36d4922b891d366abddfaa48f76adafbda80126137119f6e77e8c0";
constexpr float kPi = 3.14159265358979f;

// ---- a tiny builder for blockouts --------------------------------------------
// Every face gets its own vertices so a bake sees FLAT normals on the boxes
// (shared corners would average them into diagonal projection rays).
class Builder {
public:
    ShapeGuide::Mesh mesh;

    void quad(const float a[3], const float b[3], const float c[3], const float d[3])
    {
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
        for (const float* p : {a, b, c, d})
            mesh.positions.insert(mesh.positions.end(), {p[0], p[1], p[2]});
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    void tri(const float a[3], const float b[3], const float c[3])
    {
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
        for (const float* p : {a, b, c})
            mesh.positions.insert(mesh.positions.end(), {p[0], p[1], p[2]});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
    }
    // Axis-aligned box from its min corner to its max corner (outward CCW).
    void box(float x0, float y0, float z0, float x1, float y1, float z1)
    {
        const float p[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                               {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        quad(p[4], p[5], p[6], p[7]);   // +Z
        quad(p[1], p[0], p[3], p[2]);   // -Z
        quad(p[5], p[1], p[2], p[6]);   // +X
        quad(p[0], p[4], p[7], p[3]);   // -X
        quad(p[7], p[6], p[2], p[3]);   // +Y
        quad(p[0], p[1], p[5], p[4]);   // -Y
    }
    // Box given by centre + full size.
    void boxC(float cx, float cy, float cz, float sx, float sy, float sz)
    {
        box(cx - sx / 2, cy - sy / 2, cz - sz / 2, cx + sx / 2, cy + sy / 2, cz + sz / 2);
    }
    // Gable roof: a triangular prism from x0..x1 (ridge along X) over the
    // footprint z0..z1, eaves at height y0, ridge at y1.
    void gableX(float x0, float x1, float z0, float z1, float y0, float y1)
    {
        const float zm = 0.5f * (z0 + z1);
        const float a[3] = {x0, y0, z1}, b[3] = {x1, y0, z1};
        const float c[3] = {x1, y1, zm}, d[3] = {x0, y1, zm};
        const float e[3] = {x0, y0, z0}, f[3] = {x1, y0, z0};
        quad(a, b, c, d);   // front slope
        quad(f, e, d, c);   // back slope
        tri(e, a, d);       // -X gable
        tri(b, f, c);       // +X gable
        quad(e, f, b, a);   // underside
    }
    // Wedge with a sloped top along Z (front lower): footprint x0..x1 / z0..z1,
    // height yBack at z0 and yFront at z1. Used for windscreens and hoods.
    void wedgeZ(float x0, float x1, float z0, float z1, float yBase, float yBack, float yFront)
    {
        const float p[8][3] = {{x0, yBase, z0}, {x1, yBase, z0}, {x1, yBack, z0}, {x0, yBack, z0},
                               {x0, yBase, z1}, {x1, yBase, z1}, {x1, yFront, z1}, {x0, yFront, z1}};
        quad(p[4], p[5], p[6], p[7]);
        quad(p[1], p[0], p[3], p[2]);
        quad(p[5], p[1], p[2], p[6]);
        quad(p[0], p[4], p[7], p[3]);
        quad(p[7], p[6], p[2], p[3]);
        quad(p[0], p[1], p[5], p[4]);
    }
    // Cylinder whose axis runs along X (wheels), centre (cx, cy, cz).
    void wheelX(float cx, float cy, float cz, float radius, float width, int seg = 12)
    {
        const float x0 = cx - width / 2, x1 = cx + width / 2;
        for (int i = 0; i < seg; ++i) {
            const float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
            const float y0 = cy + radius * std::cos(a0), z0 = cz + radius * std::sin(a0);
            const float y1 = cy + radius * std::cos(a1), z1 = cz + radius * std::sin(a1);
            const float p00[3] = {x0, y0, z0}, p01[3] = {x0, y1, z1};
            const float p10[3] = {x1, y0, z0}, p11[3] = {x1, y1, z1};
            quad(p00, p01, p11, p10);
            const float c0[3] = {x0, cy, cz}, c1[3] = {x1, cy, cz};
            tri(c0, p01, p00);
            tri(c1, p10, p11);
        }
    }
    // Vertical cylinder (trunks, posts) from y0 to y1.
    void cylinderY(float cx, float cz, float radius, float y0, float y1, int seg = 10)
    {
        for (int i = 0; i < seg; ++i) {
            const float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
            const float x0 = cx + radius * std::cos(a0), z0 = cz - radius * std::sin(a0);
            const float x1 = cx + radius * std::cos(a1), z1 = cz - radius * std::sin(a1);
            const float b0[3] = {x0, y0, z0}, b1[3] = {x1, y0, z1};
            const float t0[3] = {x0, y1, z0}, t1[3] = {x1, y1, z1};
            quad(b0, b1, t1, t0);
            const float cb[3] = {cx, y0, cz}, ct[3] = {cx, y1, cz};
            tri(ct, t0, t1);
            tri(cb, b1, b0);
        }
    }
    // Low-poly ellipsoid (foliage, heads).
    void ellipsoid(float cx, float cy, float cz, float rx, float ry, float rz,
                   int seg = 10, int rings = 6)
    {
        auto pt = [&](int i, int j, float out[3]) {
            const float th = kPi * j / rings, ph = 2 * kPi * i / seg;
            out[0] = cx + rx * std::sin(th) * std::cos(ph);
            out[1] = cy + ry * std::cos(th);
            out[2] = cz - rz * std::sin(th) * std::sin(ph);
        };
        for (int j = 0; j < rings; ++j)
            for (int i = 0; i < seg; ++i) {
                float a[3], b[3], c[3], d[3];
                pt(i, j, a); pt(i + 1, j, b); pt(i + 1, j + 1, c); pt(i, j + 1, d);
                if (j == 0)              tri(a, d, c);
                else if (j == rings - 1) tri(a, d, b);
                else                     quad(a, d, c, b);
            }
    }
};

// Four (or six) wheels for a vehicle whose length runs along Z.
void wheels(Builder& b, float halfTrack, float r, float w, std::initializer_list<float> axlesZ)
{
    for (float z : axlesZ) {
        b.wheelX(-halfTrack, r, z, r, w);
        b.wheelX(halfTrack, r, z, r, w);
    }
}

ShapeGuide::Mesh buildPreset(const QString& id)
{
    Builder b;
    if (id == QLatin1String("house")) {
        b.box(-4.0f, 0.0f, -3.0f, 4.0f, 3.0f, 3.0f);          // walls
        b.gableX(-4.4f, 4.4f, -3.5f, 3.5f, 3.0f, 5.2f);      // roof with eaves
        b.box(2.0f, 3.8f, -1.6f, 2.8f, 6.0f, -0.8f);         // chimney
        b.box(-0.6f, 0.0f, 3.0f, 0.6f, 2.2f, 3.15f);         // door
        b.box(-1.2f, 0.0f, 3.0f, 1.2f, 0.25f, 4.0f);         // front step
    } else if (id == QLatin1String("shop")) {
        b.box(-3.0f, 0.0f, -2.5f, 3.0f, 3.6f, 2.5f);          // shop body
        b.box(-3.1f, 3.6f, -2.6f, 3.1f, 4.1f, 2.6f);          // parapet
        b.wedgeZ(-3.0f, 3.0f, 2.5f, 4.0f, 2.6f, 3.0f, 2.7f); // awning
        b.box(-2.2f, 3.0f, 2.5f, 2.2f, 3.5f, 2.7f);          // sign board
        b.box(-2.6f, 0.0f, 2.5f, -0.6f, 2.3f, 2.6f);         // shop window
        b.box(0.4f, 0.0f, 2.5f, 1.6f, 2.3f, 2.6f);           // door
    } else if (id == QLatin1String("apartment")) {
        b.box(-5.0f, 0.0f, -4.0f, 5.0f, 15.0f, 4.0f);         // block
        b.box(-5.2f, 15.0f, -4.2f, 5.2f, 15.6f, 4.2f);        // roof slab
        b.box(-1.5f, 15.6f, -1.5f, 1.5f, 17.5f, 1.5f);        // stair housing
        for (float y : {3.0f, 6.0f, 9.0f, 12.0f})             // balconies
            b.box(-3.5f, y - 0.1f, 4.0f, 3.5f, y + 0.3f, 5.0f);
        b.box(-1.2f, 0.0f, 4.0f, 1.2f, 2.6f, 4.6f);           // entrance
    } else if (id == QLatin1String("car")) {
        b.box(-0.9f, 0.3f, -2.2f, 0.9f, 1.0f, 2.2f);          // body
        b.wedgeZ(-0.8f, 0.8f, -1.3f, -0.4f, 1.0f, 1.5f, 1.5f);// cabin roof (rear)
        b.wedgeZ(-0.8f, 0.8f, -0.4f, 0.7f, 1.0f, 1.5f, 1.0f); // windscreen slope
        b.wedgeZ(-0.8f, 0.8f, -2.0f, -1.3f, 1.0f, 1.2f, 1.5f);// rear window
        wheels(b, 0.85f, 0.34f, 0.24f, {-1.35f, 1.35f});
    } else if (id == QLatin1String("van")) {
        b.box(-1.0f, 0.35f, -2.5f, 1.0f, 2.3f, 1.3f);         // cargo/cabin box
        b.wedgeZ(-1.0f, 1.0f, 1.3f, 2.1f, 0.35f, 2.3f, 1.2f); // sloped nose
        b.box(-1.0f, 0.35f, 2.1f, 1.0f, 1.2f, 2.5f);          // bonnet
        wheels(b, 0.95f, 0.36f, 0.26f, {-1.7f, 1.6f});
    } else if (id == QLatin1String("bus")) {
        b.box(-1.25f, 0.45f, -5.5f, 1.25f, 3.2f, 5.5f);       // body
        b.box(-1.0f, 3.2f, -3.0f, 1.0f, 3.45f, 1.0f);         // roof unit
        wheels(b, 1.15f, 0.5f, 0.3f, {-3.6f, 3.6f});
    } else if (id == QLatin1String("truck")) {
        b.box(-1.2f, 0.5f, 1.8f, 1.2f, 3.0f, 4.0f);           // cab
        b.box(-1.2f, 0.5f, 4.0f, 1.2f, 1.6f, 4.6f);           // bonnet
        b.box(-1.25f, 0.9f, -4.6f, 1.25f, 3.6f, 1.6f);        // cargo box
        b.box(-1.0f, 0.5f, -4.6f, 1.0f, 0.9f, 1.8f);          // chassis
        wheels(b, 1.1f, 0.52f, 0.36f, {-3.4f, -2.2f, 3.1f});
    } else if (id == QLatin1String("barrier")) {
        // Concrete jersey barrier: sloped trapezoid profile along X.
        const float h = 0.85f, wb = 0.30f, wm = 0.12f, wt = 0.10f, L = 1.5f;
        b.box(-L, 0.0f, -wb, L, 0.08f, wb);                   // foot
        b.wedgeZ(-L, L, -wb, 0.0f, 0.08f, 0.3f, h);           // back slope (approx)
        b.wedgeZ(-L, L, 0.0f, wb, 0.08f, h, 0.3f);            // front slope
        b.box(-L, 0.3f, -wm, L, h, wm);                       // core
        b.box(-L, h - 0.05f, -wt, L, h, wt);                  // top
    } else if (id == QLatin1String("barricade")) {
        // A protest barricade: crates, tyres and a plank wall piled up.
        b.box(-2.0f, 0.0f, -0.5f, -0.8f, 0.9f, 0.5f);
        b.box(-0.7f, 0.0f, -0.6f, 0.6f, 1.1f, 0.4f);
        b.box(0.7f, 0.0f, -0.5f, 2.0f, 0.8f, 0.5f);
        b.box(-1.4f, 0.9f, -0.4f, 0.2f, 1.7f, 0.3f);
        b.box(-2.2f, 0.0f, 0.5f, 2.2f, 1.4f, 0.65f);          // plank wall
        b.cylinderY(1.3f, 0.0f, 0.45f, 0.8f, 1.1f);           // tyre on crate
        b.cylinderY(1.3f, 0.0f, 0.45f, 1.1f, 1.4f);
    } else if (id == QLatin1String("stall")) {
        b.box(-1.5f, 0.0f, -0.6f, 1.5f, 1.0f, 0.6f);          // counter
        for (float x : {-1.45f, 1.45f})
            for (float z : {-1.0f, 0.9f})
                b.box(x - 0.06f, 0.0f, z - 0.06f, x + 0.06f, 2.3f, z + 0.06f);
        b.gableX(-1.7f, 1.7f, -1.3f, 1.3f, 2.3f, 2.9f);       // canopy
    } else if (id == QLatin1String("stage")) {
        b.box(-4.0f, 0.0f, -2.5f, 4.0f, 1.0f, 2.5f);          // platform
        b.box(-4.0f, 1.0f, -2.5f, 4.0f, 4.5f, -2.2f);         // back wall
        b.box(-4.0f, 4.5f, -2.5f, 4.0f, 4.9f, 0.5f);          // roof truss
        b.box(-4.0f, 1.0f, -2.5f, -3.7f, 4.5f, 0.5f);         // side towers
        b.box(3.7f, 1.0f, -2.5f, 4.0f, 4.5f, 0.5f);
        b.box(-1.0f, 0.0f, 2.5f, 1.0f, 0.5f, 3.3f);           // steps
    } else if (id == QLatin1String("tree")) {
        b.cylinderY(0.0f, 0.0f, 0.25f, 0.0f, 2.4f);
        b.ellipsoid(0.0f, 3.6f, 0.0f, 1.8f, 1.7f, 1.8f);
    } else if (id == QLatin1String("humanoid")) {
        // Mannequin, arms down (the pose generators handle best), 1.8 m.
        b.ellipsoid(0.0f, 1.62f, 0.0f, 0.11f, 0.13f, 0.12f, 8, 5);   // head
        b.boxC(0.0f, 1.47f, 0.0f, 0.09f, 0.08f, 0.09f);               // neck
        b.boxC(0.0f, 1.20f, 0.0f, 0.40f, 0.46f, 0.22f);               // chest
        b.boxC(0.0f, 0.90f, 0.0f, 0.34f, 0.18f, 0.20f);               // pelvis
        for (float s : {-1.0f, 1.0f}) {
            b.boxC(s * 0.26f, 1.15f, 0.0f, 0.10f, 0.32f, 0.11f);      // upper arm
            b.boxC(s * 0.27f, 0.83f, 0.02f, 0.09f, 0.30f, 0.10f);     // forearm
            b.boxC(s * 0.27f, 0.62f, 0.03f, 0.08f, 0.14f, 0.05f);     // hand
            b.boxC(s * 0.10f, 0.58f, 0.0f, 0.14f, 0.44f, 0.15f);      // thigh
            b.boxC(s * 0.10f, 0.20f, 0.0f, 0.12f, 0.36f, 0.13f);      // shin
            b.boxC(s * 0.10f, 0.03f, 0.05f, 0.11f, 0.06f, 0.25f);     // foot
        }
    }
    return b.mesh;
}

QString modelDir()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("trellis2"));
}

} // namespace

namespace ShapeGuide {

const std::vector<PresetInfo>& presets()
{
    static const std::vector<PresetInfo> kList = {
        {QStringLiteral("house"), QStringLiteral("House"), QStringLiteral("Buildings"),
         QStringLiteral("Gabled family house with chimney and front door")},
        {QStringLiteral("shop"), QStringLiteral("Shop / storefront"), QStringLiteral("Buildings"),
         QStringLiteral("Corner shop with awning, sign board and shop window")},
        {QStringLiteral("apartment"), QStringLiteral("Apartment block"), QStringLiteral("Buildings"),
         QStringLiteral("Five-storey block with balconies and a roof stair housing")},
        {QStringLiteral("stage"), QStringLiteral("Rally stage"), QStringLiteral("Buildings"),
         QStringLiteral("Outdoor stage: platform, back wall, roof truss, steps")},
        {QStringLiteral("car"), QStringLiteral("Car"), QStringLiteral("Vehicles"),
         QStringLiteral("Four-door saloon car")},
        {QStringLiteral("van"), QStringLiteral("Van"), QStringLiteral("Vehicles"),
         QStringLiteral("Delivery or police van")},
        {QStringLiteral("bus"), QStringLiteral("Bus"), QStringLiteral("Vehicles"),
         QStringLiteral("City bus")},
        {QStringLiteral("truck"), QStringLiteral("Truck"), QStringLiteral("Vehicles"),
         QStringLiteral("Box truck with cab")},
        {QStringLiteral("barrier"), QStringLiteral("Concrete barrier"), QStringLiteral("Props"),
         QStringLiteral("Jersey road barrier")},
        {QStringLiteral("barricade"), QStringLiteral("Barricade"), QStringLiteral("Props"),
         QStringLiteral("Pile of crates, tyres and planks")},
        {QStringLiteral("stall"), QStringLiteral("Market stall"), QStringLiteral("Props"),
         QStringLiteral("Street stall with canopy")},
        {QStringLiteral("tree"), QStringLiteral("Tree"), QStringLiteral("Props"),
         QStringLiteral("Street tree")},
        {QStringLiteral("humanoid"), QStringLiteral("Character (arms down)"),
         QStringLiteral("Characters"),
         QStringLiteral("Standing person; prompt-to-3D keeps this pose best")},
    };
    return kList;
}

bool isPreset(const QString& id)
{
    for (const PresetInfo& p : presets())
        if (p.id == id)
            return true;
    return false;
}

Mesh preset(const QString& id)
{
    return isPreset(id) ? buildPreset(id) : Mesh{};
}

bool readObj(const QString& path, Mesh& out, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot read %1").arg(path);
        return false;
    }
    out = Mesh{};
    while (!f.atEnd()) {
        const QList<QByteArray> t = f.readLine().simplified().split(' ');
        if (t.size() >= 4 && t[0] == "v") {
            for (int k = 1; k <= 3; ++k)
                out.positions.push_back(t[k].toFloat());
        } else if (t.size() >= 4 && t[0] == "f") {
            const int nv = static_cast<int>(out.positions.size() / 3);
            std::vector<uint32_t> poly;
            for (int k = 1; k < t.size(); ++k) {
                int i = t[k].split('/')[0].toInt();
                i = i < 0 ? nv + i : i - 1;
                if (i < 0 || i >= nv) {
                    if (error) *error = QStringLiteral("%1: face index out of range").arg(path);
                    return false;
                }
                poly.push_back(static_cast<uint32_t>(i));
            }
            for (size_t k = 1; k + 1 < poly.size(); ++k)
                out.indices.insert(out.indices.end(), {poly[0], poly[k], poly[k + 1]});
        }
    }
    if (out.empty()) {
        if (error) *error = QStringLiteral("%1 holds no triangles").arg(path);
        return false;
    }
    return true;
}

bool writeObj(const QString& path, const Mesh& mesh, const QString& objectName, QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    QTextStream s(&f);
    s << "# QtMeshEditor shape guide\n";
    if (!objectName.isEmpty())
        s << "o " << objectName << '\n';
    for (size_t v = 0; v + 2 < mesh.positions.size(); v += 3)
        s << "v " << QByteArray::number(mesh.positions[v], 'g', 7) << ' '
          << QByteArray::number(mesh.positions[v + 1], 'g', 7) << ' '
          << QByteArray::number(mesh.positions[v + 2], 'g', 7) << '\n';
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
        s << "f " << mesh.indices[t] + 1 << ' ' << mesh.indices[t + 1] + 1 << ' '
          << mesh.indices[t + 2] + 1 << '\n';
    s.flush();
    if (!f.commit()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

GridPlacement placementFor(const Mesh& m, float extent)
{
    GridPlacement p;
    if (m.positions.size() < 3)
        return p;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t v = 0; v + 2 < m.positions.size(); v += 3)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], m.positions[v + k]);
            mx[k] = std::max(mx[k], m.positions[v + k]);
        }
    float side = 0.0f;
    for (int k = 0; k < 3; ++k) {
        p.centre[k] = 0.5f * (mn[k] + mx[k]);
        side = std::max(side, mx[k] - mn[k]);
    }
    p.scale = side > 0.0f ? extent / side : 1.0f;
    return p;
}

Mesh toGrid(const Mesh& m, const GridPlacement& p)
{
    Mesh g;
    g.indices = m.indices;
    g.positions.resize(m.positions.size());
    for (size_t v = 0; v + 2 < m.positions.size(); v += 3) {
        const float e[3] = {(m.positions[v] - p.centre[0]) * p.scale,
                            (m.positions[v + 1] - p.centre[1]) * p.scale,
                            (m.positions[v + 2] - p.centre[2]) * p.scale};
        editorToGridDir(e, &g.positions[v]);
    }
    return g;
}

std::vector<uint8_t> voxelizeSurface(const Mesh& g, int res)
{
    std::vector<uint8_t> occ(static_cast<size_t>(res) * res * res, 0);
    const float h = 0.5f / static_cast<float>(res);   // half a voxel
    auto mark = [&](float x, float y, float z) {
        const int ix = std::clamp(static_cast<int>(std::floor((x + 0.5f) * res)), 0, res - 1);
        const int iy = std::clamp(static_cast<int>(std::floor((y + 0.5f) * res)), 0, res - 1);
        const int iz = std::clamp(static_cast<int>(std::floor((z + 0.5f) * res)), 0, res - 1);
        occ[(static_cast<size_t>(ix) * res + iy) * res + iz] = 1;
    };
    for (size_t t = 0; t + 2 < g.indices.size(); t += 3) {
        const float* a = &g.positions[g.indices[t] * 3];
        const float* b = &g.positions[g.indices[t + 1] * 3];
        const float* c = &g.positions[g.indices[t + 2] * 3];
        auto dist = [](const float* p, const float* q) {
            return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1])
                             + (p[2] - q[2]) * (p[2] - q[2]));
        };
        const float e = std::max({dist(a, b), dist(b, c), dist(c, a)});
        const int n = std::clamp(static_cast<int>(std::ceil(e / h)), 1, 512);
        for (int i = 0; i <= n; ++i)
            for (int j = 0; j <= n - i; ++j) {
                const float u = static_cast<float>(i) / n, v = static_cast<float>(j) / n;
                const float w = 1.0f - u - v;
                mark(u * a[0] + v * b[0] + w * c[0], u * a[1] + v * b[1] + w * c[1],
                     u * a[2] + v * b[2] + w * c[2]);
            }
    }
    return occ;
}

QString encoderModelPath()
{
    return QDir(modelDir()).filePath(QString::fromLatin1(kModelFile));
}

#ifndef ENABLE_ONNX

bool encoderAvailable() { return false; }

QString ensureEncoderBlocking(QString* error)
{
    if (error)
        *error = QStringLiteral("the shape guide needs an ONNX build (rebuild with -DENABLE_ONNX)");
    return {};
}

bool encode(const std::vector<uint8_t>&, const QString&, std::vector<float>&, QString* error)
{
    if (error)
        *error = QStringLiteral("the shape guide needs an ONNX build (rebuild with -DENABLE_ONNX)");
    return false;
}

#else

bool encoderAvailable() { return true; }

QString ensureEncoderBlocking(QString* error)
{
    QString base;
    bool defaultHosting = false;
    {
        QSettings s;
        base = s.value(QStringLiteral("ai/shapeGuideModelBaseUrl")).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_SHAPE_GUIDE_MODEL_BASE_URL");
            defaultHosting = env.isEmpty();
            base = defaultHosting ? QString::fromLatin1(kDefaultModelBaseUrl)
                                  : QString::fromUtf8(env);
        }
    }
    const QString dst = encoderModelPath();
    if (QFileInfo::exists(dst) && !defaultHosting)
        return dst;   // a mirror may serve another export: trust the file
    if (!QFileInfo::exists(dst) && !qEnvironmentVariableIsEmpty("QTMESH_SHAPE_GUIDE_NO_DOWNLOAD")) {
        if (error)
            *error = QStringLiteral("downloads disabled by QTMESH_SHAPE_GUIDE_NO_DOWNLOAD");
        return {};
    }
    if (!base.endsWith('/'))
        base += '/';
    if (!ModelDownloader::instance()) {
        if (error) *error = QStringLiteral("model downloader unavailable");
        return {};
    }
    QDir().mkpath(modelDir());
    ModelFetch::Request req;
    req.url = base + QString::fromLatin1(kModelFile);
    req.destination = dst;
    req.label = QStringLiteral("TRELLIS shape-guide encoder");
    req.timeoutMs = 900000;
    if (defaultHosting)
        req.expectedSha256 = QString::fromLatin1(kModelSha256);
    const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
    if (!fo.ok) {
        if (error) *error = fo.error;
        return {};
    }
    return fo.path;
}

bool encode(const std::vector<uint8_t>& occ, const QString& modelFile,
            std::vector<float>& latent, QString* error)
{
    constexpr int R = 64;
    if (occ.size() != static_cast<size_t>(R) * R * R) {
        if (error) *error = QStringLiteral("occupancy must be 64^3");
        return false;
    }
    if (!QFileInfo::exists(modelFile)) {
        if (error) *error = QStringLiteral("shape-guide encoder not found at %1").arg(modelFile);
        return false;
    }
    try {
        static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_shape_guide");
        Ort::SessionOptions so;
        // Plain CPU: a one-shot 64^3 encode takes about a second, and CoreML
        // gains nothing on a single 3D-conv pass.
        so.SetIntraOpNumThreads(static_cast<int>(
            std::max(1u, std::thread::hardware_concurrency() - 1)));
#ifdef _WIN32
        Ort::Session session(env, modelFile.toStdWString().c_str(), so);
#else
        Ort::Session session(env, modelFile.toStdString().c_str(), so);
#endif
        std::vector<float> in(occ.begin(), occ.end());
        const int64_t shape[5] = {1, 1, R, R, R};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inT = Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape, 5);
        Ort::AllocatorWithDefaultOptions alloc;
        auto inName = session.GetInputNameAllocated(0, alloc);
        auto outName = session.GetOutputNameAllocated(0, alloc);
        const char* inNames[] = {inName.get()};
        const char* outNames[] = {outName.get()};
        auto outs = session.Run(Ort::RunOptions{nullptr}, inNames, &inT, 1, outNames, 1);
        if (outs.empty() || !outs[0].IsTensor()) {
            if (error) *error = QStringLiteral("shape-guide encoder produced no tensor");
            return false;
        }
        const size_t n = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        if (n != 8u * 4096u) {
            if (error) *error = QStringLiteral("shape-guide encoder output has %1 values, expected 32768").arg(n);
            return false;
        }
        const float* z = outs[0].GetTensorData<float>();   // [8][16][16][16]
        latent.assign(n, 0.0f);
        for (int c = 0; c < 8; ++c)
            for (int s = 0; s < 4096; ++s) {
                const float v = z[c * 4096 + s];
                if (!std::isfinite(v)) {
                    if (error) *error = QStringLiteral("shape-guide encoder produced non-finite values");
                    return false;
                }
                latent[static_cast<size_t>(c) + 8u * s] = v;
            }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = QStringLiteral("shape-guide encoder: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
}

#endif // ENABLE_ONNX

bool writeControlLatent(const Mesh& editorMesh, const QString& modelFile,
                        const QString& outPath, QString* error)
{
    if (editorMesh.empty()) {
        if (error) *error = QStringLiteral("the shape guide has no triangles");
        return false;
    }
    const Mesh g = toGrid(editorMesh, placementFor(editorMesh));
    std::vector<float> latent;
    if (!encode(voxelizeSurface(g), modelFile, latent, error))
        return false;
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly)
        || f.write(reinterpret_cast<const char*>(latent.data()),
                   static_cast<qint64>(latent.size() * sizeof(float)))
               != static_cast<qint64>(latent.size() * sizeof(float))) {
        if (error) *error = QStringLiteral("cannot write %1").arg(outPath);
        return false;
    }
    return true;
}

} // namespace ShapeGuide
