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
    // get the animation speed
    auto mTicksPerSecond = (Ogre::Real)((0 == animation->mTicksPerSecond) ? 24.0f : animation->mTicksPerSecond);
    // Create the animation
    Ogre::Animation* ogreAnimation = skeleton->createAnimation(animation->mName.C_Str(), animation->mDuration/mTicksPerSecond);
    // Process the animation channels
    for(auto i = 0u; i < animation->mNumChannels; i++) {
        aiNodeAnim* nodeAnim = animation->mChannels[i];
        processAnimationChannel(nodeAnim, ogreAnimation, scene, i, mTicksPerSecond);
    }
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
    if (scene && scene->mRootNode) {
        if (const aiNode* channelNode = scene->mRootNode->FindNode(nodeAnim->mNodeName)) {
            aiVector3D ns, np; aiQuaternion nr;
            channelNode->mTransformation.Decompose(ns, nr, np);
            nodeBindScale = Ogre::Vector3(ns.x, ns.y, ns.z);
        }
    }
    for (int c = 0; c < 3; ++c)
        if (std::abs(nodeBindScale[c]) < 1e-8f) nodeBindScale[c] = 1.0f;

    // Process the position keys.
    // Ogre applies translation keyframes in bone-local space (TS_LOCAL):
    //   mPosition += mOrientation * delta
    // So we must store the delta in bone-local space by pre-multiplying with the
    // inverse of the bone's T-pose orientation.
    for(auto i = 0u; i < nodeAnim->mNumPositionKeys; i++) {
        aiVectorKey positionKey = nodeAnim->mPositionKeys[i];
        Ogre::Vector3 position(positionKey.mValue.x, positionKey.mValue.y, positionKey.mValue.z);
        // Compute delta in parent-local space, then rotate into bone-local space
        position = boneTPoseInverseRotation * (position - boneTPosePosition);

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
        rot = boneTPoseInverseRotation * rot; // Convert from local space to model space
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
