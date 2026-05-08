/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software
without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the
Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#ifndef VIEWPORT_SETTINGS_KEYS_H
#define VIEWPORT_SETTINGS_KEYS_H

#include <QString>

namespace ViewportSettingsKeys
{

inline int defaultFsaaSamples()
{
    return 4;
}

inline double defaultNearClip()
{
    return 0.01;
}

inline double defaultFarClip()
{
    return 10000.0;
}

inline double defaultCameraSpeed()
{
    return 1.0;
}

/**
 * @brief QSettings key strings for Viewport/ preferences (single source of truth).
 *
 * C++ and QML must stay aligned: PreferencesDialog.qml uses the same
 * string values for readSetting/writeSetting.
 */
inline const QString& gridVisible()
{
    static const QString k(QStringLiteral("Viewport/gridVisible"));
    return k;
}

inline const QString& cameraSpeed()
{
    static const QString k(QStringLiteral("Viewport/cameraSpeed"));
    return k;
}

inline const QString& nearClip()
{
    static const QString k(QStringLiteral("Viewport/nearClip"));
    return k;
}

inline const QString& farClip()
{
    static const QString k(QStringLiteral("Viewport/farClip"));
    return k;
}

inline const QString& fsaaSamples()
{
    static const QString k(QStringLiteral("Viewport/fsaaSamples"));
    return k;
}

} // namespace ViewportSettingsKeys

#endif // VIEWPORT_SETTINGS_KEYS_H
