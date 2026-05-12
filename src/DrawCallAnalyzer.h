#ifndef DRAWCALLANALYZER_H
#define DRAWCALLANALYZER_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>

namespace Ogre {
    class Entity;
}

// One row per (material, entity-list) cluster. A draw call is counted per
// SubEntity that uses the material — see DrawCallAnalyzer::analyze for the
// counting rule.
struct MaterialCluster {
    QString materialName;
    int submeshCount = 0;          // total SubEntities bound to this material
    QStringList entityNames;       // names of entities that use this material
    // Merge potential: every additional entity past the first is a draw call
    // we could save by merging — provided the geometry is compatible.
    int mergeSavings() const {
        return entityNames.isEmpty() ? 0 : static_cast<int>(entityNames.size()) - 1;
    }
};

struct MergeSuggestion {
    QString materialName;
    QStringList entityNames;
    int estimatedSavings = 0;      // draw calls saved if the merge is performed
};

struct DrawCallReport {
    int totalEntities = 0;
    int totalSubmeshes = 0;        // sum of SubEntity counts across entities
    int totalDrawCalls = 0;        // current estimated draw-call count
    int uniqueMaterials = 0;
    int potentialDrawCalls = 0;    // draw calls if all viable merges happened
    int totalSavings = 0;          // totalDrawCalls - potentialDrawCalls
    QList<MaterialCluster> clusters;
    QList<MergeSuggestion> suggestions;
};

// Pure-data analyzer.  All methods are static and side-effect free.
class DrawCallAnalyzer {
public:
    // Analyze a list of entities and produce a DrawCallReport. Null pointers
    // in the input are skipped. The draw-call estimate counts one call per
    // SubEntity (Ogre's coarsest granularity is the SubEntity render op).
    static DrawCallReport analyze(const QList<Ogre::Entity*>& entities);

    // Build a report for every entity currently attached under the scene
    // root. Convenience wrapper.
    static DrawCallReport analyzeScene();

    // Serialize the report as JSON (CLI / MCP).
    static QJsonObject toJson(const DrawCallReport& report);

    // Serialize the report as human-readable text (CLI default).
    static QString toText(const DrawCallReport& report);

    // Suggestion-list filter: only clusters with `>= minSharedEntities`
    // entities are reported, so the noise of single-instance materials
    // does not crowd the output. Default 2 (two entities = at least one
    // draw call saved by a merge).
    static QList<MergeSuggestion> buildSuggestions(
        const QList<MaterialCluster>& clusters, int minSharedEntities = 2);
};

#endif // DRAWCALLANALYZER_H
