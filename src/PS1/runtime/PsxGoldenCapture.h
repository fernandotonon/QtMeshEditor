#ifndef PSXGOLDENCAPTURE_H
#define PSXGOLDENCAPTURE_H

#include <QString>
#include <QStringList>

/** Golden-scene IDs and env vars for PS1 rip acceptance (#659). */
namespace PsxGoldenCapture {

inline constexpr const char *kSceneHomebrewStatic = "homebrew-static";
inline constexpr const char *kSceneRetailA = "retail-a";
inline constexpr const char *kSceneRetailB = "retail-b";
/** Custom-engine class (Crash / Spyro / FFVII field / MGS) — the titles that
 *  defeat every RAM-scan path and motivated the in-core pivot (#817). */
inline constexpr const char *kSceneRetailC = "retail-c";

inline constexpr const char *kEnvBios = "QTMESH_PS1_TEST_BIOS";
inline constexpr const char *kEnvSceneId = "QTMESH_PS1_GOLDEN_SCENE_ID";
inline constexpr const char *kEnvHomebrewIso = "QTMESH_PS1_GOLDEN_HOMEBREW_ISO";
inline constexpr const char *kEnvHomebrewIsoLegacy = "QTMESH_PS1_TEST_HOMEBREW_ISO";
inline constexpr const char *kEnvRetailAIso = "QTMESH_PS1_GOLDEN_RETAIL_A_ISO";
inline constexpr const char *kEnvRetailBIso = "QTMESH_PS1_GOLDEN_RETAIL_B_ISO";
inline constexpr const char *kEnvRetailCIso = "QTMESH_PS1_GOLDEN_RETAIL_C_ISO";
inline constexpr const char *kEnvRetailAIsoLegacy = "QTMESH_PS1_TEST_ISO";

inline constexpr int kMinTrianglesManualPass = 8;
inline constexpr int kMinTrianglesEnvIntegration = 3;

QStringList allSceneIds();
bool isKnownSceneId(const QString &sceneId);
QString isoEnvVarForScene(const QString &sceneId);
QString isoPathForScene(const QString &sceneId);
QString biosPath();
QString activeSceneId();
QStringList configuredSceneIds();

} // namespace PsxGoldenCapture

#endif // PSXGOLDENCAPTURE_H
