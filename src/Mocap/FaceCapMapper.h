#ifndef FACECAPMAPPER_H
#define FACECAPMAPPER_H

// Maps the 52 canonical MediaPipe/ARKit blendshape names onto the target
// mesh's actual morph-target names (epic #869, Slice C #872). Pure data —
// QtCore only — headless-tested.
//
// Matching, in priority order per canonical channel:
//   1. explicit override from the JSON sidecar ("map"),
//   2. normalized name equality: lowercase, separators ('_', '-', '.', ' ')
//      stripped, with trailing side tokens expanded first so "Mouth_Smile_L",
//      "mouthSmile.R" and "MouthSmileLeft" all normalize the same way,
//   3. alias table for common third-party conventions.
//
// Override JSON sidecar format:
//   {
//     "map":    { "jawOpen": "MyJawTarget", ... },   // canonical -> mesh name
//     "ignore": [ "tongueOut", ... ]                 // canonical names to skip
//   }
//
// Unmatched channels are REPORTED (unmatchedCanonical / unmatchedMesh),
// never silently dropped. "_neutral" (index 0) is a rest-weight channel with
// no mesh equivalent — it is excluded from mapping and reporting.

#include <QString>
#include <QStringList>

namespace FaceCapMapper {

struct Channel {
    int canonicalIndex = -1;   // into FaceCap::kBlendshapeNames
    QString meshTargetName;    // exact morph-target name on the mesh
};

struct Mapping {
    QList<Channel> channels;
    QStringList unmatchedCanonical;  // canonical names with no mesh target
    QStringList unmatchedMesh;       // mesh targets no channel drives
    QStringList ignored;             // canonical names ignored via override
    QString error;                   // override-file problem (mapping still built)

    bool isEmpty() const { return channels.isEmpty(); }
};

Mapping build(const QStringList& meshTargetNames,
              const QString& overrideJsonPath = {});

// exposed for tests: "Mouth_Smile_L" -> "mouthsmileleft"
QString normalizedName(const QString& name);

}  // namespace FaceCapMapper

#endif  // FACECAPMAPPER_H
