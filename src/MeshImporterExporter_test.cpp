#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MeshImporterExporter.h"

TEST(MeshImporterExporterTest, FormatFileURI_ValidURIAndFormat_ReturnsFormattedURI) {
    QString uri = "/path/to/file.obj";
    QString format = "Ogre XML (*.mesh.xml)";
    QString expected = "/path/to/file.obj.mesh.xml";

    QString result = MeshImporterExporter::formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterTest, FormatFileURI_URIWithExtension_ReturnsURIWithoutChanges) {
    QString uri = "/path/to/file.mesh.xml";
    QString format = "Ogre XML (*.mesh.xml)";
    QString expected = "/path/to/file.mesh.xml";

    QString result = MeshImporterExporter::formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterTest, FormatFileURI_UnknownFormat_ReturnsURIWithoutChanges) {
    QString uri = "/path/to/file.obj";
    QString format = "Unknown Format";
    QString expected = "/path/to/file.obj";

    QString result = MeshImporterExporter::formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterTest, ExportFileDialogFilter_ReturnsFilterString) {
    QString expected = "3DS (*.3ds);;Assimp Binary (*.assbin);;Collada (*.dae);;OBJ (*.obj);;OBJ without MTL (*.objnomtl);;Ogre Mesh (*.mesh);;Ogre Mesh v1.0+(*.mesh);;Ogre Mesh v1.10+(*.mesh);;Ogre Mesh v1.4+(*.mesh);;Ogre Mesh v1.7+(*.mesh);;Ogre Mesh v1.8+(*.mesh);;Ogre XML (*.mesh.xml);;PLY (*.ply);;PLY Binary (*.plyb);;STL (*.stl);;STL Binary (*.stlb);;STP (*.stp);;X (*.x);;glTF 1.0 (*.gltf);;glTF 1.0 Binary (*.glb);;glTF 2.0 (*.gltf2);;glTF 2.0 Binary (*.glb2)";

    QString result = MeshImporterExporter::exportFileDialogFilter();

    EXPECT_EQ(result, expected);
}
