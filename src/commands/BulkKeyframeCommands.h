#ifndef BULK_KEYFRAME_COMMANDS_H
#define BULK_KEYFRAME_COMMANDS_H

#include <QUndoCommand>
#include <QString>
#include <QVector>
#include <string>

namespace Ogre {
    class SkeletonInstance;
    class NodeAnimationTrack;
}

/**
 * Move N keyframes (across one or more bone tracks) by a single shared
 * delta `dt`. Atomic: undo restores every member; redo re-applies the
 * shift. Each member is identified by `(boneName, originalTime)` — if
 * the underlying track loses or rebuilds the keyframe between push and
 * undo/redo, that member is silently skipped.
 *
 * The caller must validate clamping + collision with non-selected
 * keyframes before constructing the command. The command itself trusts
 * the input — it does not check bounds.
 */
class MoveKeyframesCommand : public QUndoCommand
{
public:
    struct Item {
        std::string boneName;
        float       originalTime;
    };

    MoveKeyframesCommand(std::string entityName,
                         std::string animationName,
                         QVector<Item> items,
                         float dt,
                         QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    bool shiftAll(float fromOffset, float toOffset);

    std::string   mEntityName;
    std::string   mAnimationName;
    QVector<Item> mItems;
    float         mDt;
};

/**
 * Paste a set of keyframes (deserialized from clipboard JSON) onto their
 * respective bone tracks at the supplied absolute times. Each entry is
 * `(boneName, time, translate, rotation, scale)`. Skips entries whose
 * target time already has a keyframe on that track (returns the count
 * actually pasted via `pastedCount()`).
 *
 * Undo removes every successfully pasted keyframe. Redo re-creates them.
 */
class PasteKeyframesCommand : public QUndoCommand
{
public:
    struct Entry {
        std::string boneName;
        float       time;
        // TRS values stored flat to avoid pulling Ogre headers into this header.
        float       tx, ty, tz;
        float       rw, rx, ry, rz;
        float       sx, sy, sz;
    };

    PasteKeyframesCommand(std::string entityName,
                          std::string animationName,
                          QVector<Entry> entries,
                          QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    /// Number of entries that were actually pasted on the most recent redo
    /// (the rest were skipped due to collisions). Caller can read this
    /// after push() to surface a "skipped N due to collisions" hint.
    int pastedCount() const { return mPastedCount; }

private:
    std::string    mEntityName;
    std::string    mAnimationName;
    QVector<Entry> mEntries;
    /// True for entries that were actually written on the last redo, so
    /// undo only removes the ones we created.
    QVector<bool>  mApplied;
    int            mPastedCount = 0;
};

#endif // BULK_KEYFRAME_COMMANDS_H
