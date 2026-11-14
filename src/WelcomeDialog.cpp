#include "WelcomeDialog.h"
#include "EcosystemWidgets.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "QtMeshEcosystem.h"
#include "QtMeshIdPanel.h"
#include "SentryReporter.h"

#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
const QString kSurface = QStringLiteral("welcome");

// The four games on the Welcome card: three web games and one Roblox
// experience, so both playable platforms are represented.
const QStringList kWelcomeGames = {
    QStringLiteral("scrapfall"), QStringLiteral("ironfang"),
    QStringLiteral("mall-chase"), QStringLiteral("data-core-clash"),
};

QLabel* tipRow(const QString& title, const QString& body, QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("<b>%1</b><br>%2").arg(title, body), parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    return label;
}
} // namespace

bool WelcomeDialog::shouldShow()
{
    QSettings settings;
    return !settings.value("WelcomeScreen/dontShowAgain", false).toBool();
}

QFileDialog::Options WelcomeDialog::openFileDialogOptions()
{
    // Keep the welcome dialog on the Qt file picker so it inherits the
    // application's selected palette instead of the desktop theme.
    return QFileDialog::DontUseNativeDialog | QFileDialog::HideNameFilterDetails;
}

WelcomeDialog::WelcomeDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Welcome to QtMeshEditor"));
    setWindowIcon(QIcon(QStringLiteral(":/icones/icone.ico")));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    // Width only: an explicit minimum HEIGHT would override the layout's own
    // minimum and squeeze the word-wrapped tips and the game cards.
    setMinimumWidth(720);
    resize(860, 700);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(16);
    mainLayout->setContentsMargins(28, 24, 28, 18);

    // ---- Header: logo, name, identity line --------------------------------
    {
        auto* header = new QHBoxLayout();
        header->setSpacing(14);

        auto* logo = new QLabel(this);
        logo->setPixmap(QIcon(QStringLiteral(":/icones/icone.ico")).pixmap(48, 48));
        logo->setFixedSize(48, 48);
        header->addWidget(logo, 0, Qt::AlignTop);

        auto* titles = new QVBoxLayout();
        titles->setSpacing(2);
        auto* title = new QLabel(QStringLiteral("QtMeshEditor"), this);
        QFont tf = title->font();
        tf.setPointSizeF(tf.pointSizeF() * 1.7);
        tf.setBold(true);
        title->setFont(tf);
        titles->addWidget(title);

        auto* tagline = new QLabel(QtMeshEcosystem::tagline(), this);
        QFont gf = tagline->font();
        gf.setBold(true);
        tagline->setFont(gf);
        QPalette tp = tagline->palette();
        tp.setColor(QPalette::WindowText, EcosystemWidgets::accentColor(tp));
        tagline->setPalette(tp);
        titles->addWidget(tagline);
        titles->addWidget(EcosystemWidgets::mutedLabel(
            QStringLiteral("Open-source 3D creation and asset pipeline"), this));
        header->addLayout(titles, 1);

        // Upper right: the (optional) QtMesh ID account, with the version on
        // its heading row so the header stays as short as before.
        header->addWidget(new QtMeshIdPanel(
            kSurface, this,
            QStringLiteral("Version %1").arg(QStringLiteral(QTMESHEDITOR_VERSION))),
            0, Qt::AlignTop);
        mainLayout->addLayout(header);
    }

    mainLayout->addWidget(EcosystemWidgets::separator(this));

    // ---- Body: start actions + recent files | tips + discovery ------------
    auto* body = new QHBoxLayout();
    body->setSpacing(28);
    body->addWidget(buildStartColumn(), 3);
    body->addWidget(buildTipsColumn(), 2);
    mainLayout->addLayout(body, 1);

    mainLayout->addWidget(EcosystemWidgets::separator(this));

    // ---- Made with QtMeshEditor -------------------------------------------
    mainLayout->addWidget(buildShowcase());

    mainLayout->addWidget(EcosystemWidgets::separator(this));

    // ---- Footer: don't show again + Get Started ---------------------------
    auto* bottomLayout = new QHBoxLayout();
    auto* dontShowCheck = new QCheckBox(QStringLiteral("Don't show again"), this);
    bottomLayout->addWidget(dontShowCheck);

    // Persist "Don't show again" on ANY exit path (New Scene, Open File, Recent, Get Started)
    connect(this, &QDialog::accepted, this, [dontShowCheck]() {
        if (dontShowCheck->isChecked()) {
            QSettings settings;
            settings.setValue("WelcomeScreen/dontShowAgain", true);
        }
    });

    bottomLayout->addStretch();

    auto* getStartedBtn = new QPushButton(QStringLiteral("Get Started"), this);
    getStartedBtn->setMinimumHeight(32);
    getStartedBtn->setStyleSheet(EcosystemWidgets::secondaryButtonStyle(palette()));
    connect(getStartedBtn, &QPushButton::clicked, this, [this]() {
        m_action = Dismissed;
        SentryReporter::addBreadcrumb("ui.action", "Welcome: Dismissed");
        accept();
    });
    bottomLayout->addWidget(getStartedBtn);
    mainLayout->addLayout(bottomLayout);
}

QWidget* WelcomeDialog::buildStartColumn()
{
    auto* column = new QWidget(this);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(EcosystemWidgets::sectionHeader(QStringLiteral("Start"), column));

    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    auto* newSceneBtn = new QPushButton(QStringLiteral("New Scene"), column);
    newSceneBtn->setMinimumHeight(40);
    newSceneBtn->setDefault(true);
    newSceneBtn->setStyleSheet(EcosystemWidgets::primaryButtonStyle(palette()));
    newSceneBtn->setToolTip(QStringLiteral("Start with an empty scene"));
    connect(newSceneBtn, &QPushButton::clicked, this, [this]() {
        m_action = NewScene;
        SentryReporter::addBreadcrumb("ui.action", "Welcome: New Scene");
        accept();
    });
    btnLayout->addWidget(newSceneBtn, 1);

    auto* openFileBtn = new QPushButton(QStringLiteral("Open File..."), column);
    openFileBtn->setMinimumHeight(40);
    openFileBtn->setStyleSheet(EcosystemWidgets::secondaryButtonStyle(palette()));
    openFileBtn->setToolTip(QStringLiteral("Open a mesh, scene or animation file"));
    connect(openFileBtn, &QPushButton::clicked, this, [this]() {
        const QString filter = MeshImporterExporter::importFileDialogFilterFromExtensionList(
            Manager::defaultImportExtensions());
        QString file = QFileDialog::getOpenFileName(
            this, tr("Open 3D File"), QString(), filter, nullptr,
            openFileDialogOptions());
        if (!file.isEmpty()) {
            m_action = OpenFile;
            m_selectedFile = file;
            SentryReporter::addBreadcrumb("ui.action", "Welcome: Open File");
            accept();
        }
    });
    btnLayout->addWidget(openFileBtn, 1);
    layout->addLayout(btnLayout);

    layout->addSpacing(6);
    layout->addWidget(EcosystemWidgets::sectionHeader(QStringLiteral("Recent Files"), column));

    QSettings settings;
    const QStringList recentFiles = settings.value("RecentFiles/files").toStringList();
    QStringList existing;
    for (const QString& path : recentFiles) {
        if (QFileInfo::exists(path))
            existing.append(path);
    }

    if (existing.isEmpty()) {
        layout->addWidget(EcosystemWidgets::mutedLabel(
            QStringLiteral("No recent files yet. Files you open will appear here."), column));
        layout->addStretch();
        return column;
    }

    auto* recentList = new QListWidget(column);
    recentList->setFrameShape(QFrame::NoFrame);
    recentList->setUniformItemSizes(true);
    recentList->setAccessibleName(QStringLiteral("Recent files"));
    recentList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; }"
        "QListWidget::item { padding: 5px 6px; border-radius: 4px; }"
        "QListWidget::item:hover { background: palette(alternate-base); }"
        "QListWidget::item:selected { background: palette(highlight);"
        "  color: palette(highlighted-text); }"));
    for (const QString& path : existing) {
        QFileInfo fi(path);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   —   %2").arg(fi.fileName(), fi.absolutePath()));
        item->setToolTip(path);
        item->setData(Qt::UserRole, path);
        recentList->addItem(item);
    }
    auto openRecent = [this](QListWidgetItem* item) {
        m_action = OpenRecent;
        m_selectedFile = item->data(Qt::UserRole).toString();
        SentryReporter::addBreadcrumb("ui.action", "Welcome: Open Recent");
        accept();
    };
    connect(recentList, &QListWidget::itemDoubleClicked, this, openRecent);
    connect(recentList, &QListWidget::itemActivated, this, openRecent);
    layout->addWidget(recentList, 1);
    return column;
}

QWidget* WelcomeDialog::buildTipsColumn()
{
    auto* column = new QWidget(this);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(EcosystemWidgets::sectionHeader(QStringLiteral("Quick Tips"), column));
    layout->addWidget(tipRow(QStringLiteral("AI Chat"),
        QStringLiteral("Describe changes in natural language from the AI Chat panel."), column));
    layout->addWidget(tipRow(QStringLiteral("Shortcuts"),
        QStringLiteral("Q select, W move, E rotate, R scale, F frame. "
                       "Ctrl+/ lists them all; Ctrl+, opens Preferences."), column));
    layout->addWidget(tipRow(QStringLiteral("CLI pipeline"),
        QStringLiteral("Use <code>qtmesh</code> to convert, validate, build LODs "
                       "and process animations in batch."), column));

    // Filled by setDiscoverySuggestion(); empty until then.
    m_suggestionHost = new QVBoxLayout();
    m_suggestionHost->setContentsMargins(0, 4, 0, 0);
    layout->addLayout(m_suggestionHost);
    layout->addStretch();
    return column;
}

QWidget* WelcomeDialog::buildShowcase()
{
    auto* section = new QWidget(this);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* headerRow = new QHBoxLayout();
    headerRow->addWidget(EcosystemWidgets::sectionHeader(
        QStringLiteral("Made with QtMeshEditor"), section));
    headerRow->addStretch();
    headerRow->addWidget(EcosystemWidgets::linkButton(
        QStringLiteral("Explore QtMesh Games →"), QtMeshEcosystem::Link::Games,
        kSurface, section));
    layout->addLayout(headerRow);

    auto* cards = new QHBoxLayout();
    cards->setSpacing(10);
    for (const QString& id : kWelcomeGames) {
        for (const auto& game : QtMeshEcosystem::showcaseGames()) {
            if (game.id == id) {
                cards->addWidget(EcosystemWidgets::gameCard(game, kSurface, QSize(160, 90),
                                                            section), 1);
                break;
            }
        }
    }
    layout->addLayout(cards);

    QStringList platformBits;
    for (const auto& platform : QtMeshEcosystem::platforms()) {
        platformBits.append(platform.url.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(platform.name, platform.status.toLower())
            : platform.name);
    }
    layout->addWidget(EcosystemWidgets::mutedLabel(
        QStringLiteral("Games built with QtMeshEditor assets and workflows. Platforms: ")
            + platformBits.join(QStringLiteral(" · ")) + QLatin1Char('.'),
        section));
    return section;
}

void WelcomeDialog::setDiscoverySuggestion(const QVariantMap& suggestion,
                                           std::function<void(const QString&)> onDismiss)
{
    if (!m_suggestionHost)
        return;
    while (QLayoutItem* item = m_suggestionHost->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QString key = suggestion.value(QStringLiteral("featureKey")).toString();
    if (key.isEmpty())
        return;

    // A single bordered, word-wrapped QLabel (like the tips) with the dismiss
    // button beside it: wrapped labels nested inside a QFrame lose their
    // height-for-width in this layout and were clipped to one line.
    auto* card = new QWidget(this);
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    const bool personalized = suggestion.value(QStringLiteral("personalized")).toBool();
    auto* text = new QLabel(QStringLiteral("<b>%1%2</b><br><span style=\"color:%3\">%4</span>")
        .arg(personalized ? QStringLiteral("New to you: ") : QStringLiteral("Try this: "),
             suggestion.value(QStringLiteral("title")).toString().toHtmlEscaped(),
             EcosystemWidgets::mutedColor(palette()).name(),
             suggestion.value(QStringLiteral("blurb")).toString().toHtmlEscaped()), card);
    text->setObjectName(QStringLiteral("welcomeSuggestion"));
    text->setTextFormat(Qt::RichText);
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral(
        "QLabel#welcomeSuggestion { border: 1px solid %1; border-radius: 6px;"
        "  background: palette(alternate-base); }")
        .arg(EcosystemWidgets::accentColor(palette()).name()));
    // setMargin, not stylesheet padding: QLabel's height-for-width ignores
    // stylesheet padding and clips the last wrapped line.
    text->setMargin(8);
    // The tips column is ~260-320 px wide; reserve the height the text needs
    // at the narrow end so the blurb is never cut off.
    text->setMinimumHeight(text->heightForWidth(260));
    row->addWidget(text, 1);

    auto* hide = new QToolButton(card);
    hide->setText(QStringLiteral("\u2715"));
    hide->setAutoRaise(true);
    hide->setToolTip(QStringLiteral("Don't suggest this again"));
    hide->setAccessibleName(QStringLiteral("Dismiss suggestion"));
    connect(hide, &QToolButton::clicked, card, [card, key, onDismiss]() {
        if (onDismiss)
            onDismiss(key);
        card->hide();
    });
    row->addWidget(hide, 0, Qt::AlignTop);
    m_suggestionHost->addWidget(card);

    // The dialog was sized before the card existed, and word-wrapped labels
    // do not raise a layout's minimum height, so grow to fit it.
    if (QLayout* top = layout()) {
        top->activate();
        const int needed = top->totalHeightForWidth(width());
        if (needed > height())
            resize(width(), needed);
    }
}
