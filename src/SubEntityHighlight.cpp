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

#include "SubEntityHighlight.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QSet>
#include <Ogre.h>

SubEntityHighlight* SubEntityHighlight::m_pSingleton = nullptr;

SubEntityHighlight* SubEntityHighlight::getSingleton()
{
    if (!m_pSingleton)
        m_pSingleton = new SubEntityHighlight();
    return m_pSingleton;
}

void SubEntityHighlight::kill()
{
    if (m_pSingleton)
    {
        delete m_pSingleton;
        m_pSingleton = nullptr;
    }
}

SubEntityHighlight::SubEntityHighlight()
    : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::subEntitySelectionChanged,
            this, &SubEntityHighlight::onSubEntitySelectionChanged);
    // Also listen to general selection changes to clear highlights when
    // selection is moved to nodes or entities
    connect(SelectionSet::getSingleton(), &SelectionSet::nodeSelectionChanged,
            this, &SubEntityHighlight::onSubEntitySelectionChanged);
    connect(SelectionSet::getSingleton(), &SelectionSet::entitySelectionChanged,
            this, &SubEntityHighlight::onSubEntitySelectionChanged);
}

void SubEntityHighlight::onSubEntitySelectionChanged()
{
    auto* sel = SelectionSet::getSingleton();
    const auto& currentSubEntities = sel->getSubEntitiesSelectionList();

    // Build a set of currently selected sub-entities for quick lookup
    QSet<Ogre::SubEntity*> selectedSet(currentSubEntities.begin(), currentSubEntities.end());

    // Remove highlights from sub-entities no longer selected
    QList<Ogre::SubEntity*> toRemove;
    for (auto it = mOriginalMaterials.begin(); it != mOriginalMaterials.end(); ++it)
    {
        if (!selectedSet.contains(it.key()))
            toRemove.append(it.key());
    }
    for (Ogre::SubEntity* sub : toRemove)
        removeHighlight(sub);

    // Apply highlights to newly selected sub-entities
    for (Ogre::SubEntity* sub : currentSubEntities)
    {
        if (!mOriginalMaterials.contains(sub))
            applyHighlight(sub);
    }
}

void SubEntityHighlight::applyHighlight(Ogre::SubEntity* sub)
{
    if (!sub) return;

    std::string origName = sub->getMaterialName();
    mOriginalMaterials[sub] = origName;

    std::string highlightName = origName + "_SubMeshHighlight";
    ensureHighlightMaterial(origName);
    sub->setMaterialName(highlightName);

    SentryReporter::addBreadcrumb("ui.action",
        QString("Sub-entity highlight applied: %1").arg(QString::fromStdString(origName)));
}

void SubEntityHighlight::removeHighlight(Ogre::SubEntity* sub)
{
    if (!sub) return;

    auto it = mOriginalMaterials.find(sub);
    if (it == mOriginalMaterials.end()) return;

    sub->setMaterialName(it.value());
    mOriginalMaterials.erase(it);
}

void SubEntityHighlight::ensureHighlightMaterial(const std::string& originalMatName)
{
    std::string highlightName = originalMatName + "_SubMeshHighlight";

    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (matMgr.resourceExists(highlightName))
        return;

    // Clone the original material and add a highlight tint
    auto origMat = matMgr.getByName(originalMatName);
    if (!origMat)
    {
        // If original doesn't exist, create a simple highlight material
        auto mat = matMgr.create(highlightName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setEmissive(Ogre::ColourValue(0.15f, 0.35f, 0.55f, 1.0f));
        pass->setDiffuse(Ogre::ColourValue(0.6f, 0.7f, 0.9f, 1.0f));
        pass->setAmbient(Ogre::ColourValue(0.3f, 0.4f, 0.6f, 1.0f));
        return;
    }

    auto highlightMat = origMat->clone(highlightName);

    // Add a blue-ish emissive tint to all passes to visually distinguish
    for (unsigned short t = 0; t < highlightMat->getNumTechniques(); ++t)
    {
        auto* tech = highlightMat->getTechnique(t);
        for (unsigned short p = 0; p < tech->getNumPasses(); ++p)
        {
            auto* pass = tech->getPass(p);
            Ogre::ColourValue emissive = pass->getEmissive();
            emissive.r += 0.15f;
            emissive.g += 0.25f;
            emissive.b += 0.45f;
            pass->setEmissive(emissive);

            // Also slightly brighten diffuse to make it more obvious
            Ogre::ColourValue diffuse = pass->getDiffuse();
            diffuse.r = std::min(1.0f, diffuse.r + 0.05f);
            diffuse.g = std::min(1.0f, diffuse.g + 0.1f);
            diffuse.b = std::min(1.0f, diffuse.b + 0.2f);
            pass->setDiffuse(diffuse);
        }
    }
}
