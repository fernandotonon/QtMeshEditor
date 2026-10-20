#ifndef DNA_DNACORRECTIVEMANAGER_H
#define DNA_DNACORRECTIVEMANAGER_H

// Live body correctives for imported MetaHuman characters (epic #1095, Phase 2).
//
// Each frame, AFTER the skeleton has been posed from its animation clips, the
// manager reads the rotation of every driver joint relative to its neutral
// pose, evaluates the DNA's RigLogic (RBF solvers + joint groups) and writes
// the resulting absolute local transforms onto the corrective joints. A
// viewport toggle compares ordinary skinning with the corrected deformation.
//
// Evaluation order (what avoids a double transform): animation clips write
// ONLY the driver/skeleton joints (the DNA's corrective joints carry no
// tracks), the manager then overwrites the corrective joints from the rig, and
// Ogre skins with the result — so a corrective is never layered on top of
// itself. The hook mirrors ConstraintManager (preUpdateSceneGraph, state-set
// dirty bump so a paused clip still shows the correction).
//
// Known limit: ConstraintManager and this manager both re-pose the skeleton
// from its clips before writing, so enabling both on the SAME entity lets the
// later listener discard the earlier one's writes.

#include <OgreSceneManager.h>

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <memory>
#include <vector>

namespace Dna { class RigEvaluator; }
namespace PoseSpace { class Set; }
namespace Ogre { class Entity; class Bone; }
class QQmlEngine;
class QJSEngine;

class DnaCorrectiveManager : public QObject, public Ogre::SceneManager::Listener
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(int characterCount READ characterCount NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY statsChanged)

public:
    static DnaCorrectiveManager* instance();
    static DnaCorrectiveManager* peek() { return m_pSingleton; }
    static DnaCorrectiveManager* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    /// Register a DNA character. `entityName` is the scene node / entity name.
    /// Returns false (and attaches nothing) when the rig has no quaternion
    /// driver joints (e.g. a head DNA — facial controls are Phase 3).
    bool attach(const QString& entityName, std::shared_ptr<Dna::RigEvaluator> evaluator);
    /// Register a character whose correctives are generic pose-space RBF sets (PoseSpace::Set) instead of — or in
    /// addition to — RigLogic: each frame the sets' blend weights are computed from the driver bones' rotations and
    /// written to the entity's pose animation states (the channels PoseSpace::channelName names, attached to the
    /// mesh at import). `evaluator` may be null (a fitted or hand-authored character has no usable RigLogic); when
    /// given it is used only for its statistics — the sets already contain the corrective behaviour, so the bone
    /// correctives are NOT written (that would apply every corrective twice).
    bool attachPoseSpace(const QString& entityName, std::shared_ptr<std::vector<PoseSpace::Set>> sets,
                         std::shared_ptr<Dna::RigEvaluator> evaluator = nullptr);
    void detach(const QString& entityName);
    void clear();
    bool isAttached(const QString& entityName) const;

    bool enabled() const { return m_enabled; }
    /// Turn the corrected deformation on or off for every attached character.
    /// Off restores plain skinning (re-poses from the clips).
    void setEnabled(bool on);
    int characterCount() const { return m_chars.size(); }
    QString summary() const;

    /// Evaluate and apply now (also what the frame hook runs). Returns false
    /// when nothing was applied.
    bool evaluateNow();

    struct Stats {
        int drivers = 0;          ///< driver bones found on the skeleton
        int driven = 0;           ///< corrective bones written
        int missingDriven = 0;    ///< corrective joints the skeleton does not have (folded by the 256-bone limit)
        double calcMs = 0.0;      ///< RigLogic time of the last frame
        double applyMs = 0.0;     ///< bone-write time of the last frame
        int poseSpaceSets = 0;    ///< pose-space sets driving this character (0 = RigLogic bone correctives)
        int poseSpaceActive = 0;  ///< samples with a non-zero weight on the last frame
    };
    Stats statsFor(const QString& entityName) const;
    /// JSON-friendly stats for MCP / CLI.
    Q_INVOKABLE QVariantMap statsMap(const QString& entityName = QString()) const;

    /// Preview pose: hold `joint` at `deltaWxyz` (rotation relative to its neutral
    /// pose, in the bone's local frame) on top of whatever the clips do. Lets the
    /// CLI / MCP bend a limb without authoring a clip. Returns false when the
    /// entity or bone is unknown.
    bool setPoseOverride(const QString& entityName, const QString& joint, const std::array<double, 4>& deltaWxyz);
    void clearPoseOverrides(const QString& entityName = QString());

    /// CPU-skinned vertex positions (x,y,z per vertex, every submesh in order) at
    /// the entity's current pose — used to quantify what the correctives change.
    static std::vector<float> skinnedPositions(Ogre::Entity* entity);

    /// Pure: rotation of `current` relative to `initial` in the bone's local
    /// frame (initial^-1 * current), w,x,y,z. Exposed for tests.
    static std::array<double, 4> localDelta(const std::array<double, 4>& initial,
                                            const std::array<double, 4>& current);

    // Ogre::SceneManager::Listener
    void preUpdateSceneGraph(Ogre::SceneManager* source, Ogre::Camera* camera) override;

signals:
    void changed();
    void statsChanged();

private:
    DnaCorrectiveManager();
    ~DnaCorrectiveManager() override;
    void ensureListener();
    void detachListener();
    struct Character;
    bool applyTo(const QString& name, Character& c, bool correctives);
    bool anyOverrides() const;
    void restorePlain(const QString& name);

    static DnaCorrectiveManager* m_pSingleton;
    QHash<QString, std::shared_ptr<Character>> m_chars;
    bool m_enabled = true;
    bool m_evaluating = false;
    Ogre::SceneManager* m_listened = nullptr;
};

#endif  // DNA_DNACORRECTIVEMANAGER_H
