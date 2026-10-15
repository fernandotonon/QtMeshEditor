/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef BONENAMINGMAP_H
#define BONENAMINGMAP_H

#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

/**
 * @brief Left/right bone-name pairing for mirror operations (Skel slice E, #559).
 *
 * The plumbing every "mirror" operation needs: given a bone name, produce the
 * name of its opposite-side counterpart, so a mirror op can move a left-hand
 * value onto the right hand. Pure data — no Ogre, no Qt GUI, no singleton — so
 * every convention rule is unit-testable headlessly and the pure-data
 * `WeightPaintOps` layer can consume it without gaining an Ogre dependency.
 *
 * This REPLACES the ad-hoc heuristic that shipped inside
 * `PoseLibrary::flipBoneName`, which is now a thin forwarder (the issue's
 * "consumed without re-implementing detection" criterion). Beyond that
 * heuristic's four rules it adds `*_left`/`*_right` words, lowercase
 * `left`/`right`, bare `L_`/`R_` affixes, 3ds Max Biped standalone side
 * tokens, and user-defined regex conventions.
 *
 * ## Side vs. counterpart
 *
 * Two distinct questions, deliberately separate calls:
 *
 *   - `sideOf(name)`   — which side is this bone on? (Left / Right / Center)
 *   - `counterpart(name)` — what is the opposite-side bone called?
 *
 * A bone can have a side but no counterpart in a given skeleton (its partner
 * simply isn't there); `pairs()` reports that as unpaired rather than
 * inventing a name.
 *
 * ## Names lie
 *
 * On generated rigs (UniRig / AutoRig) the side WORDS are not reliable — the
 * `#969` fix in `AnimationMerger` exists precisely because "Left*" bones can
 * sit on the anatomical right. This class answers only the NAMING question.
 * Callers that need anatomical truth should cross-check bind-pose geometry;
 * `PoseMirror` surfaces that as a diagnostic rather than silently overriding
 * the names.
 */
namespace BoneNaming {

/// Which side of the rig a bone name claims to be on.
enum class Side {
    Center = 0, ///< no side token found (Spine, Hips, Head)
    Left,
    Right,
};

/// One user-defined convention: a regex and the replacement that flips it.
/// Both directions must be supplied — a convention is only useful if it can
/// map left→right AND right→left, and asking the user for both halves is
/// clearer than trying to invert a regex automatically.
struct CustomRule {
    QString leftPattern;      ///< regex matching a LEFT-side name
    QString leftReplacement;  ///< replacement turning that match into the RIGHT name
    QString rightPattern;     ///< regex matching a RIGHT-side name
    QString rightReplacement; ///< replacement turning that match into the LEFT name

    bool isValid() const;
};

/// Result of pairing a whole skeleton's bone list — what the inspector
/// diagnostic panel renders so a user can verify their rig follows a
/// convention BEFORE relying on mirror ops.
struct PairingReport {
    struct Pair {
        QString left;
        QString right;
        QString convention; ///< human-readable id of the rule that matched
    };
    QVector<Pair> pairs;
    /// Bones with no side token — mirrored in place across the plane.
    QStringList centerBones;
    /// Bones WITH a side token whose counterpart name is absent from the
    /// skeleton. These are the ones that make a mirror op silently incomplete,
    /// so they are called out separately from centre bones.
    QStringList unpairedSided;
    /// Names that two different source bones both flip onto. A collision means
    /// a mirror would drop data, so it is reported rather than tolerated.
    QStringList collisions;

    int pairCount() const { return static_cast<int>(pairs.size()); }
};

/// The naming conventions recognised out of the box.
/// `Custom` covers every user-supplied rule.
enum class Convention {
    None = 0,
    UnderscoreLR,   ///< Hand_l / Hand_r, Hand_L / Hand_R
    DotLR,          ///< Hand.l / Hand.r, Hand.L / Hand.R (Blender)
    LeftRightWord,  ///< LeftHand / RightHand (Mixamo, Unity HumanIK)
    SuffixLeftWord, ///< hand_left / hand_right
    AffixLetter,    ///< L_Hand / R_Hand, lHand / rHand, handL / handR
    BipedToken,     ///< "Bip001 L UpperArm" / "Bip001 R UpperArm" (3ds Max)
    Custom,
};

/// Stable, human-readable id for a convention (used by the diagnostic panel,
/// the CLI/MCP JSON payloads, and Sentry breadcrumbs).
QString conventionId(Convention c);

/**
 * @brief The pairing engine.
 *
 * Cheap to construct and copy; holds only the custom-rule list. Built-in rules
 * are always active — a custom rule ADDS a convention rather than replacing
 * the defaults, because a rig that needs a custom rule for its control bones
 * usually still names its deform bones conventionally.
 *
 * Custom rules are tried FIRST so a user can override a built-in
 * interpretation for a rig the defaults read wrongly.
 */
class Map
{
public:
    Map() = default;
    explicit Map(QVector<CustomRule> custom) : m_custom(std::move(custom)) {}

    void setCustomRules(QVector<CustomRule> rules) { m_custom = std::move(rules); }
    const QVector<CustomRule>& customRules() const { return m_custom; }
    void addCustomRule(const CustomRule& rule) { m_custom.push_back(rule); }
    void clearCustomRules() { m_custom.clear(); }

    /// Which side `boneName` claims. `Center` when no rule matches.
    Side sideOf(const QString& boneName) const;

    /// The opposite-side name for `boneName`, or an EMPTY string when the bone
    /// carries no side token (a centre bone has no counterpart — returning the
    /// input unchanged, as the old `flipBoneName` did, makes "no counterpart"
    /// indistinguishable from "maps to itself" and invites a mirror op to
    /// write a bone onto itself).
    QString counterpart(const QString& boneName) const;

    /// Which convention matched `boneName` (`None` for centre bones).
    Convention conventionOf(const QString& boneName) const;

    /// Pair up a whole skeleton's bone list for the diagnostic panel.
    /// Each pair is listed ONCE, ordered by the left-side name's position in
    /// `boneNames` so the panel's order is stable across runs.
    PairingReport pair(const QStringList& boneNames) const;

    /// Convenience: does this bone list follow any recognised convention at
    /// all? Used to disable mirror UI with a clear reason rather than letting
    /// the user run an op that can only no-op.
    bool hasAnyPair(const QStringList& boneNames) const { return pair(boneNames).pairCount() > 0; }

private:
    /// Try every rule in priority order; returns the counterpart and reports
    /// which convention matched plus the side of the INPUT name.
    QString applyRules(const QString& boneName, Convention* matched, Side* side) const;

    QVector<CustomRule> m_custom;
};

/// Build a bone-handle → counterpart-handle table from a handle-indexed name
/// list (`SkinEvaluate::EvalData::boneNames`), for the pure-data weight mirror
/// `WeightPaintOps::mirrorByPositionWithBonePairs`.
///
/// Entries with no counterpart, or whose counterpart is not present in
/// `boneNamesByHandle`, are left at -1 ("keep your own weights"). Shared by the
/// GUI controller and the CLI so the two cannot build the table differently —
/// the same reason `paintBakeToDirectory` is one core for both surfaces.
/// `pairCount`, when given, receives the number of real pairs.
std::vector<int> buildPairTable(const QStringList& boneNamesByHandle,
                                const Map& naming,
                                int* pairCount = nullptr);

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
// Kept as free functions rather than members so the `Map` above stays pure
// data (no QSettings dependency), which is what lets it be unit-tested with
// no application instance and reused from the pure-data weight-paint layer.

/// Load the user's custom conventions from QSettings. Invalid rules are
/// dropped (a half-written rule must not make every mirror op fail).
QVector<CustomRule> loadCustomRules();

/// Persist custom conventions, replacing any previously stored set.
/// Invalid rules are rejected; returns the number actually written.
int saveCustomRules(const QVector<CustomRule>& rules);

} // namespace BoneNaming

#endif // BONENAMINGMAP_H
