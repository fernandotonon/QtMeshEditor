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

#include <QApplication>
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    a.setStyle(QStyleFactory::create("Fusion"));

    QPalette newPalette;
    newPalette.setColor(QPalette::Window,          QColor( 37,  37,  37));
    newPalette.setColor(QPalette::WindowText,      QColor(212, 212, 212));
    newPalette.setColor(QPalette::Base,            QColor( 60,  60,  60));
    newPalette.setColor(QPalette::AlternateBase,   QColor( 45,  45,  45));
    newPalette.setColor(QPalette::PlaceholderText, QColor(127, 127, 127));
    newPalette.setColor(QPalette::Text,            QColor(212, 212, 212));
    newPalette.setColor(QPalette::Link,            QColor(100, 100, 100));
    newPalette.setColor(QPalette::Button,          QColor( 45,  45,  45));
    newPalette.setColor(QPalette::ButtonText,      QColor(212, 212, 212));
    newPalette.setColor(QPalette::BrightText,      QColor(240, 240, 240));
    newPalette.setColor(QPalette::Highlight,       QColor( 38,  79, 120));
    newPalette.setColor(QPalette::HighlightedText, QColor(240, 240, 240));

    newPalette.setColor(QPalette::Light,           QColor( 60,  60,  60));
    newPalette.setColor(QPalette::Midlight,        QColor( 52,  52,  52));
    newPalette.setColor(QPalette::Dark,            QColor( 30,  30,  30) );
    newPalette.setColor(QPalette::Mid,             QColor( 37,  37,  37));
    newPalette.setColor(QPalette::Shadow,          QColor( 0,    0,   0));

    newPalette.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
    newPalette.setColor(QPalette::Disabled, QPalette::Window, QColor(100, 100, 100));
    newPalette.setColor(QPalette::Disabled, QPalette::Base, QColor(100, 100, 100));
    newPalette.setColor(QPalette::Disabled, QPalette::Button, QColor(100, 100, 100));

    a.setPalette(newPalette);

    MainWindow w;
    w.show();
    
    return a.exec();
}
