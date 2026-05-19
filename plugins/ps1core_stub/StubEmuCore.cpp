#include "StubEmuCore.h"
#include "StubCaptureSynth.h"

#include <QFileInfo>

StubEmuCore::StubEmuCore()
{
    for (EmuFramebuffer &buf : m_buffers) {
        buf.width = kWidth;
        buf.height = kHeight;
        buf.rgb24.resize(kWidth * kHeight * 3);
    }
    fillTestPattern(m_buffers[m_writeIndex]);
}

QString StubEmuCore::coreId() const
{
    return QStringLiteral("stub");
}

bool StubEmuCore::loadBios(const QString &biosPath)
{
    const QFileInfo info(biosPath);
    if (!info.exists() || !info.isFile())
        return false;
    m_biosPath = info.absoluteFilePath();
    return true;
}

bool StubEmuCore::loadIso(const QString &isoPath)
{
    const QFileInfo info(isoPath);
    if (!info.exists() || !info.isFile())
        return false;
    m_isoPath = info.absoluteFilePath();
    return true;
}

bool StubEmuCore::boot(QString *errorOut)
{
    if (m_biosPath.isEmpty() || m_isoPath.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("BIOS and ISO paths are required");
        return false;
    }
    return true;
}

void StubEmuCore::runFrame()
{
    if (m_biosPath.isEmpty() || m_isoPath.isEmpty())
        return;

    EmuFramebuffer &buf = m_buffers[static_cast<size_t>(m_writeIndex)];
    buf.frameIndex = ++m_frameIndex;
    fillTestPattern(buf);
    stubFillVramPattern(m_hooks, buf.frameIndex);
    stubEmitCaptureSample(m_hooks);

    m_readIndex = m_writeIndex;
    m_writeIndex = (m_writeIndex + 1) % 3;
}

void StubEmuCore::reset()
{
    m_frameIndex = 0;
    fillTestPattern(m_buffers[m_writeIndex]);
    m_readIndex = m_writeIndex;
}

const EmuFramebuffer &StubEmuCore::framebuffer() const
{
    return m_buffers[static_cast<size_t>(m_readIndex)];
}

void StubEmuCore::setHooks(EmuHooks *hooks)
{
    m_hooks = hooks;
}

void StubEmuCore::syncCaptureMirrors()
{
    if (m_hooks)
        stubFillVramPattern(m_hooks, m_frameIndex);
}

void StubEmuCore::fillTestPattern(EmuFramebuffer &buf)
{
    const int w = buf.width;
    const int h = buf.height;
    auto *pixels = reinterpret_cast<uchar *>(buf.rgb24.data());
    const quint8 phase = static_cast<quint8>(buf.frameIndex % 256);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = (y * w + x) * 3;
            pixels[i + 0] = static_cast<uchar>((x + phase) & 0xFF);
            pixels[i + 1] = static_cast<uchar>((y + phase) & 0xFF);
            pixels[i + 2] = static_cast<uchar>(((x ^ y) + phase) & 0xFF);
        }
    }
}
