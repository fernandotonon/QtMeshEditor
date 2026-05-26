#include "PS1RipSessionWindow.h"
#include "EmuViewport.h"
#include "PS1RipGamepadBridge.h"
#include "PS1RipInputSettingsDialog.h"
#include "PS1RipLegalityDialog.h"
#include "PS1RipManager.h"
#include "Ps1CoordinateNormalizer.h"
#include "PsxJoypadBindings.h"
#include "SentryReporter.h"
#include "PsxJoypadState.h"
#include "PsxVramMirrorMode.h"
#include "VramViewerWidget.h"

#include <QAction>
#include <QSignalBlocker>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequence>
#include <QMenu>
#include <QTimer>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QCheckBox>
#include <QLocale>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr auto kSettingsGroup = "ps1Rip";
constexpr auto kBiosKey = "biosPath";
constexpr auto kRecentIsoKey = "recentIsos";
constexpr auto kDedupeStrictKey = "dedupeStrict";
constexpr auto kViewportIntegerScaleKey = "viewportIntegerScale";
constexpr auto kViewportSmoothFilterKey = "viewportSmoothFilter";
constexpr auto kViewportAspect43Key = "viewportAspect43";
constexpr auto kNormalizePrefix = "ps1Rip/normalize";
/** Persisted spinbox value for `Capture Scene` (#425). Default 5 s per the
 *  issue spec. */
constexpr auto kSceneCaptureSecondsKey = "sceneCaptureSeconds";
constexpr int kSceneCaptureSecondsDefault = 5;
constexpr int kSceneCaptureSecondsMin = 1;
constexpr int kSceneCaptureSecondsMax = 60;

QString ps1SettingsKey(const char *name)
{
    return QString::fromLatin1(kSettingsGroup) + QLatin1Char('/') + QString::fromLatin1(name);
}

/** Render a byte count compactly (#425 status footer). Uses 1024-based units
 *  because that's what every engine / inspector / Qt tool in the codebase
 *  shows for in-memory sizes. */
QString humaniseBytes(qint64 bytes)
{
    if (bytes < 1024)
        return PS1RipSessionWindow::tr("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return PS1RipSessionWindow::tr("%1 KiB").arg(QLocale().toString(double(bytes) / 1024.0, 'f', 1));
    if (bytes < 1024LL * 1024 * 1024)
        return PS1RipSessionWindow::tr("%1 MiB").arg(QLocale().toString(double(bytes) / 1024.0 / 1024.0, 'f', 2));
    return PS1RipSessionWindow::tr("%1 GiB")
        .arg(QLocale().toString(double(bytes) / 1024.0 / 1024.0 / 1024.0, 'f', 2));
}
} // namespace

PS1RipSessionWindow::PS1RipSessionWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_manager(PS1RipManager::getSingleton())
{
    PsxJoypadBindings::load();

    setWindowTitle(tr("PS1 Runtime Ripper"));
    resize(960, 720);

    m_gamepadBridge = new PS1RipGamepadBridge(this);

    m_viewport = new EmuViewport(this);
    setCentralWidget(m_viewport);

    QSettings settings;
    m_viewport->setIntegerScale(
        settings.value(ps1SettingsKey(kViewportIntegerScaleKey), true).toBool());
    m_viewport->setSmoothFiltering(
        settings.value(ps1SettingsKey(kViewportSmoothFilterKey), false).toBool());
    m_viewport->setAspectMode(
        settings.value(ps1SettingsKey(kViewportAspect43Key), true).toBool()
            ? EmuViewport::AspectMode::Display43
            : EmuViewport::AspectMode::Native);

    auto *viewMenu = menuBar()->addMenu(tr("View"));
    auto *integerScaleAct = viewMenu->addAction(tr("Integer scale"));
    integerScaleAct->setCheckable(true);
    integerScaleAct->setChecked(m_viewport->integerScale());
    integerScaleAct->setToolTip(tr("Nearest-neighbor upscale in whole-pixel steps"));
    connect(integerScaleAct, &QAction::toggled, this, [this](bool on) {
        m_viewport->setIntegerScale(on);
        QSettings().setValue(ps1SettingsKey(kViewportIntegerScaleKey), on);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                    on ? QStringLiteral("ps1_rip_viewport_integer_scale_on")
                                       : QStringLiteral("ps1_rip_viewport_integer_scale_off"));
    });

    auto *smoothFilterAct = viewMenu->addAction(tr("Bilinear filtering"));
    smoothFilterAct->setCheckable(true);
    smoothFilterAct->setChecked(m_viewport->smoothFiltering());
    smoothFilterAct->setToolTip(tr("Smooth scaling when integer scale is off"));
    connect(smoothFilterAct, &QAction::toggled, this, [this](bool on) {
        m_viewport->setSmoothFiltering(on);
        QSettings().setValue(ps1SettingsKey(kViewportSmoothFilterKey), on);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                    on ? QStringLiteral("ps1_rip_viewport_bilinear_on")
                                       : QStringLiteral("ps1_rip_viewport_bilinear_off"));
    });

    auto *inputMenu = menuBar()->addMenu(tr("Input"));
    inputMenu->addAction(tr("Keyboard mapping…"), this, &PS1RipSessionWindow::onOpenInputSettings);

    auto *aspect43Act = viewMenu->addAction(tr("4:3 display aspect (NTSC/PAL)"));
    aspect43Act->setCheckable(true);
    aspect43Act->setChecked(m_viewport->aspectMode() == EmuViewport::AspectMode::Display43);
    connect(aspect43Act, &QAction::toggled, this, [this](bool on) {
        m_viewport->setAspectMode(on ? EmuViewport::AspectMode::Display43
                                     : EmuViewport::AspectMode::Native);
        QSettings().setValue(ps1SettingsKey(kViewportAspect43Key), on);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                    on ? QStringLiteral("ps1_rip_viewport_aspect_43_on")
                                       : QStringLiteral("ps1_rip_viewport_aspect_43_off"));
    });

    auto *toolbar = addToolBar(tr("Transport"));
    toolbar->setMovable(false);

    auto *biosAct = toolbar->addAction(tr("Load BIOS…"));
    connect(biosAct, &QAction::triggered, this, &PS1RipSessionWindow::pickBios);

    auto *isoMenuBtn = new QToolButton(this);
    isoMenuBtn->setText(tr("Load ISO…"));
    isoMenuBtn->setPopupMode(QToolButton::MenuButtonPopup);
    auto *isoMenu = new QMenu(isoMenuBtn);
    auto *browseAct = isoMenu->addAction(tr("Browse…"), this, &PS1RipSessionWindow::pickIso);
    isoMenuBtn->setDefaultAction(browseAct);
    m_recentIsoMenu = isoMenu->addMenu(tr("Recent"));
    rebuildRecentIsoMenu();
    isoMenuBtn->setMenu(isoMenu);
    toolbar->addWidget(isoMenuBtn);

    auto *reloadIsoAct = toolbar->addAction(tr("Reload ISO"));
    reloadIsoAct->setToolTip(tr("Reload the current disc image"));
    connect(reloadIsoAct, &QAction::triggered, this, &PS1RipSessionWindow::onReloadIso);
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
    m_armCaptureAct = toolbar->addAction(tr("Arm Capture"));
    m_armCaptureAct->setCheckable(true);
    connect(m_armCaptureAct, &QAction::toggled, this, [this](bool on) {
        m_manager->armCapture(on);
        if (!on) {
            // Disarming clears the worker's capture buffer, so the previous
            // live counters are stale — wipe them so the footer doesn't show
            // ghost stats from a previous capture (#425).
            m_lastCaptureTriangles = 0;
            m_lastCaptureTexPages = 0;
            m_lastCaptureBytes = 0;
        }
        refreshCaptureStatusFooter();
    });
    connect(m_manager, &PS1RipManager::sessionStopped, this, [this]() {
        if (m_armCaptureAct) {
            QSignalBlocker blocker(m_armCaptureAct);
            m_armCaptureAct->setChecked(false);
        }
        m_lastCaptureTriangles = 0;
        m_lastCaptureTexPages = 0;
        m_lastCaptureBytes = 0;
        m_sceneCaptureRemaining = 0;
        m_sceneCaptureTotal = 0;
        if (m_captureSceneAct)
            m_captureSceneAct->setEnabled(true);
        if (m_sceneCaptureSecondsSpin)
            m_sceneCaptureSecondsSpin->setEnabled(true);
        if (m_stopCaptureAct)
            m_stopCaptureAct->setEnabled(false);
        if (m_hotkeyCaptureScene)
            m_hotkeyCaptureScene->setEnabled(true);
        refreshCaptureStatusFooter();
    });
    auto *captureAct = toolbar->addAction(tr("Capture Frame"));
    captureAct->setToolTip(tr("Materialise a single-frame capture (hotkey: C)"));
    connect(captureAct, &QAction::triggered, this, &PS1RipSessionWindow::onCaptureFrame);

    // Scene capture controls (#425). Spinner is bounded to a sensible range
    // so an accidentally huge number can't lock up the GUI thread on the
    // 1-Hz countdown. Default 5 s matches the issue spec.
    m_sceneCaptureSecondsSpin = new QSpinBox(this);
    m_sceneCaptureSecondsSpin->setRange(kSceneCaptureSecondsMin, kSceneCaptureSecondsMax);
    m_sceneCaptureSecondsSpin->setSuffix(tr(" s"));
    m_sceneCaptureSecondsSpin->setValue(
        settings.value(ps1SettingsKey(kSceneCaptureSecondsKey), kSceneCaptureSecondsDefault).toInt());
    m_sceneCaptureSecondsSpin->setToolTip(
        tr("Scene-capture duration in seconds (default 5, max 60)."));
    toolbar->addWidget(m_sceneCaptureSecondsSpin);

    m_captureSceneAct = toolbar->addAction(tr("Capture Scene"));
    m_captureSceneAct->setToolTip(
        tr("Accumulate every primitive over N seconds, dedupe, materialise once.\n"
           "Hotkey: Shift+C"));
    connect(m_captureSceneAct, &QAction::triggered, this, &PS1RipSessionWindow::onCaptureScene);

    m_stopCaptureAct = toolbar->addAction(tr("Stop Capture"));
    m_stopCaptureAct->setToolTip(tr("Cancel an in-flight scene capture (disarms)."));
    m_stopCaptureAct->setEnabled(false);
    connect(m_stopCaptureAct, &QAction::triggered, this, &PS1RipSessionWindow::onStopCapture);

    auto *strictDedupe = new QCheckBox(tr("Strict dedupe"), this);
    strictDedupe->setToolTip(tr("Bit-exact topology hash (off = 0.01 position snap)"));
    strictDedupe->setChecked(settings.value(ps1SettingsKey(kDedupeStrictKey), false).toBool());
    m_manager->setDedupeStrict(strictDedupe->isChecked());
    connect(strictDedupe, &QCheckBox::toggled, this, [this](bool on) {
        m_manager->setDedupeStrict(on);
        SentryReporter::addBreadcrumb(
            QStringLiteral("ui.action"),
            on ? QStringLiteral("ps1_rip_strict_dedupe_on")
               : QStringLiteral("ps1_rip_strict_dedupe_off"));
        QSettings().setValue(ps1SettingsKey(kDedupeStrictKey), on);
    });
    toolbar->addWidget(strictDedupe);
    auto *dumpVramAct = toolbar->addAction(tr("Dump VRAM"));
    dumpVramAct->setToolTip(tr("Snapshot the GPU VRAM mirror to PNG (hotkey: V)"));
    connect(dumpVramAct, &QAction::triggered, this, &PS1RipSessionWindow::onDumpVram);

    // Persist the duration whenever it changes so a session restart keeps the
    // user's last preference. Sentry breadcrumb fires here too so we can
    // correlate scene-capture cancels with the chosen duration in telemetry.
    connect(m_sceneCaptureSecondsSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [](int v) {
                QSettings().setValue(ps1SettingsKey(kSceneCaptureSecondsKey), v);
                SentryReporter::addBreadcrumb(
                    QStringLiteral("ui.action"),
                    QStringLiteral("ps1_rip_scene_capture_seconds=%1").arg(v));
            });

    m_vramViewer = new VramViewerWidget(this);
    auto *vramDock = new QDockWidget(tr("VRAM"), this);
    vramDock->setWidget(m_vramViewer);
    vramDock->setMinimumHeight(220);
    addDockWidget(Qt::BottomDockWidgetArea, vramDock);

    createNormalizerDock();

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel, /*stretch=*/1);
    // Capture status footer (#425) — distinct widget so the live 4 Hz counter
    // doesn't overwrite the mesh-built summary in the primary status label.
    m_captureFooterLabel = new QLabel(this);
    m_captureFooterLabel->setMinimumWidth(220);
    m_captureFooterLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->addPermanentWidget(m_captureFooterLabel);
    refreshCaptureStatusFooter();

    // Window-scoped hotkeys (#425) — Qt::WindowShortcut means they fire only
    // when this window has keyboard focus (matching the issue's "only while
    // focused" requirement). Wired to the same handlers as the toolbar
    // actions so behaviour stays consistent.
    m_hotkeyCaptureFrame = new QShortcut(QKeySequence(Qt::Key_C), this);
    m_hotkeyCaptureFrame->setContext(Qt::WindowShortcut);
    connect(m_hotkeyCaptureFrame, &QShortcut::activated, this, &PS1RipSessionWindow::onCaptureFrame);

    m_hotkeyCaptureScene = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_C), this);
    m_hotkeyCaptureScene->setContext(Qt::WindowShortcut);
    connect(m_hotkeyCaptureScene, &QShortcut::activated, this, &PS1RipSessionWindow::onCaptureScene);

    m_hotkeyDumpVram = new QShortcut(QKeySequence(Qt::Key_V), this);
    m_hotkeyDumpVram->setContext(Qt::WindowShortcut);
    connect(m_hotkeyDumpVram, &QShortcut::activated, this, &PS1RipSessionWindow::onDumpVram);

    connect(m_manager, &PS1RipManager::framePresented, this, &PS1RipSessionWindow::onFrame);
    connect(m_manager, &PS1RipManager::pausedChanged, this, &PS1RipSessionWindow::onPausedChanged);
    connect(m_manager, &PS1RipManager::captureProgress, this,
            &PS1RipSessionWindow::onCaptureProgress);
    connect(m_manager, &PS1RipManager::sceneCaptureStarted, this,
            &PS1RipSessionWindow::onSceneCaptureStarted);
    connect(m_manager, &PS1RipManager::sceneCaptureProgress, this,
            &PS1RipSessionWindow::onSceneCaptureProgress);
    connect(m_manager, &PS1RipManager::sceneCaptureFinished, this,
            &PS1RipSessionWindow::onSceneCaptureFinished);
    connect(m_manager, &PS1RipManager::sessionStarted, this, [this](const QString &coreId) {
        if (m_viewport) {
            m_viewport->setFocus(Qt::OtherFocusReason);
            m_viewport->grabKeyboard();
        }
        if (coreId == QStringLiteral("libretro")) {
            const QString padHint = m_gamepadBridge && m_gamepadBridge->isActive()
                                        ? tr(" · gamepad connected")
                                        : QString();
            m_statusLabel->setText(
                tr("Running — Z/X=○/✕, arrows=D-pad, Enter/Shift=Start/Select%1").arg(padHint));
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

void PS1RipSessionWindow::createNormalizerDock()
{
    QSettings qs;
    const Ps1NormalizerSettings persisted =
        Ps1CoordinateNormalizer::load(qs, QString::fromLatin1(kNormalizePrefix));

    auto *dock = new QDockWidget(tr("Normalize"), this);
    auto *body = new QWidget(dock);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);

    m_normalizeScaleSpin = new QDoubleSpinBox(body);
    m_normalizeScaleSpin->setRange(0.001, 1000.0);
    m_normalizeScaleSpin->setDecimals(4);
    m_normalizeScaleSpin->setSingleStep(0.1);
    m_normalizeScaleSpin->setValue(persisted.userScale);
    m_normalizeScaleSpin->setToolTip(
        tr("Multiplier on top of the built-in capture scale.\n"
           "1.0 = default magnitude (~FBX/glTF scale). 0.024 ≈ PSX-native (1/4096)."));
    form->addRow(tr("Scale:"), m_normalizeScaleSpin);

    auto *flipBox = new QGroupBox(tr("Flip axes"), body);
    auto *flipLayout = new QVBoxLayout(flipBox);
    flipLayout->setContentsMargins(8, 4, 8, 4);
    m_normalizeFlipX = new QCheckBox(tr("Flip X"), flipBox);
    m_normalizeFlipY = new QCheckBox(tr("Flip Y"), flipBox);
    m_normalizeFlipZ = new QCheckBox(tr("Flip Z"), flipBox);
    m_normalizeFlipX->setChecked(persisted.flipX);
    m_normalizeFlipY->setChecked(persisted.flipY);
    m_normalizeFlipZ->setChecked(persisted.flipZ);
    m_normalizeFlipX->setToolTip(tr("Mirror around X. Ogre auto-flips winding for negative-determinant scale."));
    m_normalizeFlipY->setToolTip(tr("Mirror around Y. Useful for games with unconventional up axis."));
    m_normalizeFlipZ->setToolTip(tr("Mirror around Z. Combine with another axis to keep CCW winding."));
    flipLayout->addWidget(m_normalizeFlipX);
    flipLayout->addWidget(m_normalizeFlipY);
    flipLayout->addWidget(m_normalizeFlipZ);

    m_normalizePerspectiveUV = new QCheckBox(tr("Perspective-correct UVs"), body);
    m_normalizePerspectiveUV->setChecked(persisted.perspectiveCorrectUVs);
    m_normalizePerspectiveUV->setToolTip(
        tr("Subdivide triangles with high depth variance and recompute affine\n"
           "UVs at midpoints — the classic warped-quad fix used in modern PSX\n"
           "remasters. Takes effect on the next capture (mesh data only)."));

    layout->addLayout(form);
    layout->addWidget(flipBox);
    layout->addWidget(m_normalizePerspectiveUV);
    layout->addStretch(1);

    body->setLayout(layout);
    dock->setWidget(body);
    dock->setMinimumWidth(180);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // Push the loaded settings now so the manager + any existing capture
    // nodes pick them up immediately on session resume.
    m_manager->setNormalizerSettings(persisted);

    // Per-control ui.action breadcrumbs so telemetry distinguishes a direct
    // user gesture from the downstream ps1.rip.coord.normalize emitted by the
    // manager (CodeRabbit Minor on the original #424 PR). The lambda captures
    // the breadcrumb message by value so each connect picks the right one.
    auto onChanged = [this](const QString &msg) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), msg);
        pushNormalizerSettings();
    };
    connect(m_normalizeScaleSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, onChanged](double v) {
                onChanged(QStringLiteral("ps1_rip_normalize_scale_changed=%1").arg(v, 0, 'g', 4));
            });
    connect(m_normalizeFlipX, &QCheckBox::toggled, this, [onChanged](bool on) {
        onChanged(on ? QStringLiteral("ps1_rip_normalize_flip_x_on")
                     : QStringLiteral("ps1_rip_normalize_flip_x_off"));
    });
    connect(m_normalizeFlipY, &QCheckBox::toggled, this, [onChanged](bool on) {
        onChanged(on ? QStringLiteral("ps1_rip_normalize_flip_y_on")
                     : QStringLiteral("ps1_rip_normalize_flip_y_off"));
    });
    connect(m_normalizeFlipZ, &QCheckBox::toggled, this, [onChanged](bool on) {
        onChanged(on ? QStringLiteral("ps1_rip_normalize_flip_z_on")
                     : QStringLiteral("ps1_rip_normalize_flip_z_off"));
    });
    connect(m_normalizePerspectiveUV, &QCheckBox::toggled, this, [onChanged](bool on) {
        onChanged(on ? QStringLiteral("ps1_rip_normalize_perspective_uv_on")
                     : QStringLiteral("ps1_rip_normalize_perspective_uv_off"));
    });
}

void PS1RipSessionWindow::pushNormalizerSettings()
{
    Ps1NormalizerSettings s;
    if (m_normalizeScaleSpin)
        s.userScale = static_cast<float>(m_normalizeScaleSpin->value());
    if (m_normalizeFlipX) s.flipX = m_normalizeFlipX->isChecked();
    if (m_normalizeFlipY) s.flipY = m_normalizeFlipY->isChecked();
    if (m_normalizeFlipZ) s.flipZ = m_normalizeFlipZ->isChecked();
    if (m_normalizePerspectiveUV)
        s.perspectiveCorrectUVs = m_normalizePerspectiveUV->isChecked();

    QSettings qs;
    Ps1CoordinateNormalizer::save(qs, QString::fromLatin1(kNormalizePrefix), s);
    if (m_manager)
        m_manager->setNormalizerSettings(s);
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
    if (!path.isEmpty())
        applyIsoPath(path);
}

void PS1RipSessionWindow::openRecentIso()
{
    const auto *action = qobject_cast<const QAction *>(sender());
    if (!action)
        return;
    const QString path = action->data().toString();
    if (!path.isEmpty())
        applyIsoPath(path);
}

void PS1RipSessionWindow::applyIsoPath(const QString &path)
{
    if (m_manager->loadIso(path)) {
        addRecentIso(path);
        rebuildRecentIsoMenu();
        m_statusLabel->setText(tr("ISO: %1").arg(path));
    }
}

void PS1RipSessionWindow::onReloadIso()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("ps1_rip_reload_iso"));
    if (m_manager->reloadIso())
        m_statusLabel->setText(tr("ISO reloaded: %1").arg(m_manager->isoPath()));
}

void PS1RipSessionWindow::onOpenInputSettings()
{
    PS1RipInputSettingsDialog dialog(this);
    dialog.exec();
}

void PS1RipSessionWindow::onPausedChanged(bool paused)
{
    if (paused)
        m_statusLabel->setText(tr("Paused — FPS frozen"));
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

void PS1RipSessionWindow::onCaptureScene()
{
    // Guard against the Shift+C hotkey firing while a capture is in flight
    // (the toolbar action is disabled in that case, but the QShortcut isn't
    // automatically gated by the action's enabled state) — CodeRabbit Minor
    // on #677.
    if (m_manager && m_manager->isSceneCaptureActive())
        return;
    if (m_captureSceneAct && !m_captureSceneAct->isEnabled())
        return;
    const int seconds =
        m_sceneCaptureSecondsSpin ? m_sceneCaptureSecondsSpin->value() : kSceneCaptureSecondsDefault;
    SentryReporter::addBreadcrumb(
        QStringLiteral("ui.action"),
        QStringLiteral("ps1_rip_capture_scene_requested=%1s").arg(seconds));
    m_manager->captureScene(seconds);
}

void PS1RipSessionWindow::onStopCapture()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("ps1_rip_capture_stop"));
    // Cancel any in-flight scene capture AND drop the armed flag so the user
    // returns to a clean idle state. Matches the issue's "Stop — disarms"
    // line (#425). If no scene capture is running, disarming alone is the
    // observable effect.
    m_manager->stopSceneCapture();
    m_manager->armCapture(false);
    // Backend is now disarmed, but the toolbar QAction is checkable and was
    // only following user clicks — sync its checked state explicitly so the
    // toolbar doesn't show a misleading "armed" indicator while the manager
    // is actually disarmed (Codex P2 / CodeRabbit Major on #677).
    if (m_armCaptureAct && m_armCaptureAct->isChecked()) {
        QSignalBlocker blocker(m_armCaptureAct);
        m_armCaptureAct->setChecked(false);
    }
    refreshCaptureStatusFooter();
}

void PS1RipSessionWindow::onSceneCaptureStarted(int totalSeconds)
{
    m_sceneCaptureTotal = totalSeconds;
    m_sceneCaptureRemaining = totalSeconds;
    if (m_captureSceneAct)
        m_captureSceneAct->setEnabled(false);
    if (m_sceneCaptureSecondsSpin)
        m_sceneCaptureSecondsSpin->setEnabled(false);
    if (m_stopCaptureAct)
        m_stopCaptureAct->setEnabled(true);
    // Mirror the QAction-disabled state on the keyboard shortcut so Shift+C
    // can't bypass the gate while a capture is already running
    // (CodeRabbit Minor on #677).
    if (m_hotkeyCaptureScene)
        m_hotkeyCaptureScene->setEnabled(false);
    refreshCaptureStatusFooter();
}

void PS1RipSessionWindow::onSceneCaptureProgress(int remainingSeconds, int totalSeconds)
{
    m_sceneCaptureRemaining = remainingSeconds;
    m_sceneCaptureTotal = totalSeconds;
    refreshCaptureStatusFooter();
}

void PS1RipSessionWindow::onSceneCaptureFinished(bool cancelled, const QString &captureId)
{
    Q_UNUSED(captureId);
    m_sceneCaptureRemaining = 0;
    m_sceneCaptureTotal = 0;
    if (m_captureSceneAct)
        m_captureSceneAct->setEnabled(true);
    if (m_sceneCaptureSecondsSpin)
        m_sceneCaptureSecondsSpin->setEnabled(true);
    if (m_stopCaptureAct)
        m_stopCaptureAct->setEnabled(false);
    if (m_hotkeyCaptureScene)
        m_hotkeyCaptureScene->setEnabled(true);
    if (cancelled)
        m_statusLabel->setText(tr("Scene capture cancelled"));
    refreshCaptureStatusFooter();
}

void PS1RipSessionWindow::onCaptureProgress(qint64 primitives, qint64 triangles, int texturePages,
                                            qint64 bytesEstimate)
{
    Q_UNUSED(primitives);
    m_lastCaptureTriangles = triangles;
    m_lastCaptureTexPages = texturePages;
    m_lastCaptureBytes = bytesEstimate;
    refreshCaptureStatusFooter();
}

void PS1RipSessionWindow::refreshCaptureStatusFooter()
{
    if (!m_captureFooterLabel)
        return;
    if (!m_manager) {
        m_captureFooterLabel->clear();
        return;
    }
    QString text;
    if (m_sceneCaptureRemaining > 0) {
        text = tr("Scene capture %1/%2 s · ")
                   .arg(m_sceneCaptureTotal - m_sceneCaptureRemaining)
                   .arg(m_sceneCaptureTotal);
    } else if (m_manager->isCaptureArmed()) {
        text = tr("Armed · ");
    } else {
        m_captureFooterLabel->clear();
        return;
    }
    text += tr("%1 tris · %2 tex pages · %3")
                .arg(QLocale().toString(m_lastCaptureTriangles))
                .arg(m_lastCaptureTexPages)
                .arg(humaniseBytes(m_lastCaptureBytes));
    m_captureFooterLabel->setText(text);
}

void PS1RipSessionWindow::onMeshBuilt(const QString &captureId, int capturedParts, int uniqueMeshes,
                                    int instanceCount, int vertexCount, int triangleCount,
                                    int matrixCount, uint32_t cameraMatrixId, bool hasCameraMatrix,
                                    int gteInversePercent, bool slabLike,
                                    int primsWithMatrixId, int primsTotal,
                                    PsxVramMirrorMode vramMirrorMode, Gp0CaptureStats captureStats)
{
    QString cameraText = hasCameraMatrix
                             ? tr("camera matrix #%1").arg(cameraMatrixId)
                             : tr("camera matrix unknown");
    QString matrixStats = tr("GTE inverse %1%").arg(gteInversePercent);
    // #675: surface the prim → matrix association ratio so users can tell at a glance
    // whether the bottleneck is the inverse math (`tag X/X` with low %) or the matrix
    // association (`tag 0/N`). Without this you'd have to read the Sentry breadcrumb
    // to know which subsystem to debug.
    if (primsTotal > 0)
        matrixStats += tr(" (matrix tag %1/%2)").arg(primsWithMatrixId).arg(primsTotal);
    if (slabLike)
        matrixStats += tr(" — slab-like bounds (check matrix association)");
    QString vramText = psxVramMirrorModeLabel(vramMirrorMode);
    if (vramMirrorMode != PsxVramMirrorMode::FullVram)
        vramText += tr(" — textures may be wrong");
    // GP0 capture-source breakdown surfaces the #662 FIFO bridge attribution
    // alongside the merged-RAM scan paths so users can see at a glance which
    // path produced the geometry (gp0_hook vs ram_*).
    // #674: append TMD / HMD counts so users see the model-space scanner contribution.
    // On TMD-using games this is what flips the source label to `ram_model_mesh` and the
    // GTE inverse % to non-zero. `hmd` is emitted-meshes (zero until v2 walker lands);
    // `hmd_cand` is the v1 diagnostics count of plausible HMD magic-byte candidates so
    // testers can confirm magic detection without the walker (#674 review).
    const QString gp0Text =
        tr("GP0 %1 (hook %2 / ot %3 / chain %4 / linear %5 / tmd %6 / hmd %7 / hmd_cand %8)")
            .arg(captureStats.primarySourceLabel())
            .arg(captureStats.directHookPrims)
            .arg(captureStats.ramOtPrims)
            .arg(captureStats.ramChainRootPrims)
            .arg(captureStats.ramLinearPrims)
            .arg(captureStats.ramTmdMeshes)
            .arg(captureStats.ramHmdMeshes)
            .arg(captureStats.ramHmdCandidates);
    m_statusLabel->setText(
        tr("Mesh %1 — captured %2 / unique %3 / instances %4 (%5 verts, %6 tris, %7 GTE matrices, "
           "%8, %9, VRAM: %10, %11)")
            .arg(captureId)
            .arg(capturedParts)
            .arg(uniqueMeshes)
            .arg(instanceCount)
            .arg(vertexCount)
            .arg(triangleCount)
            .arg(matrixCount)
            .arg(cameraText)
            .arg(matrixStats)
            .arg(vramText)
            .arg(gp0Text));
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

void PS1RipSessionWindow::rebuildRecentIsoMenu()
{
    if (!m_recentIsoMenu)
        return;

    m_recentIsoMenu->clear();
    const QString key = QString::fromLatin1(kSettingsGroup) + QLatin1Char('/') + QString::fromLatin1(kRecentIsoKey);
    const QStringList recent = QSettings().value(key).toStringList();
    if (recent.isEmpty()) {
        auto *empty = m_recentIsoMenu->addAction(tr("(none)"));
        empty->setEnabled(false);
        return;
    }

    for (const QString &path : recent) {
        const QFileInfo info(path);
        auto *act = m_recentIsoMenu->addAction(info.fileName());
        act->setToolTip(path);
        act->setData(path);
        connect(act, &QAction::triggered, this, &PS1RipSessionWindow::openRecentIso);
    }
}
