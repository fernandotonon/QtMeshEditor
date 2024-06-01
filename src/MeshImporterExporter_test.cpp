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

    QString result = MeshImporterExporter.formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterTest, ExportFileDialogFilter_ReturnsFilterString) {
    QString expected = "Ogre Mesh (*.mesh);;Ogre Mesh v1.10+(*.mesh);;Ogre Mesh v1.8+(*.mesh);;Ogre Mesh v1.7+(*.mesh);;Ogre Mesh v1.4+(*.mesh);;Ogre Mesh v1.0+(*.mesh);;Ogre XML (*.mesh.xml);;Collada (*.dae);;X (*.x);;STP (*.stp);;OBJ (*.obj);;OBJ without MTL (*.objnomtl);;STL (*.stl);;STL Binary (*.stlb);;PLY (*.ply);;PLY Binary (*.plyb);;3DS (*.3ds);;glTF 2.0 (*.gltf2);;glTF 2.0 Binary (*.glb2);;glTF 1.0 (*.gltf);;glTF 1.0 Binary (*.glb);;Assimp Binary (*.assbin)";

    QString result = MeshImporterExporter::exportFileDialogFilter();

    EXPECT_EQ(result, expected);
}
