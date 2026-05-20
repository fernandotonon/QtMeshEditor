#include "PsxJoypadBindings.h"
#include "PsxJoypadState.h"

#include <QSettings>

namespace {

constexpr auto kSettingsGroup = "ps1Rip";
constexpr auto kBindingsKey = "joypadKeys";

QHash<unsigned, Qt::Key> &mutableBindings()
{
    static QHash<unsigned, Qt::Key> table;
    return table;
}

void setDefault(unsigned button, Qt::Key key)
{
    mutableBindings().insert(button, key);
}

} // namespace

QString PsxJoypadBindings::buttonLabel(unsigned buttonId)
{
    switch (buttonId) {
    case PsxJoypadButton::B:
        return QObject::tr("Cross (✕)");
    case PsxJoypadButton::A:
        return QObject::tr("Circle (○)");
    case PsxJoypadButton::X:
        return QObject::tr("Triangle (△)");
    case PsxJoypadButton::Y:
        return QObject::tr("Square (□)");
    case PsxJoypadButton::Select:
        return QObject::tr("Select");
    case PsxJoypadButton::Start:
        return QObject::tr("Start");
    case PsxJoypadButton::Up:
        return QObject::tr("D-pad Up");
    case PsxJoypadButton::Down:
        return QObject::tr("D-pad Down");
    case PsxJoypadButton::Left:
        return QObject::tr("D-pad Left");
    case PsxJoypadButton::Right:
        return QObject::tr("D-pad Right");
    case PsxJoypadButton::L:
        return QObject::tr("L1");
    case PsxJoypadButton::R:
        return QObject::tr("R1");
    case PsxJoypadButton::L2:
        return QObject::tr("L2");
    case PsxJoypadButton::R2:
        return QObject::tr("R2");
    case PsxJoypadButton::L3:
        return QObject::tr("L3");
    case PsxJoypadButton::R3:
        return QObject::tr("R3");
    default:
        return QObject::tr("Button %1").arg(buttonId);
    }
}

QStringList PsxJoypadBindings::allButtonIds()
{
    QStringList ids;
    for (unsigned i = 0; i < PsxJoypadState::kButtonCount; ++i)
        ids.append(QString::number(i));
    return ids;
}

void PsxJoypadBindings::resetToDefaults()
{
    mutableBindings().clear();
    setDefault(PsxJoypadButton::Up, Qt::Key_Up);
    setDefault(PsxJoypadButton::Down, Qt::Key_Down);
    setDefault(PsxJoypadButton::Left, Qt::Key_Left);
    setDefault(PsxJoypadButton::Right, Qt::Key_Right);
    setDefault(PsxJoypadButton::B, Qt::Key_X);
    setDefault(PsxJoypadButton::A, Qt::Key_Z);
    setDefault(PsxJoypadButton::X, Qt::Key_S);
    setDefault(PsxJoypadButton::Y, Qt::Key_D);
    setDefault(PsxJoypadButton::Start, Qt::Key_Return);
    setDefault(PsxJoypadButton::Select, Qt::Key_Shift);
}

void PsxJoypadBindings::load()
{
    resetToDefaults();
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    const int count = settings.beginReadArray(QString::fromLatin1(kBindingsKey));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const unsigned button = settings.value(QStringLiteral("button")).toUInt();
        const int key = settings.value(QStringLiteral("key")).toInt();
        if (button < PsxJoypadState::kButtonCount && key != 0)
            mutableBindings().insert(button, static_cast<Qt::Key>(key));
    }
    settings.endArray();
    settings.endGroup();
}

void PsxJoypadBindings::save()
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    settings.beginWriteArray(QString::fromLatin1(kBindingsKey));
    int index = 0;
    for (auto it = mutableBindings().constBegin(); it != mutableBindings().constEnd(); ++it) {
        settings.setArrayIndex(index++);
        settings.setValue(QStringLiteral("button"), it.key());
        settings.setValue(QStringLiteral("key"), static_cast<int>(it.value()));
    }
    settings.endArray();
    settings.endGroup();
}

bool PsxJoypadBindings::mapKey(Qt::Key key, unsigned *buttonOut)
{
    for (auto it = mutableBindings().constBegin(); it != mutableBindings().constEnd(); ++it) {
        if (it.value() == key) {
            *buttonOut = it.key();
            return true;
        }
    }
    return false;
}

bool PsxJoypadBindings::mapMouse(Qt::MouseButton button, unsigned *buttonOut)
{
    switch (button) {
    case Qt::LeftButton:
        *buttonOut = PsxJoypadButton::B;
        return true;
    case Qt::RightButton:
        *buttonOut = PsxJoypadButton::A;
        return true;
    case Qt::MiddleButton:
        *buttonOut = PsxJoypadButton::Start;
        return true;
    default:
        return false;
    }
}

Qt::Key PsxJoypadBindings::keyForButton(unsigned buttonId)
{
    return mutableBindings().value(buttonId, Qt::Key_unknown);
}

void PsxJoypadBindings::setKeyForButton(unsigned buttonId, Qt::Key key)
{
    if (key == Qt::Key_unknown)
        mutableBindings().remove(buttonId);
    else
        mutableBindings().insert(buttonId, key);
}
