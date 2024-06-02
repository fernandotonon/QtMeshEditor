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

#ifndef EDITOR_VIEWPORT_H
#define EDITOR_VIEWPORT_H

#include <QtWidgets>

class OgreWidget;
class MainWindow;

class EditorViewport : public QDockWidget
{
    Q_OBJECT

public:
    EditorViewport(MainWindow* parent, int index = 0);
    ~EditorViewport();

    int           getIndex()      const;
    OgreWidget*   getOgreWidget() const;
    MainWindow*   getMainWindow() const;

signals:
    void widgetAboutToClose(EditorViewport* const& widget);

    //TODO add another function to do the job and put the events protected
public:

protected:
    virtual void closeEvent(QCloseEvent *e);
    virtual void paintEvent(QPaintEvent *e);

private:
    int                 mIndex;
    OgreWidget*         m_pOgreWidget;
    MainWindow*         m_pMainWindow;

};


#endif //EDITOR_VIEWPORT_H

