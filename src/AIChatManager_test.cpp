#include <gtest/gtest.h>
#include "AIChatManager.h"

// Which model opening the AI Chat loads (nothing is loaded at app startup).

TEST(AIChatManagerAutoLoad, NothingDownloadedPicksNothing)
{
    EXPECT_TRUE(AIChatManager::pickAutoLoadModel({}, "Qwen2.5-7B-Instruct-Q4_K_M").isEmpty());
}

TEST(AIChatManagerAutoLoad, LastUsedModelWins)
{
    const QStringList available{"gemma-3-1b-it-Q4_K_M", "Qwen2.5-7B-Instruct-Q4_K_M", "gemma-3-4b-it-Q4_K_M"};
    EXPECT_EQ(AIChatManager::pickAutoLoadModel(available, "gemma-3-4b-it-Q4_K_M"), "gemma-3-4b-it-Q4_K_M");
}

TEST(AIChatManagerAutoLoad, MissingLastUsedFallsBackToRecommendedAgentModel)
{
    // the last model was deleted: prefer one that can drive the agent over
    // whatever happens to sort first
    const QStringList available{"gemma-3-1b-it-Q4_K_M", "Qwen3-4B-Instruct-2507-Q4_K_M"};
    EXPECT_EQ(AIChatManager::pickAutoLoadModel(available, "deleted-model"), "Qwen3-4B-Instruct-2507-Q4_K_M");
    EXPECT_EQ(AIChatManager::pickAutoLoadModel(available, QString()), "Qwen3-4B-Instruct-2507-Q4_K_M");
}

TEST(AIChatManagerAutoLoad, NoRecommendedModelFallsBackToFirst)
{
    const QStringList available{"gemma-3-1b-it-Q4_K_M", "Llama-3.2-3B-Instruct-Q4_K_M"};
    EXPECT_EQ(AIChatManager::pickAutoLoadModel(available, QString()), "gemma-3-1b-it-Q4_K_M");
}

TEST(AIChatManagerAutoLoad, LastUsedMatchesByBaseName)
{
    // lastModel may have been stored from a loadModelFromPath (completeBaseName)
    const QStringList available{"other", "mymodel.gguf"};
    EXPECT_EQ(AIChatManager::pickAutoLoadModel(available, "mymodel"), "mymodel.gguf");
}

// ---- The panel itself: header picker + status banner ----

#include "AIAgentManager.h"
#include "LLMManager.h"
#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <memory>
#include <QSignalSpy>

namespace {
void registerChatPanelSingletons()
{
    static bool done = false;
    if (done) return;
    done = true;
    qmlRegisterSingletonType<AIChatManager>("AIChatPanel", 1, 0, "AIChatManager",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return AIChatManager::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<AIAgentManager>("AIChatPanel", 1, 0, "AIAgentManager",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return AIAgentManager::qmlInstance(e, nullptr); });
    // The real PropertiesPanelController needs a live Ogre scene; the panel
    // only reads theme colours from it.
    auto* theme = new QQmlPropertyMap(qApp);
    for (const char* key : {"panelColor", "headerColor", "textColor", "highlightColor",
                            "buttonColor", "borderColor", "accentColor", "inputColor"})
        theme->insert(QString::fromLatin1(key), QColor(Qt::gray));
    qmlRegisterSingletonInstance("PropertiesPanel", 1, 0, "PropertiesPanelController", theme);
}
}

TEST(AIChatPanelQml, PickerListsDownloadedModelsAndBannerExplainsState)
{
    registerChatPanelSingletons();
    auto* llm = LLMManager::instance();
    const QString originalDir = llm->modelsDirectory();
    const bool originalAuto = llm->autoLoadModel();
    llm->setAutoLoadModel(false);   // the dummy file must not be handed to llama.cpp

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    llm->setModelsDirectory(dir.path());

    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/AIChatPanel/AIChatPanel.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();

    auto* status = obj->findChild<QObject*>(QStringLiteral("modelStatus"));
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->property("visible").toBool());
    EXPECT_EQ(status->property("message").toString(), QStringLiteral("No AI model downloaded yet."));

    // A model appears (a download finished): the picker lists it, the banner
    // switches from "download" to "choose".
    QFile f(QDir(dir.path()).filePath(QStringLiteral("qtmesh-dummy.gguf")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("not a real model");
    f.close();
    AIChatManager::instance()->refreshModels();
    QCoreApplication::processEvents();
    EXPECT_EQ(AIChatManager::instance()->availableModels(), QStringList{QStringLiteral("qtmesh-dummy")});
    EXPECT_EQ(status->property("message").toString(), QStringLiteral("No model loaded."));

    auto* picker = obj->findChild<QObject*>(QStringLiteral("modelPicker"));
    ASSERT_NE(picker, nullptr) << "the header model picker exists";

    llm->setModelsDirectory(originalDir);
    llm->setAutoLoadModel(originalAuto);
}

TEST(AIChatManagerAutoLoad, OpeningTheChatWithAutoLoadOffLoadsNothing)
{
    auto* llm = LLMManager::instance();
    const bool originalAuto = llm->autoLoadModel();
    llm->setAutoLoadModel(false);
    QSignalSpy started(llm, &LLMManager::modelLoadStarted);
    AIChatManager::instance()->setChatOpen(true);
    AIChatManager::instance()->setChatOpen(false);
    EXPECT_EQ(started.count(), 0);
    llm->setAutoLoadModel(originalAuto);
}
