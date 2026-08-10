#include "MeshProcessor.h"
#include "BoneProcessor.h"
#include <cstdlib>

// Binds a float3 vertex buffer (bitangent, etc.) to the given source index.
static void bindVector3Buffer(Ogre::VertexData* vertexData, unsigned short source,
                              Ogre::VertexElementSemantic semantic,
                              const std::vector<Ogre::Vector3>& data) {
    vertexData->vertexDeclaration->addElement(source, 0, Ogre::VET_FLOAT3, semantic);
    auto buf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3),
        vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    auto* p = static_cast<float*>(buf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (const auto& v : data) {
        *p++ = v.x;
        *p++ = v.y;
        *p++ = v.z;
    }
    buf->unlock();
    vertexData->vertexBufferBinding->setBinding(source, buf);
}

// Binds a float4 vertex buffer (tangent with handedness in w) to the given source index.
static void bindVector4Buffer(Ogre::VertexData* vertexData, unsigned short source,
                              Ogre::VertexElementSemantic semantic,
                              const std::vector<Ogre::Vector4>& data) {
    vertexData->vertexDeclaration->addElement(source, 0, Ogre::VET_FLOAT4, semantic);
    auto buf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT4),
        vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    auto* p = static_cast<float*>(buf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (const auto& v : data) {
        *p++ = v.x;
        *p++ = v.y;
        *p++ = v.z;
        *p++ = v.w;
    }
    buf->unlock();
    vertexData->vertexBufferBinding->setBinding(source, buf);
}

MeshProcessor::MeshProcessor(Ogre::SkeletonPtr skeleton, bool isZup,
                             const Ogre::Quaternion& bakeRotation)
    : skeleton(skeleton), m_isZup(isZup)
{
    // Z-up metadata keeps its historical +90°X; otherwise an explicit bake
    // rotation (#933 Blender-style node orientation) applies. Never both.
    m_bakeRot = m_isZup ? Ogre::Quaternion(Ogre::Degree(90), Ogre::Vector3::UNIT_X)
                        : bakeRotation;
    m_hasBake = m_isZup ||
        !bakeRotation.equals(Ogre::Quaternion::IDENTITY, Ogre::Radian(1e-4f));
}

void MeshProcessor::processNode(aiNode* node, const aiScene* scene) {
    if (!node || !scene)
        return;

    // Reference frame = the FIRST (by scene index) skinned mesh's node world —
    // the same convention BoneProcessor uses for the re-rooted bind (#936), so
    // skeleton and geometry agree.
    if (!m_haveRef && skeleton && scene->mRootNode) {
        for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
            if (scene->mMeshes[i]->mNumBones == 0) continue;
            if (const aiNode* refNode = BoneProcessor::findMeshNode(scene->mRootNode, i))
                m_refWorldInv = BoneProcessor::nodeWorldTransform(refNode).inverse();
            break;
        }
        m_haveRef = true;
    }

    // Process each mesh located at the current node
    for(auto i = 0u; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        subMeshesData.push_back(processMesh(mesh, scene, node));
    }

    // After we've processed all of the meshes (if any) we then recursively process each of the children nodes
    for(auto i = 0u; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene);
    }
}

SubMeshData* MeshProcessor::processMesh(aiMesh* mesh, const aiScene* scene,
                                        const aiNode* node) {
    SubMeshData* subMeshData = new SubMeshData();
    if (mesh->mName.length > 0)
        subMeshData->name = mesh->mName.C_Str();

    // Rest-pose rotation baked into the vertex data (Z-up conversion or the
    // #933 Blender-style node orientation) — avoids a scene-node rotation and
    // keeps the entity in its natural pose.
    // #933: a RIGID mesh (no bones) inside a skinned model gets its full
    // placement transform (bind-of-parent-bone x node chain) baked below in
    // bindRigidMeshToParentBone — the plain rest-pose bake must not ALSO
    // apply (the parent bone's bind already carries it).
    const bool rigidUnderSkeleton = skeleton && mesh->mNumBones == 0 && node;
    const Ogre::Quaternion R_x90(m_hasBake ? m_bakeRot : Ogre::Quaternion::IDENTITY);
    const bool applyBake = m_hasBake && !rigidUnderSkeleton;
    // Frame alignment for SKINNED meshes beyond the reference one (#933):
    // exact when the rig's bind matches the node pose (the Blender export
    // convention). Identity for single-skinned-mesh scenes (Mixamo).
    Ogre::Matrix4 frameAlign = Ogre::Matrix4::IDENTITY;
    Ogre::Matrix3 frameAlignLinear = Ogre::Matrix3::IDENTITY;
    Ogre::Matrix3 frameAlignNormal = Ogre::Matrix3::IDENTITY;
    const bool hasFrameAlign =
        (skeleton && mesh->mNumBones > 0 && node)
            ? computeFrameAlign(node, frameAlign, frameAlignLinear, frameAlignNormal)
            : false;

    // Initialize blend indices and blend weights
    for(auto i = 0u; i < mesh->mNumVertices; i++) {
        // Process vertices
        Ogre::Vector3 v(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
        if (hasFrameAlign) v = frameAlign * v;
        subMeshData->vertices.push_back(applyBake ? R_x90 * v : v);

        // Process normals
        if(mesh->HasNormals()) {
            Ogre::Vector3 n(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
            if (hasFrameAlign) n = (frameAlignNormal * n).normalisedCopy();
            subMeshData->normals.push_back(applyBake ? R_x90 * n : n);
        }

        // Process texture coordinates
        if(mesh->HasTextureCoords(0)) {
            subMeshData->texCoords.push_back(Ogre::Vector2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y));
        }
    }

    // Load blend weights and blend indices
    for(auto i = 0u; i < mesh->mNumBones; i++) {
        aiBone* bone = mesh->mBones[i];
        if(!skeleton || !skeleton->hasBone(bone->mName.C_Str())) continue;
        // Retrieve the bone (it should already exist)
        Ogre::Bone* ogreBone = skeleton->getBone(bone->mName.C_Str());
        for(auto j = 0u; j < bone->mNumWeights; j++) {
            aiVertexWeight weight = bone->mWeights[j];

            Ogre::VertexBoneAssignment vba;
            vba.vertexIndex = weight.mVertexId;
            vba.boneIndex = ogreBone->getHandle();
            vba.weight = weight.mWeight;

            subMeshData->boneAssignments.push_back(vba);
        }
    }

    // Process indices
    for(auto i = 0u; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for(auto j = 0u; j < face.mNumIndices; j++) {
            subMeshData->indices.push_back(face.mIndices[j]);
        }
    }

    // Process Material Index
    subMeshData->materialIndex = mesh->mMaterialIndex;

    // Process tangents and bitangents — store tangent as float4 with handedness in w
    if(mesh->HasTangentsAndBitangents()) {
        for(auto i = 0u; i < mesh->mNumVertices; i++) {
            Ogre::Vector3 T(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
            Ogre::Vector3 B(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z);
            Ogre::Vector3 N(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
            if (hasFrameAlign) {
                // Tangent-space directions take the LINEAR part; the normal
                // takes the inverse-transpose (non-uniform-scale correct).
                T = (frameAlignLinear * T).normalisedCopy();
                B = (frameAlignLinear * B).normalisedCopy();
                N = (frameAlignNormal * N).normalisedCopy();
            }
            if (applyBake) { T = R_x90 * T; B = R_x90 * B; N = R_x90 * N; }
            // Compute handedness: if cross(N,T) is opposite to B, the tangent space is left-handed
            float handedness = (N.crossProduct(T).dotProduct(B) < 0.0f) ? -1.0f : 1.0f;
            subMeshData->tangents.push_back(Ogre::Vector4(T.x, T.y, T.z, handedness));
            subMeshData->bitangents.push_back(B);
        }
    }

    // Process vertex colors
    if(mesh->HasVertexColors(0)) {
        for(auto i = 0u; i < mesh->mNumVertices; i++) {
            subMeshData->colors.push_back(Ogre::ColourValue(mesh->mColors[0][i].r, mesh->mColors[0][i].g, mesh->mColors[0][i].b, mesh->mColors[0][i].a));
        }
    }

    // Process morph targets / blend shapes. Assimp parks each shape's
    // deformed positions in `aiAnimMesh::mVertices`; we store them
    // verbatim and convert to per-vertex deltas later, at mesh-build
    // time, where we already have the base positions in hand for the
    // Ogre::Pose constructor. Apply the same Z-up axis bake the base
    // vertex pass uses so the shape and base agree on coordinate frame.
    // Sidecar name hints only apply when the scene has exactly ONE morphed
    // mesh — the unambiguous case (a flat ordered name list can't be split
    // across submeshes safely).
    bool useNameHints = false;
    if (!m_nameHints.empty()) {
        unsigned morphedMeshes = 0;
        for (auto mi = 0u; mi < scene->mNumMeshes; mi++)
            if (scene->mMeshes[mi] && scene->mMeshes[mi]->mNumAnimMeshes > 0)
                morphedMeshes++;
        useNameHints = (morphedMeshes == 1);
    }
    for(auto am = 0u; am < mesh->mNumAnimMeshes; am++) {
        const aiAnimMesh* anim = mesh->mAnimMeshes[am];
        if (!anim || !anim->mVertices || anim->mNumVertices != mesh->mNumVertices) continue;
        MorphTargetData target;
        if (anim->mName.length > 0)
            target.name = anim->mName.C_Str();
        else if (useNameHints && am < m_nameHints.size() && !m_nameHints[am].empty())
            target.name = m_nameHints[am];
        else
            target.name = std::string("Shape_") + std::to_string(am);
        target.positions.reserve(anim->mNumVertices);
        for (auto i = 0u; i < anim->mNumVertices; i++) {
            Ogre::Vector3 v(anim->mVertices[i].x, anim->mVertices[i].y, anim->mVertices[i].z);
            if (hasFrameAlign) v = frameAlign * v;
            target.positions.push_back(applyBake ? R_x90 * v : v);
        }
        subMeshData->morphTargets.push_back(std::move(target));
    }

    // #933: Blender bone-parented rigid parts (Quaternius Robot) have no
    // vertex weights — Ogre's software vertex blend then asserts on ANY
    // render ("srcElemPos && srcElemBlendIndices && srcElemBlendWeights").
    // Bind the whole submesh to its nearest ancestor bone with weight 1 and
    // bake its node-chain placement into the vertices: renders assembled and
    // follows the bone during animation. Runs LAST so tangents and morph
    // targets (filled above) receive the placement transform too.
    if (rigidUnderSkeleton)
        bindRigidMeshToParentBone(subMeshData, node);

    return subMeshData;
}

Ogre::MeshPtr MeshProcessor::createMesh(const Ogre::String& name, const Ogre::String& group, MaterialProcessor& materialProcessor) {
    // Create the mesh
    Ogre::MeshPtr ogreMesh = Ogre::MeshManager::getSingleton().createManual(name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    // Initialize the min and max coordinates to the first vertex
    Ogre::Vector3 minCoords = subMeshesData[0]->vertices[0];
    Ogre::Vector3 maxCoords = subMeshesData[0]->vertices[0];

    for(const auto& subMeshData : subMeshesData) {
        // Create a submesh
        Ogre::SubMesh* subMesh = ogreMesh->createSubMesh();

        // Register the source name (aiMesh::mName) so named submeshes — e.g.
        // PartOps parts "head"/"torso" round-tripped through FBX — are
        // addressable by name and shown in the Scene tree. Skipped when the
        // source mesh was unnamed OR the name is already taken: nameSubMesh
        // overwrites the SubMeshNameMap entry, so a duplicate aiMesh::mName
        // would make BOTH names resolve to the last submesh (CodeRabbit). On a
        // collision we disambiguate with an index suffix instead of dropping
        // the name, so every submesh stays addressable.
        if (!subMeshData->name.empty()) {
            const unsigned short idx =
                static_cast<unsigned short>(ogreMesh->getNumSubMeshes() - 1);
            const Ogre::Mesh::SubMeshNameMap& nameMap = ogreMesh->getSubMeshNameMap();
            std::string name = subMeshData->name;
            if (nameMap.find(name) != nameMap.end()) {
                unsigned int suffix = 1;
                std::string candidate;
                do {
                    candidate = name + "_" + std::to_string(suffix++);
                } while (nameMap.find(candidate) != nameMap.end());
                name = candidate;
            }
            ogreMesh->nameSubMesh(name, idx);
        }

        // Create the vertex data
        Ogre::VertexData* vertexData = new Ogre::VertexData();
        subMesh->useSharedVertices = false;
        subMesh->vertexData = vertexData;

        // Define the vertex format
        Ogre::VertexDeclaration* vertexDecl = vertexData->vertexDeclaration;
        size_t currOffset = vertexDecl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION).getSize();
        currOffset += vertexDecl->addElement(0, currOffset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL).getSize();
        if(!subMeshData->texCoords.empty())
            vertexDecl->addElement(0, currOffset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES).getSize();

        // Set the vertex count
        vertexData->vertexCount = subMeshData->vertices.size();

        // Create the vertex buffer and set the vertex data
        Ogre::HardwareVertexBufferSharedPtr vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(vertexDecl->getVertexSize(0), vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float* pVertex = static_cast<float*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));

        // Set the vertex positions, blend weights, normals, texture coordinates, and blend indices
        for(size_t i = 0; i < subMeshData->vertices.size(); i++) {
            *pVertex++ = subMeshData->vertices[i].x;
            *pVertex++ = subMeshData->vertices[i].y;
            *pVertex++ = subMeshData->vertices[i].z;

            *pVertex++ = subMeshData->normals[i].x;
            *pVertex++ = subMeshData->normals[i].y;
            *pVertex++ = subMeshData->normals[i].z;

            if(subMeshData->texCoords.empty()) continue;
            *pVertex++ = subMeshData->texCoords[i].x;
            *pVertex++ = subMeshData->texCoords[i].y;
        }

        vbuf->unlock();
        vertexData->vertexBufferBinding->setBinding(0, vbuf);

        // Bind optional vertex attribute buffers
        unsigned short nextSource = 1;
        if (!subMeshData->tangents.empty())
            bindVector4Buffer(vertexData, nextSource++, Ogre::VES_TANGENT, subMeshData->tangents);
        if (!subMeshData->bitangents.empty())
            bindVector3Buffer(vertexData, nextSource++, Ogre::VES_BINORMAL, subMeshData->bitangents);
        if (!subMeshData->colors.empty()) {
            vertexDecl->addElement(nextSource, 0, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE);
            auto colorBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR),
                vertexData->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
            auto* pColor = static_cast<Ogre::RGBA*>(colorBuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
            for (const auto& color : subMeshData->colors)
                *pColor++ = color.getAsBYTE();
            colorBuf->unlock();
            vertexData->vertexBufferBinding->setBinding(nextSource, colorBuf);
        }

        // Create the index data and set it to the submesh
        Ogre::IndexData* indexData = subMesh->indexData;

        // Set the index count
        indexData->indexCount = subMeshData->indices.size();

        // Create the index buffer. Use 32-bit indices when the submesh has
        // more than 65535 vertices — a 16-bit buffer wraps every index past
        // 0xFFFF, which collapsed large meshes (e.g. high-res image-to-3D
        // output, 80k+ verts) onto their first 65536 vertices and tore them
        // apart on import. Match on the VERTEX count (what indices address),
        // not the index count.
        const bool use32BitIndices = subMeshData->vertices.size() > 65535;
        Ogre::HardwareIndexBufferSharedPtr ibuf =
            Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
                use32BitIndices ? Ogre::HardwareIndexBuffer::IT_32BIT
                                : Ogre::HardwareIndexBuffer::IT_16BIT,
                indexData->indexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

        // Set the indices, writing the matching element width.
        if (use32BitIndices) {
            uint32_t* pIndices = static_cast<uint32_t*>(
                ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
            for (size_t i = 0; i < subMeshData->indices.size(); i++)
                *pIndices++ = subMeshData->indices[i];
        } else {
            unsigned short* pIndices = static_cast<unsigned short*>(
                ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
            for (size_t i = 0; i < subMeshData->indices.size(); i++)
                *pIndices++ = static_cast<unsigned short>(subMeshData->indices[i]);
        }

        ibuf->unlock();
        indexData->indexBuffer = ibuf;

        // Update the min and max coordinates
        for(const Ogre::Vector3& vertex : subMeshData->vertices) {
            minCoords.x = std::min(minCoords.x, vertex.x);
            minCoords.y = std::min(minCoords.y, vertex.y);
            minCoords.z = std::min(minCoords.z, vertex.z);
            maxCoords.x = std::max(maxCoords.x, vertex.x);
            maxCoords.y = std::max(maxCoords.y, vertex.y);
            maxCoords.z = std::max(maxCoords.z, vertex.z);
        }

        // Set the bone assignments
        for(const Ogre::VertexBoneAssignment& vba : subMeshData->boneAssignments) {
            subMesh->addBoneAssignment(vba);
        }

        // Assign the material
        if(subMeshData->materialIndex < materialProcessor.size())
            subMesh->setMaterialName(materialProcessor[subMeshData->materialIndex]->getName());
    }

    // Set the bounding box and bounding sphere radius
    ogreMesh->_setBounds(Ogre::AxisAlignedBox(minCoords, maxCoords));
    ogreMesh->_setBoundingSphereRadius((maxCoords - minCoords).length() / 2.0f);

    // Morph targets / blend shapes → Ogre::Pose entries on the mesh.
    // Build per-submesh delta poses: Ogre `Pose` stores per-vertex
    // offsets relative to the base mesh. Each pose targets a specific
    // submesh (index 1-based in Ogre; 0 means "shared vertex data",
    // which our importer never uses — every aiMesh gets its own
    // SubMesh::vertexData).
    //
    // For each pose we also create a single VAT_POSE animation
    // ("MorphTrack_<name>") with a one-keyframe track at full
    // influence; the MorphAnimationManager will use the matching
    // AnimationState weight to drive live preview without us having
    // to mutate vertex buffers ourselves.
    for (size_t s = 0; s < subMeshesData.size(); ++s) {
        const auto* smd = subMeshesData[s];
        if (smd->morphTargets.empty()) continue;
        // Submesh handle is 1-based: 0 = shared, 1..N = per-submesh.
        const unsigned short targetSubmesh = static_cast<unsigned short>(s + 1);
        for (const auto& mt : smd->morphTargets) {
            if (mt.positions.size() != smd->vertices.size()) continue;
            // Lazy `createPose`: scan for the first non-zero delta
            // before allocating the Ogre::Pose. Identical-to-base
            // targets (rare with hand-authored content, more common
            // with auto-exported FBX) would otherwise leave an empty
            // pose on the mesh + an empty Animation referencing it.
            Ogre::Pose* pose = nullptr;
            for (size_t vi = 0; vi < mt.positions.size(); ++vi) {
                const Ogre::Vector3 delta = mt.positions[vi] - smd->vertices[vi];
                if (delta.squaredLength() <= 1e-12f) continue;
                if (!pose) pose = ogreMesh->createPose(targetSubmesh, mt.name);
                pose->addVertex(vi, delta);
            }
        }
    }

    // One Ogre::Animation per *unique* morph-target name. Same-named
    // poses across different submeshes (e.g. a "Smile" target on both
    // body and head) all need to drive together off the same
    // AnimationState weight, so they share one Animation with one
    // VAT_POSE track per affected submesh. Without grouping, the
    // second-and-later same-named poses would create a new
    // Ogre::Animation but Ogre::Mesh enforces unique animation names —
    // we'd either skip them (and they'd never move) or fail to import.
    const auto& poseList = ogreMesh->getPoseList();
    for (unsigned short pi = 0; pi < poseList.size(); ++pi) {
        const Ogre::Pose* pose = poseList[pi];
        if (!pose) continue;
        const Ogre::String animName = pose->getName();
        if (animName.empty()) continue;

        // First sighting of this name → create the Animation. Later
        // sightings find it via hasAnimation and just append a track.
        Ogre::Animation* anim = nullptr;
        if (ogreMesh->hasAnimation(animName)) {
            anim = ogreMesh->getAnimation(animName);
        } else {
            anim = ogreMesh->createAnimation(animName, /*length=*/0.0f);
        }
        if (!anim) continue;

        Ogre::VertexAnimationTrack* track = nullptr;
        const unsigned short handle = pose->getTarget();
        if (anim->hasVertexTrack(handle)) {
            // Same submesh + same name → rare content error. Append
            // the pose reference to the existing keyframe so the user
            // still gets some movement (better than silently dropping).
            track = anim->getVertexTrack(handle);
        } else {
            track = anim->createVertexTrack(handle, Ogre::VAT_POSE);
        }
        if (!track) continue;
        auto* kf = track->getNumKeyFrames() > 0
                       ? static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(0))
                       : track->createVertexPoseKeyFrame(0.0f);
        // Full influence on this pose; AnimationState weight scales it.
        kf->addPoseReference(pi, 1.0f);
    }

    // Compile the mesh
    ogreMesh->load();

    // Link skeleton AFTER load() so that isLoaded()==true and the
    // skeleton pointer is properly resolved (not just the name stored).
    if(skeleton) {
        ogreMesh->setSkeletonName(skeleton->getName());
    }

    // clean up to avoid memory leaks.
    subMeshesData.clear();

    return ogreMesh;
}

bool MeshProcessor::computeFrameAlign(const aiNode* node, Ogre::Matrix4& frameAlign,
                                      Ogre::Matrix3& frameAlignLinear,
                                      Ogre::Matrix3& frameAlignNormal) const
{
    frameAlign = m_refWorldInv * BoneProcessor::nodeWorldTransform(node);
    frameAlign.extract3x3Matrix(frameAlignLinear);
    // Normal matrix = inverse-transpose of the linear part — equals the
    // rotation for rigid/uniform-scale frames, correct for non-uniform ones.
    frameAlignNormal = frameAlignLinear.Inverse().Transpose();
    Ogre::Vector3 faPos;
    Ogre::Vector3 faScale;
    Ogre::Quaternion faRot;
    Ogre::Affine3(frameAlign).decomposition(faPos, faScale, faRot);
    return !faPos.positionEquals(Ogre::Vector3::ZERO, 1e-5f) ||
           !faRot.equals(Ogre::Quaternion::IDENTITY, Ogre::Radian(1e-4f)) ||
           !faScale.positionEquals(Ogre::Vector3::UNIT_SCALE, 1e-4f);
}

void MeshProcessor::bindRigidMeshToParentBone(SubMeshData* data, const aiNode* node)
{
    if (!skeleton || !data || !node)
        return;

    // Nearest ancestor node that exists as a skeleton bone (start at the
    // PARENT — Blender names the mesh object after the bone it is parented
    // to, so the mesh node itself may share the bone's name).
    Ogre::Bone* bone = nullptr;
    const aiNode* boneNode = nullptr;
    for (const aiNode* n = node->mParent; n; n = n->mParent) {
        if (n->mName.length && skeleton->hasBone(n->mName.C_Str())) {
            bone = skeleton->getBone(n->mName.C_Str());
            boneNode = n;
            break;
        }
    }
    if (!bone) {
        // Not under any bone: fall back to the first root bone so software
        // blending has data (the part then rides the root rigidly).
        const auto& roots = skeleton->getRootBones();
        if (roots.empty())
            return;
        bone = roots.front();
    }

    // Node chain from the bone's node (exclusive) down to the mesh node
    // (inclusive) — the part's placement relative to the bone.
    Ogre::Matrix4 chain = Ogre::Matrix4::IDENTITY;
    for (const aiNode* n = node; n && n != boneNode; n = n->mParent) {
        const aiMatrix4x4& m = n->mTransformation;
        chain = Ogre::Matrix4(m.a1, m.a2, m.a3, m.a4,
                              m.b1, m.b2, m.b3, m.b4,
                              m.c1, m.c2, m.c3, m.c4,
                              m.d1, m.d2, m.d3, m.d4) * chain;
    }

    // Placement in mesh space: the bone's BIND world (post-bake — the
    // skeleton is at its binding pose during import) times the node chain.
    skeleton->reset(true);
    skeleton->_updateTransforms();
    Ogre::Matrix4 bindFull = Ogre::Matrix4::IDENTITY;
    bindFull.makeTransform(bone->_getDerivedPosition(),
                           bone->_getDerivedScale(),
                           bone->_getDerivedOrientation());
    const Ogre::Matrix4 placement = bindFull * chain;
    // Rotation part for normals/tangents (assumes near-uniform scale).
    Ogre::Vector3 pPos;
    Ogre::Vector3 pScale;
    Ogre::Quaternion pRot;
    Ogre::Affine3(placement).decomposition(pPos, pScale, pRot);

    for (auto& v : data->vertices) v = placement * v;
    for (auto& n : data->normals)  n = (pRot * n).normalisedCopy();
    for (auto& t : data->tangents) {
        const Ogre::Vector3 dir = pRot * Ogre::Vector3(t.x, t.y, t.z);
        t = Ogre::Vector4(dir.x, dir.y, dir.z, t.w);
    }
    for (auto& mt : data->morphTargets)
        for (auto& v : mt.positions) v = placement * v;

    data->boneAssignments.reserve(data->vertices.size());
    for (unsigned i = 0; i < data->vertices.size(); ++i) {
        Ogre::VertexBoneAssignment vba;
        vba.vertexIndex = i;
        vba.boneIndex = bone->getHandle();
        vba.weight = 1.0f;
        data->boneAssignments.push_back(vba);
    }
}
