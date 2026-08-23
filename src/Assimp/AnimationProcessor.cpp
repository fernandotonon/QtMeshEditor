#include "AnimationProcessor.h"

#include <cmath>
#include <cstdlib>
#include <string>

AnimationProcessor::AnimationProcessor(Ogre::SkeletonPtr skeleton): skeleton(skeleton) {}

namespace {

// Reconstruct the pose NAME MeshProcessor assigned to morph-target index `am`
// of aiMesh `mesh`. MeshProcessor uses the aiAnimMesh name when present, else
// the "Shape_<index>" fallback — we must reproduce it exactly so name-based
// pose lookup lands on the right Ogre::Pose. Returns empty when out of range.
std::string morphTargetPoseName(const aiMesh* mesh, unsigned int am)
{
    if (!mesh || am >= mesh->mNumAnimMeshes)
        return {};
    const aiAnimMesh* anim = mesh->mAnimMeshes[am];
    if (anim && anim->mName.length > 0)
        return std::string(anim->mName.C_Str());
    return std::string("Shape_") + std::to_string(am);
}

// Find the pose index (into mesh->getPoseList()) for the first pose named
// `name`. -1 if none — MeshProcessor SKIPS all-zero-delta targets (lazy
// createPose), so aiAnimMesh index != pose-list index; only a name lookup is
// reliable, and a skipped (zero-delta) target correctly has no pose.
int poseIndexForName(const Ogre::MeshPtr& mesh, const std::string& name)
{
    if (name.empty())
        return -1;
    const auto& poses = mesh->getPoseList();
    for (unsigned short i = 0; i < poses.size(); ++i)
        if (poses[i] && poses[i]->getName() == name)
            return static_cast<int>(i);
    return -1;
}

// Locate the aiMesh a morph channel targets. aiMeshMorphAnim::mName is the NODE
// name; its mValues[] index the aiAnimMeshes of the mesh attached to that node.
// glTF nodes carry a single mesh, which is the case Assimp populates morph
// channels for; when a node has several meshes only meshes exposing anim-meshes
// are candidates and we take the first (multi-morph-mesh nodes don't occur in
// glTF's one-mesh-per-node model).
const aiMesh* meshForMorphChannel(const aiScene* scene, const aiNode* node)
{
    if (!scene || !node)
        return nullptr;
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        const unsigned int meshIdx = node->mMeshes[i];
        if (meshIdx >= scene->mNumMeshes)
            continue;
        const aiMesh* mesh = scene->mMeshes[meshIdx];
        if (mesh && mesh->mNumAnimMeshes > 0)
            return mesh;
    }
    return nullptr;
}

} // namespace

void AnimationProcessor::processMorphWeightAnimations(const Ogre::MeshPtr& mesh, const aiScene* scene)
{
    if (!mesh || !scene)
        return;
    // No poses on the mesh → nothing a weight clip could reference.
    if (mesh->getPoseCount() == 0)
        return;

    unsigned int generatedNameCounter = 0;

    for (unsigned int a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        if (!anim || anim->mNumMorphMeshChannels == 0)
            continue;

        const double ticksPerSecond = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 24.0;

        // One Ogre::Animation (the weight clip) per aiAnimation, named after it.
        std::string clipName = anim->mName.C_Str();
        if (clipName.empty())
            clipName = std::string("MorphAnim_") + std::to_string(generatedNameCounter++);
        // Avoid colliding with the per-target SHAPE clips MeshProcessor created
        // (each named exactly a pose name) or an already-built clip.
        if (mesh->hasAnimation(clipName)) {
            std::string base = clipName;
            unsigned int suffix = 1;
            do {
                clipName = base + "_" + std::to_string(suffix++);
            } while (mesh->hasAnimation(clipName));
        }

        Ogre::Animation* clip = mesh->createAnimation(clipName, 0.0f);
        float maxTime = 0.0f;
        bool wroteAnyKey = false;

        for (unsigned int c = 0; c < anim->mNumMorphMeshChannels; ++c) {
            const aiMeshMorphAnim* morph = anim->mMorphMeshChannels[c];
            if (!morph || morph->mNumKeys == 0)
                continue;

            // Resolve the node → mesh this channel drives so we can map value
            // indices to the matching morph-target pose names.
            const aiNode* node = scene->mRootNode
                ? scene->mRootNode->FindNode(morph->mName)
                : nullptr;
            const aiMesh* srcMesh = meshForMorphChannel(scene, node);
            // FALLBACK node resolution: some exporters (our glTF weights inject
            // after mesh-split) name the morph channel's node after a SPLIT
            // submesh (e.g. "sniff_submesh0*0") that FindNode can't match to the
            // scene node holding the mesh. When the direct node lookup fails,
            // scan ALL meshes for the first one carrying anim-meshes — a
            // single-morph-mesh model (the common case) resolves unambiguously.
            if (!srcMesh) {
                for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
                    const aiMesh* cand = scene->mMeshes[mi];
                    if (cand && cand->mNumAnimMeshes > 0) { srcMesh = cand; break; }
                }
            }
            if (!srcMesh)
                continue;

            for (unsigned int k = 0; k < morph->mNumKeys; ++k) {
                const aiMeshMorphKey& key = morph->mKeys[k];
                const float t = static_cast<float>(key.mTime / ticksPerSecond);

                for (unsigned int j = 0; j < key.mNumValuesAndWeights; ++j) {
                    const unsigned int targetIdx = key.mValues[j];
                    const double weight = key.mWeights[j];

                    // Map anim-mesh target index -> pose name -> pose index.
                    // A skipped zero-delta target has no pose: -1, drop it.
                    const std::string poseName = morphTargetPoseName(srcMesh, targetIdx);
                    const int pi = poseIndexForName(mesh, poseName);
                    if (pi < 0)
                        continue;

                    // Track keyed on the pose's target submesh handle — same
                    // grouping the authoring path uses (targets on one submesh
                    // share a track; keyframes at time t carry a pose ref each).
                    const unsigned short handle = mesh->getPoseList()[static_cast<size_t>(pi)]->getTarget();
                    Ogre::VertexAnimationTrack* track = clip->hasVertexTrack(handle)
                        ? clip->getVertexTrack(handle)
                        : clip->createVertexTrack(handle, Ogre::VAT_POSE);
                    if (!track)
                        continue;

                    // Fetch-or-create the keyframe at this time on this track.
                    Ogre::VertexPoseKeyFrame* kf = nullptr;
                    for (unsigned short ki = 0; ki < track->getNumKeyFrames(); ++ki) {
                        auto* existing = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(ki));
                        if (std::abs(existing->getTime() - t) < 1e-4f) { kf = existing; break; }
                    }
                    if (!kf)
                        kf = track->createVertexPoseKeyFrame(t);

                    kf->addPoseReference(static_cast<unsigned short>(pi),
                                         static_cast<float>(weight));
                    wroteAnyKey = true;
                    if (t > maxTime)
                        maxTime = t;
                }
            }
        }

        if (!wroteAnyKey) {
            // No usable channel resolved to a pose — drop the empty clip so we
            // don't surface a zero-track animation in the list.
            mesh->removeAnimation(clipName);
            continue;
        }
        clip->setLength(maxTime);
    }
}

void AnimationProcessor::processAnimations(const aiScene* scene) {
    for(auto i = 0u; i < scene->mNumAnimations; i++) {
        aiAnimation* animation = scene->mAnimations[i];
        processAnimation(animation, scene);
    }
}

void AnimationProcessor::processAnimation(aiAnimation* animation, const aiScene* scene) {
    // Skip channel-less animations. Assimp represents a glTF morph-weight
    // animation as an aiAnimation with mNumChannels == 0 (the weights live in
    // mMorphMeshChannels, handled separately by processMorphWeightAnimations).
    // Creating a 0-track SKELETON clip for it here is worse than useless: the
    // mesh-level VAT_POSE clip already carries the same name (e.g. "MorphAnim"),
    // so on RE-EXPORT buildAiScene emits this empty skeletal clip AND
    // injectMorphWeightAnimations appends the real one — two glTF animations
    // with the same name, which makes the file fail to re-import (Ogre throws
    // "animation already exists" and the whole load is aborted → 0 entities).
    // A skeletal clip with no node tracks animates nothing, so dropping it is
    // always safe. (issue #517: skeletal + morph coexistence)
    if (animation->mNumChannels == 0) return;
    // get the animation speed
    auto mTicksPerSecond = (Ogre::Real)((0 == animation->mTicksPerSecond) ? 24.0f : animation->mTicksPerSecond);
    // Create the animation
    Ogre::Animation* ogreAnimation = skeleton->createAnimation(animation->mName.C_Str(), animation->mDuration/mTicksPerSecond);
    // Process the animation channels
    for(auto i = 0u; i < animation->mNumChannels; i++) {
        aiNodeAnim* nodeAnim = animation->mChannels[i];
        processAnimationChannel(nodeAnim, ogreAnimation, scene, i, mTicksPerSecond);
    }
    // A channel only becomes a node track when it targets a BONE
    // (processAnimationChannel early-returns otherwise). If NONE of this
    // aiAnimation's channels hit a bone, the clip is empty — it was a
    // SceneNode-transform clip (#517 node anim, channel targets the scene
    // node, not a bone) or some other non-skeletal channel. Leaving it on the
    // skeleton produces a phantom 0-track animation that pollutes the Inspector
    // list and dope sheet and gets auto-selected (so the real skeletal clip's
    // bones never render in the dope sheet). Node clips are rebuilt separately
    // by reconstructNodeClipsFrom* — drop the phantom here. (issue #517)
    if (ogreAnimation->getNumNodeTracks() == 0)
        skeleton->removeAnimation(animation->mName.C_Str());
}

void AnimationProcessor::processAnimationChannel(aiNodeAnim* nodeAnim, Ogre::Animation* animation, const aiScene* scene, unsigned int channelIndex, Ogre::Real mTicksPerSecond) {
    if(!skeleton->hasBone(nodeAnim->mNodeName.C_Str())) return;

    // Create the animation track
    Ogre::Bone* bone = skeleton->getBone(nodeAnim->mNodeName.C_Str());
    Ogre::NodeAnimationTrack* track = animation->createNodeTrack(bone->getHandle(), bone);

    // Create a map to store keyframes by time
    std::map<double, std::tuple<Ogre::Vector3, Ogre::Quaternion, Ogre::Vector3>> keyframes;

    // Get the bone's T-pose position and orientation
    auto boneTPosePosition = bone->getPosition();
    Ogre::Quaternion boneTPoseInverseRotation = bone->getOrientation().Inverse();
    // The channel's keys live in the source NODE's local space, so the bind
    // reference for scale keys is the aiNode's own bind scale — NOT the Ogre
    // bone's (the two differ when the bind chain was re-rooted, e.g. the
    // mesh-node-relative root bind). Blender-style FBX rigs re-express the
    // armature's static ×100 scale in every animation curve; dividing by the
    // node bind scale turns that into the identity it really is (#936).
    // Guard degenerate components so a zero scale can't divide by zero.
    Ogre::Vector3 nodeBindScale = Ogre::Vector3::UNIT_SCALE;
    // Space-change prefix for POSITION/ROTATION keys: the Ogre bind can be
    // RE-ROOTED relative to the node bind (BoneProcessor's mesh-node-relative
    // root, #936), so a key equal to the node's bind transform must still
    // produce an IDENTITY delta. C = OgreBindLocal · NodeBindLocal⁻¹ maps
    // node-local keys into the Ogre bind space; it is exactly identity for
    // every bone whose two binds agree (all non-re-rooted bones, Mixamo rigs).
    bool haveSpaceChange = false;
    Ogre::Vector3 cPos = Ogre::Vector3::ZERO;
    Ogre::Quaternion cRot = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 cScale = Ogre::Vector3::UNIT_SCALE;
    if (scene && scene->mRootNode) {
        if (const aiNode* channelNode = scene->mRootNode->FindNode(nodeAnim->mNodeName)) {
            aiVector3D ns, np; aiQuaternion nr;
            channelNode->mTransformation.Decompose(ns, nr, np);
            nodeBindScale = Ogre::Vector3(ns.x, ns.y, ns.z);

            // #954: the correction must map the key from the NODE parent
            // frame into the OGRE parent frame:
            //     C = OgreParentWorldBind⁻¹ · NodeParentWorldBind
            // (An earlier form used the bone's OWN local-bind mismatch,
            // C = OgreBindLocal · NodeBindLocal⁻¹ — correct for ROOT bones,
            // where the two coincide, but wrong for deep bones: per-bone
            // bind mismatches accumulate along the ancestor chain, so on
            // Blender-FBX rigs whose skin binds differ from node binds by
            // per-bone PreRotations the arm chains played ~90° raised while
            // the legs stayed near-correct — verified against Blender's own
            // import of the Quaternius Woman.) Identity whenever node binds
            // and skin binds agree chain-wide (Mixamo-class rigs), so the
            // common case is untouched.
            // ROOT bones keep the OWN-BIND form (their re-rooting is a bone-
            // local convention, #936 — verified); deep bones need the parent-
            // chain form (per-bone bind mismatches accumulate along the
            // ancestors — a first all-parent-chain version double-corrected
            // the ROOT and tipped the whole body).
            const bool isRootBone = bone->getParent() == nullptr;
            if (isRootBone) {
                const bool invertible = std::abs(ns.x) > 1e-8f &&
                                        std::abs(ns.y) > 1e-8f &&
                                        std::abs(ns.z) > 1e-8f;
                if (invertible) {
                    Ogre::Affine3 nodeBind;
                    nodeBind.makeTransform(Ogre::Vector3(np.x, np.y, np.z),
                                           Ogre::Vector3(ns.x, ns.y, ns.z),
                                           Ogre::Quaternion(nr.w, nr.x, nr.y, nr.z));
                    Ogre::Affine3 ogreBind;
                    ogreBind.makeTransform(bone->getPosition(), bone->getScale(),
                                           bone->getOrientation());
                    const Ogre::Affine3 c = ogreBind * nodeBind.inverse();
                    c.decomposition(cPos, cScale, cRot);
                    haveSpaceChange =
                        !cPos.positionEquals(Ogre::Vector3::ZERO, 1e-5f) ||
                        !cRot.equals(Ogre::Quaternion::IDENTITY, Ogre::Radian(1e-4f)) ||
                        !cScale.positionEquals(Ogre::Vector3::UNIT_SCALE, 1e-4f);
                }
            } else {
            Ogre::Affine3 nodeParent = Ogre::Affine3::IDENTITY;
            {
                std::vector<const aiNode*> chain;
                for (const aiNode* n = channelNode->mParent; n; n = n->mParent)
                    chain.push_back(n);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    aiVector3D ps, pp; aiQuaternion pr;
                    (*it)->mTransformation.Decompose(ps, pr, pp);
                    Ogre::Affine3 t;
                    t.makeTransform(Ogre::Vector3(pp.x, pp.y, pp.z),
                                    Ogre::Vector3(ps.x, ps.y, ps.z),
                                    Ogre::Quaternion(pr.w, pr.x, pr.y, pr.z));
                    nodeParent = nodeParent * t;
                }
            }
            // The Ogre parent chain composes bone binds only (bones live
            // under the skeleton, no scene nodes in between). The NODE chain
            // above however includes the armature/scene ancestors, which the
            // OGRE side handles at the entity level — composing them here
            // would double-apply the armature transform on non-root bones.
            // So the node chain must be taken RELATIVE to the node that
            // corresponds to the Ogre root's PARENT, i.e. drop the ancestors
            // above the skeleton's root node: find the aiNode of the Ogre
            // ROOT bone and rebase both chains there.
            const Ogre::Node* ogreRoot = bone;
            while (ogreRoot->getParent()) ogreRoot = ogreRoot->getParent();
            const aiNode* rootNode =
                scene->mRootNode->FindNode(ogreRoot->getName().c_str());
            Ogre::Affine3 nodeAboveRoot = Ogre::Affine3::IDENTITY;
            if (rootNode) {
                std::vector<const aiNode*> chain;
                for (const aiNode* n = rootNode->mParent; n; n = n->mParent)
                    chain.push_back(n);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    aiVector3D ps, pp; aiQuaternion pr;
                    (*it)->mTransformation.Decompose(ps, pr, pp);
                    Ogre::Affine3 t;
                    t.makeTransform(Ogre::Vector3(pp.x, pp.y, pp.z),
                                    Ogre::Vector3(ps.x, ps.y, ps.z),
                                    Ogre::Quaternion(pr.w, pr.x, pr.y, pr.z));
                    nodeAboveRoot = nodeAboveRoot * t;
                }
            }
            Ogre::Affine3 ogreParent = Ogre::Affine3::IDENTITY;
            {
                std::vector<const Ogre::Node*> chain;
                for (const Ogre::Node* n = bone->getParent(); n;
                     n = n->getParent())
                    chain.push_back(n);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    Ogre::Affine3 t;
                    t.makeTransform((*it)->getPosition(), (*it)->getScale(),
                                    (*it)->getOrientation());
                    ogreParent = ogreParent * t;
                }
            }
            const Ogre::Matrix4 opm(ogreParent);
            const Ogre::Matrix4 narm(nodeAboveRoot);
            const bool invertible =
                std::abs(opm.determinant()) > 1e-12f &&
                std::abs(narm.determinant()) > 1e-12f;
            if (invertible) {
                // Node parent chain RELATIVE to the skeleton root's parent
                // node (rebase): nodeParentRel = nodeAboveRoot⁻¹·nodeParent.
                // Then C maps node-parent-relative keys into the Ogre parent
                // frame. For rigs whose node binds equal their skin binds
                // this telescopes to identity (Mixamo unchanged).
                // Full node-ancestor chain on the node side: skin binds
                // (the Ogre side) are MESH-space, i.e. they already include
                // the armature/scene ancestors — rebasing them out
                // reintroduced the armature transform conjugated (lying
                // body). For consistent rigs C telescopes to identity.
                const Ogre::Affine3 c = ogreParent.inverse() * nodeParent;
                c.decomposition(cPos, cScale, cRot);
                haveSpaceChange =
                    !cPos.positionEquals(Ogre::Vector3::ZERO, 1e-5f) ||
                    !cRot.equals(Ogre::Quaternion::IDENTITY, Ogre::Radian(1e-4f)) ||
                    !cScale.positionEquals(Ogre::Vector3::UNIT_SCALE, 1e-4f);
            }
            }
        }
    }
    for (int c = 0; c < 3; ++c)
        if (std::abs(nodeBindScale[c]) < 1e-8f) nodeBindScale[c] = 1.0f;
    // Map a node-local key transform into the Ogre bind space (no-op for the
    // common consistent-bind case).
    auto mapPos = [&](const Ogre::Vector3& p) {
        return haveSpaceChange ? cRot * (cScale * p) + cPos : p;
    };
    auto mapRot = [&](const Ogre::Quaternion& q) {
        return haveSpaceChange ? cRot * q : q;
    };

    // Process the position keys.
    // Ogre applies translation keyframes in bone-local space (TS_LOCAL):
    //   mPosition += mOrientation * delta
    // So we must store the delta in bone-local space by pre-multiplying with the
    // inverse of the bone's T-pose orientation.
    for(auto i = 0u; i < nodeAnim->mNumPositionKeys; i++) {
        aiVectorKey positionKey = nodeAnim->mPositionKeys[i];
        Ogre::Vector3 position(positionKey.mValue.x, positionKey.mValue.y, positionKey.mValue.z);
        // Map into the Ogre bind space first (#936 re-rooted roots), then
        // compute the delta in parent-local space and rotate it bone-local.
        position = boneTPoseInverseRotation * (mapPos(position) - boneTPosePosition);

        keyframes[positionKey.mTime] = std::make_tuple(
            position,
            Ogre::Quaternion::IDENTITY,
            Ogre::Vector3::UNIT_SCALE
            );
    }

    // Process the rotation keys
    for(auto i = 0u; i < nodeAnim->mNumRotationKeys; i++) {
        aiQuatKey rotationKey = nodeAnim->mRotationKeys[i];
        Ogre::Quaternion rot(rotationKey.mValue.w, rotationKey.mValue.x, rotationKey.mValue.y, rotationKey.mValue.z);
        rot = boneTPoseInverseRotation * mapRot(rot); // node-local → Ogre bind space → bind-relative delta
        rot.normalise(); // Normalize the quaternion
        if (keyframes.find(rotationKey.mTime) == keyframes.end()) {
            keyframes[rotationKey.mTime] = std::make_tuple(
                Ogre::Vector3::ZERO,
                rot,
                Ogre::Vector3::UNIT_SCALE
                );
        } else {
            std::get<1>(keyframes[rotationKey.mTime]) = rot;
        }
    }

    // Process the scaling keys.
    // Ogre applies keyframe scale MULTIPLICATIVELY on top of the bind pose
    // (bones reset to bind each frame), so the key must hold the scale
    // RELATIVE to the T-pose — like position/rotation above. Blender-style
    // FBX rigs re-express the armature's static scale (e.g. ×100) in every
    // animation curve; storing it raw double-applied it (bind ×100 × key
    // ×100), blowing the skinned mesh up 100× and out of frame (#936).
    for(auto i = 0u; i < nodeAnim->mNumScalingKeys; i++) {
        aiVectorKey scalingKey = nodeAnim->mScalingKeys[i];
        Ogre::Vector3 scale(scalingKey.mValue.x, scalingKey.mValue.y, scalingKey.mValue.z);
        scale = scale / nodeBindScale;
        if (keyframes.find(scalingKey.mTime) == keyframes.end()) {
            keyframes[scalingKey.mTime] = std::make_tuple(
                Ogre::Vector3::ZERO,
                Ogre::Quaternion::IDENTITY,
                scale
                );
        } else {
            std::get<2>(keyframes[scalingKey.mTime]) = scale;
        }
    }

    // Now create the keyframes in the track
    for(auto& [time, transform] : keyframes) {
        Ogre::TransformKeyFrame* keyFrame = track->createNodeKeyFrame(time/mTicksPerSecond);
        keyFrame->setTranslate(std::get<0>(transform));
        keyFrame->setRotation(std::get<1>(transform));
        keyFrame->setScale(std::get<2>(transform));
    }
}
