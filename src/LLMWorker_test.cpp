#ifdef ENABLE_LOCAL_LLM

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include "LLMWorker.h"
#include <iostream>

class LLMWorkerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    QApplication* app = nullptr;
};

TEST_F(LLMWorkerTest, Constructor)
{
    LLMWorker worker;
    EXPECT_FALSE(worker.isModelLoaded());
    EXPECT_FALSE(worker.isGenerating());
    EXPECT_TRUE(worker.getLoadedModelPath().isEmpty());
}

TEST_F(LLMWorkerTest, DefaultSettings)
{
    LLMWorker worker;
    LLMSettings settings = worker.getSettings();
    EXPECT_EQ(settings.contextSize, 8192);   // raised from 4096 for the AI agent's tool-doc prompts (#1052)
    EXPECT_EQ(settings.maxTokens, 2048);
    EXPECT_FLOAT_EQ(settings.temperature, 0.7f);
    EXPECT_EQ(settings.gpuLayers, 99);
    EXPECT_EQ(settings.threads, 0);
    EXPECT_FLOAT_EQ(settings.topP, 0.9f);
    EXPECT_EQ(settings.topK, 40);
    EXPECT_FLOAT_EQ(settings.repeatPenalty, 1.1f);
}

TEST_F(LLMWorkerTest, SetSettings)
{
    LLMWorker worker;
    LLMSettings newSettings;
    newSettings.contextSize = 8192;
    newSettings.maxTokens = 4096;
    newSettings.temperature = 0.5f;
    newSettings.gpuLayers = 50;
    newSettings.threads = 4;
    newSettings.topP = 0.8f;
    newSettings.topK = 20;
    newSettings.repeatPenalty = 1.2f;

    worker.setSettings(newSettings);
    LLMSettings retrieved = worker.getSettings();

    EXPECT_EQ(retrieved.contextSize, 8192);
    EXPECT_EQ(retrieved.maxTokens, 4096);
    EXPECT_FLOAT_EQ(retrieved.temperature, 0.5f);
    EXPECT_EQ(retrieved.gpuLayers, 50);
    EXPECT_EQ(retrieved.threads, 4);
    EXPECT_FLOAT_EQ(retrieved.topP, 0.8f);
    EXPECT_EQ(retrieved.topK, 20);
    EXPECT_FLOAT_EQ(retrieved.repeatPenalty, 1.2f);
}

TEST_F(LLMWorkerTest, RequestStopWithoutGenerating)
{
    LLMWorker worker;
    // Should not crash when stopping without generating
    worker.requestStop();
    EXPECT_FALSE(worker.isGenerating());
}

TEST_F(LLMWorkerTest, UnloadModelWithoutLoading)
{
    LLMWorker worker;
    // Should not crash
    worker.unloadModel();
    EXPECT_FALSE(worker.isModelLoaded());
}

// NOTE: LoadModelInvalidPath and LoadModelEmptyPath tests were removed because
// loadModel() calls into llama.cpp/ggml which can SIGABRT on invalid paths
// (ggml assertion failure). These tests cannot work without a real model file.


// ---- Real-model check (opt-in): QTMESH_LLM_TEST_GGUF=/path/to/model.gguf ----
// Loads the GGUF through the real worker and asks for a JSON-only reply, the
// shape every agent call has. Pins the two failure modes the formatter exists
// to prevent: a thinking model spending the reply on <think>, and a model fed
// a foreign chat format rambling instead of answering.
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
TEST(LLMWorkerRealModel, AnswersJsonInItsOwnChatFormat)
{
    const QString path = qEnvironmentVariable("QTMESH_LLM_TEST_GGUF");
    if (path.isEmpty()) GTEST_SKIP() << "set QTMESH_LLM_TEST_GGUF to a .gguf to run";
    LLMWorker worker;
    worker.initBackend();
    LLMSettings st; st.contextSize = 2048; st.maxTokens = 160; st.temperature = 0.1f;
    worker.setSettings(st);
    ASSERT_TRUE(worker.loadModel(path));
    QString reply;
    QObject::connect(&worker, &LLMWorker::generationCompleted, [&](const QString& t) { reply = t; });
    QString err;
    QObject::connect(&worker, &LLMWorker::generationError, [&](const QString& e) { err = e; });
    QElapsedTimer timer; timer.start();
    worker.generate("You control a 3D editor. Reply with ONE JSON object and nothing else: "
                    "{\"command\": <tool name>, \"args\": {...}}. Tools: scale_object(factor).",
                    "make the selected mesh twice as large");
    std::cerr << "[real model] " << timer.elapsed() << " ms: " << reply.toStdString() << std::endl;
    ASSERT_TRUE(err.isEmpty()) << err.toStdString();
    EXPECT_FALSE(reply.contains("<think>")) << "thinking leaked into the reply";
    QString json = reply.trimmed();
    if (json.startsWith("```")) { json = json.section('\n', 1); json.truncate(json.lastIndexOf("```")); }
    const QJsonDocument doc = QJsonDocument::fromJson(json.trimmed().toUtf8());
    ASSERT_TRUE(doc.isObject()) << "not a JSON object: " << reply.toStdString();
    EXPECT_EQ(doc.object().value("command").toString(), QStringLiteral("scale_object"));
    worker.unloadModel();
}

#endif // ENABLE_LOCAL_LLM

// ---- Chat formats: each shipped family gets its own template ----

namespace {
// Fragments of the real embedded templates (enough for detection).
const QString kQwen35Tmpl = "{{- '<|im_start|>assistant\\n' }}{%- if enable_thinking is defined and enable_thinking is false %}";
const QString kQwen25Tmpl = "{{ '<|im_start|>' + message['role'] + '\\n' }}";
const QString kGemma4Tmpl = "{{- '<|turn>system\\n' -}}{%- set enable_thinking = enable_thinking | default(false) -%}";
const QString kGemma3Tmpl = "{{ '<start_of_turn>' + role + '\\n' }}";
const QString kLlama3Tmpl = "{{ '<|start_header_id|>' + message['role'] + '<|end_header_id|>' }}";
}

TEST(LLMWorkerPrompt, DetectsEveryShippedFamily)
{
    using F = LLMWorker::PromptFamily;
    EXPECT_EQ(LLMWorker::detectPromptFamily(kQwen35Tmpl), F::ChatML);
    EXPECT_EQ(LLMWorker::detectPromptFamily(kQwen25Tmpl), F::ChatML);
    EXPECT_EQ(LLMWorker::detectPromptFamily(kGemma4Tmpl), F::Gemma4);
    EXPECT_EQ(LLMWorker::detectPromptFamily(kGemma3Tmpl), F::Gemma);
    EXPECT_EQ(LLMWorker::detectPromptFamily(kLlama3Tmpl), F::Llama3);
    EXPECT_EQ(LLMWorker::detectPromptFamily(QString()), F::Unknown);
    EXPECT_EQ(LLMWorker::detectPromptFamily("[INST] {{ message }} [/INST]"), F::Unknown);
}

TEST(LLMWorkerPrompt, HybridQwenStartsWithAnEmptyThinkBlock)
{
    const QString p = LLMWorker::formatChatPrompt(LLMWorker::PromptFamily::ChatML, kQwen35Tmpl, "SYS", "USER");
    EXPECT_EQ(p, "<|im_start|>system\nSYS<|im_end|>\n<|im_start|>user\nUSER<|im_end|>\n"
                 "<|im_start|>assistant\n<think>\n\n</think>\n\n");
}

TEST(LLMWorkerPrompt, InstructOnlyQwenGetsNoThinkBlock)
{
    const QString p = LLMWorker::formatChatPrompt(LLMWorker::PromptFamily::ChatML, kQwen25Tmpl, "SYS", "USER");
    EXPECT_TRUE(p.endsWith("<|im_start|>assistant\n"));
    EXPECT_FALSE(p.contains("<think>"));
}

TEST(LLMWorkerPrompt, Gemma4UsesTurnMarkersAndNoThinkToken)
{
    const QString p = LLMWorker::formatChatPrompt(LLMWorker::PromptFamily::Gemma4, kGemma4Tmpl, "SYS", "USER");
    EXPECT_EQ(p, "<|turn>system\nSYS<turn|>\n<|turn>user\nUSER<turn|>\n<|turn>model\n");
    EXPECT_FALSE(p.contains("<|think|>"));
}

TEST(LLMWorkerPrompt, Gemma3FoldsSystemIntoTheUserTurn)
{
    const QString p = LLMWorker::formatChatPrompt(LLMWorker::PromptFamily::Gemma, kGemma3Tmpl, "SYS", "USER");
    EXPECT_EQ(p, "<start_of_turn>user\nSYS\n\nUSER<end_of_turn>\n<start_of_turn>model\n");
}

TEST(LLMWorkerPrompt, Llama3HeadersAndNoLiteralBos)
{
    const QString p = LLMWorker::formatChatPrompt(LLMWorker::PromptFamily::Llama3, kLlama3Tmpl, "SYS", "USER");
    EXPECT_TRUE(p.startsWith("<|start_header_id|>system<|end_header_id|>\n\nSYS<|eot_id|>"));
    EXPECT_TRUE(p.endsWith("<|start_header_id|>assistant<|end_header_id|>\n\n"));
    EXPECT_FALSE(p.contains("<|begin_of_text|>")) << "tokenize() adds BOS; a literal one doubles it";
}

TEST(LLMWorkerPrompt, UnknownFamilyReturnsEmptySoTheCallerFallsBack)
{
    EXPECT_TRUE(LLMWorker::formatChatPrompt(LLMWorker::PromptFamily::Unknown, "x", "S", "U").isEmpty());
}

TEST(LLMWorkerPrompt, StripReasoningRemovesThoughtBlocks)
{
    EXPECT_EQ(LLMWorker::stripReasoning("<think>\nplan\n</think>\n\n{\"a\":1}"), "\n\n{\"a\":1}");
    EXPECT_EQ(LLMWorker::stripReasoning("<|channel>thought\nhmm<channel|>{\"b\":2}"), "{\"b\":2}");
    EXPECT_EQ(LLMWorker::stripReasoning("ok <think>still going"), "ok ") << "unterminated = cut off by the token limit";
    EXPECT_EQ(LLMWorker::stripReasoning("{\"plain\":true}"), "{\"plain\":true}");
}
