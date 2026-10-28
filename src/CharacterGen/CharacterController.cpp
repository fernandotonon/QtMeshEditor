#include "CharacterController.h"

#include "CharacterAssets.h"
#include "CharacterModel.h"
#include "CharacterScene.h"
#include "Clothing.h"
#include "HairTint.h"
#include "../Manager.h"
#include "../ModelDownloader.h"
#include "../SelectionSet.h"
#include "../SentryReporter.h"
#include "../UndoManager.h"

#include <QApplication>
#include <QColor>
#include <QColorDialog>
#include <QFileInfo>
#include <QSettings>

#include <QCoreApplication>
#include <QMetaObject>
#include <QUndoCommand>

#include <OgreSceneNode.h>

namespace {

/// Adds a generated character as a new node; undo destroys it, redo rebuilds it from the held assets + params.
class AddCharacterCommand : public QUndoCommand
{
public:
    struct Outcome { bool created = false; QString error, nodeName; };

    AddCharacterCommand(std::shared_ptr<CharacterGen::Assets> assets, CharacterGen::Params params, std::shared_ptr<Outcome> out)
        : m_assets(std::move(assets)), m_params(std::move(params)), m_out(std::move(out))
    {
        // Materials are named after the node. A name reused from an earlier character would keep that character's tint: the shader
        // generated for it caches the old colours, so a new skin / eye colour would not show.
        static int serial = 0;
        m_serial = ++serial;
        setText(QObject::tr("Generate character"));
    }

    void redo() override
    {
        if (m_node.isEmpty() == false && m_created) return;
        QString err;
        CharacterGen::SceneBuild sb;
        Ogre::SceneNode* node = CharacterGen::createCharacterInScene(
            *m_assets, m_params, QStringLiteral("character_%1_%2").arg(m_serial).arg(++m_revision), &sb, &err);
        if (!node) {
            m_out->error = err;
            setObsolete(true);
            return;
        }
        m_node = QString::fromStdString(node->getName());
        m_created = true;
        m_out->created = true;
        m_out->nodeName = m_node;
        SelectionSet::getSingleton()->selectOne(node);
    }

    void undo() override
    {
        if (!m_created) return;
        for (auto* n : Manager::getSingleton()->getSceneNodes())
            if (QString::fromStdString(n->getName()) == m_node) {
                SelectionSet::getSingleton()->clearList();
                Manager::getSingleton()->destroySceneNode(n);
                break;
            }
        m_created = false;
    }

private:
    std::shared_ptr<CharacterGen::Assets> m_assets;
    CharacterGen::Params m_params;
    std::shared_ptr<Outcome> m_out;
    QString m_node;
    bool m_created = false;
    int m_revision = 0;
    int m_serial = 0;
};


bool destroyNodeByName(const QString& name)
{
    for (auto* n : Manager::getSingleton()->getSceneNodes())
        if (QString::fromStdString(n->getName()) == name) {
            SelectionSet::getSingleton()->clearList();
            Manager::getSingleton()->destroySceneNode(n);
            return true;
        }
    return false;
}

/// Live preview: swaps the character on the scene for one built from new parameters. Consecutive swaps (dragging a slider) merge, so
/// the whole editing session is ONE undo step that goes back to the character as it was before the first change.
class ReplaceCharacterCommand : public QUndoCommand
{
public:
    ReplaceCharacterCommand(std::shared_ptr<CharacterGen::Assets> assets, CharacterGen::Params oldParams, CharacterGen::Params newParams,
                            QString existingNode, std::shared_ptr<AddCharacterCommand::Outcome> out)
        : m_assets(std::move(assets)), m_old(std::move(oldParams)), m_new(std::move(newParams)), m_before(std::move(existingNode)),
          m_out(std::move(out))
    {
        static int serial = 1000;
        m_serial = ++serial;
        setText(QObject::tr("Edit character"));
    }
    int id() const override { return 7421; }

    void redo() override
    {
        destroyNodeByName(m_before);
        QString err;
        Ogre::SceneNode* node = CharacterGen::createCharacterInScene(
            *m_assets, m_new, QStringLiteral("character_%1_%2").arg(m_serial).arg(++m_revision), nullptr, &err);
        if (!node) { m_out->error = err; return; }
        m_after = QString::fromStdString(node->getName());
        m_out->created = true;
        m_out->nodeName = m_after;
        SelectionSet::getSingleton()->selectOne(node);
    }

    void undo() override
    {
        destroyNodeByName(m_after);
        QString err;
        Ogre::SceneNode* node = CharacterGen::createCharacterInScene(
            *m_assets, m_old, QStringLiteral("character_%1_%2").arg(m_serial).arg(++m_revision), nullptr, &err);
        if (node) {
            m_before = QString::fromStdString(node->getName());
            SelectionSet::getSingleton()->selectOne(node);
        }
    }

    bool mergeWith(const QUndoCommand* other) override
    {
        const auto* o = dynamic_cast<const ReplaceCharacterCommand*>(other);
        if (!o || o->m_before != m_after) return false;
        m_new = o->m_new;
        m_after = o->m_after;
        return true;
    }

private:
    std::shared_ptr<CharacterGen::Assets> m_assets;
    CharacterGen::Params m_old, m_new;
    QString m_before, m_after;
    std::shared_ptr<AddCharacterCommand::Outcome> m_out;
    int m_serial = 0, m_revision = 0;
};

}  // namespace

CharacterController* CharacterController::s_instance = nullptr;

CharacterController* CharacterController::instance()
{
    if (!s_instance) s_instance = new CharacterController();
    return s_instance;
}

CharacterController* CharacterController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    CharacterController* c = instance();
    QQmlEngine::setObjectOwnership(c, QQmlEngine::CppOwnership);
    return c;
}

void CharacterController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

bool CharacterController::assetsPresent() const { return CharacterGen::CharacterAssets::present(); }

QStringList CharacterController::hairNames() const { return CharacterGen::CharacterAssets::hairStyleNames(); }
void CharacterController::setHairColour(const QString& v)
{
    CharacterGen::Rgb c;
    if (!CharacterGen::parseHairColour(v, &c)) return;
    const QString hex = CharacterGen::hairColourHex(c);
    if (hex == m_hairColour) return;
    m_hairColour = hex;
    emit hairColourChanged();
}

QVariantList CharacterController::hairColourPresets() const
{
    QVariantList list;
    for (const CharacterGen::HairColourPreset& pr : CharacterGen::hairColourPresets())
        list.append(QVariantMap{{"name", pr.name}, {"label", pr.label}, {"colour", CharacterGen::hairColourHex(pr.colour)}});
    return list;
}

QStringList CharacterController::topStyles() const { return CharacterGen::topStyleIds(); }
QStringList CharacterController::bottomStyles() const { return CharacterGen::bottomStyleIds(); }

QVariantList CharacterController::clothColours() const
{
    QVariantList list;
    for (const QString& name : CharacterGen::clothColourNames()) {
        CharacterGen::Rgb c{};
        CharacterGen::parseClothColour(name, &c);
        list.append(QVariantMap{{"name", name}, {"colour", QColor::fromRgbF(double(c[0]), double(c[1]), double(c[2])).name()}});
    }
    return list;
}

void CharacterController::setTopStyle(const QString& v)
{
    if (!CharacterGen::parseTopStyle(v, nullptr) || v.toLower() == m_topStyle) return;
    m_topStyle = v.toLower();
    emit clothesChanged();
}

void CharacterController::setBottomStyle(const QString& v)
{
    if (!CharacterGen::parseBottomStyle(v, nullptr) || v.toLower() == m_bottomStyle) return;
    m_bottomStyle = v.toLower();
    emit clothesChanged();
}

void CharacterController::setTopColour(const QString& v)
{
    CharacterGen::Rgb c{};
    if (!CharacterGen::parseClothColour(v, &c)) return;
    const QString hex = QColor::fromRgbF(double(c[0]), double(c[1]), double(c[2])).name();
    if (hex == m_topColour) return;
    m_topColour = hex;
    emit clothesChanged();
}

void CharacterController::setBottomColour(const QString& v)
{
    CharacterGen::Rgb c{};
    if (!CharacterGen::parseClothColour(v, &c)) return;
    const QString hex = QColor::fromRgbF(double(c[0]), double(c[1]), double(c[2])).name();
    if (hex == m_bottomColour) return;
    m_bottomColour = hex;
    emit clothesChanged();
}

QStringList CharacterController::skinNames() const { return CharacterGen::colourNames(CharacterGen::ColourKind::Skin); }
QString CharacterController::skinColourHex(const QString& name) const
{
    CharacterGen::Rgb c{};
    if (!CharacterGen::parseColour(name, CharacterGen::ColourKind::Skin, &c)) return QString();
    return QColor::fromRgbF(c[0], c[1], c[2]).name();
}
QString CharacterController::pickColour(const QString& initial) const
{
    const QColor c = QColorDialog::getColor(QColor(initial), QApplication::activeWindow(), QStringLiteral("Colour"),
                                            QColorDialog::DontUseNativeDialog);
    return c.isValid() ? c.name() : QString();
}
QStringList CharacterController::eyeNames() const { return CharacterGen::colourNames(CharacterGen::ColourKind::Eye); }

QVariantList CharacterController::seedBody(int seed, double variety) const
{
    variety = std::min(5.0, std::max(0.0, variety));
    QVariantList out;
    std::vector<float> v(45, 0.0f);
    if (seed >= 0) v = CharacterGen::randomCoefficients(uint32_t(seed) * 2u + 1u, 45, float(variety), 16);
    for (float x : v) out << double(x);
    return out;
}

bool CharacterController::hasCharacter() const
{
    if (m_currentNode.isEmpty() || !Manager::getSingletonPtr()) return false;
    for (auto* n : Manager::getSingleton()->getSceneNodes())
        if (QString::fromStdString(n->getName()) == m_currentNode) return true;
    return false;
}

// What each MHR body-shape mode does, from measuring it: displacement per body region, height and girth change of each mode
// (modes 0-19 shape the torso, 20-39 the head and neck, 40-44 the arms; the later modes of each group are small refinements).
QString CharacterController::shapeLabel(int m) const
{
    static const char* k[] = {
        "Weight (belly, chest)", "Hips and waist width", "Bust and hips balance", "Belly depth", "Waist width", "Hips depth",
        "Chest shape", "Chest and belly", "Waist detail", "Hips detail", "Torso detail 11", "Torso detail 12", "Hips detail 13",
        "Hips detail 14", "Hips detail 15", "Torso detail 16", "Torso detail 17", "Torso detail 18", "Torso detail 19", "Chest detail 20",
        "Neck length, head height", "Neck and head size", "Head shape 3", "Head shape 4", "Age (face)", "Head shape 6",
        "Head shape 7", "Head shape 8", "Head shape 9", "Head shape 10", "Head shape 11", "Head shape 12", "Head shape 13",
        "Head shape 14", "Head shape 15", "Head shape 16", "Head shape 17", "Head shape 18", "Head shape 19", "Head shape 20",
        "Arms", "Arms detail 2", "Arms detail 3", "Arms detail 4", "Arms detail 5"};
    return (m >= 0 && m < 45) ? QString::fromLatin1(k[m]) : QStringLiteral("Shape %1").arg(m + 1);
}

QString CharacterController::shapeHint(int m) const
{
    if (m == 0) return tr("The biggest body change: overall weight, mostly belly and chest.");
    if (m == 1) return tr("Hips and waist width: negative = narrow hips and waist (more masculine), positive = wider hips and waist (more feminine).");
    if (m == 2) return tr("Bust and hips balance: positive = a fuller, more forward bust with slimmer, flatter hips, negative = the opposite.");
    if (m >= 3 && m < 8) return tr("Torso proportions: waist, hips, belly or chest.");
    if (m < 20) return tr("A smaller refinement of the torso and hips.");
    if (m == 20 || m == 21) return tr("Neck length and the size of the head.");
    if (m < 40) return tr("A subtle change of the head's shape.");
    return tr("A subtle change of the arms.");
}

void CharacterController::setStatus(const QString& s, bool busy)
{
    m_status = s;
    m_busy = busy;
    emit statusChanged();
    QCoreApplication::processEvents();
}

void CharacterController::clearDownloadProgress()
{
    if (m_downloadProgress < 0.0 && m_downloadLabel.isEmpty()
        && m_downloadBytesReceived == 0 && m_downloadBytesTotal == 0)
        return;
    m_downloadProgress = -1.0;
    m_downloadLabel.clear();
    m_downloadBytesReceived = 0;
    m_downloadBytesTotal = 0;
    emit downloadProgressChanged();
}

void CharacterController::onModelDownloadProgress(const QString& name, qint64 received, qint64 total)
{
    if (!m_watchingDownload) return;
    // CharacterAssets labels look like "character body (MHR)" / "character head (ICT-FaceKit)".
    if (!name.contains(QLatin1String("character"), Qt::CaseInsensitive)
        && !name.contains(QLatin1String("MHR"), Qt::CaseInsensitive)
        && !name.contains(QLatin1String("ICT"), Qt::CaseInsensitive)
        && !name.contains(QLatin1String("hairstyle"), Qt::CaseInsensitive)
        && !name.contains(QLatin1String("MakeHuman"), Qt::CaseInsensitive))
        return;
    m_downloadLabel = name;
    m_downloadBytesReceived = received;
    m_downloadBytesTotal = total;
    m_downloadProgress = (total > 0) ? double(received) / double(total) : 0.0;
    emit downloadProgressChanged();
    setStatus(tr("Downloading %1…").arg(name), true);
}

QString CharacterController::generate(int seed, double variety, const QString& body, const QString& face, const QString& skin,
                                      const QString& eye, bool replace)
{
    using namespace CharacterGen;
    variety = std::min(5.0, std::max(0.0, variety));
    if (m_busy) return QStringLiteral("busy");
    if (!Manager::getSingletonPtr()) return QStringLiteral("No active scene.");
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("character.generate"));
    QString err;

    {   // always verify (cheap, cached): a corrupted default bundle is re-fetched; a custom directory is used as it is
        const bool needFetch = !CharacterAssets::present();
        QMetaObject::Connection progressConn;
        if (needFetch) {
            setStatus(tr("Downloading character assets…"), true);
            clearDownloadProgress();
            m_watchingDownload = true;
            if (auto* dl = ModelDownloader::instance()) {
                progressConn = connect(dl, &ModelDownloader::downloadProgressUpdated, this,
                                       &CharacterController::onModelDownloadProgress);
            }
        }
        const bool ok = CharacterAssets::ensureBlocking(&err);
        m_watchingDownload = false;
        if (progressConn) disconnect(progressConn);
        clearDownloadProgress();
        if (!ok) {
            setStatus(tr("Character assets unavailable: %1").arg(err), false);
            return err;
        }
    }
    // the optional hair bundle may have arrived with this ensureBlocking (a first run without hair): reload the cached assets so
    // the style can be built, and tell the panel so its hair controls appear
    if (m_assets && m_assets->hairStyles.empty() && !CharacterAssets::hairStyleNames().isEmpty()) m_assets.reset();
    if (!m_assets) {
        setStatus(tr("Loading character assets…"), true);
        auto a = std::make_shared<Assets>();
        if (!loadAssetsFromDir(CharacterAssets::dir(), *a, &err)) {
            setStatus(tr("Character assets could not be loaded: %1").arg(err), false);
            return err;
        }
        m_assets = std::move(a);
    }
    if (m_hairNamesSeen != CharacterAssets::hairStyleNames()) {
        m_hairNamesSeen = CharacterAssets::hairStyleNames();
        emit hairNamesChanged();
    }

    Params p;
    p.nativeHead = !m_assets->mhrArkit.empty();
    p.openEyes = m_openEyes;
    p.skinTexture = m_skinTexture;
    p.hairStyle = m_hairStyle;
    CharacterGen::parseHairColour(m_hairColour, &p.hairColour);
    p.bodyHair = float(m_bodyHair);
    p.eyelashes = m_eyelashes;
    p.topStyle = m_topStyle;
    p.bottomStyle = m_bottomStyle;
    CharacterGen::parseClothColour(m_topColour, &p.topColour);
    CharacterGen::parseClothColour(m_bottomColour, &p.bottomColour);
    if (seed >= 0) {
        p.bodyShape = randomCoefficients(uint32_t(seed) * 2u + 1u, m_assets->mhrShapes, float(variety), 16);
        p.faceIdentity = randomCoefficients(uint32_t(seed) * 2u + 2u, m_assets->ictIdentities, float(variety), 40);
    }
    if (!parseCoefficients(body, p.bodyShape, m_assets->mhrShapes, &err) || !parseCoefficients(face, p.faceIdentity, m_assets->ictIdentities, &err)) {
        setStatus(err, false);
        return err;
    }
    if (!skin.isEmpty() && !parseColour(skin, ColourKind::Skin, &p.skin)) { err = tr("Unknown skin colour '%1'").arg(skin); setStatus(err, false); return err; }
    if (!eye.isEmpty() && !parseColour(eye, ColourKind::Eye, &p.eye)) { err = tr("Unknown eye colour '%1'").arg(eye); setStatus(err, false); return err; }

    setStatus(tr("Building character…"), true);
    auto out = std::make_shared<AddCharacterCommand::Outcome>();
    if (replace && m_haveParams && hasCharacter())
        UndoManager::getSingleton()->push(new ReplaceCharacterCommand(m_assets, m_currentParams, p, m_currentNode, out));
    else
        UndoManager::getSingleton()->push(new AddCharacterCommand(m_assets, p, out));
    if (!out->created) {
        setStatus(out->error, false);
        return out->error.isEmpty() ? QStringLiteral("failed") : out->error;
    }
    m_currentNode = out->nodeName;
    m_currentParams = p;
    m_haveParams = true;
    setStatus(tr("Character ready (Ctrl+Z goes back)."), false);
    return {};
}
