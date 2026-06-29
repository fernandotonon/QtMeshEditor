#ifndef UV_PIPELINE_H
#define UV_PIPELINE_H

#include "UvProject.h"
#include "UvUnwrap.h"

#include <QJsonObject>
#include <QString>
#include <vector>

namespace Ogre {
class Entity;
}

/// Headless UV operations shared by CLI and MCP (issue #465).
class UvPipeline
{
public:
    struct InfoReport {
        QList<UvUnwrap::UvInfo> submeshes;
        int islandCount = 0;
        /// Upper-bound fraction of triangles with overlapping UV0 AABBs; -1 if N/A.
        double overlappingUvsRatio = -1.0;
    };

    struct SeamEdge {
        int subMeshIndex = 0;
        unsigned int vertA = 0;
        unsigned int vertB = 0;
    };

    static InfoReport analyzeEntity(const Ogre::Entity* entity, int uvChannel = 0);
    static QJsonObject infoToJson(const QString& fileName, const InfoReport& report);
    static QString infoToText(const QString& fileName, const InfoReport& report);

    static UvProject::Report projectEntity(Ogre::Entity* entity, UvProject::Mode mode,
                                           int uvChannel = 0,
                                           const UvProject::Options& opts = {});

    static bool parseSeamEdgeList(const QString& spec, std::vector<SeamEdge>& out,
                                  QString* error = nullptr);
    static bool setSeamsOnEntity(Ogre::Entity* entity, const std::vector<SeamEdge>& edges,
                                 QString* error = nullptr);

    static UvUnwrapReport unwrapEntity(Ogre::Entity* entity, const UvUnwrapOptions& opts);

    /// Re-unwrap only the listed local triangle indices on one submesh.
    static UvUnwrapReport unwrapTriangles(Ogre::Entity* entity, int subMeshIndex,
                                          const std::vector<int>& triangleIndices,
                                          const UvUnwrapOptions& opts);

    /// Parse CLI/MCP projection mode names (box, cylinder, sphere, reset).
    static UvProject::Mode parseProjectMode(const QString& name, bool* ok = nullptr);
};

#endif // UV_PIPELINE_H
