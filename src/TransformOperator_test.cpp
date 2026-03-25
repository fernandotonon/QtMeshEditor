#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Ogre.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TransformOperator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "GlobalDefinitions.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include <QSignalSpy>
#include "TestHelpers.h"

// Swap test doesn't need Manager, so it can be standalone
TEST(TransformOperatorTest, Swap) {
    int x = 1;
    int y = 2;
    TransformOperator::swap(x, y);
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 1);
}
