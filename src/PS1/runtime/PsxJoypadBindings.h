#ifndef PSXJOYPADBINDINGS_H
#define PSXJOYPADBINDINGS_H

#include <QHash>
#include <QString>
#include <Qt>

/** Configurable keyboard → libretro joypad mapping (#417). */
class PsxJoypadBindings
{
public:
    static QString buttonLabel(unsigned buttonId);
    static QStringList allButtonIds();

    static void load();
    static void save();
    static void resetToDefaults();

    static bool mapKey(Qt::Key key, unsigned *buttonOut);
    static bool mapMouse(Qt::MouseButton button, unsigned *buttonOut);

    static Qt::Key keyForButton(unsigned buttonId);
    static void setKeyForButton(unsigned buttonId, Qt::Key key);
};

#endif // PSXJOYPADBINDINGS_H
