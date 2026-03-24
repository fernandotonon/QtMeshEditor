#include "ViewCubeController.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLibraryInfo>
#include <QQuickWidget>
#include <QQmlEngine>
#include <QWidget>
#include <cmath>

#ifdef Q_OS_MACOS
#include <objc/message.h>
#include <objc/runtime.h>
#endif

ViewCubeController* ViewCubeController::s_instance = nullptr;

ViewCubeController::ViewCubeController(QWidget* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
    s_instance = this;
    if (m_mainWindow)
        m_mainWindow->installEventFilter(this);
}

ViewCubeController::~ViewCubeController()
{
    if (s_instance == this)
        s_instance = nullptr;
}

ViewCubeController* ViewCubeController::instance()
{
    return s_instance;
}

ViewCubeController* ViewCubeController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)

    if (!s_instance)
        s_instance = new ViewCubeController(nullptr);
    return s_instance;
}

// LCOV_EXCL_START — QQuickWidget creation requires native window + display
void ViewCubeController::initWidget()
{
    m_cubeWidget = new QQuickWidget();
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    // On Linux/Windows, Qt::Tool stays on top of all app windows.
    // Use Qt::SubWindow to avoid taskbar entry without staying on top.
    m_cubeWidget->setWindowFlags(Qt::FramelessWindowHint | Qt::SubWindow);
#else
    m_cubeWidget->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
#endif
    m_cubeWidget->setAttribute(Qt::WA_TranslucentBackground);
    m_cubeWidget->setClearColor(Qt::transparent);
    m_cubeWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_cubeWidget->setFixedSize(64, 64);

    qmlRegisterSingletonType<ViewCubeController>("ViewCubeModule", 1, 0, "ViewCubeController",
        [](QQmlEngine* engine, QJSEngine*) -> QObject* {
            auto* inst = ViewCubeController::instance();
            engine->setObjectOwnership(inst, QQmlEngine::CppOwnership);
            return inst;
        });

    m_cubeWidget->engine()->addImportPath(QCoreApplication::applicationDirPath() + "/qml");
    m_cubeWidget->engine()->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));

    m_cubeWidget->setSource(QUrl("qrc:/ViewCube/ViewCubeWindow.qml"));

    // Show briefly to create the native window, then hide
    m_cubeWidget->show();

#ifdef Q_OS_MACOS
    // Qt::Tool creates an NSPanel at NSFloatingWindowLevel on macOS, causing
    // it to float above ALL app windows (including material editor, dialogs).
    // Lower to NSNormalWindowLevel (0) so other windows can appear above the
    // cube when focused, while still keeping Qt::Tool benefits (no Dock icon,
    // groups with parent app, auto-hides when app loses focus).
    {
        using GetWindowFn = id (*)(id, SEL);
        using SetLevelFn = void (*)(id, SEL, long);

        auto nsView = reinterpret_cast<id>(m_cubeWidget->winId());
        auto nsWindow = reinterpret_cast<GetWindowFn>(objc_msgSend)(
            nsView, sel_registerName("window"));
        reinterpret_cast<SetLevelFn>(objc_msgSend)(
            nsWindow, sel_registerName("setLevel:"), 0L);
    }
#endif

    m_cubeWidget->hide();
}
// LCOV_EXCL_STOP

bool ViewCubeController::isVisible() const
{
    return m_visible && m_activeWidget && m_activeWidget->isVisible();
}

SpaceCamera* ViewCubeController::activeCamera() const
{
    if (!m_activeWidget)
        return nullptr;
    return m_activeWidget->getSpaceCamera();
}

void ViewCubeController::snapToView(const QString& face)
{
    SpaceCamera* cam = activeCamera();
    if (!cam)
        return;

    // View quaternions for mTarget scene node (Y-up, camera at local Z=-20)
    // "Left" places the camera on the -X side, "Right" on the +X side
    struct ViewDef { const char* name; qreal w, x, y, z; };
    static const ViewDef views[] = {
        { "Front",  1.0,    0.0,    0.0,    0.0    },
        { "Back",   0.0,    0.0,    1.0,    0.0    },
        { "Left",   0.7071, 0.0,   -0.7071, 0.0    },
        { "Right",  0.7071, 0.0,    0.7071, 0.0    },
        { "Top",    0.7071, 0.7071, 0.0,    0.0    },
        { "Bottom", 0.7071,-0.7071, 0.0,    0.0    },
    };

    for (const auto& v : views) {
        if (face.compare(v.name, Qt::CaseInsensitive) == 0) {
            cam->animateToOrientation(Ogre::Quaternion(
                static_cast<Ogre::Real>(v.w),
                static_cast<Ogre::Real>(v.x),
                static_cast<Ogre::Real>(v.y),
                static_cast<Ogre::Real>(v.z)));
            return;
        }
    }
}

void ViewCubeController::snapToDirection(qreal dx, qreal dy, qreal dz)
{
    SpaceCamera* cam = activeCamera();
    if (!cam)
        return;

    // Compute quaternion that rotates the reference direction (0,0,-1) to (dx,dy,dz)
    Ogre::Vector3 from(0, 0, -1);
    Ogre::Vector3 to(static_cast<Ogre::Real>(dx),
                     static_cast<Ogre::Real>(dy),
                     static_cast<Ogre::Real>(dz));
    to.normalise();
    Ogre::Quaternion target = from.getRotationTo(to);
    cam->animateToOrientation(target);
}

void ViewCubeController::rotateByDelta(qreal dx, qreal dy)
{
    SpaceCamera* cam = activeCamera();
    if (!cam)
        return;

    // Cancel any running animation so the drag feels immediate
    if (cam->isAnimating())
        cam->animateToOrientation(cam->getOrientation(), 0.0f);

    // Scale factor to make drag feel natural on the small cube
    Ogre::Real scaledDx = static_cast<Ogre::Real>(dx) * 3.0f;
    Ogre::Real scaledDy = static_cast<Ogre::Real>(dy) * 3.0f;

    Ogre::Radian yaw(-scaledDx * 0.05f);
    Ogre::Radian pitch(scaledDy * 0.05f);

    // Apply arcball rotation (same logic as SpaceCamera::arcBall)
    Ogre::Quaternion current = cam->getOrientation();
    Ogre::Quaternion yawQ(yaw, Ogre::Vector3::UNIT_Y);
    Ogre::Quaternion pitchQ(pitch, Ogre::Vector3::UNIT_X);
    // Yaw in world space, pitch in local space
    Ogre::Quaternion newOrientation = yawQ * current * pitchQ;
    newOrientation.normalise();
    cam->animateToOrientation(newOrientation, 0.0f);
}

// LCOV_EXCL_START — widget positioning/visibility requires native window + display
void ViewCubeController::setActiveWidget(OgreWidget* widget)
{
    if (m_activeWidget == widget) {
        // Same widget got focus again — raise the cube above the main window
        if (m_cubeWidget && isVisible())
            m_cubeWidget->raise();
        return;
    }

    if (m_activeWidget) {
        m_activeWidget->removeEventFilter(this);
        disconnect(m_activeWidget, &QObject::destroyed, this, nullptr);
    }

    m_activeWidget = widget;

    if (m_activeWidget) {
        m_activeWidget->installEventFilter(this);
        connect(m_activeWidget, &QObject::destroyed, this, [this]() {
            m_activeWidget = nullptr;
            updateWidgetVisibility();
        });
    }

    updateWidgetVisibility();
    updateOrientation();
}

void ViewCubeController::updateOrientation()
{
    // Keep position in sync every frame (handles deferred layout, window moves, etc.)
    if (m_visible)
        reposition();

    SpaceCamera* cam = activeCamera();
    if (!cam)
        return;

    const Ogre::Quaternion& q = cam->getOrientation();
    if (m_qw != q.w || m_qx != q.x || m_qy != q.y || m_qz != q.z) {
        m_qw = q.w;
        m_qx = q.x;
        m_qy = q.y;
        m_qz = q.z;
        emit orientationChanged();
    }
}

void ViewCubeController::setVisible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    updateWidgetVisibility();
    emit visibilityChanged(visible);
}

bool ViewCubeController::eventFilter(QObject* obj, QEvent* event)
{
    auto type = event->type();

    if (obj == m_activeWidget) {
        if (type == QEvent::Close || type == QEvent::Destroy) {
            m_activeWidget = nullptr;
            updateWidgetVisibility();
            return QObject::eventFilter(obj, event);
        }
        if (type == QEvent::Show) {
            updateWidgetVisibility();
            return QObject::eventFilter(obj, event);
        }

        // Reposition when active widget moves/resizes
        if (type == QEvent::Move || type == QEvent::Resize) {
            if (m_visible)
                reposition();
        }

        // Any mouse interaction with the viewport brings the main window to
        // front at NSNormalWindowLevel, pushing the cube behind it.  Re-raise
        // the cube so it stays visible over the viewport.
        if (type == QEvent::MouseButtonPress || type == QEvent::Wheel) {
            if (m_cubeWidget && isVisible())
                m_cubeWidget->raise();
        }
    }

    // Raise cube when main window regains focus (e.g. switching back from material editor)
    if (obj == m_mainWindow && type == QEvent::WindowActivate) {
        if (m_cubeWidget && isVisible())
            m_cubeWidget->raise();
    }

    return QObject::eventFilter(obj, event);
}

void ViewCubeController::reposition()
{
    if (!m_activeWidget || !m_cubeWidget)
        return;

    // Position in top-right corner of the active viewport
    const int cubeSize = 64;
    const int margin = 4;
    QPoint topRight = m_activeWidget->mapToGlobal(
        QPoint(m_activeWidget->width() - cubeSize - margin, margin));

    m_cubeWidget->move(topRight);
}

void ViewCubeController::updateWidgetVisibility()
{
    if (!m_cubeWidget)
        return;

    if (isVisible()) {
        reposition();
        m_cubeWidget->show();
        m_cubeWidget->raise();
    } else {
        m_cubeWidget->hide();
    }
}
// LCOV_EXCL_STOP
