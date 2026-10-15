#ifndef QUADRIFLOW_RUNNER_H
#define QUADRIFLOW_RUNNER_H

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

// Drives the out-of-process `qtmesh_quadriflow` helper (src/QuadriFlowHelper)
// with QProcess. QuadriFlow is never linked into the editor: it calls exit()
// from inside its solvers on failure and crashes on some inputs, and in a
// helper process either outcome is just an error message. Cancel = kill.
//
// Success is decided by the helper's "QF_DONE" line, NOT its exit code —
// upstream exits 0 on several internal failures.
namespace QuadriFlowRunner {

struct Request {
    std::vector<float>    positions;   // Nx3 (welded, manifold)
    std::vector<uint32_t> indices;     // Mx3
    int  targetFaces = 5000;
    bool preserveSharp = false;
    bool preserveBoundary = false;
    bool adaptiveScale = false;
    int  seed = 0;
    int  timeoutMs = 10 * 60 * 1000;
    QString helperPath;                // empty = defaultHelperPath()
    // (stage index 1..6, stage name) on each helper stage, and (0, "") as a
    // ~10 Hz poll while it runs. Return false to cancel (kills the helper).
    std::function<bool(int stage, const QString& name)> progress;
};

struct Outcome {
    bool ok = false;
    bool cancelled = false;
    bool timedOut = false;
    bool helperMissing = false;   // no point retrying with other input
    QString error;
    std::vector<float> positions;                  // Nx3
    std::vector<std::vector<uint32_t>> faces;      // quads (degenerates → tris)
};

// Next to the running executable (macOS: inside Contents/MacOS), overridable
// with QTMESH_QUADRIFLOW_HELPER. Empty when it isn't there.
QString defaultHelperPath();

Outcome run(const Request& req);

// OBJ helpers (exposed for tests).
bool writeObj(const QString& path, const std::vector<float>& positions,
              const std::vector<uint32_t>& indices);
bool readQuadObj(const QString& path, std::vector<float>& positions,
                 std::vector<std::vector<uint32_t>>& faces, QString* error = nullptr);

} // namespace QuadriFlowRunner

#endif // QUADRIFLOW_RUNNER_H
