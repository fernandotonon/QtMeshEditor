#ifndef BONEWEIGHTOVERLAY_H
#define BONEWEIGHTOVERLAY_H

#include <Ogre.h>
#include <OgreSubEntity.h>
#include <QObject>
#include <QTimer>
#include <map>
#include <vector>

class BoneWeightOverlay : public QObject
{
    Q_OBJECT
public:
    BoneWeightOverlay(Ogre::Entity* entity, Ogre::SceneManager* sceneMgr);
    ~BoneWeightOverlay() override;

    void setSelectedBone(unsigned short boneIndex);
    void setVisible(bool visible);
    bool isVisible() const { return mVisible; }

    /// Rebuild heat-map geometry after skeleton / mesh skin changes.
    void rebuildVisuals();
    /// Show/hide the per-vertex dots drawn on top of the heat map.
    void setShowVertices(bool show);
    bool showVertices() const { return mShowVertices; }
    /// Re-read weights and restamp colours, reusing the existing geometry.
    /// Cheap enough to call per dab during a weight-paint stroke, unlike
    /// rebuildVisuals() which destroys and recreates the ManualObject.
    void refreshColours();

    static Ogre::ColourValue weightToColor(float weight);

    /// Cached per-vertex heat-map colours, one entry per emitted section.
    /// Exposed so tests can assert that a weight edit actually restamps the
    /// colours (the thing refreshColours() exists to do) rather than merely
    /// checking that it does not crash.
    const std::vector<std::vector<Ogre::ColourValue>>& sectionColours() const
    { return mSectionColours; }

private:
    void buildOverlay();
    void createMaterial();
    void destroyOverlay();
    void pollBoneSelection();
    std::map<size_t, float> buildWeightMap(const Ogre::Mesh* mesh,
                                           const Ogre::SubMesh* submesh) const;
    void updateOverlayPositions();
    void createVertexMaterial();
    void destroyVertexOverlay();
    /// Rebuild the vertex dots from the current skinned positions.
    void updateVertexOverlay();

    Ogre::Entity* mEntity;
    Ogre::SceneManager* mSceneMgr;
    Ogre::ManualObject* mOverlay = nullptr;
    Ogre::MaterialPtr mMaterial;
    /// Vertex dots live in their OWN ManualObject rather than as extra sections
    /// of mOverlay: updateOverlayPositions() addresses sections by POSITIONAL
    /// index against the cached colour/index vectors, so interleaving a second
    /// section per vertex block would shift every index and require threading a
    /// stride through that walk. A separate object also lets the dots use a
    /// depth-TESTED material while the heat map stays depth-off.
    Ogre::ManualObject* mVertexOverlay = nullptr;
    Ogre::MaterialPtr mVertexMaterial;
    /// Larger dark point drawn under each coloured dot to act as its border.
    Ogre::MaterialPtr mVertexHaloMaterial;
    /// Flat colour for vertices NOT weighted to the selected bone (no halo).
    Ogre::MaterialPtr mVertexPlainMaterial;
    static constexpr float kDotFillSize  = 9.0f;
    static constexpr float kDotHaloSize  = 13.0f;
    /// Slightly smaller than a connected dot so the painted bone stands out.
    static constexpr float kDotPlainSize = 6.0f;
    bool mShowVertices = false;
    unsigned short mBoneIndex = 0;
    bool mVisible = false;
    bool mSoftwareAnimRequested = false;
    // Per-section cached data for efficient frame updates
    std::vector<std::vector<Ogre::ColourValue>> mSectionColours;
    std::vector<std::vector<uint32_t>> mSectionIndices;
    QTimer mUpdateTimer;
};

#endif // BONEWEIGHTOVERLAY_H
