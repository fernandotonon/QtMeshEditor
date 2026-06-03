#include "CloudAccountMenuButton.h"

#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"
#include "SentryReporter.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace {

QString cloudDisplayName()
{
    QSettings settings;
    QString display = settings.value(AppSettingsKeys::cloudUserName()).toString().trimmed();
    if (display.isEmpty())
        display = settings.value(AppSettingsKeys::cloudUserSlug()).toString().trimmed();
    if (display.isEmpty())
        display = CloudCredentialStore::loadSession().email.trimmed();
    return display;
}

} // namespace

class CloudAccountMenuButton::AvatarButton : public QToolButton {
public:
    explicit AvatarButton(QWidget* parent = nullptr)
        : QToolButton(parent)
    {
        setFixedSize(28, 28);
        setAutoRaise(true);
        setPopupMode(QToolButton::InstantPopup);
        setToolButtonStyle(Qt::ToolButtonIconOnly);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet(QStringLiteral(
            "QToolButton { padding: 0; border: none; background: transparent; }"
            "QToolButton:hover { background: palette(midlight); border-radius: 14px; }"
            "QToolButton:pressed { background: palette(mid); border-radius: 14px; }"));
    }

    void setSignedIn(bool signedIn, const QString& initials)
    {
        m_signedIn = signedIn;
        m_initials = initials;
        if (signedIn && !initials.isEmpty()) {
            setIcon(QIcon());
            setText(QString());
            setToolButtonStyle(Qt::ToolButtonIconOnly);
        } else {
            setText(QString());
            setToolButtonStyle(Qt::ToolButtonIconOnly);
            if (m_loggedOutIcon.isNull())
                m_loggedOutIcon = QIcon(QStringLiteral(":/icones/cloud_account_user.svg"));
            setIcon(m_loggedOutIcon);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        if (m_signedIn && !m_initials.isEmpty()) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);

            const QRect r = rect().adjusted(2, 2, -2, -2);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x4a, 0x7a, 0xa8));
            painter.drawEllipse(r);
            QFont font = painter.font();
            font.setBold(true);
            font.setPixelSize(10);
            painter.setFont(font);
            painter.setPen(QColor(0xf0, 0xf4, 0xf8));
            painter.drawText(r, Qt::AlignCenter, m_initials);
            paintStatusBadge(painter);
            return;
        }

        if (m_signedIn) {
            if (m_loggedOutIcon.isNull())
                m_loggedOutIcon = QIcon(QStringLiteral(":/icones/cloud_account_user.svg"));
            const QRect r = rect().adjusted(2, 2, -2, -2);
            const QPixmap pix = m_loggedOutIcon.pixmap(r.size());
            if (!pix.isNull()) {
                QPainter painter(this);
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.drawPixmap(r, pix);
                paintStatusBadge(painter);
                return;
            }
        }

        QToolButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintStatusBadge(painter);
    }

private:
    void paintStatusBadge(QPainter& painter) const
    {
        const int badgeD = 7;
        const QRect badge(rect().right() - badgeD - 1,
                          rect().bottom() - badgeD - 1,
                          badgeD,
                          badgeD);
        painter.setPen(QPen(QColor(0x2b, 0x2b, 0x2b), 1.5));
        painter.setBrush(m_signedIn ? QColor(0x4c, 0xaf, 0x50) : QColor(0x6e, 0x6e, 0x6e));
        painter.drawEllipse(badge);
    }

private:
    bool m_signedIn = false;
    QString m_initials;
    QIcon m_loggedOutIcon;
};

QString CloudAccountMenuButton::initialsFromDisplayName(const QString& displayName)
{
    const QString trimmed = displayName.trimmed();
    if (trimmed.isEmpty())
        return QString();

    const QStringList parts =
        trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return QString();

    auto firstChar = [](const QString& word) {
        for (const QChar ch : word) {
            if (ch.isLetter())
                return ch.toUpper();
        }
        return QChar();
    };

    if (parts.size() == 1) {
        const QChar a = firstChar(parts.front());
        return a.isNull() ? QString() : QString(a);
    }

    const QChar first = firstChar(parts.front());
    const QChar last = firstChar(parts.back());
    if (first.isNull() && last.isNull())
        return QString();
    if (first.isNull())
        return QString(last);
    if (last.isNull())
        return QString(first);
    return QString(first) + last;
}

CloudAccountMenuButton::CloudAccountMenuButton(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_button = new AvatarButton(this);
    m_button->setObjectName(QStringLiteral("cloudAccountButton"));

    m_menu = new QMenu(this);
    m_menu->setObjectName(QStringLiteral("menuCloud"));
    applyMenuStyle();
    buildMenu();

    m_button->setMenu(m_menu);
    layout->addWidget(m_button);

    connect(m_menu, &QMenu::aboutToShow, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar menu opened"));
        refresh();
    });

    refresh();
}

void CloudAccountMenuButton::applyMenuStyle()
{
    m_menu->setStyleSheet(QStringLiteral(
        "QMenu {"
        "  background-color: #2b2b2b;"
        "  border: 1px solid #3d3d3d;"
        "  padding: 4px 0;"
        "}"
        "QMenu::item {"
        "  padding: 7px 20px;"
        "  color: #e0e0e0;"
        "}"
        "QMenu::item:selected {"
        "  background-color: #3a3a3a;"
        "}"
        "QMenu::item:disabled {"
        "  color: #9a9a9a;"
        "}"
        "QMenu::separator {"
        "  height: 1px;"
        "  background: #3d3d3d;"
        "  margin: 5px 10px;"
        "}"));
}

void CloudAccountMenuButton::buildMenu()
{
    m_headerWidget = new QWidget(m_menu);
    m_headerWidget->setObjectName(QStringLiteral("cloudAccountMenuHeader"));
    auto* headerLayout = new QVBoxLayout(m_headerWidget);
    headerLayout->setContentsMargins(14, 10, 14, 8);
    headerLayout->setSpacing(2);

    m_headerNameLabel = new QLabel(m_headerWidget);
    m_headerNameLabel->setObjectName(QStringLiteral("cloudAccountMenuHeaderName"));
    m_headerNameLabel->setStyleSheet(QStringLiteral(
        "color: #ececec; font-size: 13px; font-weight: 600; background: transparent;"));
    m_headerNameLabel->setWordWrap(true);

    m_headerSubtitleLabel = new QLabel(tr("QtMesh Cloud account"), m_headerWidget);
    m_headerSubtitleLabel->setObjectName(QStringLiteral("cloudAccountMenuHeaderSubtitle"));
    m_headerSubtitleLabel->setStyleSheet(QStringLiteral(
        "color: #9a9a9a; font-size: 11px; background: transparent;"));

    headerLayout->addWidget(m_headerNameLabel);
    headerLayout->addWidget(m_headerSubtitleLabel);

    m_headerAction = new QWidgetAction(m_menu);
    m_headerAction->setDefaultWidget(m_headerWidget);
    m_headerAction->setEnabled(false);
    m_menu->addAction(m_headerAction);
    m_headerSeparator = m_menu->addSeparator();

    m_openProjectsAction = m_menu->addAction(tr("Open My Projects"));
    m_openProjectsAction->setObjectName(QStringLiteral("actionQtMeshCloudOpenProjects"));
    connect(m_openProjectsAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: Open My Projects"));
        emit openProjectsRequested();
    });

    m_uploadAction = m_menu->addAction(tr("Upload Files..."));
    m_uploadAction->setObjectName(QStringLiteral("actionQtMeshCloudUploadFiles"));
    connect(m_uploadAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: Upload Files"));
        emit uploadFilesRequested();
    });

    m_mainSeparator = m_menu->addSeparator();

    m_signOutAction = m_menu->addAction(tr("Sign out"));
    m_signOutAction->setObjectName(QStringLiteral("actionQtMeshCloudSignOut"));
    connect(m_signOutAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: Sign out"));
        emit signOutRequested();
    });

    m_signInAction = m_menu->addAction(tr("Sign in to QtMesh Cloud"));
    m_signInAction->setObjectName(QStringLiteral("actionQtMeshCloudSignIn"));
    connect(m_signInAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: Sign in"));
        emit signInRequested();
    });
}

void CloudAccountMenuButton::updateHeader(const QString& displayName, bool signedIn)
{
    if (const bool showHeader = signedIn && !displayName.isEmpty(); showHeader) {
        m_headerNameLabel->setText(displayName);
        m_headerSubtitleLabel->setText(tr("Signed in to QtMesh Cloud"));

        // Ensure the header is actually present in the menu, otherwise the menu can
        // end up drawing stale pixels behind items when we toggle auth state while
        // the menu is open.
        if (!m_menu->actions().contains(m_headerAction))
            m_menu->insertAction(m_openProjectsAction, m_headerAction);
        if (!m_menu->actions().contains(m_headerSeparator))
            m_menu->insertAction(m_openProjectsAction, m_headerSeparator);
    } else {
        m_headerNameLabel->clear();
        if (m_menu->actions().contains(m_headerAction))
            m_menu->removeAction(m_headerAction);
        if (m_menu->actions().contains(m_headerSeparator))
            m_menu->removeAction(m_headerSeparator);
    }

    m_menu->updateGeometry();
    m_menu->adjustSize();
    m_menu->update();
}

void CloudAccountMenuButton::refresh()
{
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const bool signedIn = CloudCredentialStore::hasSession();
    const QString display = cloudDisplayName();

    if (signedIn && !display.isEmpty()) {
        m_button->setToolTip(
            tr("QtMesh Cloud: signed in as %1").arg(display));
    } else {
        m_button->setToolTip(tr("Sign in to QtMesh Cloud"));
    }

    const QString initials = signedIn ? initialsFromDisplayName(display) : QString();
    if (auto* avatar = dynamic_cast<AvatarButton*>(m_button))
        avatar->setSignedIn(signedIn, initials);

    updateHeader(display, signedIn);

    m_openProjectsAction->setEnabled(signedIn);
    m_uploadAction->setEnabled(true);

    m_signInAction->setVisible(!signedIn);
    m_signInAction->setEnabled(!signedIn);
    m_signOutAction->setVisible(signedIn);
    m_signOutAction->setEnabled(signedIn);
}
