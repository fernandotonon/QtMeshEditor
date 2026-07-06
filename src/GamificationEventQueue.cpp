#include "GamificationEventQueue.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QtDebug>

namespace {
constexpr int kLockTimeoutMs = 1500;
constexpr int kMaxIdLength = 128;
}  // namespace

GamificationEventQueue::GamificationEventQueue(const QString& filePath, int capacity)
    : m_filePath(filePath)
    , m_capacity(qMax(1, capacity))
{
    withFileLock([this]() { loadLocked(); });
}

QJsonObject GamificationEventQueue::entryToJson(const Entry& entry)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), entry.id);
    o.insert(QStringLiteral("kind"), entry.kind);
    o.insert(QStringLiteral("body"), entry.body);
    o.insert(QStringLiteral("queuedAt"), static_cast<double>(entry.queuedAt));
    return o;
}

GamificationEventQueue::Entry GamificationEventQueue::entryFromJson(const QJsonObject& object)
{
    Entry e;
    e.id = object.value(QStringLiteral("id")).toString();
    e.kind = object.value(QStringLiteral("kind")).toString();
    e.body = object.value(QStringLiteral("body")).toObject();
    e.queuedAt = static_cast<qint64>(object.value(QStringLiteral("queuedAt")).toDouble());
    return e;
}

bool GamificationEventQueue::withFileLock(const std::function<void()>& fn) const
{
    // The lock file cannot be created in a missing directory (first run).
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QLockFile lock(m_filePath + QStringLiteral(".lock"));
    lock.setStaleLockTime(30 * 1000);
    if (!lock.tryLock(kLockTimeoutMs)) {
        qWarning() << "GamificationEventQueue: could not lock" << m_filePath;
        return false;
    }
    fn();
    return true;
}

void GamificationEventQueue::loadLocked()
{
    m_entries.clear();
    QFile file(m_filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;
    const QJsonArray events = doc.object().value(QStringLiteral("events")).toArray();
    QSet<QString> seen;
    for (const QJsonValue& v : events) {
        Entry e = entryFromJson(v.toObject());
        if (e.id.isEmpty() || e.kind.isEmpty() || seen.contains(e.id))
            continue;
        seen.insert(e.id);
        m_entries.append(e);
    }
}

bool GamificationEventQueue::saveLocked() const
{
    const QFileInfo info(m_filePath);
    QDir().mkpath(info.absolutePath());

    QJsonArray events;
    for (const Entry& e : m_entries)
        events.append(entryToJson(e));
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("events"), events);

    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "GamificationEventQueue: could not write" << m_filePath;
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return file.commit();
}

void GamificationEventQueue::mergeFromDisk()
{
    // Union of in-memory entries and whatever another process persisted,
    // keeping disk (older) entries first so eviction stays FIFO.
    const QList<Entry> mine = m_entries;
    loadLocked();
    QSet<QString> seen;
    for (const Entry& e : std::as_const(m_entries))
        seen.insert(e.id);
    for (const Entry& e : mine) {
        if (!seen.contains(e.id)) {
            seen.insert(e.id);
            m_entries.append(e);
        }
    }
}

void GamificationEventQueue::enforceCapacity()
{
    while (m_entries.size() > m_capacity) {
        const Entry dropped = m_entries.takeFirst();
        ++m_evicted;
        qWarning() << "GamificationEventQueue: capacity" << m_capacity
                   << "exceeded — evicting oldest event" << dropped.id;
    }
}

bool GamificationEventQueue::append(const Entry& entry)
{
    if (entry.id.isEmpty() || entry.id.size() > kMaxIdLength || entry.kind.isEmpty())
        return false;
    for (const Entry& e : std::as_const(m_entries)) {
        if (e.id == entry.id)
            return true;  // already queued
    }

    bool saved = false;
    const bool locked = withFileLock([this, &entry, &saved]() {
        mergeFromDisk();
        bool present = false;
        for (const Entry& e : std::as_const(m_entries)) {
            if (e.id == entry.id) {
                present = true;
                break;
            }
        }
        if (!present)
            m_entries.append(entry);
        enforceCapacity();
        saved = saveLocked();
    });
    if (!locked)
        m_entries.append(entry);  // keep in memory; retried on next append
    return locked && saved;
}

QList<GamificationEventQueue::Entry> GamificationEventQueue::peek(const QString& kind,
                                                                  int maxCount) const
{
    QList<Entry> out;
    for (const Entry& e : m_entries) {
        if (!kind.isEmpty() && e.kind != kind)
            continue;
        out.append(e);
        if (maxCount >= 0 && out.size() >= maxCount)
            break;
    }
    return out;
}

void GamificationEventQueue::acknowledge(const QStringList& ids)
{
    if (ids.isEmpty())
        return;
    const QSet<QString> acked(ids.cbegin(), ids.cend());
    withFileLock([this, &acked]() {
        mergeFromDisk();
        for (int i = m_entries.size() - 1; i >= 0; --i) {
            if (acked.contains(m_entries.at(i).id))
                m_entries.removeAt(i);
        }
        saveLocked();
    });
}

void GamificationEventQueue::clear()
{
    withFileLock([this]() {
        m_entries.clear();
        saveLocked();
    });
    m_entries.clear();
}

void GamificationEventQueue::reload()
{
    withFileLock([this]() { loadLocked(); });
}
