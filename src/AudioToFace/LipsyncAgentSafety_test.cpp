#include <gtest/gtest.h>
#include "AICapabilityRegistry.h"
#include <QJsonObject>

// Pin the two agent-safety classifications for generate_lipsync: it writes a
// file (so overwriting an existing one must prompt) and it mutates the scene
// (so it must NOT be treated as read-only, or the agent skips the undo macro).
//
// The overwrite protection comes from the `outputKey` SHAPE regex matching
// the argument NAME `output_path`, not from the `^generate_` writerTool
// prefix — mutation testing showed the test still passes with `generate_`
// removed from that prefix list, because writerTool only governs AMBIGUOUS
// keys (`path`, `file`, `file_path`). Naming the real mechanism here so a
// future rename of the argument is understood to be the thing that would
// break this, not a change to the tool-name list.
TEST(LipsyncAgentSafety, OverwritingAnExistingOutputIsFlagged)
{
    QJsonObject args;
    args["audio_path"] = "/tmp/x.wav";
    args["output_path"] = "/tmp/exists.glb";
    auto existsYes = [](const QString&) { return true; };
    auto existsNo  = [](const QString&) { return false; };

    EXPECT_FALSE(AICapabilityRegistry::destructiveReason(
        "generate_lipsync", args, existsYes).isEmpty())
        << "overwriting an existing export must require confirmation";
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason(
        "generate_lipsync", args, existsNo).isEmpty())
        << "writing a NEW file must not prompt";
}

// Demonstrates the mechanism named above: the protection follows the
// ARGUMENT NAME. A differently-named output argument would not be flagged,
// which is the real thing to preserve when touching this tool's schema.
TEST(LipsyncAgentSafety, ProtectionFollowsTheArgumentName)
{
    auto existsYes = [](const QString&) { return true; };
    QJsonObject good;
    good["output_path"] = "/tmp/exists.glb";
    QJsonObject renamed;
    renamed["destination_blob"] = "/tmp/exists.glb";

    EXPECT_FALSE(AICapabilityRegistry::destructiveReason(
        "generate_lipsync", good, existsYes).isEmpty());
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason(
        "generate_lipsync", renamed, existsYes).isEmpty())
        << "an output argument outside the outputKey shape is NOT protected — "
           "keep the schema's name in that family";
}

TEST(LipsyncAgentSafety, IsNotReadOnly)
{
    EXPECT_FALSE(AICapabilityRegistry::isReadOnly("generate_lipsync"))
        << "it writes keyframes, so the agent must wrap it in an undo macro";
}
