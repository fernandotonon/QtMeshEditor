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

#ifndef SUBENTITYHIGHLIGHT_H
#define SUBENTITYHIGHLIGHT_H

#include <QObject>
#include <QMap>
#include <QString>

namespace Ogre {
    class SubEntity;
}

/**
 * @brief Manages visual highlighting of selected sub-entities.
 *
 * When a sub-entity is selected, its material is replaced with a tinted
 * highlight version. The original material is stored and restored when
 * the sub-entity is deselected.
 */
class SubEntityHighlight : public QObject
{
    Q_OBJECT

public:
    static SubEntityHighlight* getSingleton();
    static void kill();

public slots:
    /// Called when sub-entity selection changes. Updates highlights accordingly.
    void onSubEntitySelectionChanged();

private:
    SubEntityHighlight();
    ~SubEntityHighlight() override = default;

    /// Apply highlight tint to a sub-entity's material.
    void applyHighlight(Ogre::SubEntity* sub);

    /// Remove highlight and restore original material.
    void removeHighlight(Ogre::SubEntity* sub);

    /// Create or retrieve the highlight version of a material.
    void ensureHighlightMaterial(const std::string& originalMatName);

    static SubEntityHighlight* m_pSingleton;

    /// Maps sub-entity pointer -> original material name (for restore on deselect)
    QMap<Ogre::SubEntity*, std::string> mOriginalMaterials;
};

#endif // SUBENTITYHIGHLIGHT_H
