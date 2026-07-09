#pragma once

#include <QStringList>

struct LightSnapshot;

namespace Ogre
{
class Entity;
}

namespace LightLinking
{

/// Ogre matches lights to objects when `(lightMask & objectMask) != 0`.
/// We allocate bits 1..31 per linked light (bit 0 reserved). At most 31
/// simultaneous link rules — document this limit in CLAUDE.md.
constexpr uint32_t kDefaultMask = 0xFFFFFFFFu;
constexpr int kMaxLinkChannels = 31;

enum class Mode
{
    None = 0,
    Include,
    Exclude
};

QString modeToString(Mode mode);
Mode modeFromString(const QString& text);

/// Apply linking from a snapshot onto the live scene (light + entity masks).
void applyFromSnapshot(const LightSnapshot& snapshot);

/// Remove a light's channel from the pool and reset entity bits for that channel.
void onLightDeleted(const LightSnapshot& snapshot);

/// New mesh entities pick up include/exclude masks from active rules.
void onEntityCreated(Ogre::Entity* entity);

/// Collect every scene entity scene-node name (for the inspector picker).
QStringList allEntityNodeNames();

/// Test / scene-reset helper.
void clearAllRules();

} // namespace LightLinking
