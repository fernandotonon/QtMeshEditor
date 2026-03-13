#include "ViewCubeController.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"

#include <QEvent>
#include <QWidget>
#include <cmath>

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
    // We access the orientation through animateToOrientation with 0 duration
    // to avoid needing to expose the target node directly.
    // Instead, compute the new orientation ourselves.
    Ogre::Quaternion current = cam->getOrientation();
    Ogre::Quaternion yawQ(yaw, Ogre::Vector3::UNIT_Y);
    Ogre::Quaternion pitchQ(pitch, Ogre::Vector3::UNIT_X);
    // Yaw in world space, pitch in local space
    Ogre::Quaternion newOrientation = yawQ * current * pitchQ;
    newOrientation.normalise();
    cam->animateToOrientation(newOrientation, 0.0f);
}

void ViewCubeController::setActiveWidget(OgreWidget* widget)
{
    if (m_activeWidget == widget)
        return;

    if (m_activeWidget) {
        m_activeWidget->removeEventFilter(this);
        disconnect(m_activeWidget, &QObject::destroyed, this, nullptr);
    }

    m_activeWidget = widget;

    if (m_activeWidget) {
        m_activeWidget->installEventFilter(this);
        connect(m_activeWidget, &QObject::destroyed, this, [this]() {
            m_activeWidget = nullptr;
            emit visibilityChanged(isVisible());
        });
    }

    emit visibilityChanged(isVisible());
    reposition();
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
    emit visibilityChanged(visible);
    if (visible)
        reposition();
}

bool ViewCubeController::eventFilter(QObject* obj, QEvent* event)
{
    auto type = event->type();

    if (obj == m_activeWidget &&
        (type == QEvent::Hide || type == QEvent::Close || type == QEvent::Destroy)) {
        if (type == QEvent::Close || type == QEvent::Destroy)
            m_activeWidget = nullptr;
        emit visibilityChanged(isVisible());
        return QObject::eventFilter(obj, event);
    }

    // Reposition when active widget or main window moves/resizes
    if (type == QEvent::Move || type == QEvent::Resize) {
        if (m_visible && m_activeWidget &&
            (obj == m_activeWidget || obj == m_mainWindow))
            reposition();
    }
    return QObject::eventFilter(obj, event);
}

void ViewCubeController::reposition()
{
    if (!m_activeWidget)
        return;

    // Position in top-right corner with a margin
    const int cubeSize = 64;
    const int margin = 4;
    QPoint topRight = m_activeWidget->mapToGlobal(
        QPoint(m_activeWidget->width() - cubeSize - margin, margin));

    if (m_windowX != topRight.x() || m_windowY != topRight.y()) {
        m_windowX = topRight.x();
        m_windowY = topRight.y();
        emit positionChanged();
    }
}
