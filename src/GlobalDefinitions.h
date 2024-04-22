/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) HogPog Team (www.hogpog.com.br)

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

#ifndef GLOBAL_DEFINITIONS_H
#define GLOBAL_DEFINITIONS_H

#define ZORDER_OVERLAY Ogre::RENDER_QUEUE_OVERLAY

#define GUI_MATERIAL_NAME "GUI_Material"

#define GIZMO_QUERY_FLAGS       0x01
#define SCENE_QUERY_FLAGS       0xFE

#define GUI_VISIBILITY_FLAGS    0x01
#define SCENE_VISIBILITY_FLAGS  0xFE

#define TRANSFORM_OBJECT_NAME       "Transform"
#define SELECTIONBOX_OBJECT_NAME    "SelectionBox"

#define NODE_DATA       Qt::UserRole+1
#define ENTITY_DATA     Qt::UserRole+2
#define SUBENTITY_DATA  Qt::UserRole+3

#endif //GLOBAL_DEFINITIONS_H
