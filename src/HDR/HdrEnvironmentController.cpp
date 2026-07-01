#include "HDR/HdrEnvironmentController.h"

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrViewportController.h"
#include "SentryReporter.h"

#include <algorithm>
#include <QFileDialog>
#include <QFileInfo>
#include <QQmlEngine>
#include <QWidget>

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
    refreshBundledList();
    connectManagerSignals();
}

void HdrEnvironmentController::connectManagerSignals()
{
    auto* hdrMgr = HDREnvironmentManager::getSingleton();
    connect(hdrMgr, &HDREnvironmentManager::environmentChanged, this, [this]() {
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

void HdrEnvironmentController::refreshBundledList()
{
    m_bundledEnvironments = HDREnvironmentManager::listBundledEnvironments();
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
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                       QStringLiteral("hdr.loadEnvironment=%1")
                                           .arg(QFileInfo(pathOrBundledName).fileName()));
    }
    return ok;
}

QString HdrEnvironmentController::browseEnvironment()
{
    QWidget* parent = nullptr;
    const QString path = QFileDialog::getOpenFileName(
        parent,
        QObject::tr("Select HDR Environment"),
        QString(),
        QObject::tr("HDR Images (*.hdr *.exr);;All Files (*)"));
    if (path.isEmpty())
        return {};
    if (loadEnvironment(path))
        return path;
    return {};
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
