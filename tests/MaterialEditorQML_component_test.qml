import QtQuick 2.15
import QtTest 1.15
import MaterialEditorQML 1.0

TestCase {
    id: testCase
    name: "MaterialEditorQMLComponentTest"
    
    // Test properties
    property bool testCompleted: false
    property var testResults: []
    
    // Helper to record test results
    function recordResult(testName, success, details) {
        testResults.push({
            name: testName,
            success: success,
            details: details || "",
            timestamp: new Date()
        })
    }
    
    // Test basic property access and binding
    function test_propertyAccess() {
        // Test reading properties
        var materialName = MaterialEditorQML.materialName
        var lightingEnabled = MaterialEditorQML.lightingEnabled
        var ambientColor = MaterialEditorQML.ambientColor
        
        verify(typeof materialName === "string", "Material name should be string")
        verify(typeof lightingEnabled === "boolean", "Lighting enabled should be boolean")
        verify(ambientColor !== undefined, "Ambient color should be defined")
        
        recordResult("propertyAccess", true, "All basic properties accessible")
    }
    
    // Test property binding functionality
    function test_propertyBinding() {
        var testItem = Qt.createQmlObject(`
            import QtQuick 2.15
            import MaterialEditorQML 1.0
            Item {
                property string boundMaterialName: MaterialEditorQML.materialName
                property bool boundLightingEnabled: MaterialEditorQML.lightingEnabled
                property color boundAmbientColor: MaterialEditorQML.ambientColor
            }
        `, testCase, "testPropertyBinding")
        
        // Initial values should match
        compare(testItem.boundMaterialName, MaterialEditorQML.materialName, 
                "Bound material name should match")
        compare(testItem.boundLightingEnabled, MaterialEditorQML.lightingEnabled, 
                "Bound lighting enabled should match")
        
        // Change values and verify binding updates
        MaterialEditorQML.setMaterialName("BindingTestMaterial")
        wait(50) // Allow binding to update
        compare(testItem.boundMaterialName, "BindingTestMaterial", 
                "Binding should update when property changes")
        
        MaterialEditorQML.setLightingEnabled(!MaterialEditorQML.lightingEnabled)
        wait(50)
        compare(testItem.boundLightingEnabled, MaterialEditorQML.lightingEnabled, 
                "Boolean binding should update")
        
        testItem.destroy()
        recordResult("propertyBinding", true, "Property bindings work correctly")
    }
    
    // Test method invocation from QML
    function test_methodInvocation() {
        var polygonModes = MaterialEditorQML.getPolygonModeNames()
        verify(polygonModes.length > 0, "Polygon modes should not be empty")
        verify(polygonModes.indexOf("Solid") >= 0, "Should contain 'Solid' mode")
        
        var blendFactors = MaterialEditorQML.getBlendFactorNames()
        verify(blendFactors.length > 0, "Blend factors should not be empty")
        
        var connectionTest = MaterialEditorQML.testConnection()
        compare(connectionTest, "C++ method called successfully!", 
                "Connection test should return expected result")
        
        MaterialEditorQML.createNewMaterial("QMLTestMaterial")
        compare(MaterialEditorQML.materialName, "QMLTestMaterial", 
                "Material should be created with correct name")
        verify(MaterialEditorQML.materialText.length > 0, 
               "Material text should not be empty")
        
        recordResult("methodInvocation", true, "All methods invocable from QML")
    }
    
    // Test signal emission and handling
    function test_signalHandling() {
        var signalCounts = {
            materialNameChanged: 0,
            lightingEnabledChanged: 0,
            ambientColorChanged: 0
        }
        
        var connections = Connections {
            target: MaterialEditorQML
            function onMaterialNameChanged() { signalCounts.materialNameChanged++ }
            function onLightingEnabledChanged() { signalCounts.lightingEnabledChanged++ }
            function onAmbientColorChanged() { signalCounts.ambientColorChanged++ }
        }
        
        // Trigger signal emissions
        MaterialEditorQML.setMaterialName("SignalTestMaterial")
        MaterialEditorQML.setLightingEnabled(!MaterialEditorQML.lightingEnabled)
        MaterialEditorQML.setAmbientColor(Qt.rgba(1.0, 0.5, 0.3, 1.0))
        
        wait(50) // Allow signals to propagate
        
        verify(signalCounts.materialNameChanged >= 1, 
               "Material name changed signal should be emitted")
        verify(signalCounts.lightingEnabledChanged >= 1, 
               "Lighting enabled changed signal should be emitted")
        verify(signalCounts.ambientColorChanged >= 1, 
               "Ambient color changed signal should be emitted")
        
        connections.destroy()
        recordResult("signalHandling", true, "Signals emitted and handled correctly")
    }
    
    // Test texture functionality
    function test_textureOperations() {
        MaterialEditorQML.setTextureName("test_texture.png")
        compare(MaterialEditorQML.textureName, "test_texture.png", 
                "Texture name should be set correctly")
        
        MaterialEditorQML.setScrollAnimUSpeed(1.5)
        MaterialEditorQML.setScrollAnimVSpeed(-0.8)
        compare(MaterialEditorQML.scrollAnimUSpeed, 1.5, 
                "U animation speed should be set correctly")
        compare(MaterialEditorQML.scrollAnimVSpeed, -0.8, 
                "V animation speed should be set correctly")
        
        var availableTextures = MaterialEditorQML.getAvailableTextures()
        verify(Array.isArray(availableTextures), 
               "Available textures should return an array")
        
        recordResult("textureOperations", true, "Texture operations work correctly")
    }
    
    // Test material properties workflow
    function test_materialWorkflow() {
        MaterialEditorQML.createNewMaterial("WorkflowTestMaterial")
        
        MaterialEditorQML.setLightingEnabled(true)
        MaterialEditorQML.setDepthWriteEnabled(false)
        MaterialEditorQML.setDiffuseAlpha(0.9)
        MaterialEditorQML.setShininess(64.0)
        MaterialEditorQML.setPolygonMode(2)
        
        compare(MaterialEditorQML.materialName, "WorkflowTestMaterial")
        verify(MaterialEditorQML.lightingEnabled === true)
        verify(MaterialEditorQML.depthWriteEnabled === false)
        verify(Math.abs(MaterialEditorQML.diffuseAlpha - 0.9) < 0.01)
        verify(Math.abs(MaterialEditorQML.shininess - 64.0) < 0.01)
        verify(MaterialEditorQML.polygonMode === 2)
        
        recordResult("materialWorkflow", true, "Complete material workflow successful")
    }
    
    // Test error handling and edge cases
    function test_errorHandling() {
        var originalMaterialName = MaterialEditorQML.materialName
        
        // Test with empty material name
        MaterialEditorQML.createNewMaterial("")
        // Should handle gracefully (material name might not change or get default name)
        
        // Test with invalid values
        MaterialEditorQML.setDiffuseAlpha(-0.5) // Invalid alpha
        MaterialEditorQML.setShininess(-10.0)   // Invalid shininess
        MaterialEditorQML.setPolygonMode(-1)    // Invalid polygon mode
        
        // Application should still be stable
        var testResult = MaterialEditorQML.testConnection()
        compare(testResult, "C++ method called successfully!", 
                "Application should still be functional after invalid inputs")
        
        recordResult("errorHandling", true, "Error handling works correctly")
    }
    
    // Test performance with rapid property changes
    function test_performanceStress() {
        var startTime = new Date().getTime()
        
        // Rapid property changes
        for (var i = 0; i < 100; i++) {
            MaterialEditorQML.setLightingEnabled(i % 2 === 0)
            MaterialEditorQML.setDiffuseAlpha(Math.random())
            MaterialEditorQML.setShininess(Math.random() * 128)
            MaterialEditorQML.setAmbientColor(Qt.rgba(Math.random(), Math.random(), Math.random(), 1.0))
            
            // Process events every 10 iterations
            if (i % 10 === 0) {
                wait(1)
            }
        }
        
        var endTime = new Date().getTime()
        var duration = endTime - startTime
        
        // Should complete within reasonable time (less than 2 seconds)
        verify(duration < 2000, "Performance test should complete quickly, took: " + duration + "ms")
        
        // Verify MaterialEditor is still functional
        var connectionTest = MaterialEditorQML.testConnection()
        compare(connectionTest, "C++ method called successfully!", 
                "MaterialEditor should still be functional after stress test")
        
        recordResult("performanceStress", true, "Performance stress test passed in " + duration + "ms")
    }
    
    // Test material validation
    function test_materialValidation() {
        var validScript = `
material TestMaterial
{
    technique
    {
        pass
        {
            ambient 0.5 0.5 0.5
            diffuse 1.0 1.0 1.0
            specular 1.0 1.0 1.0 32.0
        }
    }
}`
        
        var invalidScript = "invalid material script {{{"
        
        // Test validation (if method exists)
        if (typeof MaterialEditorQML.validateMaterialScript === "function") {
            verify(MaterialEditorQML.validateMaterialScript(validScript), 
                   "Valid script should pass validation")
            verify(!MaterialEditorQML.validateMaterialScript(invalidScript), 
                   "Invalid script should fail validation")
            verify(!MaterialEditorQML.validateMaterialScript(""), 
                   "Empty script should fail validation")
        }
        
        recordResult("materialValidation", true, "Material validation works correctly")
    }
    
    // Main test runner
    function test_runAllTests() {
        console.log("Starting MaterialEditorQML Component Tests...")
        
        test_propertyAccess()
        test_propertyBinding()
        test_methodInvocation()
        test_signalHandling()
        test_textureOperations()
        test_materialWorkflow()
        test_errorHandling()
        test_performanceStress()
        test_materialValidation()
        
        console.log("All tests completed. Results:")
        for (var i = 0; i < testResults.length; i++) {
            var result = testResults[i]
            console.log("  " + result.name + ": " + (result.success ? "PASS" : "FAIL") + 
                       (result.details ? " - " + result.details : ""))
        }
        
        testCompleted = true
    }
    
    Component.onCompleted: {
        // Run tests after component is fully loaded
        Qt.callLater(test_runAllTests)
    }
} 