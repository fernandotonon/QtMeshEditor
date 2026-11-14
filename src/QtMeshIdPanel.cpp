#include "QtMeshIdPanel.h"

#include "EcosystemWidgets.h"
#include "QtMeshId.h"
#include "SentryReporter.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QPixmap avatarPixmap(const QString& initials, const QColor& fill, const QColor& text, qreal dpr)
{
    const int side = 26;
    QPixmap pm(QSize(side, side) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(QRectF(0, 0, side, side));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(text);
    p.drawText(QRectF(0, 0, side, side), Qt::AlignCenter,
               initials.isEmpty() ? QStringLiteral("?") : initials);
    return pm;
}

} // namespace

QtMeshIdPanel::QtMeshIdPanel(const QString& surface, QWidget* parent, const QString& headerNote)
    : QWidget(parent), m_surface(surface), m_headerNote(headerNote)
{
    setObjectName(QStringLiteral("qtmeshIdPanel"));
    setMinimumWidth(300);
    setMaximumWidth(360);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(4);
    rebuild();
    connect(QtMeshId::instance(), &QtMeshId::sessionChanged, this, &QtMeshIdPanel::rebuild);
}

void QtMeshIdPanel::clear()
{
    // Detach first, delete later: a clicked button (Sign out) may still be on
    // the stack when the session change rebuilds the panel.
    const auto retire = [](QWidget* w) {
        w->hide();
        w->setParent(nullptr);
        w->deleteLater();
    };
    const auto drain = [&retire](QLayout* layout, auto& self) -> void {
        while (QLayoutItem* item = layout->takeAt(0)) {
            if (QWidget* w = item->widget())
                retire(w);
            else if (QLayout* inner = item->layout())
                self(inner, self);
            delete item;
        }
    };
    drain(m_layout, drain);
}

QPushButton* QtMeshIdPanel::textButton(const QString& text, const QString& objectName)
{
    auto* button = new QPushButton(text, this);
    button->setObjectName(objectName);
    button->setFlat(true);
    button->setAutoDefault(false);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: 1px solid transparent; padding: 2px 4px;"
        "  background: transparent; }"
        "QPushButton:hover { text-decoration: underline; }"
        "QPushButton:focus { border: 1px solid %1; border-radius: 4px; }")
        .arg(EcosystemWidgets::accentColor(palette()).name()));
    return button;
}

void QtMeshIdPanel::rebuild()
{
    clear();
    auto* id = QtMeshId::instance();
    const QtMeshId::State state = id->state();
    const QPalette pal = palette();

    auto* heading = new QHBoxLayout();
    heading->addWidget(EcosystemWidgets::sectionHeader(QStringLiteral("QtMesh ID"), this));
    heading->addStretch();
    if (!m_headerNote.isEmpty()) {
        auto* note = EcosystemWidgets::mutedLabel(m_headerNote, this);
        note->setWordWrap(false);
        heading->addWidget(note);
    }
    m_layout->addLayout(heading);

    if (state == QtMeshId::State::SignedIn) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* avatar = new QLabel(this);
        avatar->setObjectName(QStringLiteral("qtmeshIdAvatar"));
        avatar->setPixmap(avatarPixmap(id->initials(), EcosystemWidgets::accentColor(pal),
                                       pal.color(QPalette::Window),
                                       qMax<qreal>(1.0, devicePixelRatioF())));
        avatar->setFixedSize(26, 26);
        row->addWidget(avatar);
        const QString name = id->displayName().toHtmlEscaped();
        auto* status = new QLabel(QStringLiteral("✓ Connected as <b>%1</b>").arg(name), this);
        status->setObjectName(QStringLiteral("qtmeshIdStatus"));
        status->setTextFormat(Qt::RichText);
        status->setWordWrap(true);
        row->addWidget(status, 1);
        m_layout->addLayout(row);

        auto* actions = new QHBoxLayout();
        actions->setSpacing(2);
        actions->setContentsMargins(30, 0, 0, 0);
        auto* manage = textButton(QStringLiteral("Manage Account"), QStringLiteral("qtmeshIdManage"));
        manage->setToolTip(QtMeshId::manageAccountUrl().toString());
        connect(manage, &QPushButton::clicked, this, [this]() {
            QtMeshId::openManageAccount(m_surface);
        });
        auto* signOut = textButton(QStringLiteral("Sign out"), QStringLiteral("qtmeshIdSignOut"));
        connect(signOut, &QPushButton::clicked, this, [this]() {
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                QStringLiteral("QtMesh ID: sign out (%1)").arg(m_surface));
            QtMeshId::instance()->signOut();
        });
        actions->addWidget(manage);
        actions->addWidget(signOut);
        actions->addStretch();
        m_layout->addLayout(actions);
        return;
    }

    if (state == QtMeshId::State::Connecting) {
        auto* status = EcosystemWidgets::mutedLabel(
            QStringLiteral("Connecting… finish signing in in your browser."), this);
        status->setObjectName(QStringLiteral("qtmeshIdStatus"));
        m_layout->addWidget(status);
        return;
    }

    auto* blurb = EcosystemWidgets::mutedLabel(
        state == QtMeshId::State::Expired
            ? QStringLiteral("Your session expired. Sign in again to reconnect "
                             "QtMesh Cloud.")
            : QStringLiteral("Your account for the QtMesh ecosystem. Optional — "
                             "sign in for QtMesh Cloud projects and progress sync."),
        this);
    blurb->setObjectName(QStringLiteral("qtmeshIdStatus"));
    m_layout->addWidget(blurb);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(6);
    auto* signIn = new QPushButton(QStringLiteral("Sign in with QtMesh ID"), this);
    signIn->setObjectName(QStringLiteral("qtmeshIdSignIn"));
    signIn->setAutoDefault(false);
    signIn->setMinimumHeight(28);
    signIn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    signIn->setStyleSheet(EcosystemWidgets::secondaryButtonStyle(pal)
                          + QStringLiteral("QPushButton { padding: 0 12px; }"));
    connect(signIn, &QPushButton::clicked, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
            QStringLiteral("QtMesh ID: sign in (%1)").arg(m_surface));
        QtMeshId::instance()->signIn(window());
    });
    actions->addWidget(signIn);
    if (state == QtMeshId::State::SignedOut) {
        auto* create = textButton(QStringLiteral("Create a free account"),
                                  QStringLiteral("qtmeshIdCreate"));
        create->setToolTip(QStringLiteral(
            "Opens the QtMesh ID page in your browser, where new accounts are created."));
        connect(create, &QPushButton::clicked, this, [this]() {
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                QStringLiteral("QtMesh ID: create account (%1)").arg(m_surface));
            QtMeshId::instance()->signIn(window());
        });
        actions->addWidget(create);
    }
    actions->addStretch();
    m_layout->addLayout(actions);
}
