#include "CharacterController.h"

#include "CharacterAssets.h"
#include "CharacterModel.h"
#include "CharacterScene.h"
#include "../Manager.h"
#include "../SelectionSet.h"
#include "../SentryReporter.h"
#include "../UndoManager.h"

#include <QCoreApplication>
#include <QUndoCommand>

#include <OgreSceneNode.h>

namespace {

/// Adds a generated character as a new node; undo destroys it, redo rebuilds it from the held assets + params.
class AddCharacterCommand : public QUndoCommand
{
public:
    struct Outcome { bool created = false; QString error; };

    AddCharacterCommand(std::shared_ptr<CharacterGen::Assets> assets, CharacterGen::Params params, std::shared_ptr<Outcome> out)
        : m_assets(std::move(assets)), m_params(std::move(params)), m_out(std::move(out))
    {
        setText(QObject::tr("Generate character"));
    }

    void redo() override
    {
        if (m_node.isEmpty() == false && m_created) return;
        QString err;
        CharacterGen::SceneBuild sb;
        Ogre::SceneNode* node = CharacterGen::createCharacterInScene(
            *m_assets, m_params, QStringLiteral("character_%1").arg(++m_revision), &sb, &err);
        if (!node) {
            m_out->error = err;
            setObsolete(true);
            return;
        }
        m_node = QString::fromStdString(node->getName());
        m_created = true;
        m_out->created = true;
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

QStringList CharacterController::skinNames() const { return CharacterGen::colourNames(CharacterGen::ColourKind::Skin); }
QStringList CharacterController::eyeNames() const { return CharacterGen::colourNames(CharacterGen::ColourKind::Eye); }

void CharacterController::setStatus(const QString& s, bool busy)
{
    m_status = s;
    m_busy = busy;
    emit statusChanged();
    QCoreApplication::processEvents();
}

QString CharacterController::generate(int seed, double variety, const QString& body, const QString& face, const QString& skin,
                                      const QString& eye)
{
    using namespace CharacterGen;
    if (m_busy) return QStringLiteral("busy");
    if (!Manager::getSingletonPtr()) return QStringLiteral("No active scene.");
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("character.generate"));
    QString err;

    if (!CharacterAssets::present()) {
        setStatus(tr("Downloading character assets…"), true);
        if (!CharacterAssets::ensureBlocking(&err)) {
            setStatus(tr("Character assets unavailable: %1").arg(err), false);
            return err;
        }
    }
    if (!m_assets) {
        setStatus(tr("Loading character assets…"), true);
        auto a = std::make_shared<Assets>();
        if (!loadAssetsFromDir(CharacterAssets::dir(), *a, &err)) {
            setStatus(tr("Character assets could not be loaded: %1").arg(err), false);
            return err;
        }
        m_assets = std::move(a);
    }

    Params p;
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
    UndoManager::getSingleton()->push(new AddCharacterCommand(m_assets, p, out));
    if (!out->created) {
        setStatus(out->error, false);
        return out->error.isEmpty() ? QStringLiteral("failed") : out->error;
    }
    setStatus(tr("Character created (Ctrl+Z removes it)."), false);
    return {};
}
