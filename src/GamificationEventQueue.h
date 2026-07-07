#ifndef GAMIFICATION_EVENT_QUEUE_H
#define GAMIFICATION_EVENT_QUEUE_H

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

/// Persistent, offline-tolerant queue of pending gamification events (#797).
///
/// Append-only JSON file in the app data dir; every entry carries a
/// client-generated idempotency id (also the server-side dedup key), so a
/// flush retried after a crash or a lost response can never double-award.
/// Bounded: oldest entries are FIFO-evicted past `capacity()` with a logged
/// warning (never silently). Cross-process safe (GUI + CLI can enqueue
/// concurrently) via a QLockFile around every read-modify-write; ids removed
/// by this instance are tombstoned so a stale in-memory snapshot can never
/// write them back.
///
/// Pure data — no network, no Ogre. GamificationManager owns the flush policy.
class GamificationEventQueue {
public:
    struct Entry {
        QString id;        ///< idempotency id (server dedup key), <=128 chars
        QString kind;      ///< "feature" | "operation"
        /// Cloud user slug active when the event was recorded; empty when it
        /// was queued logged-out ("unclaimed" — flushable by any account).
        /// Prevents user A's events from posting to user B's account after
        /// an account switch.
        QString owner;
        QJsonObject body;  ///< exact event object for the cloud endpoint
        qint64 queuedAt = 0;  ///< epoch ms (local bookkeeping only)
    };

    /// @p filePath JSON file backing the queue (created on first append).
    explicit GamificationEventQueue(const QString& filePath, int capacity = 500);

    QString filePath() const { return m_filePath; }
    int capacity() const { return m_capacity; }

    /// Appends one entry (reloading the file first so concurrent writers
    /// merge instead of clobbering). Returns false when the entry is invalid
    /// or the file could not be locked/written; the entry is kept in memory
    /// and retried on the next append/save.
    bool append(const Entry& entry);

    /// Up to @p maxCount oldest entries of @p kind ("" = any kind).
    QList<Entry> peek(const QString& kind, int maxCount) const;

    /// Removes acknowledged ids and persists. Safe to call with ids that are
    /// no longer present.
    void acknowledge(const QStringList& ids);

    /// Drops every pending entry (privacy "delete my data" / stream opt-out
    /// paths). Returns false when the persisted wipe failed (lock or write) —
    /// in-memory entries are kept in that case so the caller can retry.
    bool clear();

    /// Drops every pending entry of @p kind (stream opt-out). Returns false
    /// when the persisted removal failed.
    bool removeKind(const QString& kind);

    int size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }

    /// Number of entries FIFO-evicted over this instance's lifetime.
    int evictedCount() const { return m_evicted; }

    /// Re-reads the backing file (e.g. after another process appended).
    void reload();

    static QJsonObject entryToJson(const Entry& entry);
    static Entry entryFromJson(const QJsonObject& object);

private:
    bool withFileLock(const std::function<void()>& fn) const;
    void loadLocked();
    bool saveLocked() const;
    void mergeFromDisk();
    void enforceCapacity();
    void tombstone(const QSet<QString>& ids);

    QString m_filePath;
    int m_capacity = 500;
    QList<Entry> m_entries;
    /// Ids this instance removed (ack/clear/removeKind), excluded from every
    /// disk merge so removals survive concurrent writers' stale snapshots.
    QSet<QString> m_removedIds;
    int m_evicted = 0;
};

#endif  // GAMIFICATION_EVENT_QUEUE_H
