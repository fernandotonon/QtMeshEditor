#include "HDR/HdrViewportOverlay.h"

#include "HDR/HdrEnvironmentController.h"
#include "OgreWidget.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLibraryInfo>
#include <QQmlEngine>
#include <QQuickWidget>
#include <QWidget>

HdrViewportOverlay::HdrViewportOverlay(QWidget* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
    if (m_mainWindow)
        m_mainWindow->installEventFilter(this);
    initWidget();
}

HdrViewportOverlay::~HdrViewportOverlay() = default;

bool HdrViewportOverlay::isVisible() const
{
    return m_visible && m_activeWidget && m_activeWidget->isVisible();
}

void HdrViewportOverlay::initWidget()
{
    m_overlayWidget = new QQuickWidget();
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    m_overlayWidget->setWindowFlags(Qt::FramelessWindowHint | Qt::SubWindow);
#else
    m_overlayWidget->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
#endif
    m_overlayWidget->setAttribute(Qt::WA_TranslucentBackground);
    m_overlayWidget->setClearColor(Qt::transparent);
    m_overlayWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_overlayWidget->setFixedSize(196, 118);
    m_overlayWidget->setAttribute(Qt::WA_ShowWithoutActivating);

    qmlRegisterSingletonType<HdrEnvironmentController>(
        "HdrEnvironment", 1, 0, "HdrEnvironmentController",
        [](QQmlEngine* engine, QJSEngine*) -> QObject* {
            auto* inst = HdrEnvironmentController::instance();
            engine->setObjectOwnership(inst, QQmlEngine::CppOwnership);
            return inst;
        });

    m_overlayWidget->engine()->addImportPath(QCoreApplication::applicationDirPath() + "/qml");
    m_overlayWidget->engine()->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));
    m_overlayWidget->setSource(QUrl(QStringLiteral("qrc:/HdrEnvironment/HdrViewportOverlay.qml")));
    m_overlayWidget->hide();

    connect(HdrEnvironmentController::instance(),
            &HdrEnvironmentController::environmentChanged,
            this,
            &HdrViewportOverlay::onEnvironmentStateChanged);
    connect(HdrEnvironmentController::instance(),
            &HdrEnvironmentController::overlayVisibleChanged,
            this,
            &HdrViewportOverlay::onEnvironmentStateChanged);
}

void HdrViewportOverlay::setActiveWidget(OgreWidget* widget)
{
    if (m_activeWidget == widget)
        return;

    if (m_activeWidget) {
        m_activeWidget->removeEventFilter(this);
        disconnect(m_activeWidget, &QObject::destroyed, this, nullptr);
    }

    m_activeWidget = widget;
    HdrEnvironmentController::instance()->setActiveWidget(widget);

    if (m_activeWidget) {
        m_activeWidget->installEventFilter(this);
        connect(m_activeWidget, &QObject::destroyed, this, [this]() {
            m_activeWidget = nullptr;
            updateWidgetVisibility();
        });
    }

    updateWidgetVisibility();
    reposition();
}

void HdrViewportOverlay::setVisible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    updateWidgetVisibility();
    emit visibilityChanged(visible);
}

void HdrViewportOverlay::reposition()
{
    if (!m_activeWidget || !m_overlayWidget)
        return;

    constexpr int kViewCubeSize = 64;
    constexpr int kMargin = 4;
    const int overlayW = m_overlayWidget->width();
    const int x = m_activeWidget->width() - overlayW - kMargin;
    const int y = kMargin + kViewCubeSize + kMargin;
    const QPoint topRight = m_activeWidget->mapToGlobal(QPoint(x, y));
    m_overlayWidget->move(topRight);
}

bool HdrViewportOverlay::eventFilter(QObject* obj, QEvent* event)
{
    const auto type = event->type();
    if (obj == m_activeWidget) {
        if (type == QEvent::Close || type == QEvent::Destroy) {
            m_activeWidget = nullptr;
            updateWidgetVisibility();
        } else if (type == QEvent::Show) {
            updateWidgetVisibility();
        } else if (type == QEvent::Move || type == QEvent::Resize) {
            if (m_visible)
                reposition();
        }
    }

    if (obj == m_mainWindow && (type == QEvent::Move || type == QEvent::Resize)) {
        if (m_visible)
            reposition();
    }

    return QObject::eventFilter(obj, event);
}

void HdrViewportOverlay::updateWidgetVisibility()
{
    if (!m_overlayWidget)
        return;

    const bool show = m_visible && m_activeWidget && m_activeWidget->isVisible()
        && HdrEnvironmentController::instance()->hasEnvironment();
    if (show) {
        reposition();
        m_overlayWidget->show();
        m_overlayWidget->raise();
    } else {
        m_overlayWidget->hide();
    }
}

void HdrViewportOverlay::onEnvironmentStateChanged()
{
    updateWidgetVisibility();
}
