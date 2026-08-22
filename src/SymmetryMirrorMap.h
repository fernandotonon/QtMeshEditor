/*
-----------------------------------------------------------------------------------
This source file is part of QtMeshEditor.

Paint v2 Slice E — topology-aware symmetry mirror (issue #548).

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef SYMMETRYMIRRORMAP_H
#define SYMMETRYMIRRORMAP_H

#include <OgreVector.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

class EditableMesh;

/**
 * @brief Topology-aware mirror correspondence for symmetric texture painting.
 *
 * For a mesh that is (nearly) symmetric in POSITION but whose UV unwrap is
 * asymmetric, a purely geometric mirror (reflect the 3D hit, re-raycast for the
 * UV) lands on the mirror geometry but reads the WRONG UV. This class builds a
 * per-vertex position mirror correspondence across one axis, then maps a dab
 * (submesh, triangle, barycentric) to the mirror triangle and remaps the
 * barycentric weights to that triangle's own corner order — so the mirrored dab
 * samples the mirror triangle's UVs, which is correct even under an asymmetric
 * unwrap.
 *
 * Pure data (no Ogre::Entity / GPU): operates on EditableMesh, so it is fully
 * unit-testable headless.
 */
class SymmetryMirrorMap
{
public:
    /// A reference to a vertex in the flattened (submesh, index) space.
    struct VertRef {
        int submesh = -1;
        int index = -1;
        bool valid() const { return submesh >= 0 && index >= 0; }
        bool operator==(const VertRef& o) const {
            return submesh == o.submesh && index == o.index;
        }
    };

    /**
     * Build the correspondence across a single axis.
     * @param mesh     source geometry.
     * @param axisBit  one of SymAxisX(1)/SymAxisY(2)/SymAxisZ(4).
     * @param pivot    mesh-local pivot the reflection plane passes through.
     * @param weldTol  max distance a vertex may be from its reflected partner.
     * @return true if a usable map was built (see coverage()).
     */
    bool build(const EditableMesh& mesh, int axisBit,
               const Ogre::Vector3& pivot, float weldTol);

    bool valid() const { return m_valid; }
    /// Fraction of vertices that found a mirror partner (0..1).
    float coverage() const { return m_coverage; }

    /**
     * Map a primary dab to its mirror dab.
     * @param submesh          submesh of the primary triangle.
     * @param corner           the primary triangle's three CORNER vertex indices
     *                         (into that submesh), in stored order.
     * @param bary             barycentric weights on those three corners.
     * @param outSubmesh,outTriangle  the mirror triangle.
     * @param outBary          barycentric weights PERMUTED to the mirror
     *                         triangle's own stored corner order.
     * @return false when no topological correspondence exists (caller falls
     *         back to the geometric resolver).
     *
     * The caller supplies the corner vertex indices (from its hit cache), so the
     * map stays self-contained and needs no back-reference to the mesh.
     */
    bool mirrorDab(int submesh, const int corner[3], const float bary[3],
                   int& outSubmesh, int& outTriangle, float outBary[3]) const;

private:
    // Per (submesh) → per (vertex index) → mirror VertRef (invalid if none).
    std::vector<std::vector<VertRef>> m_vertMirror;
    // Sorted (a<=b<=c) global-vertex-id triple → (submesh, triangle).
    std::unordered_map<uint64_t, std::pair<int, int>> m_faceByVerts;
    // Per (submesh) → per (triangle) → its three GLOBAL corner ids, in stored
    // order (so mirrorDab can permute barycentrics to that order).
    std::vector<std::vector<std::array<int, 3>>> m_faceCorners;
    // Flattened base id per submesh, so (submesh,index) → a single global id.
    std::vector<int> m_submeshBase;
    bool  m_valid = false;
    float m_coverage = 0.0f;

    int  globalId(int submesh, int index) const {
        return m_submeshBase[static_cast<size_t>(submesh)] + index;
    }
    static uint64_t triKey(int a, int b, int c);
};

#endif // SYMMETRYMIRRORMAP_H
