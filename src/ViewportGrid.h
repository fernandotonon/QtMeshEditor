/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c) HogPog Team (www.hogpog.com.br)
///
/// The MIT License
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////*/

#ifndef VIEWPORT_GRID_H
#define VIEWPORT_GRID_H

#include <QObject>
#include <OgrePrerequisites.h>
#include <OgreBlendMode.h>
#include <Ogre.h>


namespace Ogre{
    class SceneNode;
    class ManualObject;
    class ColourValue;
}
class ViewportGrid : public QObject
{
    Q_OBJECT

public:
    ViewportGrid(const Ogre::ColourValue &colour = Ogre::ColourValue::White, const unsigned int scale = 10);
    ~ViewportGrid();

private:

    Ogre::ManualObject*     m_pGridLine;
    Ogre::SceneNode*        m_pGridLineNode;
    Ogre::ColourValue       mGridColor;
    unsigned int            mScale;
    Ogre::Real              mFade;
    Ogre::Vector3           mPosition;

    void createGrid(void);

public:
    // Accessors
    Ogre::uint32 getQueryFlags(void)            const;
    const Ogre::Real& getFading(void)           const;
    const Ogre::ColourValue& getColour (void)   const;
    const unsigned int getScale(void)           const;
    Ogre::Vector3 getPosition() const;

    // Mutators   
    void setQueryFlags(Ogre::uint32 flags = 0xFF);
    void setPosition(const Ogre::Vector3 &position);

public slots:
    void setVisible(bool visible = true);


};


#endif //VIEWPORT_GRID_H

