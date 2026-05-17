#ifndef STUBEMUCORE_H
#define STUBEMUCORE_H

#include "EmuCore.h"

#include <QString>
#include <array>

/** Reference / CI core: validates BIOS+ISO paths and renders a test pattern (#415). */
class StubEmuCore final : public EmuCore
{
public:
    StubEmuCore();

    QString coreId() const override;
    bool loadBios(const QString &biosPath) override;
    bool loadIso(const QString &isoPath) override;
    void runFrame() override;
    void reset() override;
    const EmuFramebuffer &framebuffer() const override;
    void setHooks(EmuHooks *hooks) override;

private:
    void fillTestPattern(EmuFramebuffer &buf);

    static constexpr int kWidth = 320;
    static constexpr int kHeight = 240;

    QString m_biosPath;
    QString m_isoPath;
    EmuHooks *m_hooks = nullptr;
    quint64 m_frameIndex = 0;
    std::array<EmuFramebuffer, 3> m_buffers{};
    int m_readIndex = 0;
    int m_writeIndex = 1;
};

#endif // STUBEMUCORE_H
