#include "about.h"
#include "EcosystemWidgets.h"
#include "QtMeshEcosystem.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
const QString kSurface = QStringLiteral("about");

QString link(const QString& href, const QString& text)
{
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(href, text);
}

QString link(QtMeshEcosystem::Link which, const QString& text)
{
    return link(QtMeshEcosystem::url(which).toString(), text);
}

/// Scrollable page so small windows / large fonts never clip a tab.
QWidget* scrollPage(QWidget* content, QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}

QVBoxLayout* pageLayout(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    return layout;
}

QLabel* boldLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    return label;
}
} // namespace

About::About(QWidget *parent) :
    QDialog(parent)
{
    setWindowTitle(tr("About QtMeshEditor"));
    setWindowIcon(QIcon(QStringLiteral(":/icones/icone.ico")));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setMinimumWidth(600);
    resize(680, 740);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 14);
    root->setSpacing(14);

    // ---- Header ------------------------------------------------------------
    auto* header = new QHBoxLayout();
    header->setSpacing(14);
    auto* logo = new QLabel(this);
    logo->setPixmap(QIcon(QStringLiteral(":/icones/icone.ico")).pixmap(64, 64));
    logo->setFixedSize(64, 64);
    header->addWidget(logo, 0, Qt::AlignTop);

    auto* titles = new QVBoxLayout();
    titles->setSpacing(2);
    auto* name = new QLabel(QStringLiteral("QtMeshEditor"), this);
    QFont nf = name->font();
    nf.setPointSizeF(nf.pointSizeF() * 1.7);
    nf.setBold(true);
    name->setFont(nf);
    titles->addWidget(name);

    m_versionText = new QLabel(QString("Version: ") + QTMESHEDITOR_VERSION, this);
    m_versionText->setObjectName(QStringLiteral("versionText"));
    m_versionText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(m_versionText);

    auto* tagline = new QLabel(QtMeshEcosystem::tagline(), this);
    QFont gf = tagline->font();
    gf.setBold(true);
    tagline->setFont(gf);
    QPalette tp = tagline->palette();
    tp.setColor(QPalette::WindowText, EcosystemWidgets::accentColor(tp));
    tagline->setPalette(tp);
    titles->addWidget(tagline);
    header->addLayout(titles, 1);
    root->addLayout(header);

    // ---- Tabs ----------------------------------------------------------------
    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(buildAboutTab(), tr("About"));
    m_tabs->addTab(buildEcosystemTab(), tr("QtMesh Ecosystem"));
    m_tabs->addTab(buildSupportTab(), tr("Support Development"));
    root->addWidget(m_tabs, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

About::~About() = default;

QString About::getVersionText() const {
    return m_versionText->text();
}

QWidget* About::buildAboutTab()
{
    auto* page = new QWidget();
    auto* layout = pageLayout(page);

    auto* description = new QLabel(QtMeshEcosystem::description(), page);
    description->setWordWrap(true);
    layout->addWidget(description);

    layout->addWidget(EcosystemWidgets::sectionHeader(tr("What it does"), page));
    auto* capabilities = new QLabel(QStringLiteral(
        "<ul style=\"margin-left:-20px\">"
        "<li><b>Modeling</b> — edit meshes, UVs, materials, textures and lighting in a real-time viewport.</li>"
        "<li><b>Asset conversion</b> — FBX, glTF/GLB, OBJ, Collada, Ogre mesh and more, with validation and optimisation.</li>"
        "<li><b>AI-assisted 3D</b> — generate meshes from an image or text prompt, PBR maps, textures and segmentation, running locally.</li>"
        "<li><b>Rigging</b> — automatic skeletons, skin weights, weight painting and facial blendshapes.</li>"
        "<li><b>Animation</b> — retargeting, text-to-motion, motion capture, constraints and VAT export.</li>"
        "<li><b>Automation</b> — the <code>qtmesh</code> CLI and an MCP server for pipelines and AI agents.</li>"
        "</ul>"), page);
    capabilities->setTextFormat(Qt::RichText);
    capabilities->setWordWrap(true);
    layout->addWidget(capabilities);

    layout->addWidget(EcosystemWidgets::linkLabel(
        link(QtMeshEcosystem::Link::Website, QStringLiteral("qtmesh.dev"))
        + QStringLiteral("  ·  ")
        + link(QtMeshEcosystem::Link::GitHub, tr("Source code on GitHub"))
        + QStringLiteral("  ·  ")
        + link(QtMeshEcosystem::Link::Issues, tr("Report an issue")),
        kSurface, page));

    // Primary authorship: concise and visible.
    auto* author = EcosystemWidgets::linkLabel(
        tr("Created and maintained by %1")
            .arg(link(QStringLiteral("https://github.com/fernandotonon"),
                      QStringLiteral("Fernando Tonon"))),
        kSurface, page);
    author->setObjectName(QStringLiteral("aboutAuthor"));
    QFont authorFont = author->font();
    authorFont.setBold(true);
    author->setFont(authorFont);
    layout->addWidget(author);

    layout->addWidget(EcosystemWidgets::linkLabel(
        tr("Powered by %1, %2 and %3.")
            .arg(link(QStringLiteral("https://www.ogre3d.org/"), QStringLiteral("Ogre3D")),
                 link(QStringLiteral("https://www.qt.io/"), QStringLiteral("Qt")),
                 link(QStringLiteral("https://www.assimp.org/"), QStringLiteral("Assimp")))
        + QStringLiteral("<br>")
        + tr("Released under the %1. Third-party AI models, HDRIs and motion data keep their own licences (%2).")
            .arg(link(QStringLiteral("https://github.com/fernandotonon/QtMeshEditor/blob/master/License"),
                      tr("MIT License")),
                 link(QStringLiteral("https://github.com/fernandotonon/QtMeshEditor/blob/master/THIRD_PARTY_AI_MODELS.md"),
                      tr("notices"))),
        kSurface, page));

    // Historical and community contributors: secondary. The full list lives
    // in GitHub's contributor history, so nothing here goes stale.
    layout->addWidget(EcosystemWidgets::sectionHeader(tr("Acknowledgments"), page));
    auto* thanks = EcosystemWidgets::linkLabel(
        tr("Thanks to %1 and all open-source contributors who have helped improve "
           "QtMeshEditor over the years.")
            .arg(link(QStringLiteral("https://github.com/akira215"), QStringLiteral("Akira")))
        + QStringLiteral("<br>")
        + link(QStringLiteral("https://github.com/fernandotonon/QtMeshEditor/graphs/contributors"),
               tr("View all contributors")),
        kSurface, page);
    thanks->setObjectName(QStringLiteral("aboutAcknowledgments"));
    QPalette thanksPal = thanks->palette();
    thanksPal.setColor(QPalette::WindowText, EcosystemWidgets::mutedColor(thanksPal));
    thanks->setPalette(thanksPal);
    layout->addWidget(thanks);
    layout->addStretch();
    return scrollPage(page, this);
}

QWidget* About::buildEcosystemTab()
{
    auto* page = new QWidget();
    auto* layout = pageLayout(page);

    // Create. Refine. Play.
    auto* pillars = new QGridLayout();
    pillars->setHorizontalSpacing(16);
    pillars->setVerticalSpacing(2);
    const struct { const char* title; const char* body; } kPillars[] = {
        { "Create", "Generate and import 3D assets." },
        { "Refine", "Model, rig, animate and optimise them in QtMeshEditor." },
        { "Play", "Explore games built with these workflows." },
    };
    for (int i = 0; i < 3; ++i) {
        auto* title = boldLabel(tr(kPillars[i].title), page);
        QPalette tp = title->palette();
        tp.setColor(QPalette::WindowText, EcosystemWidgets::accentColor(tp));
        title->setPalette(tp);
        pillars->addWidget(title, 0, i);
        auto* body = EcosystemWidgets::mutedLabel(tr(kPillars[i].body), page);
        body->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        pillars->addWidget(body, 1, i, Qt::AlignTop);
        pillars->setColumnStretch(i, 1);
    }
    layout->addLayout(pillars);

    layout->addSpacing(4);
    layout->addWidget(EcosystemWidgets::sectionHeader(tr("Products"), page));
    auto* products = new QGridLayout();
    products->setHorizontalSpacing(14);
    products->setVerticalSpacing(8);
    const struct { const char* name; const char* body; QtMeshEcosystem::Link link; const char* linkText; }
        kProducts[] = {
        { "QtMeshEditor", "Open-source 3D editor and asset pipeline.",
          QtMeshEcosystem::Link::GitHub, "GitHub" },
        { "QtMesh Cloud", "Cloud projects, uploads and account services for your assets.",
          QtMeshEcosystem::Link::Cloud, "qtmesh.dev" },
        { "QtMesh Games", "Games that show what these workflows produce.",
          QtMeshEcosystem::Link::Games, "games.qtmesh.dev" },
    };
    for (int row = 0; row < 3; ++row) {
        products->addWidget(boldLabel(QString::fromUtf8(kProducts[row].name), page), row, 0,
                            Qt::AlignTop);
        auto* body = new QLabel(tr(kProducts[row].body), page);
        body->setWordWrap(true);
        products->addWidget(body, row, 1, Qt::AlignTop);
        products->addWidget(EcosystemWidgets::linkLabel(
            link(kProducts[row].link, QString::fromUtf8(kProducts[row].linkText)), kSurface, page),
            row, 2, Qt::AlignTop | Qt::AlignRight);
    }
    products->setColumnStretch(1, 1);
    layout->addLayout(products);

    layout->addSpacing(4);
    layout->addWidget(EcosystemWidgets::sectionHeader(tr("Made with QtMeshEditor"), page));
    auto* cards = new QGridLayout();
    cards->setSpacing(10);
    const auto& games = QtMeshEcosystem::showcaseGames();
    for (int i = 0; i < int(games.size()); ++i) {
        cards->addWidget(EcosystemWidgets::gameCard(games.at(i), kSurface, QSize(124, 70), page),
                         i / 3, i % 3);
    }
    layout->addLayout(cards);

    QStringList platformBits;
    for (const auto& platform : QtMeshEcosystem::platforms()) {
        platformBits.append(platform.url.isEmpty()
            ? QStringLiteral("<b>%1</b>: %2").arg(platform.name, platform.status.toLower())
            : QStringLiteral("<b>%1</b>: %2").arg(link(platform.url, platform.name),
                                                platform.status.toLower()));
    }
    layout->addWidget(EcosystemWidgets::linkLabel(
        platformBits.join(QStringLiteral("  ·  ")), kSurface, page));

    auto* exploreRow = new QHBoxLayout();
    exploreRow->addStretch();
    exploreRow->addWidget(EcosystemWidgets::linkButton(
        tr("Explore QtMesh Games →"), QtMeshEcosystem::Link::Games, kSurface, page));
    layout->addLayout(exploreRow);
    layout->addStretch();
    return scrollPage(page, this);
}

QWidget* About::buildSupportTab()
{
    auto* page = new QWidget();
    auto* layout = pageLayout(page);

    auto* intro = new QLabel(tr(
        "QtMeshEditor is an independently developed open-source project. "
        "If it is useful to you, here are a few ways to help it keep growing. "
        "All of them are optional."), page);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    const struct { const char* title; const char* body; QtMeshEcosystem::Link link; const char* action; }
        kWays[] = {
        { "Contribute", "Report bugs, suggest features, improve docs or send a pull request.",
          QtMeshEcosystem::Link::Contribute, "Contribute on GitHub" },
        { "Sponsor", "Recurring or one-time support through GitHub Sponsors.",
          QtMeshEcosystem::Link::Sponsor, "Sponsor on GitHub" },
        { "Play", "Play and share QtMesh Games. They show what the editor can do and help fund it.",
          QtMeshEcosystem::Link::Games, "Explore QtMesh Games" },
    };
    for (const auto& way : kWays) {
        auto* row = new QHBoxLayout();
        row->setSpacing(12);
        auto* text = new QVBoxLayout();
        text->setSpacing(2);
        text->addWidget(boldLabel(tr(way.title), page));
        text->addWidget(EcosystemWidgets::mutedLabel(tr(way.body), page));
        row->addLayout(text, 1);
        auto* button = new QPushButton(tr(way.action), page);
        button->setMinimumHeight(30);
        button->setStyleSheet(EcosystemWidgets::secondaryButtonStyle(palette()));
        button->setToolTip(QtMeshEcosystem::url(way.link).toString());
        const auto target = way.link;
        connect(button, &QPushButton::clicked, this, [target]() {
            QtMeshEcosystem::open(target, kSurface);
        });
        row->addWidget(button, 0, Qt::AlignVCenter);
        layout->addLayout(row);
        layout->addSpacing(4);
    }
    layout->addStretch();
    return scrollPage(page, this);
}
