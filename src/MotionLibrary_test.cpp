#include <gtest/gtest.h>

#include "MotionLibrary.h"

#include <QByteArray>

// MotionLibrary's parsing + prompt-matching core is Ogre-free, so these run on
// any build with no GL/network (the download path is exercised separately).

namespace {
// Minimal valid library: 2 clips, each 2 frames x 22 joints (identity quats).
QByteArray miniLib()
{
    QByteArray pose = "[";
    for (int j = 0; j < 22; ++j) pose += (j ? ",[0,0,0,1]" : "[0,0,0,1]");
    pose += "]";
    const QByteArray frames = "[" + pose + "," + pose + "]";   // 2 frames
    QByteArray json = "{\"schema\":\"qtmesh-motion-library-v1\",\"fps\":30,";
    json += "\"joints\":[";
    const char* J[] = {"hip","abdomen","chest","neck","neck1","head","rcollar",
        "rshoulder","relbow","rhand","lcollar","lshoulder","lelbow","lhand",
        "rbuttock","rhip","rknee","rfoot","lbuttock","lhip","lknee","lfoot"};
    for (int j = 0; j < 22; ++j) { if (j) json += ","; json += "\""; json += J[j]; json += "\""; }
    json += "],\"clips\":[";
    json += "{\"action\":\"walk\",\"source\":\"CMU 02_01\",\"quats\":" + frames + "},";
    json += "{\"action\":\"run\",\"source\":\"CMU 02_03\",\"quats\":" + frames + "}";
    json += "]}";
    return json;
}
} // namespace

TEST(MotionLibrary, ParsesValidLibrary)
{
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(miniLib())) << lib.error().toStdString();
    EXPECT_TRUE(lib.isLoaded());
    EXPECT_EQ(lib.clipCount(), 2);
    EXPECT_EQ(lib.clip(0).action.toStdString(), "walk");
    EXPECT_EQ(lib.clip(0).frames, 2);
    EXPECT_EQ(lib.clip(0).quats.size(), 2u);
    EXPECT_EQ(lib.clip(0).quats[0].size(), 22u);   // 22 canonical joints
}

TEST(MotionLibrary, RejectsWrongSchema)
{
    MotionLibrary lib;
    EXPECT_FALSE(lib.loadFromJson("{\"schema\":\"nope\",\"clips\":[]}"));
    EXPECT_FALSE(lib.error().isEmpty());
}

TEST(MotionLibrary, RejectsWrongJointCount)
{
    // a clip frame with only 3 joints (not 22) must be rejected by the guard.
    QByteArray bad = "{\"schema\":\"qtmesh-motion-library-v1\",\"fps\":30,"
                     "\"joints\":[],\"clips\":[{\"action\":\"x\",\"quats\":"
                     "[[[0,0,0,1],[0,0,0,1],[0,0,0,1]]]}]}";
    MotionLibrary lib;
    EXPECT_FALSE(lib.loadFromJson(bad));
}

TEST(MotionLibrary, AcceptsV2AndParsesCmuRest)
{
    // schema v2 carries an optional 22-entry cmuRestWorld used by the retarget
    // change-of-basis. v1 (no rest) must still load (hasCmuRest()==false).
    MotionLibrary v1;
    ASSERT_TRUE(v1.loadFromJson(miniLib())) << v1.error().toStdString();
    EXPECT_FALSE(v1.hasCmuRest());
    EXPECT_TRUE(v1.cmuRestWorld().empty());

    QByteArray v2 = miniLib();
    v2.replace("qtmesh-motion-library-v1", "qtmesh-motion-library-v2");
    // inject a cmuRestWorld of 22 distinct-ish quats before the closing brace
    QByteArray rest = ",\"cmuRestWorld\":[";
    for (int j = 0; j < 22; ++j) rest += (j ? ",[0,0,0,1]" : "[0,0,0,1]");
    rest += "]}";
    v2.replace(v2.size() - 1, 1, rest);   // swap trailing '}' for ",cmuRestWorld...}"
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(v2)) << lib.error().toStdString();
    EXPECT_TRUE(lib.hasCmuRest());
    EXPECT_EQ(lib.cmuRestWorld().size(), 22u);
    EXPECT_EQ(lib.clipCount(), 2);   // clips still parse alongside the rest
}

TEST(MotionLibrary, IgnoresWrongSizedCmuRest)
{
    // a cmuRestWorld that isn't exactly 22 entries is ignored (hasCmuRest false),
    // but the library still loads — the retarget falls back to v1 transport.
    QByteArray v2 = miniLib();
    v2.replace("qtmesh-motion-library-v1", "qtmesh-motion-library-v2");
    v2.replace(v2.size() - 1, 1, ",\"cmuRestWorld\":[[0,0,0,1],[0,0,0,1]]}");
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(v2)) << lib.error().toStdString();
    EXPECT_FALSE(lib.hasCmuRest());
}

TEST(MotionLibrary, ParsesPerClipRestWorldAndRestDir)
{
    // v3 clips extracted by --dump-canonical carry per-clip restWorld (22
    // source-bind quats) + restDir (22 canonical bind bone directions) that
    // switch the retarget to the bind-referenced path. Absent → empty.
    MotionLibrary plain;
    ASSERT_TRUE(plain.loadFromJson(miniLib())) << plain.error().toStdString();
    EXPECT_TRUE(plain.clip(0).restWorld.empty());
    EXPECT_TRUE(plain.clip(0).restDir.empty());

    QByteArray rest = ",\"restWorld\":[";
    for (int j = 0; j < 22; ++j) rest += (j ? ",[0,0,0.5,0.866]" : "[0,0,0.5,0.866]");
    rest += "],\"restDir\":[";
    for (int j = 0; j < 22; ++j) rest += (j ? ",[0,1,0]" : "[0,1,0]");
    rest += "]";
    QByteArray v3 = miniLib();
    const int at = v3.indexOf("\"quats\"");             // first (walk) clip
    const int end = v3.indexOf("},", at);               // its closing brace
    ASSERT_GT(end, 0);
    v3.insert(end, rest);
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(v3)) << lib.error().toStdString();
    ASSERT_EQ(lib.clip(0).restWorld.size(), 22u);
    ASSERT_EQ(lib.clip(0).restDir.size(), 22u);
    EXPECT_NEAR(lib.clip(0).restWorld[3][2], 0.5f, 1e-6f);
    EXPECT_NEAR(lib.clip(0).restDir[3][1], 1.0f, 1e-6f);
    EXPECT_TRUE(lib.clip(1).restWorld.empty());   // only the walk clip got it

    // Wrong-sized arrays are ignored, clip still loads.
    QByteArray badSz = miniLib();
    badSz.insert(badSz.indexOf("},", badSz.indexOf("\"quats\"")),
                 ",\"restWorld\":[[0,0,0,1]],\"restDir\":[[0,1,0]]");
    MotionLibrary lib2;
    ASSERT_TRUE(lib2.loadFromJson(badSz)) << lib2.error().toStdString();
    EXPECT_TRUE(lib2.clip(0).restWorld.empty());
    EXPECT_TRUE(lib2.clip(0).restDir.empty());
    EXPECT_EQ(lib2.clipCount(), 2);
}

TEST(MotionLibrary, MatchesDirectActionWord)
{
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(miniLib()));
    QString action;
    EXPECT_EQ(lib.matchPrompt("walking confidently down the street", &action), 0);
    EXPECT_EQ(action.toStdString(), "walk");
    EXPECT_EQ(lib.matchPrompt("a slow run", &action), 1);
    EXPECT_EQ(action.toStdString(), "run");
}

TEST(MotionLibrary, MatchesSynonyms)
{
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(miniLib()));
    QString action;
    // "jog"/"sprint" → run (synonym map), since the lib has no "jog" clip.
    EXPECT_EQ(lib.matchPrompt("jogging in the park", &action), 1);
    EXPECT_EQ(action.toStdString(), "run");
    EXPECT_EQ(lib.matchPrompt("sprint to the finish", &action), 1);
}

TEST(MotionLibrary, UnknownPromptReturnsMinusOne)
{
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(miniLib()));
    EXPECT_EQ(lib.matchPrompt("fly to the moon"), -1);
}

TEST(MotionLibrary, ActionsListed)
{
    MotionLibrary lib;
    ASSERT_TRUE(lib.loadFromJson(miniLib()));
    auto a = lib.actions();
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].toStdString(), "walk");
}
