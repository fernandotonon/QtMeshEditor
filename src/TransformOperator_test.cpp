#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Ogre.h>
#include "TransformOperator.h"

// Test if getSingleton returns a valid pointer
TEST(TransformOperatorTest, GetSingleton) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NE(instance, nullptr);
}

// Test if getSingleton always returns the same instance
TEST(TransformOperatorTest, SingletonInstance) {
    TransformOperator* instance1 = TransformOperator::getSingleton();
    TransformOperator* instance2 = TransformOperator::getSingleton();
    EXPECT_EQ(instance1, instance2);
}

// Test if kill deletes the instance
TEST(TransformOperatorTest, Kill) {
    TransformOperator* instance = TransformOperator::getSingleton();
    TransformOperator::kill();
    TransformOperator* instance2 = TransformOperator::getSingleton();
    EXPECT_NE(instance, instance2);
}

// Test if setTransformState sets the state correctly
TEST(TransformOperatorTest, SetSelectionBoxColour) {
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setSelectionBoxColour(Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
    EXPECT_EQ(instance->getSelectionBoxColour(), Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
}

TEST(TransformOperatorTest, Swap) {
    int x = 1;
    int y = 2;
    TransformOperator::swap(x, y);
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 1);
}
