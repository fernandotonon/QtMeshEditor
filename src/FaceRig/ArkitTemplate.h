#ifndef ARKITTEMPLATE_H
#define ARKITTEMPLATE_H

// The ARKit blendshape TEMPLATE that face auto-rig (#889) transfers onto a
// user mesh. It is the ICT-FaceKit generic-neutral head (MIT, USC-ICT) plus
// its 52 ARKit-named expression deltas, packed by scripts/export-arkit-
// template.py into one binary (arkit_template.bin) and downloaded on first
// use to AppData/ai_models/facerig/.
//
// Ogre-free, pure data — the NonRigidICP (#891) / DeformationTransfer (#892)
// stages consume it; headless-unit-tested. Shape names are the canonical
// FaceCap::kBlendshapeNames (the mocap-52 vocabulary), so the generated
// morph targets match what face capture drives.

#include <QString>
#include <QStringList>

#include <array>
#include <vector>

namespace FaceRig {

struct ArkitShape {
    QString name;                    // a FaceCap::kBlendshapeNames entry
    std::vector<float> deltas;       // vertexCount*3, (expr - neutral)
};

class ArkitTemplate {
public:
    bool valid() const { return m_vertexCount > 0 && !m_shapes.empty(); }
    int vertexCount() const { return m_vertexCount; }
    int faceCount() const { return m_faceCount; }
    int shapeCount() const { return static_cast<int>(m_shapes.size()); }

    // neutral positions, vertexCount*3 (x,y,z interleaved)
    const std::vector<float>& neutral() const { return m_neutral; }
    // triangle vertex indices, faceCount*3
    const std::vector<int>& faces() const { return m_faces; }
    const std::vector<ArkitShape>& shapes() const { return m_shapes; }
    QStringList shapeNames() const;

    // Load from an explicit arkit_template.bin path.
    bool load(const QString& path, QString* error = nullptr);

    // ---- model management (the house pattern) ----------------------------
    static QString modelPath();      // AppData/ai_models/facerig/arkit_template.bin
    static bool present();
    // Blocking first-use download; returns the path or empty
    // (offline guard QTMESH_FACERIG_NO_DOWNLOAD; base URL override
    // QTMESH_FACERIG_MODEL_BASE_URL / QSettings ai/facerigModelBaseUrl).
    static QString ensureModelBlocking();

private:
    int m_vertexCount = 0;
    int m_faceCount = 0;
    std::vector<float> m_neutral;
    std::vector<int> m_faces;
    std::vector<ArkitShape> m_shapes;
};

}  // namespace FaceRig

#endif  // ARKITTEMPLATE_H
