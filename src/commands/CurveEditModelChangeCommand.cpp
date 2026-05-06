#include "CurveEditModelChangeCommand.h"

#include "../CurveEditModel.h"

#include <QObject>
#include <QString>
#include <utility>

CurveEditModelChangeCommand::CurveEditModelChangeCommand(
        std::string skeleton, std::string animation,
        std::string bone, std::string channel, double time,
        double oldIn, double oldOut, int oldMode,
        double newIn, double newOut, int newMode,
        QUndoCommand* parent)
    : QUndoCommand(parent)
    , mSkeleton(std::move(skeleton))
    , mAnimation(std::move(animation))
    , mBone(std::move(bone))
    , mChannel(std::move(channel))
    , mTime(time)
    , mOldIn(oldIn), mOldOut(oldOut), mOldMode(oldMode)
    , mNewIn(newIn), mNewOut(newOut), mNewMode(newMode)
{
    setText(QObject::tr("Edit curve handle"));
}

void CurveEditModelChangeCommand::apply(double in, double out, int mode)
{
    auto* m = CurveEditModel::instance();
    // setTangents auto-promotes to Bezier — call setMode last so an
    // explicit Linear/Stepped/Auto undo target sticks.
    m->setTangents(QString::fromStdString(mSkeleton),
                   QString::fromStdString(mAnimation),
                   QString::fromStdString(mBone),
                   QString::fromStdString(mChannel),
                   mTime, in, out);
    m->setMode(QString::fromStdString(mSkeleton),
               QString::fromStdString(mAnimation),
               QString::fromStdString(mBone),
               QString::fromStdString(mChannel),
               mTime, mode);
}

void CurveEditModelChangeCommand::redo() { apply(mNewIn, mNewOut, mNewMode); }
void CurveEditModelChangeCommand::undo() { apply(mOldIn, mOldOut, mOldMode); }
