#include <gtest/gtest.h>

#include "CharacterController.h"
#include "AnimationControlController.h"
#include "PropertiesPanelController.h"
#include "TestHelpers.h"
#include "ThemeManager.h"
#include "../ImageTo3D/ImageSourceController.h"

#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>

#include <memory>

using namespace CharacterGen;

TEST(CharacterShapeControls, ProportionsStartAtOneAreClampedAndReset)
{
    auto* c = CharacterController::instance();
    ASSERT_NE(c, nullptr);
    c->resetProportions();
    const QVariantList start = c->proportions();
    ASSERT_EQ(start.size(), 6);
    for (const QVariant& v : start) EXPECT_DOUBLE_EQ(v.toDouble(), 1.0);
    c->setProportion(0, 0.9);
    EXPECT_DOUBLE_EQ(c->proportions()[0].toDouble(), 0.9);
    c->setProportion(1, 9.0);
    EXPECT_DOUBLE_EQ(c->proportions()[1].toDouble(), 1.6);   // a body out of range is not a person
    c->setProportion(7, 1.2);                                // out-of-range index: ignored
    EXPECT_EQ(c->proportions().size(), 6);
    c->resetProportions();
    EXPECT_DOUBLE_EQ(c->proportions()[0].toDouble(), 1.0);
}

TEST(CharacterShapeControls, EverySliderHasALabelAndAHint)
{
    auto* c = CharacterController::instance();
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < c->proportionCount(); ++i) { EXPECT_FALSE(c->proportionLabel(i).isEmpty()); EXPECT_FALSE(c->proportionHint(i).isEmpty()); }
    for (int i = 0; i < c->measureCount(); ++i) { EXPECT_FALSE(c->measureLabel(i).isEmpty()); EXPECT_FALSE(c->measureHint(i).isEmpty()); EXPECT_GT(c->measureRange(i), 0.0); }
    EXPECT_TRUE(c->proportionLabel(99).isEmpty());
    EXPECT_TRUE(c->measureLabel(-1).isEmpty());
    // the 45 raw modes are named too (no "Head shape 3" placeholders): hand-named ones and measured fallbacks
    for (int m = 0; m < 45; ++m) EXPECT_FALSE(c->shapeLabel(m).isEmpty());
    EXPECT_EQ(c->shapeLabel(4), QStringLiteral("Waist width"));
    EXPECT_NE(c->shapeLabel(0), c->shapeLabel(1));
}

TEST(CharacterShapeControls, MeasureHelpersAreInertWithoutAssets)
{
    auto* c = CharacterController::instance();
    ASSERT_NE(c, nullptr);
    QVariantList coefficients;
    for (int i = 0; i < 45; ++i) coefficients << 0.0;
    // without the bundles the model cannot be built: offsets read 0 and the coefficients come back unchanged (when the bundles are present they are real numbers)
    const double offset = c->measureOffset(1, coefficients);
    EXPECT_TRUE(std::isfinite(offset));
    const QVariantList out = c->coefficientsWithMeasure(1, 2.0, coefficients);
    EXPECT_EQ(out.size(), 45);
    EXPECT_EQ(c->coefficientsWithMeasure(99, 2.0, coefficients), coefficients);
}

// The whole panel (it embeds the shared image picker through a Loader) must instantiate against the controllers without errors.
TEST(CharacterShapeControls, PanelQmlLoadsWithoutErrors)
{
    ASSERT_TRUE(tryInitOgre());   // PropertiesPanelController builds the TransformOperator, which needs the Manager
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ThemeManager::qmlInstance(e, s); });
    qmlRegisterSingletonType<AnimationControlController>("AnimationControl", 1, 0, "AnimationControlController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return AnimationControlController::qmlInstance(e, s); });
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return PropertiesPanelController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<CharacterController>("PropertiesPanel", 1, 0, "CharacterController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return CharacterController::qmlInstance(e, s); });
    qmlRegisterSingletonType<ImageSourceController>("PropertiesPanel", 1, 0, "ImageSourceController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ImageSourceController::create(e, s); });
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/AnimationControl/CharacterGeneratorPanel.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();
}
