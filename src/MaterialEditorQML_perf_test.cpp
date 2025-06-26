#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <chrono>
#include <vector>
#include "MaterialEditorQML.h"

class MaterialEditorQMLPerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure QApplication exists
        if (!QApplication::instance()) {
            int argc = 0;
            char* argv[] = { nullptr };
            app = std::make_unique<QApplication>(argc, argv);
        }
        
        // Create MaterialEditorQML instance
        editor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
        ASSERT_NE(editor, nullptr);
        
        // Create initial material
        editor->createNewMaterial("PerformanceTestMaterial");
    }

    void TearDown() override {
        editor = nullptr;
    }

private:
    std::unique_ptr<QApplication> app;

protected:
    MaterialEditorQML* editor;

    // Helper function to generate random colors
    QColor randomColor() {
        return QColor(
            QRandomGenerator::global()->bounded(256),
            QRandomGenerator::global()->bounded(256),
            QRandomGenerator::global()->bounded(256)
        );
    }

    // Helper function to generate random float in range
    float randomFloat(float min, float max) {
        return min + static_cast<float>(QRandomGenerator::global()->generateDouble()) * (max - min);
    }
};

// Test performance of basic property setting
class BasicPropertyPerformanceTest : public MaterialEditorQMLPerformanceTest {};

TEST_F(BasicPropertyPerformanceTest, MassivePropertyChanges) {
    const int iterations = 1000;
    QElapsedTimer timer;
    
    timer.start();
    
    for (int i = 0; i < iterations; ++i) {
        editor->setLightingEnabled(i % 2 == 0);
        editor->setDepthWriteEnabled(i % 3 == 0);
        editor->setDepthCheckEnabled(i % 5 == 0);
        editor->setDiffuseAlpha(randomFloat(0.0f, 1.0f));
        editor->setShininess(randomFloat(0.0f, 128.0f));
        editor->setPolygonMode(i % 3);
    }
    
    qint64 elapsed = timer.elapsed();
    
    // Should complete within reasonable time (less than 1 second for 1000 iterations)
    EXPECT_LT(elapsed, 1000);
    
    qDebug() << "BasicPropertyPerformanceTest: " << iterations 
             << " property changes completed in" << elapsed << "ms"
             << "(" << (double)elapsed / iterations << "ms per change)";
}

TEST_F(BasicPropertyPerformanceTest, ColorPropertyPerformance) {
    const int iterations = 500;
    QElapsedTimer timer;
    
    timer.start();
    
    for (int i = 0; i < iterations; ++i) {
        editor->setAmbientColor(randomColor());
        editor->setDiffuseColor(randomColor());
        editor->setSpecularColor(randomColor());
        editor->setEmissiveColor(randomColor());
    }
    
    qint64 elapsed = timer.elapsed();
    
    // Should complete within reasonable time
    EXPECT_LT(elapsed, 500);
    
    qDebug() << "ColorPropertyPerformance: " << iterations 
             << " color changes completed in" << elapsed << "ms"
             << "(" << (double)elapsed / iterations << "ms per change)";
}

// Test performance of signal emissions
class SignalPerformanceTest : public MaterialEditorQMLPerformanceTest {};

TEST_F(SignalPerformanceTest, SignalEmissionOverhead) {
    const int iterations = 500;
    
    // Connect multiple signal spies
    QSignalSpy materialNameSpy(editor, &MaterialEditorQML::materialNameChanged);
    QSignalSpy lightingSpy(editor, &MaterialEditorQML::lightingEnabledChanged);
    QSignalSpy ambientSpy(editor, &MaterialEditorQML::ambientColorChanged);
    QSignalSpy diffuseSpy(editor, &MaterialEditorQML::diffuseColorChanged);
    
    QElapsedTimer timer;
    timer.start();
    
    for (int i = 0; i < iterations; ++i) {
        editor->setMaterialName(QString("TestMaterial_%1").arg(i));
        editor->setLightingEnabled(i % 2 == 0);
        editor->setAmbientColor(randomColor());
        editor->setDiffuseColor(randomColor());
    }
    
    qint64 elapsed = timer.elapsed();
    
    // Verify all signals were emitted
    EXPECT_EQ(materialNameSpy.count(), iterations);
    EXPECT_EQ(ambientSpy.count(), iterations);
    EXPECT_EQ(diffuseSpy.count(), iterations);
    
    qDebug() << "SignalEmissionOverhead: " << iterations 
             << " property changes with signal monitoring completed in" << elapsed << "ms";
}

// Test performance of material creation and deletion
class MaterialLifecyclePerformanceTest : public MaterialEditorQMLPerformanceTest {};

TEST_F(MaterialLifecyclePerformanceTest, MaterialCreationSpeed) {
    const int iterations = 100;
    QElapsedTimer timer;
    
    timer.start();
    
    for (int i = 0; i < iterations; ++i) {
        QString materialName = QString("SpeedTestMaterial_%1").arg(i);
        editor->createNewMaterial(materialName);
        
        // Verify creation was successful
        EXPECT_EQ(editor->materialName(), materialName);
        EXPECT_FALSE(editor->materialText().isEmpty());
    }
    
    qint64 elapsed = timer.elapsed();
    
    // Should complete within reasonable time
    EXPECT_LT(elapsed, 2000); // Less than 2 seconds for 100 materials
    
    qDebug() << "MaterialCreationSpeed: " << iterations 
             << " materials created in" << elapsed << "ms"
             << "(" << (double)elapsed / iterations << "ms per material)";
}

// Test performance of texture operations
class TexturePerformanceTest : public MaterialEditorQMLPerformanceTest {};

TEST_F(TexturePerformanceTest, TextureNameChanges) {
    const int iterations = 500;
    QElapsedTimer timer;
    
    QStringList textureNames;
    for (int i = 0; i < iterations; ++i) {
        textureNames << QString("texture_%1.png").arg(i);
    }
    
    timer.start();
    
    for (const QString& textureName : textureNames) {
        editor->setTextureName(textureName);
        editor->setScrollAnimUSpeed(randomFloat(-2.0, 2.0));
        editor->setScrollAnimVSpeed(randomFloat(-2.0, 2.0));
    }
    
    qint64 elapsed = timer.elapsed();
    
    // Should complete within reasonable time
    EXPECT_LT(elapsed, 1000);
    
    qDebug() << "TextureNameChanges: " << iterations 
             << " texture operations completed in" << elapsed << "ms";
}

// Test performance of utility functions
class UtilityFunctionPerformanceTest : public MaterialEditorQMLPerformanceTest {};

TEST_F(UtilityFunctionPerformanceTest, EnumerationFunctions) {
    const int iterations = 1000;
    QElapsedTimer timer;
    
    timer.start();
    
    for (int i = 0; i < iterations; ++i) {
        QStringList polygonModes = editor->getPolygonModeNames();
        QStringList blendFactors = editor->getBlendFactorNames();
        QString connectionTest = editor->testConnection();
        
        // Verify they return valid data
        EXPECT_GT(polygonModes.size(), 0);
        EXPECT_GT(blendFactors.size(), 0);
        EXPECT_EQ(connectionTest, "C++ method called successfully!");
    }
    
    qint64 elapsed = timer.elapsed();
    
    // Should be very fast since these are typically static operations
    EXPECT_LT(elapsed, 200);
    
    qDebug() << "EnumerationFunctions: " << iterations 
             << " utility function calls completed in" << elapsed << "ms";
}

// Test memory usage and stability under stress
class StressTest : public MaterialEditorQMLPerformanceTest {};

TEST_F(StressTest, MemoryStabilityTest) {
    const int iterations = 1000;
    
    QElapsedTimer timer;
    timer.start();
    
    for (int i = 0; i < iterations; ++i) {
        // Create a new material every few iterations
        if (i % 50 == 0) {
            editor->createNewMaterial(QString("StressTestMaterial_%1").arg(i));
        }
        
        // Set multiple properties rapidly
        editor->setLightingEnabled(i % 2 == 0);
        editor->setAmbientColor(randomColor());
        editor->setDiffuseAlpha(randomFloat(0.0f, 1.0f));
        editor->setTextureName(QString("stress_texture_%1.png").arg(i % 100));
        
        // Process events occasionally
        if (i % 100 == 0) {
            QApplication::processEvents();
        }
    }
    
    qint64 elapsed = timer.elapsed();
    
    // Should complete without crashes
    EXPECT_LT(elapsed, 10000); // Less than 10 seconds
    
    // MaterialEditor should still be functional
    editor->setMaterialName("FinalStressTestMaterial");
    EXPECT_EQ(editor->materialName(), "FinalStressTestMaterial");
    
    qDebug() << "MemoryStabilityTest: " << iterations 
             << " iterations completed in" << elapsed << "ms";
}

// Benchmark specific operations
class BenchmarkTest : public MaterialEditorQMLPerformanceTest {};

TEST_F(BenchmarkTest, SingleOperationBenchmarks) {
    const int warmupIterations = 100;
    const int benchmarkIterations = 1000;
    
    // Warm up
    for (int i = 0; i < warmupIterations; ++i) {
        editor->setAmbientColor(randomColor());
    }
    
    // Benchmark color setting
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < benchmarkIterations; ++i) {
        editor->setAmbientColor(randomColor());
    }
    qint64 colorTime = timer.elapsed();
    
    // Benchmark float setting
    timer.restart();
    for (int i = 0; i < benchmarkIterations; ++i) {
        editor->setDiffuseAlpha(randomFloat(0.0f, 1.0f));
    }
    qint64 floatTime = timer.elapsed();
    
    // Benchmark bool setting
    timer.restart();
    for (int i = 0; i < benchmarkIterations; ++i) {
        editor->setLightingEnabled(i % 2 == 0);
    }
    qint64 boolTime = timer.elapsed();
    
    // Benchmark string setting
    timer.restart();
    for (int i = 0; i < benchmarkIterations; ++i) {
        editor->setTextureName(QString("benchmark_texture_%1.png").arg(i % 10));
    }
    qint64 stringTime = timer.elapsed();
    
    qDebug() << "SingleOperationBenchmarks (" << benchmarkIterations << " iterations):";
    qDebug() << "  Color setting:" << colorTime << "ms (" 
             << (double)colorTime / benchmarkIterations * 1000.0 << "μs per operation)";
    qDebug() << "  Float setting:" << floatTime << "ms (" 
             << (double)floatTime / benchmarkIterations * 1000.0 << "μs per operation)";
    qDebug() << "  Bool setting:" << boolTime << "ms (" 
             << (double)boolTime / benchmarkIterations * 1000.0 << "μs per operation)";
    qDebug() << "  String setting:" << stringTime << "ms (" 
             << (double)stringTime / benchmarkIterations * 1000.0 << "μs per operation)";
    
    // All operations should be reasonably fast
    EXPECT_LT(colorTime, 500);   // Less than 0.5ms per color change on average
    EXPECT_LT(floatTime, 200);   // Less than 0.2ms per float change on average
    EXPECT_LT(boolTime, 100);    // Less than 0.1ms per bool change on average
    EXPECT_LT(stringTime, 300);  // Less than 0.3ms per string change on average
} 