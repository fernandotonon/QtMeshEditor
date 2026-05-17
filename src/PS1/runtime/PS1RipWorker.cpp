#include "PS1RipWorker.h"

PS1RipWorker::PS1RipWorker(QObject *parent)
    : QObject(parent)
{
}

void PS1RipWorker::startEmulation()
{
    m_running = true;
    m_paused = false;
    emit emulationStarted();
}

void PS1RipWorker::stopEmulation()
{
    if (!m_running)
        return;
    m_running = false;
    m_paused = false;
    emit emulationStopped();
}

void PS1RipWorker::pauseEmulation()
{
    if (!m_running)
        return;
    m_paused = !m_paused;
}

void PS1RipWorker::stepFrame()
{
    if (!m_running)
        return;
    ++m_frameIndex;
    emit frameAdvanced(m_frameIndex);
}
