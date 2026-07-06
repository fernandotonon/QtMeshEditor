#include <gtest/gtest.h>

#include <QDir>
#include <QJsonObject>
#include <QTemporaryDir>

#include "GamificationEventQueue.h"

namespace {

GamificationEventQueue::Entry makeEntry(const QString& id,
                                        const QString& kind = QStringLiteral("feature"))
{
    GamificationEventQueue::Entry e;
    e.id = id;
    e.kind = kind;
    QJsonObject body;
    body.insert(QStringLiteral("id"), id);
    body.insert(QStringLiteral("feature"), QStringLiteral("retopo"));
    e.body = body;
    e.queuedAt = 1783000000000LL;
    return e;
}

}  // namespace

class GamificationEventQueueTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_dir.isValid());
        m_path = m_dir.filePath(QStringLiteral("queue.json"));
    }

    QTemporaryDir m_dir;
    QString m_path;
};

TEST_F(GamificationEventQueueTest, AppendPersistsAcrossInstances)
{
    {
        GamificationEventQueue queue(m_path);
        EXPECT_TRUE(queue.isEmpty());
        EXPECT_TRUE(queue.append(makeEntry(QStringLiteral("a"))));
        EXPECT_TRUE(queue.append(makeEntry(QStringLiteral("b"), QStringLiteral("operation"))));
        EXPECT_EQ(queue.size(), 2);
    }
    GamificationEventQueue reloaded(m_path);
    EXPECT_EQ(reloaded.size(), 2);
    const auto features = reloaded.peek(QStringLiteral("feature"), 10);
    ASSERT_EQ(features.size(), 1);
    EXPECT_EQ(features.first().id, QStringLiteral("a"));
    EXPECT_EQ(features.first().body.value(QStringLiteral("feature")).toString(),
              QStringLiteral("retopo"));
    EXPECT_EQ(reloaded.peek(QStringLiteral("operation"), 10).size(), 1);
    EXPECT_EQ(reloaded.peek(QString(), 10).size(), 2);
}

TEST_F(GamificationEventQueueTest, DuplicateIdsAreIgnored)
{
    GamificationEventQueue queue(m_path);
    EXPECT_TRUE(queue.append(makeEntry(QStringLiteral("a"))));
    EXPECT_TRUE(queue.append(makeEntry(QStringLiteral("a"))));
    EXPECT_EQ(queue.size(), 1);
}

TEST_F(GamificationEventQueueTest, InvalidEntriesRejected)
{
    GamificationEventQueue queue(m_path);
    EXPECT_FALSE(queue.append(makeEntry(QString())));
    EXPECT_FALSE(queue.append(makeEntry(QString(129, QLatin1Char('x')))));
    GamificationEventQueue::Entry noKind = makeEntry(QStringLiteral("k"));
    noKind.kind.clear();
    EXPECT_FALSE(queue.append(noKind));
    EXPECT_TRUE(queue.isEmpty());
}

TEST_F(GamificationEventQueueTest, AcknowledgeRemovesAndPersists)
{
    GamificationEventQueue queue(m_path);
    queue.append(makeEntry(QStringLiteral("a")));
    queue.append(makeEntry(QStringLiteral("b")));
    queue.append(makeEntry(QStringLiteral("c")));
    queue.acknowledge({QStringLiteral("a"), QStringLiteral("c"),
                       QStringLiteral("never-existed")});
    EXPECT_EQ(queue.size(), 1);
    GamificationEventQueue reloaded(m_path);
    ASSERT_EQ(reloaded.size(), 1);
    EXPECT_EQ(reloaded.peek(QString(), 10).first().id, QStringLiteral("b"));
}

TEST_F(GamificationEventQueueTest, CapacityEvictsOldestFifo)
{
    GamificationEventQueue queue(m_path, /*capacity=*/3);
    for (int i = 0; i < 5; ++i)
        queue.append(makeEntry(QStringLiteral("id%1").arg(i)));
    EXPECT_EQ(queue.size(), 3);
    EXPECT_EQ(queue.evictedCount(), 2);
    const auto entries = queue.peek(QString(), 10);
    EXPECT_EQ(entries.first().id, QStringLiteral("id2"));
    EXPECT_EQ(entries.last().id, QStringLiteral("id4"));
}

TEST_F(GamificationEventQueueTest, ClearDropsEverything)
{
    GamificationEventQueue queue(m_path);
    queue.append(makeEntry(QStringLiteral("a")));
    EXPECT_TRUE(queue.clear());
    EXPECT_TRUE(queue.isEmpty());
    GamificationEventQueue reloaded(m_path);
    EXPECT_TRUE(reloaded.isEmpty());
}

TEST_F(GamificationEventQueueTest, RemoveKindDropsOnlyThatKind)
{
    GamificationEventQueue queue(m_path);
    queue.append(makeEntry(QStringLiteral("f1")));
    queue.append(makeEntry(QStringLiteral("o1"), QStringLiteral("operation")));
    queue.append(makeEntry(QStringLiteral("f2")));
    EXPECT_TRUE(queue.removeKind(QStringLiteral("feature")));
    EXPECT_EQ(queue.size(), 1);
    EXPECT_EQ(queue.peek(QString(), 10).first().kind, QStringLiteral("operation"));
    GamificationEventQueue reloaded(m_path);
    EXPECT_EQ(reloaded.size(), 1);
}

TEST_F(GamificationEventQueueTest, StaleSnapshotCannotResurrectRemovedIds)
{
    // Process A holds entries in memory; process B acknowledges them on
    // disk. A's next append must not write the removed ids back.
    GamificationEventQueue a(m_path);
    a.append(makeEntry(QStringLiteral("stale")));

    GamificationEventQueue b(m_path);
    b.acknowledge({QStringLiteral("stale")});
    EXPECT_TRUE(b.isEmpty());

    b.append(makeEntry(QStringLiteral("fresh")));  // b writes; 'stale' stays gone
    GamificationEventQueue reloaded(m_path);
    ASSERT_EQ(reloaded.size(), 1);
    EXPECT_EQ(reloaded.peek(QString(), 10).first().id, QStringLiteral("fresh"));

    // Same guarantee within one instance after clear().
    GamificationEventQueue c(m_path);
    EXPECT_TRUE(c.clear());
    c.append(makeEntry(QStringLiteral("post-clear")));
    EXPECT_EQ(c.size(), 1);
    EXPECT_EQ(c.peek(QString(), 10).first().id, QStringLiteral("post-clear"));
}

TEST_F(GamificationEventQueueTest, OwnerRoundTripsThroughPersistence)
{
    {
        GamificationEventQueue queue(m_path);
        auto owned = makeEntry(QStringLiteral("owned"));
        owned.owner = QStringLiteral("ada");
        queue.append(owned);
        queue.append(makeEntry(QStringLiteral("unclaimed")));
    }
    GamificationEventQueue reloaded(m_path);
    const auto entries = reloaded.peek(QString(), 10);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries.first().owner, QStringLiteral("ada"));
    EXPECT_TRUE(entries.last().owner.isEmpty());
}

TEST_F(GamificationEventQueueTest, ConcurrentWritersMergeById)
{
    // Two queue instances over the same file (GUI + CLI process model).
    GamificationEventQueue a(m_path);
    GamificationEventQueue b(m_path);
    a.append(makeEntry(QStringLiteral("from-a")));
    b.append(makeEntry(QStringLiteral("from-b")));
    // b merged a's persisted entry during its own read-modify-write.
    EXPECT_EQ(b.size(), 2);
    GamificationEventQueue reloaded(m_path);
    EXPECT_EQ(reloaded.size(), 2);
}

TEST_F(GamificationEventQueueTest, EntryJsonRoundTrip)
{
    const auto e = makeEntry(QStringLiteral("round"), QStringLiteral("operation"));
    const auto back = GamificationEventQueue::entryFromJson(
        GamificationEventQueue::entryToJson(e));
    EXPECT_EQ(back.id, e.id);
    EXPECT_EQ(back.kind, e.kind);
    EXPECT_EQ(back.body, e.body);
    EXPECT_EQ(back.queuedAt, e.queuedAt);
}
