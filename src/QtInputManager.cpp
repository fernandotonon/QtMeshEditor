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

#include "QtInputManager.h"
#include <QObject>
#include <QDebug>

QtInputManager QtInputManager::mInstance;

void QtInputManager::AddKeyListener(QtKeyListener* listener)
{
    mKeyListeners.append(listener);
}

void QtInputManager::AddMouseListener(QtMouseListener* listener)
{
    mMouseListeners.append(listener);
}

void QtInputManager::RemoveKeyListener(QtKeyListener* listener)
{
    mRemovedKeyListeners.append(listener);
}

void QtInputManager::RemoveMouseListener(QtMouseListener* listener)
{
    mRemovedMouseListeners.append(listener);
}
void QtInputManager::keyPressEvent(QKeyEvent *event)
{
    foreach(QtKeyListener* l,mRemovedKeyListeners)
    {
        mKeyListeners.removeOne(l);
    }
    mRemovedKeyListeners.clear();
    foreach(QtKeyListener* l,mKeyListeners)
    {
        l->keyPressEvent(event);
    }
}

void QtInputManager::keyReleaseEvent(QKeyEvent *event)
{
    foreach(QtKeyListener* l,mRemovedKeyListeners)
    {
        mKeyListeners.removeOne(l);
    }
    mRemovedKeyListeners.clear();
    foreach(QtKeyListener* l,mKeyListeners)
    {
        l->keyReleaseEvent(event);
    }
}

void QtInputManager::mousePressEvent(QMouseEvent *event)
{
    foreach(QtMouseListener* l,mRemovedMouseListeners)
    {
        mMouseListeners.removeOne(l);
    }
    mRemovedMouseListeners.clear();
    foreach(QtMouseListener* l,mMouseListeners)
    {
        l->mousePressEvent(event);
    }
}

void QtInputManager::mouseReleaseEvent(QMouseEvent *event)
{
    foreach(QtMouseListener* l,mRemovedMouseListeners)
    {
        mMouseListeners.removeOne(l);
    }
    mRemovedMouseListeners.clear();
    foreach(QtMouseListener* l,mMouseListeners)
    {
        l->mouseReleaseEvent(event);
    }
}

void QtInputManager::mouseMoveEvent(QMouseEvent *event)
{
    foreach(QtMouseListener* l,mRemovedMouseListeners)
    {
        mMouseListeners.removeOne(l);
    }
    mRemovedMouseListeners.clear();
    foreach(QtMouseListener* l,mMouseListeners)
    {
        l->mouseMoveEvent(event);
    }
}

void QtInputManager::wheelEvent(QWheelEvent *event)
{
    foreach(QtMouseListener* l,mRemovedMouseListeners)
    {
        mMouseListeners.removeOne(l);
    }
    mRemovedMouseListeners.clear();
    foreach(QtMouseListener* l,mMouseListeners)
    {
        l->wheelEvent(event);
    }
}
