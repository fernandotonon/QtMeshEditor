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

#include <QComboBox>

#include <QDebug>
#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreResourceManager.h>
#include <OgreMaterialManager.h>

#include "GlobalDefinitions.h"

#include "MaterialComboDelegate.h"

MaterialComboDelegate::MaterialComboDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}


QWidget *MaterialComboDelegate::createEditor(QWidget *parent,
    const QStyleOptionViewItem &/* option */,
    const QModelIndex &/* index */) const
{
    QComboBox *editor = new QComboBox(parent);

    QStringList materialList;

    Ogre::ResourceManager::ResourceMapIterator materialIterator = Ogre::MaterialManager::getSingleton().getResourceIterator();
    while (materialIterator.hasMoreElements())
    {
        materialList.append(Ogre::static_pointer_cast<Ogre::Material>(materialIterator.peekNextValue())->getName().data());
        materialIterator.moveNext();
    }

    editor->addItems(materialList);
    editor->setFrame(false);
    editor->setEditable(false);
    connect(editor, SIGNAL(currentIndexChanged(int)), this, SLOT(commitAndCloseEditor()));


    return editor;
}
void MaterialComboDelegate::commitAndCloseEditor()
{
    QWidget* signalSender = qobject_cast<QWidget*>(sender());
    if(signalSender)
    {
        emit commitData(signalSender);
        emit closeEditor(signalSender);
    }
}

void MaterialComboDelegate::setEditorData(QWidget *editor,
                                    const QModelIndex &index) const
{
    if(index.column()!=2)
        return QStyledItemDelegate::setEditorData(editor, index);

    QComboBox* box = static_cast<QComboBox*>(editor);
    box->setCurrentText(materialNameFromIndex(index));
}

void MaterialComboDelegate::setModelData(QWidget *editor, QAbstractItemModel *model,
                                   const QModelIndex &index) const
{
    if(index.column()!=2)
        return QStyledItemDelegate::setModelData(editor, model, index);

    QComboBox* box = static_cast<QComboBox*>(editor);

    QString value = box->currentText();
    model->setData(index, value, Qt::DisplayRole/*Qt::EditRole*/);

}

void MaterialComboDelegate::updateEditorGeometry(QWidget *editor,
    const QStyleOptionViewItem &option, const QModelIndex &/* index */) const
{
    editor->setGeometry(option.rect);
}

void MaterialComboDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    if(index.column()!=2)
        return QStyledItemDelegate::initStyleOption(option, index);

    option->text = materialNameFromIndex(index);

}

QString MaterialComboDelegate::materialNameFromIndex(const QModelIndex& index) const
{
    QModelIndex secondColumn = index.model()->index(index.row(),1);

    Ogre::SubEntity* subEntity = (Ogre::SubEntity*)index.model()->data(secondColumn , SUBENTITY_DATA).value<void *>();

    if(subEntity)
        return subEntity->getMaterialName().data();

    return QString();
}
