#include "PS1RipSessionWindow.h"
#include "EmuViewport.h"
#include "PS1RipLegalityDialog.h"
#include "PS1RipManager.h"
#include "SentryReporter.h"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>

namespace {
constexpr auto kSettingsGroup = "ps1Rip";
constexpr auto kBiosKey = "biosPath";
constexpr auto kRecentIsoKey = "recentIsos";
} // namespace

PS1RipSessionWindow::PS1RipSessionWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_manager(PS1RipManager::getSingleton())
{
    setWindowTitle(tr("PS1 Runtime Ripper"));
    resize(960, 720);

    m_viewport = new EmuViewport(this);
    setCentralWidget(m_viewport);

    auto *toolbar = addToolBar(tr("Transport"));
    toolbar->setMovable(false);

    auto *biosAct = toolbar->addAction(tr("Load BIOS…"));
    connect(biosAct, &QAction::triggered, this, &PS1RipSessionWindow::pickBios);
    auto *isoAct = toolbar->addAction(tr("Load ISO…"));
    connect(isoAct, &QAction::triggered, this, &PS1RipSessionWindow::pickIso);
    toolbar->addSeparator();
    auto *startAct = toolbar->addAction(tr("Start"));
    connect(startAct, &QAction::triggered, this, &PS1RipSessionWindow::onStart);
    auto *stopAct = toolbar->addAction(tr("Stop"));
    connect(stopAct, &QAction::triggered, this, &PS1RipSessionWindow::onStop);
    auto *pauseAct = toolbar->addAction(tr("Pause"));
    connect(pauseAct, &QAction::triggered, this, &PS1RipSessionWindow::onPause);
    auto *stepAct = toolbar->addAction(tr("Step"));
    connect(stepAct, &QAction::triggered, this, &PS1RipSessionWindow::onStep);
    auto *resetAct = toolbar->addAction(tr("Reset"));
    connect(resetAct, &QAction::triggered, this, &PS1RipSessionWindow::onReset);

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);

    connect(m_manager, &PS1RipManager::framePresented, this, &PS1RipSessionWindow::onFrame);
    connect(m_manager, &PS1RipManager::error, this, &PS1RipSessionWindow::onError);

    const QString bios = QSettings().value(QString::fromLatin1(kSettingsGroup) + QLatin1Char('/')
                                               + QString::fromLatin1(kBiosKey))
                                 .toString();
    if (!bios.isEmpty())
        m_manager->loadBios(bios);

    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.viewport.open"),
                                QStringLiteral("PS1 rip session window opened"));
}

PS1RipSessionWindow::~PS1RipSessionWindow()
{
    if (m_manager && m_manager->isSessionActive())
        m_manager->stop();
    SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.viewport.close"),
                                QStringLiteral("PS1 rip session window closed"));
}

void PS1RipSessionWindow::showSession(QWidget *parent)
{
    if (!PS1RipLegalityDialog::isAcknowledged()) {
        PS1RipLegalityDialog dialog(parent);
        if (dialog.exec() != QDialog::Accepted)
            return;
    }

    auto *window = new PS1RipSessionWindow(parent);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
}

void PS1RipSessionWindow::closeEvent(QCloseEvent *event)
{
    if (m_manager)
        m_manager->stop();
    QMainWindow::closeEvent(event);
}

void PS1RipSessionWindow::pickBios()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select PS1 BIOS"), QString(),
        tr("BIOS images (*.bin *.rom);;All files (*)"));
    if (path.isEmpty())
        return;

    if (m_manager->loadBios(path)) {
        QSettings().setValue(QString::fromLatin1(kSettingsGroup) + QLatin1Char('/') + QString::fromLatin1(kBiosKey),
                           path);
        m_statusLabel->setText(tr("BIOS: %1").arg(path));
    }
}

void PS1RipSessionWindow::pickIso()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select PS1 disc image"), QString(),
        tr("Disc images (*.bin *.cue *.iso *.img);;All files (*)"));
    if (path.isEmpty())
        return;

    if (m_manager->loadIso(path)) {
        addRecentIso(path);
        m_statusLabel->setText(tr("ISO: %1").arg(path));
    }
}

void PS1RipSessionWindow::onStart()
{
    m_manager->start();
}

void PS1RipSessionWindow::onStop()
{
    m_manager->stop();
}

void PS1RipSessionWindow::onPause()
{
    m_manager->pause();
}

void PS1RipSessionWindow::onStep()
{
    m_manager->step();
}

void PS1RipSessionWindow::onReset()
{
    m_manager->stop();
    m_manager->start();
}

void PS1RipSessionWindow::onFrame(const QImage &frame, quint64 frameIndex)
{
    m_viewport->setFrame(frame);
    updateFps(frameIndex);
    m_viewport->setFps(m_smoothedFps);
}

void PS1RipSessionWindow::onError(const QString &message)
{
    QMessageBox::warning(this, tr("PS1 Runtime Ripper"), message);
}

void PS1RipSessionWindow::updateFps(quint64 frameIndex)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastFrameMs > 0 && frameIndex > m_lastFrameIndex) {
        const double dt = static_cast<double>(now - m_lastFrameMs) / 1000.0;
        if (dt > 0.0) {
            const double instant = static_cast<double>(frameIndex - m_lastFrameIndex) / dt;
            m_smoothedFps = (m_smoothedFps <= 0.0) ? instant : (m_smoothedFps * 0.85 + instant * 0.15);
        }
    }
    m_lastFrameMs = now;
    m_lastFrameIndex = frameIndex;
}

void PS1RipSessionWindow::addRecentIso(const QString &path)
{
    const QString key = QString::fromLatin1(kSettingsGroup) + QLatin1Char('/') + QString::fromLatin1(kRecentIsoKey);
    QStringList recent = QSettings().value(key).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 5)
        recent.removeLast();
    QSettings().setValue(key, recent);
}
