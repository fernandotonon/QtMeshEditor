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

// Test performance of signal emissions
class SignalPerformanceTest : public MaterialEditorQMLPerformanceTest {};

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

// Benchmark specific operations
class BenchmarkTest : public MaterialEditorQMLPerformanceTest {};
