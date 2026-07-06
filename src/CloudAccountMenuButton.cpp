#include "CloudAccountMenuButton.h"

#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"
#include "GamificationManager.h"
#include "GamificationTypes.h"
#include "SentryReporter.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
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
        // The avatar (both states) is drawn directly in paintEvent — never via
        // setIcon. An earlier version used an SVG QIcon for the logged-out
        // state, which renders blank in deployed builds that don't bundle the
        // Qt SVG icon-engine plugin (the icon showed locally but vanished in
        // the installed app). Painting the glyph ourselves has no plugin
        // dependency and looks identical on every platform.
        setIcon(QIcon());
        setText(QString());
        setToolButtonStyle(Qt::ToolButtonIconOnly);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QToolButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect r = rect().adjusted(2, 2, -2, -2);

        if (m_signedIn && !m_initials.isEmpty()) {
            // Signed-in with known initials: filled circle + initials.
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x4a, 0x7a, 0xa8));
            painter.drawEllipse(r);
            QFont font = painter.font();
            font.setBold(true);
            font.setPixelSize(10);
            painter.setFont(font);
            painter.setPen(QColor(0xf0, 0xf4, 0xf8));
            painter.drawText(r, Qt::AlignCenter, m_initials);
        } else {
            // Generic person glyph (head + shoulders) for the logged-out
            // state, or signed-in without resolvable initials.
            paintPersonGlyph(painter, r);
        }

        paintStatusBadge(painter);
    }

private:
    // Draws a simple head-and-shoulders silhouette inside @p r, matching the
    // look of the previous cloud_account_user.svg without needing the SVG
    // plugin to be deployed.
    void paintPersonGlyph(QPainter& painter, const QRect& r) const
    {
        const QColor glyph = m_signedIn ? QColor(0x4a, 0x7a, 0xa8)
                                        : QColor(0xb0, 0xb0, 0xb0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glyph);

        const qreal w = r.width();
        const qreal h = r.height();
        const QPointF c = QPointF(r.center()) + QPointF(0.5, 0.5);

        // Head: circle in the upper third.
        const qreal headD = w * 0.40;
        const QRectF head(c.x() - headD / 2.0,
                          r.top() + h * 0.12,
                          headD, headD);
        painter.drawEllipse(head);

        // Shoulders: a wide rounded cap at the bottom (clipped to the button).
        const qreal shW = w * 0.70;
        const qreal shH = h * 0.55;
        const QRectF shoulders(c.x() - shW / 2.0,
                               r.top() + h * 0.52,
                               shW, shH);
        painter.drawRoundedRect(shoulders, shW * 0.5, shW * 0.5);
    }

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
        // Opportunistic stats refresh so the status block stays current
        // without polling (renders last-cached immediately, updates async).
        if (CloudCredentialStore::hasSession())
            GamificationManager::instance()->refreshStatsIfStale();
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

    // ---- Gamification status block (E-P4 #800) ----
    m_gamifyWidget = new QWidget(m_menu);
    m_gamifyWidget->setObjectName(QStringLiteral("cloudAccountGamifyStatus"));
    auto* gamifyLayout = new QVBoxLayout(m_gamifyWidget);
    gamifyLayout->setContentsMargins(14, 6, 14, 8);
    gamifyLayout->setSpacing(3);

    m_gamifyLevelLabel = new QLabel(m_gamifyWidget);
    m_gamifyLevelLabel->setObjectName(QStringLiteral("cloudAccountGamifyLevel"));
    m_gamifyLevelLabel->setStyleSheet(QStringLiteral(
        "color: #ececec; font-size: 12px; font-weight: 600; background: transparent;"));

    const QString barStyle = QStringLiteral(
        "QProgressBar { background: #1f1f1f; border: none; border-radius: 2px;"
        "  min-height: 4px; max-height: 4px; }"
        "QProgressBar::chunk { background: #4a7aa8; border-radius: 2px; }");
    m_gamifyXpBar = new QProgressBar(m_gamifyWidget);
    m_gamifyXpBar->setObjectName(QStringLiteral("cloudAccountGamifyXpBar"));
    m_gamifyXpBar->setTextVisible(false);
    m_gamifyXpBar->setStyleSheet(barStyle);

    m_gamifyNextLabel = new QLabel(m_gamifyWidget);
    m_gamifyNextLabel->setObjectName(QStringLiteral("cloudAccountGamifyNext"));
    m_gamifyNextLabel->setStyleSheet(QStringLiteral(
        "color: #9a9a9a; font-size: 11px; background: transparent;"));
    m_gamifyNextLabel->setWordWrap(true);

    m_gamifyNextBar = new QProgressBar(m_gamifyWidget);
    m_gamifyNextBar->setObjectName(QStringLiteral("cloudAccountGamifyNextBar"));
    m_gamifyNextBar->setTextVisible(false);
    m_gamifyNextBar->setStyleSheet(barStyle);

    gamifyLayout->addWidget(m_gamifyLevelLabel);
    gamifyLayout->addWidget(m_gamifyXpBar);
    gamifyLayout->addSpacing(4);
    gamifyLayout->addWidget(m_gamifyNextLabel);
    gamifyLayout->addWidget(m_gamifyNextBar);

    // Like the header above, the gamification entries are physically added /
    // removed from the menu in updateGamificationSection() — QMenu (macOS in
    // particular) keeps painting hidden QWidgetActions and mis-tracks item
    // hover geometry when actions are merely setVisible(false).
    m_gamifyAction = new QWidgetAction(m_menu);
    m_gamifyAction->setDefaultWidget(m_gamifyWidget);
    m_gamifyAction->setEnabled(false);

    m_achievementsAction = new QAction(tr("View My Achievements…"), m_menu);
    m_achievementsAction->setObjectName(QStringLiteral("actionQtMeshCloudAchievements"));
    connect(m_achievementsAction, &QAction::triggered, this, []() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: View Achievements"));
        GamificationManager::instance()->openProfile();
    });

    m_enableSyncAction = new QAction(tr("Enable Progress Sync…"), m_menu);
    m_enableSyncAction->setObjectName(QStringLiteral("actionQtMeshCloudEnableSync"));
    connect(m_enableSyncAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: Enable Progress Sync"));
        GamificationManager::instance()->acceptConsent();
        GamificationManager::instance()->refreshStats();
        refresh();
    });

    m_gamifySeparator = new QAction(m_menu);
    m_gamifySeparator->setSeparator(true);

    m_openProjectsAction = m_menu->addAction(tr("My Cloud Projects…"));
    m_openProjectsAction->setObjectName(QStringLiteral("actionQtMeshCloudOpenProjects"));
    connect(m_openProjectsAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: My Cloud Projects"));
        emit openProjectsRequested();
    });

    m_uploadAction = m_menu->addAction(tr("Upload to QtMesh Cloud..."));
    m_uploadAction->setObjectName(QStringLiteral("actionQtMeshCloudUploadFiles"));
    connect(m_uploadAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: Upload Files"));
        emit uploadFilesRequested();
    });

    m_feedbackAction = m_menu->addAction(tr("Send Feedback..."));
    m_feedbackAction->setObjectName(QStringLiteral("actionQtMeshCloudSendFeedback"));
    connect(m_feedbackAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud toolbar: Send Feedback"));
        emit feedbackRequested();
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

void CloudAccountMenuButton::updateGamificationSection(bool signedIn)
{
    if (!m_gamifyAction)
        return;

    auto* gamify = GamificationManager::instance();
    const bool syncOn = gamify->syncEnabled();
    const bool showStats = signedIn && syncOn && gamify->statsAvailable();
    const bool showSyncing = signedIn && syncOn && !gamify->statsAvailable();
    const bool showEnable = signedIn && !syncOn;
    const bool showAchievements = signedIn && syncOn && !gamify->profileUrl().isEmpty();

    // Rebuild the section by physically removing / re-inserting the actions
    // (before "My Cloud Projects…") — see the note in buildMenu().
    for (QAction* action : {static_cast<QAction*>(m_gamifyAction), m_achievementsAction,
                            m_enableSyncAction, m_gamifySeparator}) {
        if (m_menu->actions().contains(action))
            m_menu->removeAction(action);
    }

    if (showStats || showSyncing)
        m_menu->insertAction(m_openProjectsAction, m_gamifyAction);
    if (showEnable)
        m_menu->insertAction(m_openProjectsAction, m_enableSyncAction);
    if (showAchievements)
        m_menu->insertAction(m_openProjectsAction, m_achievementsAction);
    if (showStats || showSyncing || showEnable || showAchievements)
        m_menu->insertAction(m_openProjectsAction, m_gamifySeparator);

    m_menu->updateGeometry();
    m_menu->adjustSize();
    m_menu->update();

    if (showSyncing) {
        // Enabled but the first stats fetch hasn't landed yet (it refreshes
        // async on menu open) — say so instead of showing an empty block.
        m_gamifyLevelLabel->setText(tr("Syncing progress…"));
        m_gamifyXpBar->setVisible(false);
        m_gamifyNextLabel->setVisible(false);
        m_gamifyNextBar->setVisible(false);
        return;
    }
    if (!showStats)
        return;

    QString levelText = tr("Level %1 · %2 XP").arg(gamify->level()).arg(gamify->xp());
    if (gamify->currentStreak() > 0)
        levelText += tr(" · 🔥 %n-day streak", nullptr, gamify->currentStreak());
    m_gamifyLevelLabel->setText(levelText);

    m_gamifyXpBar->setVisible(true);
    m_gamifyXpBar->setRange(0, qMax(1, gamify->xpSpan()));
    m_gamifyXpBar->setValue(qBound(0, gamify->xpIntoLevel(), qMax(1, gamify->xpSpan())));
    m_gamifyXpBar->setToolTip(tr("%1 / %2 XP to level %3")
                                  .arg(gamify->xpIntoLevel())
                                  .arg(gamify->xpSpan())
                                  .arg(gamify->level() + 1));

    const QVariantList next = gamify->nextUnlockables();
    const bool hasNext = !next.isEmpty();
    m_gamifyNextLabel->setVisible(hasNext);
    m_gamifyNextBar->setVisible(hasNext);
    if (hasNext) {
        const QVariantMap u = next.first().toMap();
        m_gamifyNextLabel->setText(tr("Next: %1 — %2 (%3/%4)")
                                       .arg(u.value(QStringLiteral("title")).toString(),
                                            u.value(QStringLiteral("description")).toString())
                                       .arg(u.value(QStringLiteral("current")).toLongLong())
                                       .arg(u.value(QStringLiteral("threshold")).toLongLong()));
        const int threshold =
            qMax(1, static_cast<int>(u.value(QStringLiteral("threshold")).toLongLong()));
        m_gamifyNextBar->setRange(0, threshold);
        m_gamifyNextBar->setValue(
            qBound(0, static_cast<int>(u.value(QStringLiteral("current")).toLongLong()),
                   threshold));
    }
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
    updateGamificationSection(signedIn);

    m_openProjectsAction->setEnabled(signedIn);
    if (signedIn)
        setUploadEnabled(m_uploadAssetAvailable);
    else if (m_uploadAction)
        m_uploadAction->setEnabled(true);

    m_signInAction->setVisible(!signedIn);
    m_signInAction->setEnabled(!signedIn);
    m_signOutAction->setVisible(signedIn);
    m_signOutAction->setEnabled(signedIn);
}

void CloudAccountMenuButton::setUploadEnabled(bool enabled)
{
    m_uploadAssetAvailable = enabled;
    if (!m_uploadAction)
        return;
    if (!CloudCredentialStore::hasSession()) {
        m_uploadAction->setEnabled(true);
        m_uploadAction->setToolTip({});
        return;
    }
    m_uploadAction->setEnabled(enabled);
    if (!enabled)
        m_uploadAction->setToolTip(tr("Open a model first"));
    else
        m_uploadAction->setToolTip({});
}
