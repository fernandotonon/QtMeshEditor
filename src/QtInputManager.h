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

#ifndef QTINPUTMANAGER_H
#define QTINPUTMANAGER_H

#include "QtKeyListener.h"
#include "QtMouseListener.h"

class QtInputManager
{
public:
    static QtInputManager& getInstance(){return mInstance;}
    static QtInputManager* getInstancePtr(){return &mInstance;}

    void AddKeyListener(QtKeyListener* listener);
    void AddMouseListener(QtMouseListener* listener);
    void RemoveKeyListener(QtKeyListener* listener);
    void RemoveMouseListener(QtMouseListener* listener);

    void keyPressEvent(QKeyEvent *event);
    void keyReleaseEvent(QKeyEvent *event);

    void mousePressEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void wheelEvent(QWheelEvent *event);
private:
    QtInputManager();
    static QtInputManager mInstance;
    QList<QtKeyListener*> mKeyListeners;
    QList<QtMouseListener*> mMouseListeners;

    QList<QtKeyListener*> mRemovedKeyListeners;
    QList<QtMouseListener*> mRemovedMouseListeners;
};

#endif // QTINPUTMANAGER_H
