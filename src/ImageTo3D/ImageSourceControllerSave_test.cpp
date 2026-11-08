// Saving the generated source image (user request): a prompt-generated image
// otherwise lives only in AppData under a timestamped name and is discarded
// after the mesh is built, so prompt-to-image was unusable on its own.
//
// These cases are the path handling, which is where the bugs live — the copy
// itself is QFile::copy. No GPU/ONNX needed.
#include <gtest/gtest.h>

#include "ImageTo3D/ImageSourceController.h"

#include "AnimationControlController.h"
#include "PropertiesPanelController.h"
#include "TestHelpers.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>

#include <memory>

namespace {

// Write a tiny real PNG to act as the "generated" source image.
bool writePng(const QString& path)
{
    QImage img(4, 4, QImage::Format_RGB888);
    img.fill(Qt::magenta);
    return img.save(path, "PNG");
}

} // namespace

TEST(ImageSourceControllerSave, RefusesWhenThereIsNoSelectedImage)
{
    auto* c = ImageSourceController::instance();
    ASSERT_NE(c, nullptr);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Nothing selected (a fresh controller, or after clearSelectedImage).
    if (c->selectedImagePath().isEmpty())
        EXPECT_FALSE(c->saveSelectedImageAs(dir.filePath("out.png")))
            << "no source → refuse rather than write an empty file";
}

TEST(ImageSourceControllerSave, SuggestedFileNameIsFilesystemSafeAndAlwaysHasAnExtension)
{
    auto* c = ImageSourceController::instance();
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

// The shared picker is plain QML loaded from the qrc by BOTH the Image -> 3D section and the Character Generator: it must load and
// instantiate against the controller singletons without a single binding / type error.
TEST(ImageSourcePickerQml, LoadsWithoutErrors)
{
    ASSERT_TRUE(tryInitOgre());   // PropertiesPanelController builds the TransformOperator, which needs the Manager
    qmlRegisterSingletonType<AnimationControlController>("AnimationControl", 1, 0, "AnimationControlController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return AnimationControlController::qmlInstance(e, s); });
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return PropertiesPanelController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<ImageSourceController>("PropertiesPanel", 1, 0, "ImageSourceController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ImageSourceController::create(e, s); });
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/AnimationControl/ImageSourcePicker.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();
}

TEST(ImageSourceController, LockedWhileAnyConsumerHoldsIt)
{
    auto* c = ImageSourceController::instance();
    ASSERT_NE(c, nullptr);
    QObject a, b;
    EXPECT_FALSE(c->locked());
    c->setLocked(&a, true);
    c->setLocked(&b, true);
    EXPECT_TRUE(c->locked());
    c->setLocked(&a, false);
    EXPECT_TRUE(c->locked()) << "the second consumer still works on the image";
    c->clearSelectedImage();          // refused while locked (nothing selected, but it must not crash or unlock)
    EXPECT_TRUE(c->locked());
    c->setLocked(&b, false);
    EXPECT_FALSE(c->locked());
}

TEST(ImageSourceController, SetSelectedImageSelectsAndClears)
{
    auto* c = ImageSourceController::instance();
    ASSERT_NE(c, nullptr);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString png = dir.filePath("photo.png");
    ASSERT_TRUE(writePng(png));
    c->setSelectedImage(png);
    EXPECT_EQ(c->selectedImagePath(), png);
    EXPECT_FALSE(c->previewSource().isEmpty());
    EXPECT_TRUE(c->selectedImageUrl().startsWith(QStringLiteral("file:")));
    c->setSelectedImage(QString());
    EXPECT_TRUE(c->selectedImagePath().isEmpty());
    EXPECT_TRUE(c->previewSource().isEmpty());
}
