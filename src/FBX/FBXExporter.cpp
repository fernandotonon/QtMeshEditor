/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#include "FBXExporter.h"

#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreSubEntity.h>
#include <OgreSkeleton.h>
#include <OgreBone.h>
#include <OgreAnimation.h>
#include <OgreKeyFrame.h>
#include <OgreHardwareBufferManager.h>
#include <OgreLogManager.h>
#include <OgreMaterialManager.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreDataStream.h>

#include <fstream>
#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <cstdint>

// FBX time: 1 second = 46186158000 FBX ticks
static constexpr int64_t FBX_TICKS_PER_SECOND = 46186158000LL;

// FBX version 7300 (v7.3)
static constexpr uint32_t FBX_VERSION = 7300;

// ─── Z-mirror helpers ────────────────────────────────────────────
// Ogre stores data that was ConvertToLeftHanded during Assimp import
// (Z negated, UVs flipped, winding reversed).  To produce a correct
// FBX file that round-trips through Assimp reimport (which applies
// ConvertToLeftHanded again), we must undo the LH transform here:
//   negate Z positions/normals, flip UV V, reverse winding,
//   and mirror bone transforms across Z.

// Build a 4×4 local transform matrix from position, scale, orientation
static Ogre::Matrix4 buildLocalMatrix(const Ogre::Vector3& pos,
                                       const Ogre::Vector3& scl,
                                       const Ogre::Quaternion& ori)
{
    Ogre::Matrix3 rot3;
    ori.ToRotationMatrix(rot3);
    return Ogre::Matrix4(
        rot3[0][0]*scl.x, rot3[0][1]*scl.y, rot3[0][2]*scl.z, pos.x,
        rot3[1][0]*scl.x, rot3[1][1]*scl.y, rot3[1][2]*scl.z, pos.y,
        rot3[2][0]*scl.x, rot3[2][1]*scl.y, rot3[2][2]*scl.z, pos.z,
        0, 0, 0, 1
    );
}

// Compute the global bind-pose matrix for a bone from initial (bind) transforms.
// Unlike _getFullTransform() this is immune to the current animation state.
static Ogre::Matrix4 computeGlobalBindPose(Ogre::Bone* bone)
{
    Ogre::Matrix4 local = buildLocalMatrix(bone->getInitialPosition(),
                                           bone->getInitialScale(),
                                           bone->getInitialOrientation());
    auto* parent = dynamic_cast<Ogre::Bone*>(bone->getParent());
    if (parent)
        return computeGlobalBindPose(parent) * local;
    return local;
}

// Mirror a quaternion across the Z-plane:
//   rotation axis (ax,ay,az) → (ax,ay,-az) and angle negates
//   ⇒ q(w,x,y,z) → q(w,-x,-y,z)
static Ogre::Quaternion mirrorZ(const Ogre::Quaternion& q)
{
    return Ogre::Quaternion(q.w, -q.x, -q.y, q.z);
}

// Quaternion → Euler XYZ (degrees)
// Assimp's FBX RotOrder_EulerXYZ composes: R = Rz * Ry * Rx
// (see FBXConverter.cpp GetRotationMatrix — order is inverted for left-multiply).
// Decompose the rotation matrix accordingly.
static void quaternionToEulerXYZ(const Ogre::Quaternion& q,
                                  double& rx, double& ry, double& rz)
{
    double w = q.w, x = q.x, y = q.y, z = q.z;
    // For R = Rz * Ry * Rx:  R[2][0] = -sin(ry)
    // From quaternion: R[2][0] = 2(xz - wy)
    double sinp = std::clamp(2.0 * (w * y - x * z), -1.0, 1.0);
    ry = std::asin(sinp);

    double cosp = std::cos(ry);
    if (cosp > 1e-6)
    {
        // rx = atan2(R[2][1], R[2][2]) = atan2(2(yz + wx), 1 - 2(x² + y²))
        rx = std::atan2(2.0 * (y * z + w * x), 1.0 - 2.0 * (x * x + y * y));
        // rz = atan2(R[1][0], R[0][0]) = atan2(2(xy + wz), 1 - 2(y² + z²))
        rz = std::atan2(2.0 * (x * y + w * z), 1.0 - 2.0 * (y * y + z * z));
    }
    else
    {
        // Gimbal lock: set rz = 0, solve rx from remaining elements
        rz = 0.0;
        rx = std::atan2(-(2.0 * (x * y - w * z)), 1.0 - 2.0 * (x * x + z * z));
    }
    rx *= 180.0 / M_PI;
    ry *= 180.0 / M_PI;
    rz *= 180.0 / M_PI;
}

// 4x4 matrix → 16 doubles (row-major) with Z-mirror applied.
// Z-mirror: M' = S * M * S where S = diag(1,1,-1,1).
// Elements [0][2],[1][2],[2][0],[2][1],[2][3],[3][2] are negated.
static void matrix4ToDoublesMirrorZ(const Ogre::Matrix4& m, double* out)
{
    // FBX uses row-vector convention (v' = v * M) with translation in the last
    // row.  Ogre uses column-vector convention (v' = M * v) with translation in
    // the last column.  Writing transposed maps Ogre column-major → FBX row-major.
    Ogre::Matrix4 mz = m;
    // Z-mirror: negate the six off-diagonal Z elements (undo ConvertToLeftHanded)
    mz[0][2] = -mz[0][2];
    mz[1][2] = -mz[1][2];
    mz[2][0] = -mz[2][0];
    mz[2][1] = -mz[2][1];
    mz[2][3] = -mz[2][3];
    mz[3][2] = -mz[3][2];
    // Write transposed so translation lands in the last row for FBX
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c * 4 + r] = mz[r][c];
}

// ═══════════════════════════════════════════════════════════════════
//  FBX Binary Writer
// ═══════════════════════════════════════════════════════════════════

class FBXBinaryWriter
{
public:
    explicit FBXBinaryWriter(std::ofstream& out) : m_out(out) {}

    // ── Header ───────────────────────────────────────────────────
    void writeHeader()
    {
        // "Kaydara FBX Binary  \x00" (21 chars) + 0x1A 0x00
        const char magic[] = "Kaydara FBX Binary  ";
        m_out.write(magic, 21);       // 21 bytes including trailing \0
        char pad[2] = {0x1A, 0x00};
        m_out.write(pad, 2);
        writeU32(FBX_VERSION);        // 4 bytes → total 27
    }

    // ── Node begin/end with endOffset backpatching ───────────────
    void beginNode(const std::string& name)
    {
        NodeInfo ni;
        ni.endOffsetPos = static_cast<uint32_t>(m_out.tellp());
        writeU32(0);                  // endOffset placeholder
        ni.numPropsPos = static_cast<uint32_t>(m_out.tellp());
        writeU32(0);                  // numProperties placeholder
        ni.propListLenPos = static_cast<uint32_t>(m_out.tellp());
        writeU32(0);                  // propertyListLen placeholder
        uint8_t nameLen = static_cast<uint8_t>(name.size());
        m_out.write(reinterpret_cast<const char*>(&nameLen), 1);
        m_out.write(name.data(), nameLen);
        ni.propStartPos = static_cast<uint32_t>(m_out.tellp());
        ni.numProps = 0;
        m_nodeStack.push_back(ni);
    }

    void endNode()
    {
        auto& ni = m_nodeStack.back();

        // Write null sentinel (13 zero bytes) to terminate children
        writeNullRecord();

        uint32_t endOff = static_cast<uint32_t>(m_out.tellp());
        uint32_t propListLen = ni.propEndPos - ni.propStartPos;

        // Backpatch
        m_out.seekp(ni.endOffsetPos);
        writeU32(endOff);
        m_out.seekp(ni.numPropsPos);
        writeU32(ni.numProps);
        m_out.seekp(ni.propListLenPos);
        writeU32(propListLen);
        m_out.seekp(endOff);

        m_nodeStack.pop_back();
    }

    // endNode variant for leaf nodes (no children, no null record)
    void endNodeLeaf()
    {
        auto& ni = m_nodeStack.back();

        uint32_t endOff = static_cast<uint32_t>(m_out.tellp());
        uint32_t propListLen = ni.propEndPos - ni.propStartPos;

        m_out.seekp(ni.endOffsetPos);
        writeU32(endOff);
        m_out.seekp(ni.numPropsPos);
        writeU32(ni.numProps);
        m_out.seekp(ni.propListLenPos);
        writeU32(propListLen);
        m_out.seekp(endOff);

        m_nodeStack.pop_back();
    }

    // Mark that properties are done (for propEndPos tracking)
    void endProperties()
    {
        if (!m_nodeStack.empty())
            m_nodeStack.back().propEndPos = static_cast<uint32_t>(m_out.tellp());
    }

    // ── Property writers ─────────────────────────────────────────
    void writePropertyBool(bool v)
    {
        char type = 'C';
        m_out.write(&type, 1);
        uint8_t val = v ? 1 : 0;
        m_out.write(reinterpret_cast<const char*>(&val), 1);
        incrPropCount();
    }

    void writePropertyI(int32_t v)
    {
        char type = 'I';
        m_out.write(&type, 1);
        m_out.write(reinterpret_cast<const char*>(&v), 4);
        incrPropCount();
    }

    void writePropertyL(int64_t v)
    {
        char type = 'L';
        m_out.write(&type, 1);
        m_out.write(reinterpret_cast<const char*>(&v), 8);
        incrPropCount();
    }

    void writePropertyF(float v)
    {
        char type = 'F';
        m_out.write(&type, 1);
        m_out.write(reinterpret_cast<const char*>(&v), 4);
        incrPropCount();
    }

    void writePropertyD(double v)
    {
        char type = 'D';
        m_out.write(&type, 1);
        m_out.write(reinterpret_cast<const char*>(&v), 8);
        incrPropCount();
    }

    void writePropertyS(const std::string& s)
    {
        char type = 'S';
        m_out.write(&type, 1);
        uint32_t len = static_cast<uint32_t>(s.size());
        m_out.write(reinterpret_cast<const char*>(&len), 4);
        m_out.write(s.data(), len);
        incrPropCount();
    }

    void writePropertyR(const std::vector<uint8_t>& data)
    {
        char type = 'R';
        m_out.write(&type, 1);
        uint32_t len = static_cast<uint32_t>(data.size());
        m_out.write(reinterpret_cast<const char*>(&len), 4);
        m_out.write(reinterpret_cast<const char*>(data.data()), len);
        incrPropCount();
    }

    void writePropertyArrayD(const std::vector<double>& arr)
    {
        char type = 'd';
        m_out.write(&type, 1);
        uint32_t count = static_cast<uint32_t>(arr.size());
        writeU32(count);
        writeU32(0);  // encoding = 0 (uncompressed)
        uint32_t byteLen = count * 8;
        writeU32(byteLen);
        m_out.write(reinterpret_cast<const char*>(arr.data()), byteLen);
        incrPropCount();
    }

    void writePropertyArrayI(const std::vector<int32_t>& arr)
    {
        char type = 'i';
        m_out.write(&type, 1);
        uint32_t count = static_cast<uint32_t>(arr.size());
        writeU32(count);
        writeU32(0);  // encoding = 0 (uncompressed)
        uint32_t byteLen = count * 4;
        writeU32(byteLen);
        m_out.write(reinterpret_cast<const char*>(arr.data()), byteLen);
        incrPropCount();
    }

    void writePropertyArrayF(const std::vector<float>& arr)
    {
        char type = 'f';
        m_out.write(&type, 1);
        uint32_t count = static_cast<uint32_t>(arr.size());
        writeU32(count);
        writeU32(0);  // encoding = 0 (uncompressed)
        uint32_t byteLen = count * 4;
        writeU32(byteLen);
        m_out.write(reinterpret_cast<const char*>(arr.data()), byteLen);
        incrPropCount();
    }

    void writePropertyArrayL(const std::vector<int64_t>& arr)
    {
        char type = 'l';
        m_out.write(&type, 1);
        uint32_t count = static_cast<uint32_t>(arr.size());
        writeU32(count);
        writeU32(0);
        uint32_t byteLen = count * 8;
        writeU32(byteLen);
        m_out.write(reinterpret_cast<const char*>(arr.data()), byteLen);
        incrPropCount();
    }

    // ── Null record (13 zero bytes for v7300) ────────────────────
    void writeNullRecord()
    {
        char zeros[13] = {};
        m_out.write(zeros, 13);
    }

    // ── Footer ───────────────────────────────────────────────────
    void writeFooter()
    {
        // Top-level null sentinel
        writeNullRecord();

        // Footer: generate padding and unknown footer bytes
        // Pad to 16-byte alignment with 0 bytes, then write footer
        auto pos = m_out.tellp();
        int mod = static_cast<int>(pos) % 16;
        if (mod != 0)
        {
            int padLen = 16 - mod;
            std::vector<char> pad(padLen, 0);
            m_out.write(pad.data(), padLen);
        }

        // 4 bytes of padding
        uint32_t zero = 0;
        m_out.write(reinterpret_cast<const char*>(&zero), 4);

        // FBX footer magic: version + some fixed bytes
        // Standard 16-byte footer ID
        const uint8_t footerId[] = {
            0xF8, 0x5A, 0x8C, 0x6A, 0xDE, 0xF5, 0xD9, 0x7E,
            0xEC, 0xE9, 0x0C, 0xE3, 0x75, 0x8F, 0x29, 0x0B
        };
        m_out.write(reinterpret_cast<const char*>(footerId), 16);

        // Pad with zeros to another 16-byte boundary + 4
        pos = m_out.tellp();
        mod = static_cast<int>(pos) % 16;
        if (mod != 0)
        {
            int padLen = 16 - mod;
            std::vector<char> pad(padLen, 0);
            m_out.write(pad.data(), padLen);
        }

        // Final version stamp
        writeU32(FBX_VERSION);
        // 120 bytes of zeros
        char finalZeros[120] = {};
        m_out.write(finalZeros, 120);
        // Footer magic repeated
        m_out.write(reinterpret_cast<const char*>(footerId), 16);
    }

private:
    void writeU32(uint32_t v)
    {
        m_out.write(reinterpret_cast<const char*>(&v), 4);
    }

    void incrPropCount()
    {
        if (!m_nodeStack.empty())
        {
            m_nodeStack.back().numProps++;
            m_nodeStack.back().propEndPos = static_cast<uint32_t>(m_out.tellp());
        }
    }

    struct NodeInfo {
        uint32_t endOffsetPos = 0;
        uint32_t numPropsPos = 0;
        uint32_t propListLenPos = 0;
        uint32_t propStartPos = 0;
        uint32_t propEndPos = 0;
        uint32_t numProps = 0;
    };

    std::ofstream& m_out;
    std::vector<NodeInfo> m_nodeStack;
};

// ═══════════════════════════════════════════════════════════════════
//  FBX Document Builder
// ═══════════════════════════════════════════════════════════════════

class FBXDocumentBuilder
{
public:
    explicit FBXDocumentBuilder(FBXBinaryWriter& w) : m_w(w) {}

    bool build(const Ogre::Entity* entity)
    {
        const Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) return false;

        m_hasSkeleton = entity->hasSkeleton();
        m_skeleton = m_hasSkeleton ? mesh->getSkeleton().get() : nullptr;
        m_entity = entity;
        m_mesh = mesh.get();

        // Reset skeleton to bind pose before reading transforms
        if (m_skeleton)
            m_skeleton->reset();

        m_w.writeHeader();

        writeHeaderExtension();
        writeGlobalSettings();
        writeDocuments();
        writeReferences();
        writeDefinitions();
        writeObjects();
        writeConnections();

        m_w.writeFooter();
        return true;
    }

    bool buildSkeletonOnly(const Ogre::Skeleton* skeleton)
    {
        if (!skeleton) return false;
        m_skeletonOnly = true;
        m_hasSkeleton = true;
        m_skeleton = const_cast<Ogre::Skeleton*>(skeleton);
        m_entity = nullptr;
        m_mesh = nullptr;

        m_skeleton->reset();

        m_w.writeHeader();
        writeHeaderExtension();
        writeGlobalSettings();
        writeDocuments();
        writeReferences();
        writeDefinitions();
        writeObjects();
        writeConnections();
        m_w.writeFooter();
        return true;
    }

private:
    int64_t nextId() { return m_nextId++; }

    // ── FBXHeaderExtension ───────────────────────────────────────
    void writeHeaderExtension()
    {
        m_w.beginNode("FBXHeaderExtension");
        m_w.endProperties();

        // FBXHeaderVersion
        m_w.beginNode("FBXHeaderVersion");
        m_w.writePropertyI(1003);
        m_w.endProperties();
        m_w.endNodeLeaf();

        // FBXVersion
        m_w.beginNode("FBXVersion");
        m_w.writePropertyI(static_cast<int32_t>(FBX_VERSION));
        m_w.endProperties();
        m_w.endNodeLeaf();

        // EncryptionType
        m_w.beginNode("EncryptionType");
        m_w.writePropertyI(0);
        m_w.endProperties();
        m_w.endNodeLeaf();

        // CreationTimeStamp
        m_w.beginNode("CreationTimeStamp");
        m_w.endProperties();
        m_w.beginNode("Version"); m_w.writePropertyI(1000); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.beginNode("Year"); m_w.writePropertyI(2025); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.beginNode("Month"); m_w.writePropertyI(1); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.beginNode("Day"); m_w.writePropertyI(1); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.beginNode("Hour"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.beginNode("Minute"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.beginNode("Second"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.beginNode("Millisecond"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
        m_w.endNode(); // CreationTimeStamp

        // Creator
        m_w.beginNode("Creator");
        m_w.writePropertyS("QtMeshEditor FBX Exporter");
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.endNode(); // FBXHeaderExtension
    }

    // ── GlobalSettings ───────────────────────────────────────────
    void writeGlobalSettings()
    {
        m_w.beginNode("GlobalSettings");
        m_w.endProperties();

        m_w.beginNode("Version");
        m_w.writePropertyI(1000);
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.beginNode("Properties70");
        m_w.endProperties();

        writeP70int("UpAxis", 1);
        writeP70int("UpAxisSign", 1);
        writeP70int("FrontAxis", 2);
        writeP70int("FrontAxisSign", 1);
        writeP70int("CoordAxis", 0);
        writeP70int("CoordAxisSign", 1);
        writeP70int("OriginalUpAxis", 1);
        writeP70int("OriginalUpAxisSign", 1);
        // Ogre stores positions in meters (Assimp's FBX importer applied
        // UnitScaleFactor*0.01 on the original import, converting cm→m).
        // Setting UnitScaleFactor=100 tells the reimporter that 1 unit = 1 m,
        // so the root-node scale becomes 100*0.01 = 1.0 (no additional scaling).
        writeP70double("UnitScaleFactor", 100.0);
        writeP70double("OriginalUnitScaleFactor", 100.0);
        writeP70enum("TimeMode", 6); // 30 fps
        writeP70enum("TimeProtocol", 2);
        writeP70enum("SnapOnFrameMode", 0);
        writeP70KTime("TimeSpanStart", 0);
        writeP70KTime("TimeSpanStop", FBX_TICKS_PER_SECOND);
        writeP70double("CustomFrameRate", -1.0);

        m_w.endNode(); // Properties70
        m_w.endNode(); // GlobalSettings
    }

    // ── Documents ────────────────────────────────────────────────
    void writeDocuments()
    {
        m_w.beginNode("Documents");
        m_w.endProperties();

        m_w.beginNode("Count");
        m_w.writePropertyI(1);
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.beginNode("Document");
        m_w.writePropertyL(m_documentId);
        m_w.writePropertyS("Scene");
        m_w.writePropertyS("Scene");
        m_w.endProperties();

        m_w.beginNode("Properties70");
        m_w.endProperties();
        writeP70compound("SourceObject", "");
        writeP70string("ActiveAnimStackName", "");
        m_w.endNode(); // Properties70

        m_w.beginNode("RootNode");
        m_w.writePropertyL(0);
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.endNode(); // Document
        m_w.endNode(); // Documents
    }

    // ── References ───────────────────────────────────────────────
    void writeReferences()
    {
        m_w.beginNode("References");
        m_w.endProperties();
        m_w.endNode();
    }

    // ── Definitions ──────────────────────────────────────────────
    void writeDefinitions()
    {
        // Count object types
        int defCount = 1; // GlobalSettings always
        int modelCount = (m_mesh ? static_cast<int>(m_mesh->getNumSubMeshes()) : 0); // one mesh model per submesh
        int geomCount = (m_mesh ? static_cast<int>(m_mesh->getNumSubMeshes()) : 0);
        int matCount = 0;
        int deformerCount = 0;
        int nodeAttrCount = 0;
        int poseCount = 0;
        int textureCount = 0;
        int videoCount = 0;
        int animStackCount = 0;
        int animLayerCount = 0;
        int animCurveNodeCount = 0;
        int animCurveCount = 0;

        // Count unique materials and textures
        std::set<std::string> matNames;
        std::set<std::string> texNames;
        if (m_entity) {
            for (const auto* sub : m_entity->getSubEntities())
            {
                auto mat = sub->getMaterial();
                matNames.insert(mat->getName());
                if (mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0)
                {
                    auto* pass = mat->getTechnique(0)->getPass(0);
                    for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti)
                    {
                        auto texName = pass->getTextureUnitState(ti)->getTextureName();
                        if (!texName.empty())
                            texNames.insert(texName);
                    }
                }
            }
        }
        matCount = static_cast<int>(matNames.size());
        textureCount = static_cast<int>(texNames.size());
        videoCount = textureCount;

        if (m_hasSkeleton)
        {
            unsigned short numBones = m_skeleton->getNumBones();
            modelCount += numBones; // bone models
            nodeAttrCount = numBones; // bone node attributes
            poseCount = 1; // BindPose

            // Skin deformers (1 per submesh) + cluster deformers (1 per bone-per-submesh that has weights)
            if (m_mesh) {
                deformerCount = geomCount; // skin deformers
                for (unsigned int si = 0; si < m_mesh->getNumSubMeshes(); ++si)
                {
                    const auto* subMesh = m_mesh->getSubMesh(si);
                    const auto& boneAssignments = subMesh->useSharedVertices
                        ? m_mesh->getBoneAssignments() : subMesh->getBoneAssignments();
                    std::set<unsigned short> boneIndices;
                    for (const auto& [_, vba] : boneAssignments)
                        boneIndices.insert(vba.boneIndex);
                    deformerCount += static_cast<int>(boneIndices.size());
                }
            }

            if (m_skeleton->getNumAnimations() > 0)
            {
                animStackCount = m_skeleton->getNumAnimations();
                animLayerCount = animStackCount;
                // Per animation: per bone track → 3 curve nodes (T, R, S) + 9 curves (XYZ each)
                for (unsigned short ai = 0; ai < m_skeleton->getNumAnimations(); ++ai)
                {
                    auto* anim = m_skeleton->getAnimation(ai);
                    auto numTracks = static_cast<int>(anim->_getNodeTrackList().size());
                    animCurveNodeCount += numTracks * 3;
                    animCurveCount += numTracks * 9;
                }
            }
        }

        int totalObjects = 1 + modelCount + geomCount + matCount + nodeAttrCount +
                           deformerCount + poseCount + textureCount + videoCount +
                           animStackCount + animLayerCount +
                           animCurveNodeCount + animCurveCount;

        defCount += (modelCount > 0 ? 1 : 0);
        defCount += (geomCount > 0 ? 1 : 0);
        defCount += (matCount > 0 ? 1 : 0);
        defCount += (nodeAttrCount > 0 ? 1 : 0);
        defCount += (deformerCount > 0 ? 1 : 0);
        defCount += (poseCount > 0 ? 1 : 0);
        defCount += (textureCount > 0 ? 1 : 0);
        defCount += (videoCount > 0 ? 1 : 0);
        defCount += (animStackCount > 0 ? 1 : 0);
        defCount += (animLayerCount > 0 ? 1 : 0);
        defCount += (animCurveNodeCount > 0 ? 1 : 0);
        defCount += (animCurveCount > 0 ? 1 : 0);

        m_w.beginNode("Definitions");
        m_w.endProperties();

        m_w.beginNode("Version");
        m_w.writePropertyI(100);
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.beginNode("Count");
        m_w.writePropertyI(totalObjects);
        m_w.endProperties();
        m_w.endNodeLeaf();

        writeObjectType("GlobalSettings", 1);
        if (modelCount > 0) writeObjectType("Model", modelCount);
        if (geomCount > 0) writeObjectType("Geometry", geomCount);
        if (matCount > 0) writeObjectType("Material", matCount);
        if (nodeAttrCount > 0) writeObjectType("NodeAttribute", nodeAttrCount);
        if (deformerCount > 0) writeObjectType("Deformer", deformerCount);
        if (poseCount > 0) writeObjectType("Pose", poseCount);
        if (textureCount > 0) writeObjectType("Texture", textureCount);
        if (videoCount > 0) writeObjectType("Video", videoCount);
        if (animStackCount > 0) writeObjectType("AnimationStack", animStackCount);
        if (animLayerCount > 0) writeObjectType("AnimationLayer", animLayerCount);
        if (animCurveNodeCount > 0) writeObjectType("AnimationCurveNode", animCurveNodeCount);
        if (animCurveCount > 0) writeObjectType("AnimationCurve", animCurveCount);

        m_w.endNode(); // Definitions
    }

    void writeObjectType(const std::string& typeName, int count)
    {
        m_w.beginNode("ObjectType");
        m_w.writePropertyS(typeName);
        m_w.endProperties();

        m_w.beginNode("Count");
        m_w.writePropertyI(count);
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.endNode();
    }

    // ── Objects ──────────────────────────────────────────────────
    void writeObjects()
    {
        m_w.beginNode("Objects");
        m_w.endProperties();

        if (!m_skeletonOnly) {
            writeGeometryObjects();
            writeMeshModels();
            writeMaterialObjects();
            writeTextureObjects();
        }
        if (m_hasSkeleton)
        {
            writeBoneModels();
            if (!m_skeletonOnly)
                writeSkinDeformers();
            writeBindPose();
            if (m_skeleton->getNumAnimations() > 0)
                writeAnimations();
        }

        m_w.endNode(); // Objects
    }

    // ── Geometry objects (one per submesh) ────────────────────────
    void writeGeometryObjects()
    {
        for (unsigned int si = 0; si < m_mesh->getNumSubMeshes(); ++si)
        {
            const Ogre::SubMesh* subMesh = m_mesh->getSubMesh(si);
            const Ogre::VertexData* vData = subMesh->useSharedVertices
                ? m_mesh->sharedVertexData : subMesh->vertexData;
            if (!vData || vData->vertexCount == 0) continue;

            int64_t geomId = nextId();
            m_geomIds.push_back(geomId);
            m_geomSubmeshIndices.push_back(si);

            std::string geomName = std::string(m_entity->getName()) +
                                   "_submesh" + std::to_string(si);

            m_w.beginNode("Geometry");
            m_w.writePropertyL(geomId);
            m_w.writePropertyS(geomName + std::string("\x00\x01", 2) + "Geometry");
            m_w.writePropertyS("Mesh");
            m_w.endProperties();

            // ── Vertices (Z-mirrored, no unit scaling) ──
            std::vector<double> positions;
            const auto* posElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            if (posElem)
            {
                auto vbuf = vData->vertexBufferBinding->getBuffer(posElem->getSource());
                auto* base = static_cast<const unsigned char*>(
                    vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                positions.resize(vData->vertexCount * 3);
                for (size_t j = 0; j < vData->vertexCount; ++j)
                {
                    const Ogre::Real* p;
                    posElem->baseVertexPointerToElement(
                        const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                    positions[j * 3 + 0] = p[0];
                    positions[j * 3 + 1] = p[1];
                    positions[j * 3 + 2] = -static_cast<double>(p[2]); // negate Z
                }
                vbuf->unlock();
            }
            m_w.beginNode("Vertices");
            m_w.writePropertyArrayD(positions);
            m_w.endProperties();
            m_w.endNodeLeaf();

            // ── PolygonVertexIndex (winding reversed for Z-mirror) ──
            std::vector<int32_t> polyIndices;
            const Ogre::IndexData* iData = subMesh->indexData;
            if (iData && iData->indexCount > 0)
            {
                auto ibuf = iData->indexBuffer;
                auto* ibase = static_cast<const unsigned char*>(
                    ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                bool use32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
                polyIndices.resize(iData->indexCount);
                for (size_t f = 0; f < iData->indexCount / 3; ++f)
                {
                    // Read original triangle indices
                    int32_t i0, i1, i2;
                    if (use32) {
                        i0 = static_cast<int32_t>(reinterpret_cast<const uint32_t*>(ibase)[f * 3 + 0]);
                        i1 = static_cast<int32_t>(reinterpret_cast<const uint32_t*>(ibase)[f * 3 + 1]);
                        i2 = static_cast<int32_t>(reinterpret_cast<const uint32_t*>(ibase)[f * 3 + 2]);
                    } else {
                        i0 = static_cast<int32_t>(reinterpret_cast<const uint16_t*>(ibase)[f * 3 + 0]);
                        i1 = static_cast<int32_t>(reinterpret_cast<const uint16_t*>(ibase)[f * 3 + 1]);
                        i2 = static_cast<int32_t>(reinterpret_cast<const uint16_t*>(ibase)[f * 3 + 2]);
                    }
                    // Reverse winding: (v0, v1, v2) → (v0, v2, v1)
                    // FBX convention: last index is -(idx+1)
                    polyIndices[f * 3 + 0] = i0;
                    polyIndices[f * 3 + 1] = i2;
                    polyIndices[f * 3 + 2] = -(i1 + 1);
                }
                ibuf->unlock();
            }
            m_w.beginNode("PolygonVertexIndex");
            m_w.writePropertyArrayI(polyIndices);
            m_w.endProperties();
            m_w.endNodeLeaf();

            // ── LayerElementNormal ──
            const auto* normElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
            if (normElem)
            {
                std::vector<double> normals(vData->vertexCount * 3);
                auto vbuf = vData->vertexBufferBinding->getBuffer(normElem->getSource());
                auto* base = static_cast<const unsigned char*>(
                    vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                for (size_t j = 0; j < vData->vertexCount; ++j)
                {
                    const Ogre::Real* p;
                    normElem->baseVertexPointerToElement(
                        const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                    normals[j * 3 + 0] = p[0];
                    normals[j * 3 + 1] = p[1];
                    normals[j * 3 + 2] = -static_cast<double>(p[2]); // negate Z
                }
                vbuf->unlock();

                m_w.beginNode("LayerElementNormal");
                m_w.writePropertyI(0);
                m_w.endProperties();

                m_w.beginNode("Version"); m_w.writePropertyI(101); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("Name"); m_w.writePropertyS(""); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("MappingInformationType"); m_w.writePropertyS("ByPolygonVertex"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("ReferenceInformationType"); m_w.writePropertyS("Direct"); m_w.endProperties(); m_w.endNodeLeaf();

                // Expand normals to per-polygon-vertex with reversed winding
                // PolygonVertexIndex stores (v0, v2, v1) per triangle, so
                // normals must be expanded in the same order.
                std::vector<double> expandedNormals;
                if (iData && iData->indexCount > 0)
                {
                    expandedNormals.resize(iData->indexCount * 3);
                    auto ibuf = iData->indexBuffer;
                    auto* ibase2 = static_cast<const unsigned char*>(
                        ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                    bool use32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
                    for (size_t f = 0; f < iData->indexCount / 3; ++f)
                    {
                        uint32_t vi0 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 0]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 0];
                        uint32_t vi1 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 1]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 1];
                        uint32_t vi2 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 2]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 2];
                        // Reversed winding: (v0, v2, v1) to match PolygonVertexIndex
                        size_t base = f * 9;
                        expandedNormals[base + 0] = normals[vi0 * 3 + 0];
                        expandedNormals[base + 1] = normals[vi0 * 3 + 1];
                        expandedNormals[base + 2] = normals[vi0 * 3 + 2];
                        expandedNormals[base + 3] = normals[vi2 * 3 + 0];
                        expandedNormals[base + 4] = normals[vi2 * 3 + 1];
                        expandedNormals[base + 5] = normals[vi2 * 3 + 2];
                        expandedNormals[base + 6] = normals[vi1 * 3 + 0];
                        expandedNormals[base + 7] = normals[vi1 * 3 + 1];
                        expandedNormals[base + 8] = normals[vi1 * 3 + 2];
                    }
                    ibuf->unlock();
                }
                else
                {
                    expandedNormals = normals;
                }

                m_w.beginNode("Normals");
                m_w.writePropertyArrayD(expandedNormals);
                m_w.endProperties();
                m_w.endNodeLeaf();

                m_w.endNode(); // LayerElementNormal
            }

            // ── LayerElementUV ──
            const auto* tcElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
            if (tcElem)
            {
                std::vector<double> uvs(vData->vertexCount * 2);
                auto vbuf = vData->vertexBufferBinding->getBuffer(tcElem->getSource());
                auto* base = static_cast<const unsigned char*>(
                    vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                for (size_t j = 0; j < vData->vertexCount; ++j)
                {
                    const Ogre::Real* p;
                    tcElem->baseVertexPointerToElement(
                        const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                    uvs[j * 2 + 0] = p[0];
                    uvs[j * 2 + 1] = 1.0 - p[1]; // flip V
                }
                vbuf->unlock();

                m_w.beginNode("LayerElementUV");
                m_w.writePropertyI(0);
                m_w.endProperties();

                m_w.beginNode("Version"); m_w.writePropertyI(101); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("Name"); m_w.writePropertyS("UVMap"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("MappingInformationType"); m_w.writePropertyS("ByPolygonVertex"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("ReferenceInformationType"); m_w.writePropertyS("IndexToDirect"); m_w.endProperties(); m_w.endNodeLeaf();

                m_w.beginNode("UV");
                m_w.writePropertyArrayD(uvs);
                m_w.endProperties();
                m_w.endNodeLeaf();

                // UV index with reversed winding to match PolygonVertexIndex
                std::vector<int32_t> uvIndex;
                if (iData && iData->indexCount > 0)
                {
                    auto ibuf = iData->indexBuffer;
                    auto* ibase2 = static_cast<const unsigned char*>(
                        ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                    bool use32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
                    uvIndex.resize(iData->indexCount);
                    for (size_t f = 0; f < iData->indexCount / 3; ++f)
                    {
                        uint32_t vi0 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 0]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 0];
                        uint32_t vi1 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 1]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 1];
                        uint32_t vi2 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 2]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 2];
                        // Reversed winding: (v0, v2, v1)
                        uvIndex[f * 3 + 0] = static_cast<int32_t>(vi0);
                        uvIndex[f * 3 + 1] = static_cast<int32_t>(vi2);
                        uvIndex[f * 3 + 2] = static_cast<int32_t>(vi1);
                    }
                    ibuf->unlock();
                }

                m_w.beginNode("UVIndex");
                m_w.writePropertyArrayI(uvIndex);
                m_w.endProperties();
                m_w.endNodeLeaf();

                m_w.endNode(); // LayerElementUV
            }

            // ── LayerElementColor (vertex colors) ──
            const auto* colElem = vData->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
            if (colElem)
            {
                std::vector<double> colors(vData->vertexCount * 4);
                auto vbuf = vData->vertexBufferBinding->getBuffer(colElem->getSource());
                auto* base = static_cast<const unsigned char*>(
                    vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                for (size_t j = 0; j < vData->vertexCount; ++j)
                {
                    const Ogre::RGBA* p;
                    colElem->baseVertexPointerToElement(
                        const_cast<unsigned char*>(base + j * vbuf->getVertexSize()), &p);
                    Ogre::ColourValue cv;
                    if (colElem->getType() == Ogre::VET_COLOUR_ABGR)
                        cv.setAsABGR(*p);
                    else
                        cv.setAsARGB(*p);
                    colors[j * 4 + 0] = cv.r;
                    colors[j * 4 + 1] = cv.g;
                    colors[j * 4 + 2] = cv.b;
                    colors[j * 4 + 3] = cv.a;
                }
                vbuf->unlock();

                // Expand to per-polygon-vertex with reversed winding to match PolygonVertexIndex
                std::vector<double> expandedColors;
                if (iData && iData->indexCount > 0)
                {
                    expandedColors.resize(iData->indexCount * 4);
                    auto ibuf = iData->indexBuffer;
                    auto* ibase2 = static_cast<const unsigned char*>(
                        ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                    bool use32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
                    for (size_t f = 0; f < iData->indexCount / 3; ++f)
                    {
                        uint32_t vi0 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 0]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 0];
                        uint32_t vi1 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 1]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 1];
                        uint32_t vi2 = use32
                            ? reinterpret_cast<const uint32_t*>(ibase2)[f * 3 + 2]
                            : reinterpret_cast<const uint16_t*>(ibase2)[f * 3 + 2];

                        // Reversed winding: (v0, v2, v1)
                        size_t base = f * 12;
                        auto copy = [&](size_t outVertex, uint32_t vi) {
                            expandedColors[outVertex + 0] = colors[vi * 4 + 0];
                            expandedColors[outVertex + 1] = colors[vi * 4 + 1];
                            expandedColors[outVertex + 2] = colors[vi * 4 + 2];
                            expandedColors[outVertex + 3] = colors[vi * 4 + 3];
                        };
                        copy(base + 0, vi0);
                        copy(base + 4, vi2);
                        copy(base + 8, vi1);
                    }
                    ibuf->unlock();
                }
                else
                {
                    expandedColors = colors;
                }

                m_w.beginNode("LayerElementColor");
                m_w.writePropertyI(0);
                m_w.endProperties();

                m_w.beginNode("Version"); m_w.writePropertyI(101); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("Name"); m_w.writePropertyS(""); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("MappingInformationType"); m_w.writePropertyS("ByPolygonVertex"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("ReferenceInformationType"); m_w.writePropertyS("Direct"); m_w.endProperties(); m_w.endNodeLeaf();

                m_w.beginNode("Colors");
                m_w.writePropertyArrayD(expandedColors);
                m_w.endProperties();
                m_w.endNodeLeaf();

                m_w.endNode(); // LayerElementColor
            }

            // ── LayerElementMaterial ──
            {
                // Each Model has exactly one material connected, so index is always 0
                int matIndex = 0;

                m_w.beginNode("LayerElementMaterial");
                m_w.writePropertyI(0);
                m_w.endProperties();

                m_w.beginNode("Version"); m_w.writePropertyI(101); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("Name"); m_w.writePropertyS(""); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("MappingInformationType"); m_w.writePropertyS("AllSame"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("ReferenceInformationType"); m_w.writePropertyS("IndexToDirect"); m_w.endProperties(); m_w.endNodeLeaf();

                m_w.beginNode("Materials");
                m_w.writePropertyArrayI({matIndex});
                m_w.endProperties();
                m_w.endNodeLeaf();

                m_w.endNode(); // LayerElementMaterial
            }

            // ── Layer ──
            m_w.beginNode("Layer");
            m_w.writePropertyI(0);
            m_w.endProperties();

            m_w.beginNode("Version"); m_w.writePropertyI(100); m_w.endProperties(); m_w.endNodeLeaf();

            if (normElem)
            {
                m_w.beginNode("LayerElement");
                m_w.endProperties();
                m_w.beginNode("Type"); m_w.writePropertyS("LayerElementNormal"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("TypedIndex"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.endNode();
            }
            if (tcElem)
            {
                m_w.beginNode("LayerElement");
                m_w.endProperties();
                m_w.beginNode("Type"); m_w.writePropertyS("LayerElementUV"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("TypedIndex"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.endNode();
            }
            if (colElem)
            {
                m_w.beginNode("LayerElement");
                m_w.endProperties();
                m_w.beginNode("Type"); m_w.writePropertyS("LayerElementColor"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("TypedIndex"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.endNode();
            }
            {
                m_w.beginNode("LayerElement");
                m_w.endProperties();
                m_w.beginNode("Type"); m_w.writePropertyS("LayerElementMaterial"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("TypedIndex"); m_w.writePropertyI(0); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.endNode();
            }

            m_w.endNode(); // Layer
            m_w.endNode(); // Geometry
        }
    }

    // ── Mesh Models (one per submesh) ─────────────────────────────
    void writeMeshModels()
    {
        for (size_t gi = 0; gi < m_geomIds.size(); ++gi)
        {
            unsigned int si = m_geomSubmeshIndices[gi];
            int64_t modelId = nextId();
            m_meshModelIds.push_back(modelId);

            std::string modelName = std::string(m_entity->getName()) +
                                    "_submesh" + std::to_string(si);

            m_w.beginNode("Model");
            m_w.writePropertyL(modelId);
            m_w.writePropertyS(modelName + std::string("\x00\x01", 2) + "Model");
            m_w.writePropertyS("Mesh");
            m_w.endProperties();

            m_w.beginNode("Version"); m_w.writePropertyI(232); m_w.endProperties(); m_w.endNodeLeaf();

            m_w.beginNode("Properties70");
            m_w.endProperties();
            writeP70LclTranslation(0.0, 0.0, 0.0);
            writeP70LclRotation(0.0, 0.0, 0.0);
            writeP70LclScaling(1.0, 1.0, 1.0);
            m_w.endNode(); // Properties70

            m_w.beginNode("Shading"); m_w.writePropertyBool(true); m_w.endProperties(); m_w.endNodeLeaf();
            m_w.beginNode("Culling"); m_w.writePropertyS("CullingOff"); m_w.endProperties(); m_w.endNodeLeaf();

            m_w.endNode(); // Model
        }
    }

    // ── Material objects ─────────────────────────────────────────
    void writeMaterialObjects()
    {
        std::set<std::string> seen;
        for (const auto* sub : m_entity->getSubEntities())
        {
            auto mat = sub->getMaterial();
            if (!seen.insert(mat->getName()).second) continue;

            int64_t matId = nextId();
            m_materialIds[mat->getName()] = matId;

            m_w.beginNode("Material");
            m_w.writePropertyL(matId);
            m_w.writePropertyS(mat->getName() + std::string("\x00\x01", 2) + "Material");
            m_w.writePropertyS("");
            m_w.endProperties();

            m_w.beginNode("Version"); m_w.writePropertyI(102); m_w.endProperties(); m_w.endNodeLeaf();
            m_w.beginNode("ShadingModel"); m_w.writePropertyS("Phong"); m_w.endProperties(); m_w.endNodeLeaf();

            m_w.beginNode("Properties70");
            m_w.endProperties();

            if (mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0)
            {
                auto* pass = mat->getTechnique(0)->getPass(0);
                auto d = pass->getDiffuse();
                writeP70Color("DiffuseColor", d.r, d.g, d.b);
                auto s = pass->getSpecular();
                writeP70Color("SpecularColor", s.r, s.g, s.b);
                auto a = pass->getAmbient();
                writeP70Color("AmbientColor", a.r, a.g, a.b);
                auto e = pass->getSelfIllumination();
                writeP70Color("EmissiveColor", e.r, e.g, e.b);
                writeP70Number("Shininess", pass->getShininess());
                writeP70Number("Opacity", d.a);
            }

            m_w.endNode(); // Properties70
            m_w.endNode(); // Material
        }
    }

    // ── Bone Models ──────────────────────────────────────────────
    void writeBoneModels()
    {
        for (unsigned short bi = 0; bi < m_skeleton->getNumBones(); ++bi)
        {
            auto* bone = m_skeleton->getBone(bi);
            int64_t boneModelId = nextId();
            int64_t boneAttrId = nextId();
            m_boneModelIds[bone->getHandle()] = boneModelId;
            m_boneAttrIds[bone->getHandle()] = boneAttrId;

            // NodeAttribute (LimbNode)
            m_w.beginNode("NodeAttribute");
            m_w.writePropertyL(boneAttrId);
            m_w.writePropertyS(std::string(bone->getName()) + std::string("\x00\x01", 2) + "NodeAttribute");
            m_w.writePropertyS("LimbNode");
            m_w.endProperties();

            m_w.beginNode("TypeFlags"); m_w.writePropertyS("Skeleton"); m_w.endProperties(); m_w.endNodeLeaf();

            m_w.endNode(); // NodeAttribute

            // Model (LimbNode) — Z-mirrored, using initial (bind pose) values.
            // All bones use raw transforms directly. BoneProcessor on reimport
            // also reads transforms directly (no inversion), so the round-trip
            // preserves the original values. External tools like Blender also
            // read these transforms directly and get correct bone positions.
            Ogre::Vector3 pos = bone->getInitialPosition();
            Ogre::Quaternion ori = bone->getInitialOrientation();
            Ogre::Vector3 scl = bone->getInitialScale();

            Ogre::Quaternion mirroredRot = mirrorZ(ori);
            double rx, ry, rz;
            quaternionToEulerXYZ(mirroredRot, rx, ry, rz);

            m_w.beginNode("Model");
            m_w.writePropertyL(boneModelId);
            m_w.writePropertyS(std::string(bone->getName()) + std::string("\x00\x01", 2) + "Model");
            m_w.writePropertyS("LimbNode");
            m_w.endProperties();

            m_w.beginNode("Version"); m_w.writePropertyI(232); m_w.endProperties(); m_w.endNodeLeaf();

            m_w.beginNode("Properties70");
            m_w.endProperties();

            writeP70LclTranslation(pos.x, pos.y, -pos.z); // negate Z
            writeP70LclRotation(rx, ry, rz);
            writeP70LclScaling(scl.x, scl.y, scl.z);

            m_w.endNode(); // Properties70

            m_w.beginNode("Shading"); m_w.writePropertyBool(true); m_w.endProperties(); m_w.endNodeLeaf();
            m_w.beginNode("Culling"); m_w.writePropertyS("CullingOff"); m_w.endProperties(); m_w.endNodeLeaf();

            m_w.endNode(); // Model
        }
    }

    // ── Skin Deformers ───────────────────────────────────────────
    void writeSkinDeformers()
    {
        for (size_t gi = 0; gi < m_geomIds.size(); ++gi)
        {
            unsigned int si = m_geomSubmeshIndices[gi];
            const Ogre::SubMesh* subMesh = m_mesh->getSubMesh(si);

            int64_t skinId = nextId();
            m_skinIds.push_back(skinId);

            m_w.beginNode("Deformer");
            m_w.writePropertyL(skinId);
            m_w.writePropertyS("Skin_" + std::to_string(si) + std::string("\x00\x01", 2) + "Deformer");
            m_w.writePropertyS("Skin");
            m_w.endProperties();

            m_w.beginNode("Version"); m_w.writePropertyI(101); m_w.endProperties(); m_w.endNodeLeaf();
            m_w.beginNode("Link_DeformAcuracy"); m_w.writePropertyD(50.0); m_w.endProperties(); m_w.endNodeLeaf();

            m_w.endNode(); // Deformer (Skin)

            // Collect bone assignments grouped by bone index
            const auto& boneAssignments = subMesh->useSharedVertices
                ? m_mesh->getBoneAssignments() : subMesh->getBoneAssignments();
            std::map<unsigned short, std::vector<std::pair<int32_t, double>>> boneWeightsMap;
            for (const auto& [vertIdx, vba] : boneAssignments)
            {
                boneWeightsMap[vba.boneIndex].push_back(
                    {static_cast<int32_t>(vba.vertexIndex), vba.weight});
            }

            // Per-bone Cluster sub-deformers
            for (const auto& [boneIdx, weights] : boneWeightsMap)
            {
                auto* bone = m_skeleton->getBone(boneIdx);
                int64_t clusterId = nextId();

                // Store connection info
                m_clusterConnections.push_back({clusterId, skinId, boneIdx, si});

                m_w.beginNode("Deformer");
                m_w.writePropertyL(clusterId);
                m_w.writePropertyS("Cluster_" + std::string(bone->getName()) +
                                   "_" + std::to_string(si) + std::string("\x00\x01", 2) + "SubDeformer");
                m_w.writePropertyS("Cluster");
                m_w.endProperties();

                m_w.beginNode("Version"); m_w.writePropertyI(100); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("UserData"); m_w.writePropertyS(""); m_w.writePropertyS(""); m_w.endProperties(); m_w.endNodeLeaf();

                // Indexes and Weights
                std::vector<int32_t> indices;
                std::vector<double> weightValues;
                indices.reserve(weights.size());
                weightValues.reserve(weights.size());
                for (const auto& [vi, w] : weights)
                {
                    indices.push_back(vi);
                    weightValues.push_back(w);
                }

                m_w.beginNode("Indexes");
                m_w.writePropertyArrayI(indices);
                m_w.endProperties();
                m_w.endNodeLeaf();

                m_w.beginNode("Weights");
                m_w.writePropertyArrayD(weightValues);
                m_w.endProperties();
                m_w.endNodeLeaf();

                // TransformLink = bone's global bind pose, Z-mirrored
                // Use computeGlobalBindPose (from initial transforms) instead of
                // _getFullTransform() which may reflect the current animation state
                Ogre::Matrix4 boneGlobal = computeGlobalBindPose(bone);
                double transformLinkArr[16];
                matrix4ToDoublesMirrorZ(boneGlobal, transformLinkArr);

                // Transform = bone_global^{-1} @ mesh_global (in bone space, per FBX convention)
                // Blender's FBX importer computes mesh_global = TransformLink @ Transform,
                // so storing bone^{-1} ensures all clusters produce the same mesh_global.
                // Since our mesh is at origin, mesh_global = identity → Transform = bone^{-1}.
                Ogre::Matrix4 boneGlobalMirrored;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        boneGlobalMirrored[r][c] = transformLinkArr[c * 4 + r];
                Ogre::Matrix4 transformMat = boneGlobalMirrored.inverse();
                double transformArr[16];
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        transformArr[c * 4 + r] = transformMat[r][c];

                m_w.beginNode("Transform");
                m_w.writePropertyArrayD(std::vector<double>(transformArr, transformArr + 16));
                m_w.endProperties();
                m_w.endNodeLeaf();

                m_w.beginNode("TransformLink");
                m_w.writePropertyArrayD(std::vector<double>(transformLinkArr, transformLinkArr + 16));
                m_w.endProperties();
                m_w.endNodeLeaf();

                m_w.endNode(); // Deformer (Cluster)
            }
        }
    }

    // ── Animations ───────────────────────────────────────────────
    void writeAnimations()
    {
        for (unsigned short ai = 0; ai < m_skeleton->getNumAnimations(); ++ai)
        {
            auto* ogreAnim = m_skeleton->getAnimation(ai);
            int64_t stackId = nextId();
            int64_t layerId = nextId();

            m_animStackIds.push_back(stackId);
            m_animLayerToStack.push_back({layerId, stackId});

            double duration = ogreAnim->getLength();
            int64_t startTime = 0;
            int64_t stopTime = static_cast<int64_t>(duration * FBX_TICKS_PER_SECOND);

            // AnimationStack
            m_w.beginNode("AnimationStack");
            m_w.writePropertyL(stackId);
            m_w.writePropertyS(ogreAnim->getName() + std::string("\x00\x01", 2) + "AnimStack");
            m_w.writePropertyS("");
            m_w.endProperties();

            m_w.beginNode("Properties70");
            m_w.endProperties();
            writeP70KTime("LocalStart", startTime);
            writeP70KTime("LocalStop", stopTime);
            m_w.endNode(); // Properties70

            m_w.endNode(); // AnimationStack

            // AnimationLayer
            m_w.beginNode("AnimationLayer");
            m_w.writePropertyL(layerId);
            m_w.writePropertyS(ogreAnim->getName() + "_Layer" + std::string("\x00\x01", 2) + "AnimLayer");
            m_w.writePropertyS("");
            m_w.endProperties();

            m_w.beginNode("Properties70");
            m_w.endProperties();
            writeP70Number("Weight", 100.0);
            m_w.endNode(); // Properties70

            m_w.endNode(); // AnimationLayer

            // Per-bone tracks
            for (const auto& [handle, track] : ogreAnim->_getNodeTrackList())
            {
                auto* bone = dynamic_cast<Ogre::Bone*>(track->getAssociatedNode());
                if (!bone) continue;

                auto boneIt = m_boneModelIds.find(bone->getHandle());
                if (boneIt == m_boneModelIds.end()) continue;
                int64_t boneModelId = boneIt->second;

                Ogre::Vector3 bindPos = bone->getInitialPosition();
                Ogre::Quaternion bindRot = bone->getInitialOrientation();

                auto numKF = track->getNumKeyFrames();

                // Collect keyframe data
                std::vector<int64_t> times(numKF);
                std::vector<double> tx(numKF), ty(numKF), tz(numKF);
                std::vector<double> rxArr(numKF), ryArr(numKF), rzArr(numKF);
                std::vector<double> sx(numKF), sy(numKF), sz(numKF);

                for (unsigned short ki = 0; ki < numKF; ++ki)
                {
                    auto* kf = track->getNodeKeyFrame(ki);
                    times[ki] = static_cast<int64_t>(kf->getTime() * FBX_TICKS_PER_SECOND);

                    Ogre::Vector3 pos = bindPos + kf->getTranslate();
                    tx[ki] = pos.x;
                    ty[ki] = pos.y;
                    tz[ki] = -static_cast<double>(pos.z); // negate Z

                    Ogre::Quaternion rot = mirrorZ(bindRot * kf->getRotation());
                    rot.normalise();
                    double erx, ery, erz;
                    quaternionToEulerXYZ(rot, erx, ery, erz);

                    // Euler angle continuity: keep each axis within 180°
                    // of the previous keyframe to avoid sudden full-rotation
                    // jumps caused by equivalent Euler representations.
                    if (ki > 0)
                    {
                        auto unroll = [](double prev, double cur) {
                            double d = cur - prev;
                            if (d > 180.0)       cur -= 360.0 * std::ceil((d - 180.0) / 360.0);
                            else if (d < -180.0) cur += 360.0 * std::ceil((-d - 180.0) / 360.0);
                            return cur;
                        };
                        erx = unroll(rxArr[ki - 1], erx);
                        ery = unroll(ryArr[ki - 1], ery);
                        erz = unroll(rzArr[ki - 1], erz);
                    }

                    rxArr[ki] = erx;
                    ryArr[ki] = ery;
                    rzArr[ki] = erz;

                    Ogre::Vector3 scl = kf->getScale();
                    sx[ki] = scl.x;
                    sy[ki] = scl.y;
                    sz[ki] = scl.z;
                }

                // Per-channel time arrays: collapse flat curves to a single key so FBX payloads
                // stay smaller without changing motion (constant channel == one sample).
                std::vector<int64_t> tTx = times, tTy = times, tTz = times;
                std::vector<int64_t> tRx = times, tRy = times, tRz = times;
                std::vector<int64_t> tSx = times, tSy = times, tSz = times;

                auto compactIfFlat = [](std::vector<int64_t>& kt, std::vector<double>& v, double eps) {
                    if (v.size() <= 1)
                        return;
                    const double r = v[0];
                    for (size_t i = 1; i < v.size(); ++i) {
                        if (std::fabs(v[i] - r) > eps)
                            return;
                    }
                    kt.resize(1);
                    v.resize(1);
                };

                compactIfFlat(tTx, tx, 1e-6);
                compactIfFlat(tTy, ty, 1e-6);
                compactIfFlat(tTz, tz, 1e-6);
                compactIfFlat(tRx, rxArr, 1e-4); // Euler degrees
                compactIfFlat(tRy, ryArr, 1e-4);
                compactIfFlat(tRz, rzArr, 1e-4);
                compactIfFlat(tSx, sx, 1e-6);
                compactIfFlat(tSy, sy, 1e-6);
                compactIfFlat(tSz, sz, 1e-6);

                // AnimationCurveNode T
                int64_t cnT = nextId();
                writeAnimCurveNode(cnT, "T", "d|X", "d|Y", "d|Z",
                                   tx.empty() ? 0 : tx[0],
                                   ty.empty() ? 0 : ty[0],
                                   tz.empty() ? 0 : tz[0]);
                m_animCurveNodeConns.push_back({cnT, layerId, boneModelId, "Lcl Translation"});

                // AnimationCurveNode R
                int64_t cnR = nextId();
                writeAnimCurveNode(cnR, "R", "d|X", "d|Y", "d|Z",
                                   rxArr.empty() ? 0 : rxArr[0],
                                   ryArr.empty() ? 0 : ryArr[0],
                                   rzArr.empty() ? 0 : rzArr[0]);
                m_animCurveNodeConns.push_back({cnR, layerId, boneModelId, "Lcl Rotation"});

                // AnimationCurveNode S
                int64_t cnS = nextId();
                writeAnimCurveNode(cnS, "S", "d|X", "d|Y", "d|Z",
                                   sx.empty() ? 1 : sx[0],
                                   sy.empty() ? 1 : sy[0],
                                   sz.empty() ? 1 : sz[0]);
                m_animCurveNodeConns.push_back({cnS, layerId, boneModelId, "Lcl Scaling"});

                // 9 AnimationCurves: TX, TY, TZ, RX, RY, RZ, SX, SY, SZ
                auto writeCurve = [&](const std::vector<int64_t>& t, const std::vector<double>& vals,
                                      int64_t curveNodeId, const std::string& channel)
                {
                    int64_t curveId = nextId();
                    m_w.beginNode("AnimationCurve");
                    m_w.writePropertyL(curveId);
                    m_w.writePropertyS(std::string("\x00\x01", 2) + "AnimCurve");
                    m_w.writePropertyS("");
                    m_w.endProperties();

                    m_w.beginNode("Default"); m_w.writePropertyD(vals.empty() ? 0.0 : vals[0]); m_w.endProperties(); m_w.endNodeLeaf();

                    m_w.beginNode("KeyVer"); m_w.writePropertyI(4008); m_w.endProperties(); m_w.endNodeLeaf();

                    m_w.beginNode("KeyTime");
                    m_w.writePropertyArrayL(t);
                    m_w.endProperties();
                    m_w.endNodeLeaf();

                    m_w.beginNode("KeyValueFloat");
                    std::vector<float> fVals(vals.begin(), vals.end());
                    m_w.writePropertyArrayF(fVals);
                    m_w.endProperties();
                    m_w.endNodeLeaf();

                    // AttrFlags — interpolation: cubic
                    m_w.beginNode("KeyAttrFlags");
                    m_w.writePropertyArrayI({24840});
                    m_w.endProperties();
                    m_w.endNodeLeaf();

                    m_w.beginNode("KeyAttrDataFloat");
                    m_w.writePropertyArrayF({0.0f, 0.0f, 0.0218f, 0.0f});
                    m_w.endProperties();
                    m_w.endNodeLeaf();

                    m_w.beginNode("KeyAttrRefCount");
                    m_w.writePropertyArrayI({static_cast<int32_t>(t.size())});
                    m_w.endProperties();
                    m_w.endNodeLeaf();

                    m_w.endNode(); // AnimationCurve

                    m_animCurveConns.push_back({curveId, curveNodeId, channel});
                };

                writeCurve(tTx, tx, cnT, "d|X");
                writeCurve(tTy, ty, cnT, "d|Y");
                writeCurve(tTz, tz, cnT, "d|Z");
                writeCurve(tRx, rxArr, cnR, "d|X");
                writeCurve(tRy, ryArr, cnR, "d|Y");
                writeCurve(tRz, rzArr, cnR, "d|Z");
                writeCurve(tSx, sx, cnS, "d|X");
                writeCurve(tSy, sy, cnS, "d|Y");
                writeCurve(tSz, sz, cnS, "d|Z");
            }
        }
    }

    void writeAnimCurveNode(int64_t id, const std::string& name,
                            const std::string& , const std::string& , const std::string& ,
                            double dx, double dy, double dz)
    {
        m_w.beginNode("AnimationCurveNode");
        m_w.writePropertyL(id);
        m_w.writePropertyS(name + std::string("\x00\x01", 2) + "AnimCurveNode");
        m_w.writePropertyS("");
        m_w.endProperties();

        m_w.beginNode("Properties70");
        m_w.endProperties();

        // d|X, d|Y, d|Z properties
        m_w.beginNode("P");
        m_w.writePropertyS("d|X"); m_w.writePropertyS("Number"); m_w.writePropertyS("");
        m_w.writePropertyS("A"); m_w.writePropertyD(dx);
        m_w.endProperties(); m_w.endNodeLeaf();

        m_w.beginNode("P");
        m_w.writePropertyS("d|Y"); m_w.writePropertyS("Number"); m_w.writePropertyS("");
        m_w.writePropertyS("A"); m_w.writePropertyD(dy);
        m_w.endProperties(); m_w.endNodeLeaf();

        m_w.beginNode("P");
        m_w.writePropertyS("d|Z"); m_w.writePropertyS("Number"); m_w.writePropertyS("");
        m_w.writePropertyS("A"); m_w.writePropertyD(dz);
        m_w.endProperties(); m_w.endNodeLeaf();

        m_w.endNode(); // Properties70
        m_w.endNode(); // AnimationCurveNode
    }

    // ── BindPose ─────────────────────────────────────────────────
    void writeBindPose()
    {
        int64_t poseId = nextId();
        int poseNodeCount = static_cast<int>(m_meshModelIds.size()) +
                            m_skeleton->getNumBones(); // mesh models + all bones

        m_w.beginNode("Pose");
        m_w.writePropertyL(poseId);
        m_w.writePropertyS("BIND_POSES" + std::string("\x00\x01", 2) + "Pose");
        m_w.writePropertyS("BindPose");
        m_w.endProperties();

        m_w.beginNode("Type");
        m_w.writePropertyS("BindPose");
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.beginNode("Version");
        m_w.writePropertyI(100);
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.beginNode("NbPoseNodes");
        m_w.writePropertyI(poseNodeCount);
        m_w.endProperties();
        m_w.endNodeLeaf();

        // Mesh model PoseNodes (identity — meshes have no transform)
        {
            double identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            for (auto modelId : m_meshModelIds)
                writePoseNode(modelId, identity);
        }

        // Bone PoseNodes (global bind pose, Z-mirrored)
        for (unsigned short bi = 0; bi < m_skeleton->getNumBones(); ++bi)
        {
            auto* bone = m_skeleton->getBone(bi);
            Ogre::Matrix4 globalBind = computeGlobalBindPose(bone);
            double mat[16];
            matrix4ToDoublesMirrorZ(globalBind, mat);
            writePoseNode(m_boneModelIds[bone->getHandle()], mat);
        }

        m_w.endNode(); // Pose
    }

    void writePoseNode(int64_t nodeId, const double* mat16)
    {
        m_w.beginNode("PoseNode");
        m_w.endProperties();

        m_w.beginNode("Node");
        m_w.writePropertyL(nodeId);
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.beginNode("Matrix");
        m_w.writePropertyArrayD(std::vector<double>(mat16, mat16 + 16));
        m_w.endProperties();
        m_w.endNodeLeaf();

        m_w.endNode(); // PoseNode
    }

    // ── Texture objects ─────────────────────────────────────────
    static std::vector<uint8_t> readOgreResourceBytes(const std::string& resourceName)
    {
        const auto readAll = [](const Ogre::DataStreamPtr& stream) -> std::vector<uint8_t> {
            if (!stream)
                return {};

            std::vector<uint8_t> data;
            if (const size_t sz = stream->size(); sz > 0 && sz != static_cast<size_t>(-1)) {
                data.resize(sz);
                const size_t n = stream->read(data.data(), sz);
                data.resize(n);
                return data;
            }

            // Fallback if size is unknown: read in chunks
            constexpr size_t kChunk = 64 * 1024;
            std::vector<uint8_t> chunk(kChunk);
            while (!stream->eof()) {
                const size_t n = stream->read(chunk.data(), kChunk);
                if (n == 0)
                    break;
                data.insert(data.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
            }
            return data;
        };

        try {
            const auto& rgm = Ogre::ResourceGroupManager::getSingleton();
            const auto openInGroup = [&](const Ogre::String& group) -> Ogre::DataStreamPtr {
                if (group.empty() || !rgm.resourceExists(group, resourceName))
                    return {};
                return rgm.openResource(resourceName, group);
            };

            // Prefer the group Ogre says contains it.
            if (const Ogre::String preferred = rgm.findGroupContainingResource(resourceName); !preferred.empty()) {
                if (auto stream = openInGroup(preferred))
                    return readAll(stream);
            }

            // Then try default group.
            if (auto stream = openInGroup(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME))
                return readAll(stream);

            // Finally scan all groups (dynamic groups may exist, e.g. path-based groups).
            for (const auto& g : rgm.getResourceGroups()) {
                if (auto stream = openInGroup(g))
                    return readAll(stream);
            }

            return {};
        } catch (const Ogre::Exception&) {
            return {};
        } catch (const std::exception&) {
            return {};
        }
    }

    void writeTextureObjects()
    {
        std::set<std::string> seen;
        for (const auto* sub : m_entity->getSubEntities())
        {
            auto mat = sub->getMaterial();
            if (mat->getNumTechniques() == 0 || mat->getTechnique(0)->getNumPasses() == 0)
                continue;

            auto* pass = mat->getTechnique(0)->getPass(0);
            for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti)
            {
                std::string texName = pass->getTextureUnitState(ti)->getTextureName();
                if (texName.empty() || !seen.insert(texName).second)
                    continue;

                int64_t texId = nextId();
                int64_t vidId = nextId();
                m_textureIds[texName] = texId;
                m_videoIds[texName] = vidId;

                // Texture object
                m_w.beginNode("Texture");
                m_w.writePropertyL(texId);
                m_w.writePropertyS(texName + std::string("\x00\x01", 2) + "Texture");
                m_w.writePropertyS("");
                m_w.endProperties();

                m_w.beginNode("Type"); m_w.writePropertyS("TextureVideoClip"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("Version"); m_w.writePropertyI(202); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("TextureName"); m_w.writePropertyS(texName); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("FileName"); m_w.writePropertyS(texName); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("RelativeFilename"); m_w.writePropertyS(texName); m_w.endProperties(); m_w.endNodeLeaf();

                m_w.beginNode("Properties70");
                m_w.endProperties();
                writeP70enum("CurrentTextureBlendMode", 0);
                writeP70string("UVSet", "UVMap");
                writeP70int("UseMaterial", 1);
                m_w.endNode(); // Properties70

                m_w.endNode(); // Texture

                // Video (clip) object
                m_w.beginNode("Video");
                m_w.writePropertyL(vidId);
                m_w.writePropertyS(texName + std::string("\x00\x01", 2) + "Video");
                m_w.writePropertyS("Clip");
                m_w.endProperties();

                m_w.beginNode("Type"); m_w.writePropertyS("Clip"); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("FileName"); m_w.writePropertyS(texName); m_w.endProperties(); m_w.endNodeLeaf();
                m_w.beginNode("RelativeFilename"); m_w.writePropertyS(texName); m_w.endProperties(); m_w.endNodeLeaf();

                // Embed texture bytes when the resource is available. Many tools (e.g. Mixamo exports)
                // expect texture payloads to be embedded via Video.Content.
                if (const auto bytes = readOgreResourceBytes(texName); !bytes.empty()) {
                    m_w.beginNode("Content");
                    m_w.writePropertyR(bytes);
                    m_w.endProperties();
                    m_w.endNodeLeaf();
                }

                m_w.endNode(); // Video
            }
        }
    }

    // ── Connections ──────────────────────────────────────────────
    void writeConnections()
    {
        m_w.beginNode("Connections");
        m_w.endProperties();

        // Each mesh model → root (id 0), geometry → its model, material → its model
        for (size_t gi = 0; gi < m_meshModelIds.size(); ++gi)
        {
            int64_t modelId = m_meshModelIds[gi];
            writeConnection("OO", modelId, 0);
            writeConnection("OO", m_geomIds[gi], modelId);

            // Connect the submesh's material to this model
            unsigned int si = m_geomSubmeshIndices[gi];
            auto* subEnt = m_entity->getSubEntity(si);
            auto matIt = m_materialIds.find(subEnt->getMaterial()->getName());
            if (matIt != m_materialIds.end())
                writeConnection("OO", matIt->second, modelId);
        }

        // Texture → Material (OP) — connect to ALL materials that use each texture
        {
            // Track (texName, matName, fbxProperty) tuples
            std::set<std::tuple<std::string, std::string, std::string>> texMatPairs;
            if (m_entity) {
                for (const auto* sub : m_entity->getSubEntities())
                {
                    auto mat = sub->getMaterial();
                    if (mat->getNumTechniques() == 0 || mat->getTechnique(0)->getNumPasses() == 0) continue;
                    auto* pass = mat->getTechnique(0)->getPass(0);
                    for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti)
                    {
                        auto* tus = pass->getTextureUnitState(ti);
                        std::string texName = tus->getTextureName();
                        if (!texName.empty()) {
                            std::string fbxProp = (tus->getName() == "normal_map") ? "NormalMap" : "DiffuseColor";
                            texMatPairs.insert({texName, mat->getName(), fbxProp});
                        }
                    }
                }
            }
            for (const auto& [texName, matName, fbxProp] : texMatPairs)
            {
                auto texIt = m_textureIds.find(texName);
                auto matIt = m_materialIds.find(matName);
                if (texIt != m_textureIds.end() && matIt != m_materialIds.end())
                    writeConnection("OP", texIt->second, matIt->second, fbxProp);
            }
        }
        // Video → Texture (OO)
        for (const auto& [texName, texId] : m_textureIds)
        {
            auto vidIt = m_videoIds.find(texName);
            if (vidIt != m_videoIds.end())
                writeConnection("OO", vidIt->second, texId);
        }

        if (m_hasSkeleton)
        {
            // Bone NodeAttribute → bone Model
            for (const auto& [handle, attrId] : m_boneAttrIds)
                writeConnection("OO", attrId, m_boneModelIds[handle]);

            // Bone hierarchy: root bones → root (id 0), child bones → parent bone model
            for (unsigned short bi = 0; bi < m_skeleton->getNumBones(); ++bi)
            {
                auto* bone = m_skeleton->getBone(bi);
                int64_t boneModelId = m_boneModelIds[bone->getHandle()];
                if (!bone->getParent())
                    writeConnection("OO", boneModelId, 0);
                else
                {
                    auto* parentBone = dynamic_cast<Ogre::Bone*>(bone->getParent());
                    if (parentBone)
                        writeConnection("OO", boneModelId, m_boneModelIds[parentBone->getHandle()]);
                }
            }

            if (!m_skeletonOnly) {
                // Skin → Geometry
                for (size_t i = 0; i < m_skinIds.size() && i < m_geomIds.size(); ++i)
                    writeConnection("OO", m_skinIds[i], m_geomIds[i]);

                // Cluster → Skin, Bone → Cluster
                for (const auto& cc : m_clusterConnections)
                {
                    writeConnection("OO", cc.clusterId, cc.skinId);
                    writeConnection("OO", m_boneModelIds[cc.boneHandle], cc.clusterId);
                }
            }

            // AnimationStack → scene root
            for (auto stackId : m_animStackIds)
                writeConnection("OO", stackId, 0);

            // Animation connections
            for (const auto& [layerId, stackId] : m_animLayerToStack)
                writeConnection("OO", layerId, stackId);

            for (const auto& acn : m_animCurveNodeConns)
            {
                writeConnection("OO", acn.curveNodeId, acn.layerId);
                writeConnection("OP", acn.curveNodeId, acn.boneModelId, acn.property);
            }

            for (const auto& ac : m_animCurveConns)
                writeConnection("OP", ac.curveId, ac.curveNodeId, ac.channel);
        }

        m_w.endNode(); // Connections
    }

    void writeConnection(const std::string& type, int64_t child, int64_t parent,
                         const std::string& property = "")
    {
        m_w.beginNode("C");
        m_w.writePropertyS(type);
        m_w.writePropertyL(child);
        m_w.writePropertyL(parent);
        if (!property.empty())
            m_w.writePropertyS(property);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    // ── P70 helpers ──────────────────────────────────────────────
    void writeP70int(const std::string& name, int val)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("int");
        m_w.writePropertyS("Integer");
        m_w.writePropertyS("");
        m_w.writePropertyI(val);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70double(const std::string& name, double val)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("double");
        m_w.writePropertyS("Number");
        m_w.writePropertyS("");
        m_w.writePropertyD(val);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70enum(const std::string& name, int val)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("enum");
        m_w.writePropertyS("");
        m_w.writePropertyS("");
        m_w.writePropertyI(val);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70KTime(const std::string& name, int64_t val)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("KTime");
        m_w.writePropertyS("Time");
        m_w.writePropertyS("");
        m_w.writePropertyL(val);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70string(const std::string& name, const std::string& val)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("KString");
        m_w.writePropertyS("");
        m_w.writePropertyS("");
        m_w.writePropertyS(val);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70compound(const std::string& name, const std::string& val)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("Compound");
        m_w.writePropertyS("");
        m_w.writePropertyS("");
        m_w.writePropertyS(val);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70LclTranslation(double x, double y, double z)
    {
        m_w.beginNode("P");
        m_w.writePropertyS("Lcl Translation");
        m_w.writePropertyS("Lcl Translation");
        m_w.writePropertyS("");
        m_w.writePropertyS("A");
        m_w.writePropertyD(x);
        m_w.writePropertyD(y);
        m_w.writePropertyD(z);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70LclRotation(double x, double y, double z)
    {
        m_w.beginNode("P");
        m_w.writePropertyS("Lcl Rotation");
        m_w.writePropertyS("Lcl Rotation");
        m_w.writePropertyS("");
        m_w.writePropertyS("A");
        m_w.writePropertyD(x);
        m_w.writePropertyD(y);
        m_w.writePropertyD(z);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70LclScaling(double x, double y, double z)
    {
        m_w.beginNode("P");
        m_w.writePropertyS("Lcl Scaling");
        m_w.writePropertyS("Lcl Scaling");
        m_w.writePropertyS("");
        m_w.writePropertyS("A");
        m_w.writePropertyD(x);
        m_w.writePropertyD(y);
        m_w.writePropertyD(z);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70Color(const std::string& name, double r, double g, double b)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("Color");
        m_w.writePropertyS("");
        m_w.writePropertyS("A");
        m_w.writePropertyD(r);
        m_w.writePropertyD(g);
        m_w.writePropertyD(b);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    void writeP70Number(const std::string& name, double val)
    {
        m_w.beginNode("P");
        m_w.writePropertyS(name);
        m_w.writePropertyS("Number");
        m_w.writePropertyS("");
        m_w.writePropertyS("A");
        m_w.writePropertyD(val);
        m_w.endProperties();
        m_w.endNodeLeaf();
    }

    // ── Member data ──────────────────────────────────────────────
    FBXBinaryWriter& m_w;
    const Ogre::Entity* m_entity = nullptr;
    const Ogre::Mesh* m_mesh = nullptr;
    Ogre::Skeleton* m_skeleton = nullptr;
    bool m_hasSkeleton = false;
    bool m_skeletonOnly = false;

    int64_t m_nextId = 1000000;
    int64_t m_documentId = 100000;
    std::vector<int64_t> m_meshModelIds;

    std::vector<int64_t> m_geomIds;
    std::vector<unsigned int> m_geomSubmeshIndices; // submesh index for each entry in m_geomIds
    std::map<std::string, int64_t> m_materialIds;
    std::map<std::string, int64_t> m_textureIds;
    std::map<std::string, int64_t> m_videoIds;
    std::map<unsigned short, int64_t> m_boneModelIds;
    std::map<unsigned short, int64_t> m_boneAttrIds;

    std::vector<int64_t> m_skinIds;
    std::vector<int64_t> m_animStackIds;

    struct ClusterConnection {
        int64_t clusterId;
        int64_t skinId;
        unsigned short boneHandle;
        unsigned int submeshIndex;
    };
    std::vector<ClusterConnection> m_clusterConnections;

    std::vector<std::pair<int64_t, int64_t>> m_animLayerToStack; // layer→stack

    struct AnimCurveNodeConn {
        int64_t curveNodeId;
        int64_t layerId;
        int64_t boneModelId;
        std::string property;
    };
    std::vector<AnimCurveNodeConn> m_animCurveNodeConns;

    struct AnimCurveConn {
        int64_t curveId;
        int64_t curveNodeId;
        std::string channel;
    };
    std::vector<AnimCurveConn> m_animCurveConns;
};

// ═══════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════

bool FBXExporter::exportFBX(const Ogre::Entity* entity, const QString& filePath)
{
    if (!entity || filePath.isEmpty())
        return false;

    std::ofstream out(filePath.toStdString(), std::ios::binary);
    if (!out.is_open())
    {
        Ogre::LogManager::getSingleton().logError(
            "FBXExporter: failed to open " + filePath.toStdString() + " for writing");
        return false;
    }

    FBXBinaryWriter writer(out);
    FBXDocumentBuilder builder(writer);
    bool ok = builder.build(entity);

    out.close();

    if (!ok)
    {
        Ogre::LogManager::getSingleton().logError(
            "FBXExporter: failed to build FBX document for " + std::string(entity->getName()));
    }

    return ok;
}

bool FBXExporter::exportSkeletonOnlyFBX(const Ogre::Skeleton* skeleton, const QString& filePath)
{
    if (!skeleton || filePath.isEmpty())
        return false;

    std::ofstream out(filePath.toStdString(), std::ios::binary);
    if (!out.is_open())
    {
        Ogre::LogManager::getSingleton().logError(
            "FBXExporter: failed to open " + filePath.toStdString() + " for writing");
        return false;
    }

    FBXBinaryWriter writer(out);
    FBXDocumentBuilder builder(writer);
    bool ok = builder.buildSkeletonOnly(skeleton);

    out.close();

    if (!ok)
    {
        Ogre::LogManager::getSingleton().logError(
            "FBXExporter: failed to build skeleton-only FBX document");
    }

    return ok;
}
