#ifndef RECONSTRUCTEDMESH_H
#define RECONSTRUCTEDMESH_H

#include <QString>
#include <QVector>

#include <cstdint>

/** One vertex in reconstructed editor space (#422). */
struct ReconstructedVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t diffuseArgb = 0xFFFFFFFFu;
};

struct ReconstructedSubMesh {
    QString materialName;
    QVector<ReconstructedVertex> vertices;
    QVector<uint32_t> indices;
};

struct ReconstructedMesh {
    QString meshName;
    QVector<ReconstructedSubMesh> subMeshes;
    int vertexCount = 0;
    int triangleCount = 0;

    bool isEmpty() const { return subMeshes.isEmpty(); }
};

#endif // RECONSTRUCTEDMESH_H
