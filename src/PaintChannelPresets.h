/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#ifndef PAINTCHANNELPRESETS_H
#define PAINTCHANNELPRESETS_H

// Paint v2 Slice D (#547) — channel-aware paint presets.
//
// Five one-click presets that set the active PBR channel plus channel-tuned
// brush parameters (radius / strength / falloff / colour source / blend mode /
// stamp). Modeled on MaterialPresetLibrary (name-list + applyPreset dispatch);
// the config table is pure data so it can be unit-tested without the
// TexturePaintController / a live Ogre scene.

#include <QObject>
#include <QStringList>
#include <QQmlEngine>
#include <QVector>

#include "PaintChannel.h"

class PaintChannelPresets : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QStringList presetNames READ presetNames CONSTANT)

public:
    /// One preset's brush configuration. Pure data (no Qt paint types) so the
    /// table is unit-testable in isolation.
    struct Preset {
        QString name;
        PaintChannelNS::Channel channel = PaintChannelNS::Channel::BaseColor;
        double radius = 0.05;      ///< mesh-local brush radius
        double strength = 1.0;     ///< 0..1
        double falloff = 2.0;      ///< brush edge falloff
        int colorR = 255, colorG = 255, colorB = 255;
        int blendMode = 0;         ///< PaintLayerBlend::Mode (0 = Normal)
        int brushTool = 0;         ///< TexturePaintController::BrushTool
        QString stamp;             ///< optional stamp asset name ("" = round brush)
        QString note;              ///< short description for tooltips
    };

    static PaintChannelPresets* instance();
    static PaintChannelPresets* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QStringList presetNames() const;
    /// Apply a preset by name: sets the active channel + brush params on the
    /// TexturePaintController singleton. No-op (returns false) for unknown names.
    Q_INVOKABLE bool applyPreset(const QString& name);

    /// The pure-data preset table (for tests / tooltips). Stable order matches
    /// presetNames().
    static const QVector<Preset>& presets();
    /// Look up a preset by name; returns false if not found.
    static bool findPreset(const QString& name, Preset& out);

signals:
    void presetApplied(const QString& name);

private:
    PaintChannelPresets();
    ~PaintChannelPresets() override = default;
    static PaintChannelPresets* m_pSingleton;
};

#endif // PAINTCHANNELPRESETS_H
