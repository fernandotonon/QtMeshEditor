#include "HDR/HdrEnvironmentController.h"

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrViewportController.h"
#include "SentryReporter.h"

#include <algorithm>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QQmlEngine>
#include <QSettings>
#include <QSet>
#include <QWidget>

namespace {
constexpr QLatin1String kRecentPathsKey("HdrEnvironment/recentPaths");
constexpr int kMaxRecentPaths = 10;
} // namespace

HdrEnvironmentController* HdrEnvironmentController::s_instance = nullptr;

HdrEnvironmentController* HdrEnvironmentController::instance()
{
    if (!s_instance)
        s_instance = new HdrEnvironmentController(); // NOSONAR — singleton
    return s_instance;
}

HdrEnvironmentController* HdrEnvironmentController::qmlInstance(QQmlEngine* engine,
                                                                QJSEngine* scriptEngine)
{
    Q_UNUSED(scriptEngine)
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void HdrEnvironmentController::kill()
{
    delete s_instance; // NOSONAR — singleton
    s_instance = nullptr;
}

HdrEnvironmentController::HdrEnvironmentController(QObject* parent)
    : QObject(parent)
{
    loadRecentPaths();
    refreshBundledList();
    connectManagerSignals();
}

void HdrEnvironmentController::connectManagerSignals()
{
    auto* hdrMgr = HDREnvironmentManager::getSingleton();
    connect(hdrMgr, &HDREnvironmentManager::environmentChanged, this, [this]() {
        rebuildEnvironmentChoices();
        emit environmentChanged();
        emit overlayVisibleChanged();
    });
    connect(hdrMgr, &HDREnvironmentManager::iblPrecomputeCompleted, this, [this]() {
        emit iblReadyChanged();
    });
    connect(hdrMgr, &HDREnvironmentManager::tonemapChanged, this, &HdrEnvironmentController::tonemapChanged);
    connect(hdrMgr, &HDREnvironmentManager::skyboxDefaultChanged, this, [this]() {
        emit skyboxChanged();
        emit viewportOverridesChanged();
    });
    connect(hdrMgr, &HDREnvironmentManager::backgroundBlurChanged, this, &HdrEnvironmentController::backgroundBlurChanged);
}

void HdrEnvironmentController::loadRecentPaths()
{
    QSettings settings;
    m_recentEnvironments = settings.value(kRecentPathsKey).toStringList();
    m_recentEnvironments.removeDuplicates();
}

void HdrEnvironmentController::rememberRecentPath(const QString& resolvedPath)
{
    if (resolvedPath.isEmpty())
        return;

    m_recentEnvironments.removeAll(resolvedPath);
    m_recentEnvironments.prepend(resolvedPath);
    while (m_recentEnvironments.size() > kMaxRecentPaths)
        m_recentEnvironments.removeLast();

    QSettings settings;
    settings.setValue(kRecentPathsKey, m_recentEnvironments);
    rebuildEnvironmentChoices();
}

void HdrEnvironmentController::rebuildEnvironmentChoices()
{
    QStringList labels;
    QStringList keys;
    QSet<QString> seenLabels;

    auto appendChoice = [&](const QString& loadKey, const QString& label) {
        if (label.isEmpty() || seenLabels.contains(label))
            return;
        seenLabels.insert(label);
        labels.append(label);
        keys.append(loadKey);
    };

    for (const QString& bundled : m_bundledEnvironments)
        appendChoice(bundled, bundled);

    for (const QString& path : m_recentEnvironments) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            continue;
        const QString label = info.fileName();
        if (m_bundledEnvironments.contains(label))
            continue;
        appendChoice(info.absoluteFilePath(), label);
    }

    const QString current = currentEnvironment();
    if (!current.isEmpty()) {
        const QFileInfo currentInfo(current);
        const QString label = currentInfo.fileName();
        bool alreadyListed = false;
        for (int i = 0; i < keys.size(); ++i) {
            const QFileInfo keyInfo(keys[i]);
            if (keyInfo.isAbsolute()) {
                if (keyInfo.canonicalFilePath() == currentInfo.canonicalFilePath()) {
                    alreadyListed = true;
                    break;
                }
            } else if (keys[i] == label || keys[i] == current) {
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed)
            appendChoice(currentInfo.isAbsolute() ? currentInfo.absoluteFilePath() : current, label);
    }

    if (labels == m_environmentChoices && keys == m_choiceLoadKeys)
        return;

    m_environmentChoices = labels;
    m_choiceLoadKeys = keys;
    emit environmentChoicesChanged();
}

void HdrEnvironmentController::refreshBundledList()
{
    m_bundledEnvironments = HDREnvironmentManager::listBundledEnvironments();
    rebuildEnvironmentChoices();
}

int HdrEnvironmentController::currentChoiceIndex() const
{
    const QString current = currentEnvironment();
    if (current.isEmpty())
        return -1;

    const QFileInfo currentInfo(current);
    const QString currentCanon = currentInfo.canonicalFilePath();

    for (int i = 0; i < m_choiceLoadKeys.size(); ++i) {
        const QString& key = m_choiceLoadKeys[i];
        const QFileInfo keyInfo(key);
        if (keyInfo.isAbsolute()) {
            if (keyInfo.canonicalFilePath() == currentCanon)
                return i;
        } else if (key == currentInfo.fileName() || key == current) {
            return i;
        }
    }
    return -1;
}

QString HdrEnvironmentController::currentEnvironment() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->currentEnvironment();
    return {};
}

QString HdrEnvironmentController::currentEnvironmentLabel() const
{
    const QString path = currentEnvironment();
    if (path.isEmpty())
        return QStringLiteral("(none)");
    return QFileInfo(path).fileName();
}

bool HdrEnvironmentController::hasEnvironment() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->hasEnvironment();
    return false;
}

bool HdrEnvironmentController::iblReady() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->isIblReady();
    return false;
}

int HdrEnvironmentController::tonemapOperator() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return static_cast<int>(hdrMgr->tonemapOperator());
    return static_cast<int>(HdrTonemap::Operator::ACES);
}

void HdrEnvironmentController::setTonemapOperator(int op)
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (!hdrMgr)
        return;
    const auto clamped = static_cast<HdrTonemap::Operator>(std::clamp(op, 0, 2));
    hdrMgr->setTonemapOperator(clamped);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                   QStringLiteral("hdr.tonemapOperator=%1").arg(op));
    emit tonemapChanged();
}

float HdrEnvironmentController::exposureEv() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->exposureEv();
    return 0.f;
}

void HdrEnvironmentController::setExposureEv(float value)
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr()) {
        hdrMgr->setExposureEv(value);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                       QStringLiteral("hdr.exposureEv=%1").arg(value));
        emit tonemapChanged();
    }
}

float HdrEnvironmentController::whitePoint() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->whitePoint();
    return 1.f;
}

void HdrEnvironmentController::setWhitePoint(float value)
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr()) {
        hdrMgr->setWhitePoint(value);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                       QStringLiteral("hdr.whitePoint=%1").arg(value));
        emit tonemapChanged();
    }
}

bool HdrEnvironmentController::defaultSkyBoxVisible() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->defaultSkyBoxVisible();
    return true;
}

void HdrEnvironmentController::setDefaultSkyBoxVisible(bool visible)
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr()) {
        hdrMgr->setDefaultSkyBoxVisible(visible);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                       QStringLiteral("hdr.skyboxDefault=%1").arg(visible));
        emit skyboxChanged();
    }
}

float HdrEnvironmentController::backgroundBlur() const
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr())
        return hdrMgr->backgroundBlur();
    return 0.f;
}

void HdrEnvironmentController::setBackgroundBlur(float blur)
{
    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr()) {
        hdrMgr->setBackgroundBlur(blur);
        emit backgroundBlurChanged();
    }
}

OgreWidget* HdrEnvironmentController::activeWidget() const
{
    if (m_activeWidget)
        return m_activeWidget;
    if (auto* ctrl = HdrViewportController::getSingletonPtr())
        return ctrl->activeWidget();
    return nullptr;
}

bool HdrEnvironmentController::activeSkyBoxVisible() const
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr())
            return ctrl->skyBoxVisible(widget);
    }
    return defaultSkyBoxVisible();
}

void HdrEnvironmentController::setActiveSkyBoxVisible(bool visible)
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr()) {
            ctrl->setSkyBoxVisible(widget, visible);
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                           QStringLiteral("hdr.viewportSkybox=%1").arg(visible));
            emit viewportOverridesChanged();
        }
    }
}

bool HdrEnvironmentController::activeTonemapOverride() const
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr())
            return ctrl->tonemapOverride(widget);
    }
    return false;
}

void HdrEnvironmentController::setActiveTonemapOverride(bool enabled)
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr()) {
            ctrl->setTonemapOverride(widget, enabled);
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                           QStringLiteral("hdr.viewportTonemapOverride=%1")
                                               .arg(enabled));
            emit viewportOverridesChanged();
        }
    }
}

int HdrEnvironmentController::activeTonemapOperator() const
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr())
            return static_cast<int>(ctrl->tonemapOperator(widget));
    }
    return tonemapOperator();
}

void HdrEnvironmentController::setActiveTonemapOperator(int op)
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr()) {
            ctrl->setTonemapOperator(widget, static_cast<HdrTonemap::Operator>(std::clamp(op, 0, 2)));
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                           QStringLiteral("hdr.viewportTonemapOperator=%1").arg(op));
            emit viewportOverridesChanged();
        }
    }
}

float HdrEnvironmentController::activeExposureEv() const
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr())
            return ctrl->exposureEv(widget);
    }
    return exposureEv();
}

void HdrEnvironmentController::setActiveExposureEv(float value)
{
    if (auto* widget = activeWidget()) {
        if (auto* ctrl = HdrViewportController::getSingletonPtr()) {
            ctrl->setExposureEv(widget, value);
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                           QStringLiteral("hdr.viewportExposureEv=%1").arg(value));
            emit viewportOverridesChanged();
        }
    }
}

bool HdrEnvironmentController::overlayVisible() const
{
    return hasEnvironment();
}

bool HdrEnvironmentController::loadEnvironment(const QString& pathOrBundledName)
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (!hdrMgr)
        return false;
    const bool ok = hdrMgr->loadEnvironment(pathOrBundledName);
    if (ok) {
        rememberRecentPath(hdrMgr->currentEnvironment());
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                       QStringLiteral("hdr.loadEnvironment=%1")
                                           .arg(QFileInfo(pathOrBundledName).fileName()));
    }
    return ok;
}

bool HdrEnvironmentController::loadEnvironmentChoice(int index)
{
    if (index < 0 || index >= m_choiceLoadKeys.size())
        return false;
    return loadEnvironment(m_choiceLoadKeys[index]);
}

QString HdrEnvironmentController::browseStartDirectory() const
{
    const QString current = currentEnvironment();
    if (!current.isEmpty()) {
        const QFileInfo info(current);
        if (info.isAbsolute())
            return info.absolutePath();
    }
    if (!m_recentEnvironments.isEmpty()) {
        const QFileInfo info(m_recentEnvironments.first());
        if (info.exists())
            return info.absolutePath();
    }
    return QString();
}

void HdrEnvironmentController::browseForEnvironment()
{
    // Match VATBakerController::chooseOutputDir / MaterialEditorQML file
    // pickers: raise the active window and parent the dialog to it. Parenting
    // to MainWindow from a deferred slot centers a window-modal Qt dialog on
    // the main frame and drags the whole app when moved.
    QApplication::processEvents();
    QWidget* parent = QApplication::activeWindow();
    if (parent) {
        parent->raise();
        parent->activateWindow();
    }
    QApplication::processEvents();

    QFileDialog::Options options = QFileDialog::DontUseCustomDirectoryIcons;
#ifdef Q_OS_MACOS
    // Native pickers hosted from QQuickWidget have been observed to no-op on macOS.
    options |= QFileDialog::DontUseNativeDialog;
#endif

    const QString path = QFileDialog::getOpenFileName(
        parent,
        tr("Select HDR Environment"),
        browseStartDirectory(),
        tr("HDR Images (*.hdr *.exr);;All Files (*)"),
        nullptr,
        options);

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  path.isEmpty()
                                      ? QStringLiteral("hdr.browseCancelled")
                                      : QStringLiteral("hdr.browseAccepted=%1")
                                            .arg(QFileInfo(path).fileName()));
    completeBrowseFromDialog(path);
}

QString HdrEnvironmentController::browseEnvironment()
{
    browseForEnvironment();
    return {};
}

void HdrEnvironmentController::completeBrowseFromDialog(const QString& path)
{
    if (path.isEmpty())
        return;
    loadEnvironment(path);
}

void HdrEnvironmentController::resetTonemap()
{
    setTonemapOperator(static_cast<int>(HdrTonemap::Operator::ACES));
    setExposureEv(0.f);
    setWhitePoint(1.f);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("hdr.resetTonemap"));
}

void HdrEnvironmentController::setActiveWidget(OgreWidget* widget)
{
    m_activeWidget = widget;
    if (auto* ctrl = HdrViewportController::getSingletonPtr())
        ctrl->setActiveWidget(widget);
    emit viewportOverridesChanged();
    emit overlayVisibleChanged();
}

QString HdrEnvironmentController::tonemapOperatorName(int op) const
{
    switch (static_cast<HdrTonemap::Operator>(std::clamp(op, 0, 2))) {
    case HdrTonemap::Operator::Reinhard:
        return QStringLiteral("Reinhard");
    case HdrTonemap::Operator::AgX:
        return QStringLiteral("AgX");
    case HdrTonemap::Operator::ACES:
    default:
        return QStringLiteral("ACES");
    }
}
