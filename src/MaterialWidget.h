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

#ifndef MATERIAL_WIDGET_H
#define MATERIAL_WIDGET_H

#include <QTableWidget>

namespace Ogre {
    class Entity;
    class SubEntity;
}


class MaterialWidget : public QTableWidget
{
    Q_OBJECT

public:
    explicit MaterialWidget(QWidget *parent = nullptr);
    virtual ~MaterialWidget() = default;

private:
    void populateTableWithEntities(const QList<Ogre::Entity*>& entities);
    void populateTableWithSubEntities(const QList<Ogre::SubEntity*>& subEntities);

private slots:
    void onNodeSelected();
    void onEntitySelected();
    void onSubEntitySelected();
    void onMaterialChanged(int row, int column);

};

#endif // MATERIAL_WIDGET_H
