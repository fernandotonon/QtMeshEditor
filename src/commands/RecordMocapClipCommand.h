#ifndef RECORD_MOCAP_CLIP_COMMAND_H
#define RECORD_MOCAP_CLIP_COMMAND_H

// Undoable wrapper around MocapRecorder::recordFace (epic #869, Slice D
// #873): a recorded take — potentially thousands of weight keyframes plus a
// head clip — is ONE undo step.
//
// First redo() snapshots the pre-existing clips (the mesh VAT_POSE weight
// clip, the "<clip>_Head" skeletal clip, and the scene-level node clip, each
// of which may or may not exist) and runs the recorder with the samples the
// command owns. undo() removes the recorded clips and restores the snapshots
// keyframe-for-keyframe. Subsequent redo()s re-run the recorder on the same
// inputs (deterministic).
//
// Only compiled under ENABLE_MOCAP (the recorder doesn't exist otherwise).

#ifdef ENABLE_MOCAP

#include <QUndoCommand>

#include "../Mocap/FaceCapMapper.h"
#include "../Mocap/MocapRecorder.h"
#include "../Mocap/MocapLiveTypes.h"

#include <string>
#include <vector>

class RecordMocapClipCommand : public QUndoCommand
{
public:
    RecordMocapClipCommand(std::string entityName,
                           std::vector<FaceSample> samples,
                           FaceCapMapper::Mapping mapping,
                           MocapRecorder::FaceRecordOptions options,
                           QUndoCommand* parent = nullptr);
    ~RecordMocapClipCommand() override;

    void undo() override;
    void redo() override;

    const MocapRecorder::FaceRecordReport& report() const { return m_report; }

private:
    struct Snapshots;

    std::string m_entityName;
    std::vector<FaceSample> m_samples;
    FaceCapMapper::Mapping m_mapping;
    MocapRecorder::FaceRecordOptions m_options;
    MocapRecorder::FaceRecordReport m_report;
    std::unique_ptr<Snapshots> m_before;
    bool m_snapshotTaken = false;
};

// Body-take variant (Slice E #874): one undo step around
// MocapRecorder::recordBody. Snapshots the pre-existing skeletal clip.
class RecordBodyClipCommand : public QUndoCommand
{
public:
    RecordBodyClipCommand(std::string entityName,
                          std::vector<std::vector<std::array<float, 4>>> clipQuats,
                          int fps, MocapRecorder::BodyRecordOptions options,
                          QUndoCommand* parent = nullptr);
    RecordBodyClipCommand(std::string entityName,
                          std::vector<BodyLiveFrame> liveFrames,
                          int fps, MocapRecorder::BodyRecordOptions options,
                          QUndoCommand* parent = nullptr);
    ~RecordBodyClipCommand() override;

    void undo() override;
    void redo() override;

    const MocapRecorder::BodyRecordReport& report() const { return m_report; }

private:
    struct Snapshot;

    std::string m_entityName;
    std::vector<std::vector<std::array<float, 4>>> m_clipQuats;
    std::vector<BodyLiveFrame> m_liveFrames;
    int m_fps;
    MocapRecorder::BodyRecordOptions m_options;
    MocapRecorder::BodyRecordReport m_report;
    std::unique_ptr<Snapshot> m_before;
    bool m_snapshotTaken = false;
};

#endif  // ENABLE_MOCAP
#endif  // RECORD_MOCAP_CLIP_COMMAND_H
