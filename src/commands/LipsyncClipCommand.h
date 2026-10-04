#ifndef LIPSYNC_CLIP_COMMAND_H
#define LIPSYNC_CLIP_COMMAND_H

// Audio2Face (#1019): one undo step for a generated lipsync take.
//
// A take is thousands of weight keyframes, so it must be a single Ctrl+Z.
// `QUndoStack::beginMacro` alone does NOT achieve that — a macro only groups
// commands PUSHED while it is open, and the write path calls
// `MorphAnimationManager::writeWeightKeyOn` directly, so an empty macro is
// exactly as undoable as nothing at all.
//
// This is the MorphCommands / RecordMocapClipCommand shape, minus the head
// clip lipsync never writes, and deliberately NOT behind ENABLE_MOCAP: the
// whole point of the lipsync path is that animating from a file must not
// require the webcam stack.
//
// The frames are owned by the command so redo() can replay them; undo()
// restores the clip that was there before, keyframe for keyframe (which may
// be no clip at all — the common case).

#include <QString>
#include <QUndoCommand>

#include <memory>
#include <string>
#include <vector>

class LipsyncClipCommand : public QUndoCommand
{
public:
    /// One key time and the pose weights written at it. `poseIndex` is the
    /// mesh's own pose index, so the command never re-resolves names.
    struct Key {
        float time = 0.0f;
        std::vector<std::pair<unsigned short, float>> poseRefs;
    };

    LipsyncClipCommand(std::string entityName,
                       QString clipName,
                       std::vector<Key> keys,
                       QUndoCommand* parent = nullptr);
    ~LipsyncClipCommand() override;

    void undo() override;
    void redo() override;

    /// Keyframes actually written by the last redo().
    int keysWritten() const { return m_keysWritten; }

private:
    struct Snapshot;

    std::string m_entityName;
    QString m_clipName;
    std::vector<Key> m_keys;
    std::unique_ptr<Snapshot> m_before;
    bool m_snapshotTaken = false;
    int m_keysWritten = 0;
};

#endif  // LIPSYNC_CLIP_COMMAND_H
