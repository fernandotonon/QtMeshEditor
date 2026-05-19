#include "PS1RipSessionWindow.h"
#include "EmuViewport.h"
#include "PS1RipLegalityDialog.h"
#include "PS1RipManager.h"
#include "SentryReporter.h"
#include "PsxJoypadState.h"
#include "VramViewerWidget.h"

#include <QAction>
#include <QSignalBlocker>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QFileDialog>
#include <QTimer>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QCheckBox>
#include <QToolBar>

namespace {
constexpr auto kSettingsGroup = "ps1Rip";
constexpr auto kBiosKey = "biosPath";
constexpr auto kRecentIsoKey = "recentIsos";
constexpr auto kDedupeStrictKey = "dedupeStrict";
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
    toolbar->addSeparator();
    auto *armCapAct = toolbar->addAction(tr("Arm Capture"));
    armCapAct->setCheckable(true);
    connect(armCapAct, &QAction::toggled, this, [this](bool on) { m_manager->armCapture(on); });
    connect(m_manager, &PS1RipManager::sessionStopped, this, [armCapAct]() {
        QSignalBlocker blocker(armCapAct);
        armCapAct->setChecked(false);
    });
    auto *captureAct = toolbar->addAction(tr("Capture Frame"));
    connect(captureAct, &QAction::triggered, this, &PS1RipSessionWindow::onCaptureFrame);
    auto *strictDedupe = new QCheckBox(tr("Strict dedupe"), this);
    strictDedupe->setToolTip(tr("Bit-exact topology hash (off = 0.01 position snap)"));
    strictDedupe->setChecked(
        QSettings().value(QString::fromLatin1(kSettingsGroup) + QLatin1Char('/')
                              + QString::fromLatin1(kDedupeStrictKey), false)
            .toBool());
    m_manager->setDedupeStrict(strictDedupe->isChecked());
    connect(strictDedupe, &QCheckBox::toggled, this, [this](bool on) {
        m_manager->setDedupeStrict(on);
        QSettings().setValue(QString::fromLatin1(kSettingsGroup) + QLatin1Char('/')
                                 + QString::fromLatin1(kDedupeStrictKey),
                             on);
    });
    toolbar->addWidget(strictDedupe);
    auto *dumpVramAct = toolbar->addAction(tr("Dump VRAM"));
    connect(dumpVramAct, &QAction::triggered, this, &PS1RipSessionWindow::onDumpVram);

    m_vramViewer = new VramViewerWidget(this);
    auto *vramDock = new QDockWidget(tr("VRAM"), this);
    vramDock->setWidget(m_vramViewer);
    vramDock->setMinimumHeight(220);
    addDockWidget(Qt::BottomDockWidgetArea, vramDock);

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);

    connect(m_manager, &PS1RipManager::framePresented, this, &PS1RipSessionWindow::onFrame);
    connect(m_manager, &PS1RipManager::sessionStarted, this, [this](const QString &coreId) {
        if (m_viewport) {
            m_viewport->setFocus(Qt::OtherFocusReason);
            m_viewport->grabKeyboard();
        }
        if (coreId == QStringLiteral("libretro")) {
            m_statusLabel->setText(
                tr("Running — click viewport: arrows, X/Enter=✕, Z/○=back, P=Start, Tab=Select"));
        } else if (coreId == QStringLiteral("stub"))
            m_statusLabel->setText(tr("Running (stub — test pattern only)"));
        else
            m_statusLabel->setText(tr("Running (core: %1)").arg(coreId));
    });
    connect(m_manager, &PS1RipManager::vramFrameUpdated, this,
            [this](const QVector<uint16_t> &cells, const QImage &preview) {
                if (m_vramViewer)
                    m_vramViewer->setVramData(cells, preview);
            });
    connect(m_manager, &PS1RipManager::sessionStopped, this, [this]() {
        PsxJoypadState::resetAll();
        if (m_viewport)
            m_viewport->releaseKeyboard();
        m_statusLabel->setText(tr("Stopped"));
    });
    connect(m_manager, &PS1RipManager::error, this, &PS1RipSessionWindow::onError);
    connect(m_manager, &PS1RipManager::vramDumped, this, &PS1RipSessionWindow::onVramDumped);
    connect(m_manager, &PS1RipManager::meshBuilt, this, &PS1RipSessionWindow::onMeshBuilt);

    const QString savedBios = QSettings().value(QString::fromLatin1(kSettingsGroup) + QLatin1Char('/')
                                                  + QString::fromLatin1(kBiosKey))
                                    .toString();
    if (!savedBios.isEmpty()) {
        QTimer::singleShot(0, this, [this, savedBios]() {
            if (m_manager->loadBios(savedBios))
                m_statusLabel->setText(tr("BIOS: %1").arg(savedBios));
        });
    }

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
    if (m_viewport)
        m_viewport->releaseKeyboard();
    if (m_manager)
        m_manager->stop();
    QMainWindow::closeEvent(event);
}

void PS1RipSessionWindow::pickBios()
{
    QWidget *dialogParent = isVisible() ? this : QApplication::activeWindow();
    const QString path = QFileDialog::getOpenFileName(
        dialogParent, tr("Select PS1 BIOS"), QString(),
        tr("BIOS images (*.bin *.rom);;All files (*)"),
        nullptr, QFileDialog::DontUseNativeDialog);
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
    QWidget *dialogParent = isVisible() ? this : QApplication::activeWindow();
    const QString path = QFileDialog::getOpenFileName(
        dialogParent, tr("Select PS1 disc image"), QString(),
        tr("Disc images (*.cue *.chd *.pbp *.iso *.img *.bin *.exe);;All files (*)"),
        nullptr, QFileDialog::DontUseNativeDialog);
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
    if (!m_manager->isSessionActive() && !m_manager->isStartPending()) {
        m_manager->start();
        return;
    }

    connect(m_manager, &PS1RipManager::sessionStopped, this,
            [this]() { m_manager->start(); }, Qt::SingleShotConnection);
    m_manager->stop();
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

void PS1RipSessionWindow::onDumpVram()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("ps1_rip_dump_vram"));
    m_manager->dumpVRAM();
}

void PS1RipSessionWindow::onCaptureFrame()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("ps1_rip_capture_frame"));
    m_manager->captureFrame();
}

void PS1RipSessionWindow::onMeshBuilt(const QString &captureId, int capturedParts, int uniqueMeshes,
                                    int instanceCount, int vertexCount, int triangleCount)
{
    m_statusLabel->setText(
        tr("Mesh %1 — captured %2 / unique %3 / instances %4 (%5 verts, %6 tris)")
            .arg(captureId)
            .arg(capturedParts)
            .arg(uniqueMeshes)
            .arg(instanceCount)
            .arg(vertexCount)
            .arg(triangleCount));
}

void PS1RipSessionWindow::onVramDumped(const QString &captureId, const QString &pngPath,
                                       const QVector<uint16_t> &cells, const QImage &nativePreview)
{
    if (m_vramViewer)
        m_vramViewer->setVramData(cells, nativePreview);
    m_statusLabel->setText(tr("VRAM dumped: %1 → %2").arg(captureId, pngPath));
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
