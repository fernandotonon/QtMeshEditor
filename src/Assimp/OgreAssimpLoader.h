#ifndef __AssimpLoader_h__
#define __AssimpLoader_h__

#include <OgreMesh.h>
#include <OgrePlugin.h>
#include <OgreAssimpExports.h>

struct aiScene;
struct aiNode;
struct aiBone;
struct aiMesh;
struct aiMaterial;
struct aiAnimation;

template <typename TReal> class aiMatrix4x4t;
typedef aiMatrix4x4t<float> aiMatrix4x4;

namespace Assimp
{
class Importer;
}

using namespace Ogre;

class AssimpLoader
{
public:
    enum LoaderParams
    {
        // 3ds max exports the animation over a longer time frame than the animation actually plays for
        // this is a fix for that
        LP_CUT_ANIMATION_WHERE_NO_FURTHER_CHANGE = 1 << 0,

        // Quiet mode - don't output anything
        LP_QUIET_MODE = 1 << 1
    };

    struct Options
    {
        float animationSpeedModifier;
        int params;
        int postProcessSteps;
        String customAnimationName;
        float maxEdgeAngle;

        Options() : animationSpeedModifier(1), params(0), postProcessSteps(0), maxEdgeAngle(30) {}
    };

    AssimpLoader();
    virtual ~AssimpLoader();

    bool load(const String& source, Mesh* mesh, SkeletonPtr& skeletonPtr,
              const Options& options = Options());

    bool load(const DataStreamPtr& source, Mesh* mesh, SkeletonPtr& skeletonPtr,
              const Options& options = Options());

private:
    bool _load(const char* name, Assimp::Importer& importer, Mesh* mesh, SkeletonPtr& skeletonPtr,
               const Options& options);
    bool createSubMesh(const String& name, int index, const aiNode* pNode, const aiMesh* mesh,
                       const MaterialPtr& matptr, Mesh* mMesh, AxisAlignedBox& mAAB);
    void grabNodeNamesFromNode(const aiScene* mScene, const aiNode* pNode);
    void grabBoneNamesFromNode(const aiScene* mScene, const aiNode* pNode);
    void computeNodesDerivedTransform(const aiScene* mScene, const aiNode* pNode,
                                      const aiMatrix4x4& accTransform);
    void createBonesFromNode(const aiScene* mScene, const aiNode* pNode);
    void createBoneHiearchy(const aiScene* mScene, const aiNode* pNode);
    void loadDataFromNode(const aiScene* mScene, const aiNode* pNode, Mesh* mesh);
    void markAllChildNodesAsNeeded(const aiNode* pNode);
    void flagNodeAsNeeded(const char* name);
    bool isNodeNeeded(const char* name);
    void parseAnimation(const aiScene* mScene, int index, aiAnimation* anim);
    typedef std::map<String, bool> boneMapType;
    boneMapType boneMap;
    // aiNode* mSkeletonRootNode;
    int mLoaderParams;

    String mCustomAnimationName;

    typedef std::map<String, const aiNode*> BoneNodeMap;
    BoneNodeMap mBoneNodesByName;

    typedef std::map<String, const aiBone*> BoneMap;
    BoneMap mBonesByName;

    typedef std::map<String, Affine3> NodeTransformMap;
    NodeTransformMap mNodeDerivedTransformByName;

    SkeletonPtr mSkeleton;

    static int msBoneCount;

    bool mQuietMode;
    bool mUseIndexBuffer;
    Real mTicksPerSecond;
    Real mAnimationSpeedModifier;
};

#endif // __AssimpLoader_h__
