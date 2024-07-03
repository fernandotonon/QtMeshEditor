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

#include <QtDebug>

#include "EditorViewport.h"
#include "OgreWidget.h"
#include "mainwindow.h"

#include <Ogre.h>

#ifdef __gnu_linux__
    #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        #include <QX11Info>
    #endif
#endif

EditorViewport::EditorViewport(MainWindow* parent, int index)
    :QDockWidget(tr("Viewport ") + QString::number(index)), mIndex(index),
      m_pOgreWidget(nullptr), m_pMainWindow(parent)
{
    // Essential behavior of dock widget
    setAllowedAreas( Qt::AllDockWidgetAreas);
    setFeatures( DockWidgetClosable | DockWidgetMovable | DockWidgetFloatable);

    m_pOgreWidget = new OgreWidget(this);

    setWidget(m_pOgreWidget);
}

EditorViewport::~EditorViewport()
{
    if(m_pOgreWidget)
    {
        delete m_pOgreWidget;
        m_pOgreWidget = nullptr;
    }

    destroy();
}

int EditorViewport::getIndex() const
{   return mIndex;  }

OgreWidget* EditorViewport::getOgreWidget()   const
{   return m_pOgreWidget;   }

MainWindow*   EditorViewport::getMainWindow() const
{   return m_pMainWindow;   }

void EditorViewport::paintEvent(QPaintEvent *e)
{
    if (m_pOgreWidget->hasFocus())
    {
        setPalette(QPalette(QColor(0, 255, 127)));
        setAutoFillBackground(true);
    }
    else
    {
        setPalette(QGuiApplication::palette());
        setAutoFillBackground(true);
    }

    QDockWidget::paintEvent(e);
    //m_pOgreWidget->update();
    e->accept();
}

void EditorViewport::closeEvent(QCloseEvent *e)
{
    emit widgetAboutToClose(this); // Alert Main Window to rearrange other widget
    e->accept();
}
