#include "DrawCallAnalyzer.h"
#include "Manager.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreMaterial.h>

#include <QHash>
#include <QJsonArray>
#include <algorithm>

namespace {

QString materialNameOrPlaceholder(const Ogre::SubEntity* sub)
{
    if (!sub) return QStringLiteral("(none)");
    const auto mat = sub->getMaterial();
    if (!mat) return QStringLiteral("(none)");
    return QString::fromStdString(mat->getName());
}

} // namespace

DrawCallReport DrawCallAnalyzer::analyze(const QList<Ogre::Entity*>& entities)
{
    DrawCallReport report;

    // Hash<material name → cluster>. We accumulate in insertion order via a
    // parallel list of keys so the output is stable across runs.
    QHash<QString, MaterialCluster> byMaterial;
    QStringList materialOrder;

    for (const Ogre::Entity* entity : entities) {
        if (!entity) continue;
        report.totalEntities++;
        const QString entityName = QString::fromStdString(entity->getName());
        const unsigned int numSubs = entity->getNumSubEntities();
        report.totalSubmeshes += static_cast<int>(numSubs);

        // Per-entity material set: a single entity that has 3 submeshes all
        // bound to the same material still costs 3 draw calls (Ogre cannot
        // batch them), but the merge-suggestion grouping should count the
        // entity once per unique material it uses.
        QSet<QString> entityMaterials;
        for (unsigned int i = 0; i < numSubs; ++i) {
            const Ogre::SubEntity* sub = entity->getSubEntity(i);
            const QString matName = materialNameOrPlaceholder(sub);
            report.totalDrawCalls++; // one draw call per SubEntity

            MaterialCluster& cluster = byMaterial[matName];
            if (!materialOrder.contains(matName)) {
                materialOrder.append(matName);
                cluster.materialName = matName;
            }
            cluster.submeshCount++;
            entityMaterials.insert(matName);
        }

        // Now record entity-per-material membership (once per material).
        for (const QString& matName : entityMaterials) {
            MaterialCluster& cluster = byMaterial[matName];
            if (!cluster.entityNames.contains(entityName))
                cluster.entityNames.append(entityName);
        }
    }

    report.uniqueMaterials = materialOrder.size();
    for (const QString& matName : materialOrder)
        report.clusters.append(byMaterial.value(matName));

    report.suggestions = buildSuggestions(report.clusters);
    for (const MergeSuggestion& s : report.suggestions)
        report.totalSavings += s.estimatedSavings;

    // Sort suggestions by savings (descending) so the most valuable merges
    // surface first. Stable order on ties so output is deterministic.
    std::stable_sort(report.suggestions.begin(), report.suggestions.end(),
        [](const MergeSuggestion& a, const MergeSuggestion& b) {
            return a.estimatedSavings > b.estimatedSavings;
        });

    report.potentialDrawCalls = report.totalDrawCalls - report.totalSavings;
    return report;
}

// LCOV_EXCL_START — exercised only when Ogre is initialised (skipped in unit tests).
DrawCallReport DrawCallAnalyzer::analyzeScene()
{
    QList<Ogre::Entity*> entities;
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return analyze(entities);

    for (Ogre::SceneNode* node : mgr->getSceneNodes()) {
        if (!node) continue;
        for (unsigned int i = 0; i < node->numAttachedObjects(); ++i) {
            Ogre::MovableObject* obj = node->getAttachedObject(i);
            if (!obj || obj->getMovableType() != "Entity") continue;
            entities.append(static_cast<Ogre::Entity*>(obj));
        }
    }
    return analyze(entities);
}
// LCOV_EXCL_STOP

QList<MergeSuggestion> DrawCallAnalyzer::buildSuggestions(
    const QList<MaterialCluster>& clusters, int minSharedEntities)
{
    QList<MergeSuggestion> out;
    for (const MaterialCluster& c : clusters) {
        if (c.entityNames.size() < minSharedEntities) continue;
        MergeSuggestion s;
        s.materialName = c.materialName;
        s.entityNames = c.entityNames;
        s.estimatedSavings = c.mergeSavings();
        out.append(s);
    }
    return out;
}

QJsonObject DrawCallAnalyzer::toJson(const DrawCallReport& report)
{
    QJsonObject obj;
    QJsonObject totals;
    totals["entities"] = report.totalEntities;
    totals["submeshes"] = report.totalSubmeshes;
    totals["drawCalls"] = report.totalDrawCalls;
    totals["uniqueMaterials"] = report.uniqueMaterials;
    totals["potentialDrawCalls"] = report.potentialDrawCalls;
    totals["totalSavings"] = report.totalSavings;
    obj["totals"] = totals;

    QJsonArray clusters;
    for (const MaterialCluster& c : report.clusters) {
        QJsonObject co;
        co["material"] = c.materialName;
        co["submeshCount"] = c.submeshCount;
        QJsonArray names;
        for (const QString& n : c.entityNames) names.append(n);
        co["entities"] = names;
        co["mergeSavings"] = c.mergeSavings();
        clusters.append(co);
    }
    obj["clusters"] = clusters;

    QJsonArray suggestions;
    for (const MergeSuggestion& s : report.suggestions) {
        QJsonObject so;
        so["material"] = s.materialName;
        so["estimatedSavings"] = s.estimatedSavings;
        QJsonArray names;
        for (const QString& n : s.entityNames) names.append(n);
        so["entities"] = names;
        suggestions.append(so);
    }
    obj["suggestions"] = suggestions;

    return obj;
}

QString DrawCallAnalyzer::toText(const DrawCallReport& report)
{
    QString out;
    QTextStream s(&out);

    s << "Draw Call Analysis\n";
    s << "==================\n\n";

    s << "Entities:       " << report.totalEntities << "\n";
    s << "Submeshes:      " << report.totalSubmeshes << "\n";
    s << "Draw calls:     " << report.totalDrawCalls << "\n";
    s << "Unique mats:    " << report.uniqueMaterials << "\n";
    s << "After merges:   " << report.potentialDrawCalls
      << " (saves " << report.totalSavings << ")\n\n";

    if (!report.clusters.isEmpty()) {
        s << "Materials:\n";
        for (const MaterialCluster& c : report.clusters) {
            s << "  " << c.materialName
              << "  submeshes=" << c.submeshCount
              << "  entities=" << c.entityNames.size() << "\n";
        }
        s << "\n";
    }

    if (!report.suggestions.isEmpty()) {
        s << "Merge suggestions (saves >0 draw calls):\n";
        for (const MergeSuggestion& sug : report.suggestions) {
            s << "  " << sug.materialName
              << "  merge " << sug.entityNames.size()
              << " entities → save " << sug.estimatedSavings
              << " draw calls\n";
            for (const QString& n : sug.entityNames)
                s << "    - " << n << "\n";
        }
    } else if (report.totalEntities > 0) {
        s << "No merge opportunities (each material is used by at most one entity).\n";
    }

    return out;
}
