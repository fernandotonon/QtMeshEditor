// Saving the generated source image (user request): a prompt-generated image
// otherwise lives only in AppData under a timestamped name and is discarded
// after the mesh is built, so prompt-to-image was unusable on its own.
//
// These cases are the path handling, which is where the bugs live — the copy
// itself is QFile::copy. No GPU/ONNX needed.
#include <gtest/gtest.h>

#include "ImageTo3D/MeshGenController.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

namespace {

// Write a tiny real PNG to act as the "generated" source image.
bool writePng(const QString& path)
{
    QImage img(4, 4, QImage::Format_RGB888);
    img.fill(Qt::magenta);
    return img.save(path, "PNG");
}

} // namespace

TEST(MeshGenControllerSave, RefusesWhenThereIsNoSelectedImage)
{
    auto* c = MeshGenController::instance();
    ASSERT_NE(c, nullptr);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Nothing selected (a fresh controller, or after clearSelectedImage).
    if (c->selectedImagePath().isEmpty())
        EXPECT_FALSE(c->saveSelectedImageAs(dir.filePath("out.png")))
            << "no source → refuse rather than write an empty file";
}

TEST(MeshGenControllerSave, SuggestedFileNameIsFilesystemSafeAndAlwaysHasAnExtension)
{
    auto* c = MeshGenController::instance();
    ASSERT_NE(c, nullptr);
    const QString name = c->suggestedImageFileName();
    EXPECT_TRUE(name.endsWith(".png")) << name.toStdString();
    // A caption is a SENTENCE — it must not reach the filesystem verbatim.
    for (const QChar ch : name)
        EXPECT_TRUE(ch.isLetterOrNumber() || ch == '_' || ch == '-' || ch == '.')
            << "unsafe character in suggested name: " << name.toStdString();
    EXPECT_FALSE(name.isEmpty());
    EXPECT_LE(name.size(), 64) << "captions are long; the name must stay bounded";
}
