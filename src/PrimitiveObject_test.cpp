#include <gtest/gtest.h>
#include "PrimitiveObject.h"

TEST(PrimitivesTest, CreateDefaultPrimitive)
{
    PrimitiveObject primitive("");
    ASSERT_EQ(primitive.getName(), "");
    ASSERT_EQ(primitive.getType(), PrimitiveObject::PrimitiveType::AP_NONE);
    ASSERT_EQ(primitive.getSceneNode(), nullptr);
    ASSERT_EQ(primitive.getNumIterations(), 1);
    ASSERT_EQ(primitive.getNumSegBase(), 1);
    ASSERT_EQ(primitive.getNumSegCircle(), 1);
    ASSERT_EQ(primitive.getNumSegX(), 1);
    ASSERT_EQ(primitive.getNumSegY(), 1);
    ASSERT_EQ(primitive.getNumSegZ(), 1);
    ASSERT_EQ(primitive.getInnerRadius(), 0.5f);
    ASSERT_EQ(primitive.getSectionRadius(), 0.5f);
    ASSERT_EQ(primitive.getChamferRadius(), 1.0f);
    ASSERT_EQ(primitive.getOuterRadius(), 1.0f);
    ASSERT_EQ(primitive.getRadius(), 1.0f);
    ASSERT_EQ(primitive.getHeight(), 1.0f);
    ASSERT_EQ(primitive.getSizeZ(), 1.0f);
    ASSERT_EQ(primitive.getSizeY(), 1.0f);
    ASSERT_EQ(primitive.getSizeX(), 1.0f);
}
