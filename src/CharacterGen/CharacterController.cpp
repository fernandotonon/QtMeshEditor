#include "CharacterController.h"
#include <QFileDialog>
#include <QThread>
#include <QPointer>
#include <QImageReader>
#include "Proportions.h"

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
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include "HairLibrary.h"

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
    static bool restored = false;      // the GUI restores the last Hair Studio state once (unit tests use instance() and start clean)
    if (!restored) { restored = true; CharacterGen::Hair::Library::purgeTrash(); c->loadHairStudioSettings(); }
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
    saveHairStudioSettings();   // a colour-only edit must be persisted too
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
QString CharacterController::pickPhoto() const
{
    static QString lastDir;
    const QString f = QFileDialog::getOpenFileName(QApplication::activeWindow(), tr("Choose a photo of a person"), lastDir,
                                                   tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff)"), nullptr, QFileDialog::DontUseNativeDialog);
    if (!f.isEmpty()) lastDir = QFileInfo(f).absolutePath();
    return f;
}

void CharacterController::clearPhoto()
{
    m_photoPath.clear();
    emit photoChanged();
}

void CharacterController::generateFromPhoto(const QString& path)
{
    using namespace CharacterGen;
    if (m_busy) return;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("character.from_photo"));
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage photo = reader.read();
    if (photo.isNull()) { setStatus(tr("That image could not be read: %1").arg(reader.errorString()), false); return; }
    QString err;
    if (!prepareAssets(&err)) return;   // (sets the status)
    // the models download on the main thread (the downloader's thread); the worker then finds them on disk
    setStatus(tr("Checking the photo models (they download on first use)…"), true);
    if (!PhotoCharacter::prepareModels(&err)) { setStatus(err, false); return; }
    setStatus(tr("Reading the photo…"), true);
    const std::shared_ptr<Assets> assets = m_assets;
    PhotoCharacter::Options options;
    QPointer<CharacterController> self(this);
    QThread* worker = QThread::create([self, assets, photo, options, path] {
        auto result = std::make_shared<PhotoCharacter::Result>(PhotoCharacter::estimate(photo, *assets, options, [self](const QString& line) {
            // only the stage lines (they end with an ellipsis) go to the status; the measurements are for the CLI log
            if (self && line.endsWith(QChar(0x2026))) QMetaObject::invokeMethod(self.data(), [self, line] { if (self) self->setStatus(line, true); }, Qt::QueuedConnection);
        }));
        if (self) QMetaObject::invokeMethod(self.data(), [self, result, path] { if (self) self->finishPhoto(result, path); }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void CharacterController::finishPhoto(const std::shared_ptr<CharacterGen::PhotoCharacter::Result>& result, const QString& path)
{
    using namespace CharacterGen;
    if (!result->ok) { setStatus(tr("The photo could not be used: %1").arg(result->error), false); return; }
    const PhotoCharacter::Result& r = *result;
    auto hex = [](const Rgb& c) { return QColor::fromRgbF(double(c[0]), double(c[1]), double(c[2])).name(); };
    // the controller's own state first (proportions, hair, clothes, face texture), then the panel gets the coefficients and colours
    m_proportions = {double(r.proportions.height), double(r.proportions.legs), double(r.proportions.arms), double(r.proportions.torso), double(r.proportions.shoulders), double(r.proportions.head)};
    emit proportionsChanged();
    if (r.haveHairStyle) {
        if (r.hairStyle.startsWith(QLatin1String("proc:"))) m_hairParams = r.hairProcedural;
        setHairStyle(r.hairStyle);
    }
    if (r.haveHairColour) setHairColour(hex(r.hairColour));
    if (r.haveClothes) {
        setTopStyle(r.topStyle);
        setBottomStyle(r.bottomStyle);
        if (r.haveTopColour) setTopColour(hex(r.topColour));
        if (r.haveBottomColour) setBottomColour(hex(r.bottomColour));
    }
    m_photoPath = path;
    emit photoChanged();
    QVariantList coefficients;
    for (float v : r.bodyShape) coefficients << double(v);
    setStatus(tr("Photo read: building the character…"), false);
    emit photoFitted(coefficients, r.haveSkin ? hex(r.skin) : QString(), r.haveEye ? hex(r.eye) : QString());
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
const CharacterGen::BodyShape::Model* CharacterController::shapeModel() const
{
    if (m_shapeModel) return m_shapeModel->valid() ? m_shapeModel.get() : nullptr;
    std::shared_ptr<CharacterGen::Assets> a = m_assets;
    if (!a) {
        if (!CharacterGen::CharacterAssets::present()) return nullptr;   // never downloads from a label lookup
        a = std::make_shared<CharacterGen::Assets>();
        if (!CharacterGen::loadAssetsFromDir(CharacterGen::CharacterAssets::dir(), *a, nullptr)) return nullptr;
    }
    m_shapeModel = std::make_shared<CharacterGen::BodyShape::Model>();
    if (!m_shapeModel->build(*a)) return nullptr;
    // (a lookup made while the panel was already showing: the bindings that read the model must run again)
    QMetaObject::invokeMethod(const_cast<CharacterController*>(this), [this] { const_cast<CharacterController*>(this)->bumpShapeRevision(); }, Qt::QueuedConnection);
    return m_shapeModel.get();
}

namespace {
const CharacterGen::BodyShape::Measure kSliderMeasures[5] = {CharacterGen::BodyShape::ChestWidth, CharacterGen::BodyShape::WaistWidth, CharacterGen::BodyShape::HipsWidth, CharacterGen::BodyShape::BellyDepth, CharacterGen::BodyShape::ChestDepth};
}

QVariantList CharacterController::proportions() const
{
    QVariantList l;
    for (double v : m_proportions) l << v;
    return l;
}

QString CharacterController::proportionLabel(int i) const
{
    static const char* k[] = {"Height", "Leg length", "Arm length", "Torso length", "Shoulder width", "Head size"};
    return (i >= 0 && i < 6) ? tr(k[i]) : QString();
}

QString CharacterController::proportionHint(int i) const
{
    switch (i) {
    case 0: return tr("Overall height: the whole body scales, feet on the floor. 100% is the mean body (about 173 cm).");
    case 1: return tr("Leg length (the legs scale about the hips).");
    case 2: return tr("Arm length (the arms and hands scale about the shoulders).");
    case 3: return tr("Torso length: the spine between the pelvis and the neck.");
    case 4: return tr("Shoulder width: how far the shoulders sit from the spine.");
    case 5: return tr("Head size (the head scales about the neck).");
    default: return QString();
    }
}

void CharacterController::setProportion(int i, double value)
{
    if (i < 0 || i >= 6) return;
    value = std::clamp(value, 0.6, 1.6);
    if (value == m_proportions[size_t(i)]) return;
    m_proportions[size_t(i)] = value;
    emit proportionsChanged();
}

void CharacterController::resetProportions()
{
    m_proportions = {1, 1, 1, 1, 1, 1};
    emit proportionsChanged();
}

QString CharacterController::measureLabel(int i) const
{
    return (i >= 0 && i < 5) ? tr(CharacterGen::BodyShape::info(kSliderMeasures[i]).label) : QString();
}

QString CharacterController::measureHint(int i) const
{
    return (i >= 0 && i < 5) ? tr(CharacterGen::BodyShape::info(kSliderMeasures[i]).hint) : QString();
}

double CharacterController::measureRange(int i) const
{
    return (i >= 0 && i < 5) ? double(CharacterGen::BodyShape::info(kSliderMeasures[i]).range) : 0.0;
}

static std::vector<float> floatsOf(const QVariantList& l)
{
    std::vector<float> v;
    v.reserve(size_t(l.size()));
    for (const QVariant& x : l) v.push_back(x.toFloat());
    return v;
}

double CharacterController::measureOffset(int i, const QVariantList& coefficients) const
{
    const CharacterGen::BodyShape::Model* m = shapeModel();
    return (m && i >= 0 && i < 5) ? double(m->offset(kSliderMeasures[i], floatsOf(coefficients))) : 0.0;
}

QVariantList CharacterController::coefficientsWithMeasure(int i, double target, const QVariantList& coefficients) const
{
    const CharacterGen::BodyShape::Model* m = shapeModel();
    if (!m || i < 0 || i >= 5) return coefficients;
    const std::vector<float> c = m->withOffset(kSliderMeasures[i], float(target), floatsOf(coefficients));
    QVariantList out;
    for (float v : c) out << double(v);
    return out;
}

QString CharacterController::shapeLabel(int m) const
{
    // measured with `qtmesh character --measure-body` (see BodyShape.h): what one unit of each identity mode does to the body
    static const char* k[] = {
        "Weight (overall girth)", "Hips vs shoulders", "Bust vs hips", "Belly depth", "Waist width", "Hips vs belly",
        "Chest depth", "Hips narrowness", "Torso detail", "Waist vs hips"};
    if (m >= 0 && m < 10) return tr(k[m]);
    static const char* head[] = {"Head size and height", "Neck length", "Head roundness", "Narrow, tall head", "Face age", "Narrow head"};
    if (m >= 20 && m < 26) return tr(head[m - 20]);
    if (const CharacterGen::BodyShape::Model* model = shapeModel(); model && m >= 0 && m < model->shapes()) {
        int best = -1;
        float bestScore = 0.0f;
        for (int q = 0; q < CharacterGen::BodyShape::MeasureCount; ++q) {
            const float score = std::fabs(model->effect(CharacterGen::BodyShape::Measure(q), m)) / CharacterGen::BodyShape::info(CharacterGen::BodyShape::Measure(q)).range;
            if (score > bestScore) { bestScore = score; best = q; }
        }
        if (best >= 0 && bestScore > 0.004f) return tr("%1 detail").arg(tr(CharacterGen::BodyShape::info(CharacterGen::BodyShape::Measure(best)).label));
    }
    if (m >= 40 && m < 45) return tr("Arm detail %1").arg(m - 39);
    if (m >= 10 && m < 20) return tr("Torso detail %1").arg(m + 1);
    if (m >= 20 && m < 40) return tr("Head detail %1").arg(m - 19);
    return tr("Shape %1").arg(m + 1);
}

QString CharacterController::shapeHint(int m) const
{
    const CharacterGen::BodyShape::Model* model = shapeModel();
    if (!model || m < 0 || m >= model->shapes()) return tr("A change of the body's shape.");
    std::vector<std::pair<float, int>> eff;
    for (int q = 0; q < CharacterGen::BodyShape::MeasureCount; ++q) {
        const float e = model->effect(CharacterGen::BodyShape::Measure(q), m);
        if (std::fabs(e) >= 0.05f) eff.push_back({std::fabs(e), q});
    }
    std::sort(eff.rbegin(), eff.rend());
    if (eff.empty()) return tr("A very subtle change (under a millimetre per step).");
    QStringList parts;
    for (size_t i = 0; i < std::min<size_t>(3, eff.size()); ++i) {
        const auto q = CharacterGen::BodyShape::Measure(eff[i].second);
        parts << QStringLiteral("%1 %2 cm %3").arg(model->effect(q, m) >= 0 ? QStringLiteral("+") : QStringLiteral("−")).arg(double(eff[i].first), 0, 'f', 1).arg(tr(CharacterGen::BodyShape::info(q).label).toLower());
    }
    return tr("Per +1.0 on the slider: %1.").arg(parts.join(QStringLiteral(", ")));
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

// The character bundles on disk, verified and loaded (downloads on first use). MAIN THREAD. Sets the status line; false with `err` on failure.
bool CharacterController::prepareAssets(QString* err)
{
    using namespace CharacterGen;
    {   // always verify (cheap, cached): a corrupted default bundle is re-fetched; a custom directory is used as it is.
        // Busy for EVERY ensureBlocking call — present() only checks body+head, but ensureBlocking can still fetch a
        // missing licence/hair file or re-fetch a digest mismatch, and its nested event loop would otherwise leave
        // Generate clickable (re-entrant generate()).
        QMetaObject::Connection progressConn;
        setStatus(CharacterAssets::present() ? tr("Checking character assets…")
                                             : tr("Downloading character assets…"),
                  true);
        clearDownloadProgress();
        m_watchingDownload = true;
        if (auto* dl = ModelDownloader::instance()) {
            progressConn = connect(dl, &ModelDownloader::downloadProgressUpdated, this,
                                   &CharacterController::onModelDownloadProgress);
        }
        const bool ok = CharacterAssets::ensureBlocking(err);
        m_watchingDownload = false;
        if (progressConn) disconnect(progressConn);
        clearDownloadProgress();
        if (!ok) {
            setStatus(tr("Character assets unavailable: %1").arg(*err), false);
            return false;
        }
    }
    // the optional hair bundle may have arrived with this ensureBlocking (a first run without hair): reload the cached assets so
    // the style can be built, and tell the panel so its hair controls appear
    if (m_assets && m_assets->hairStyles.empty() && !CharacterAssets::hairStyleNames().isEmpty()) m_assets.reset();
    if (!m_assets) {
        setStatus(tr("Loading character assets…"), true);
        auto a = std::make_shared<Assets>();
        if (!loadAssetsFromDir(CharacterAssets::dir(), *a, err)) {
            setStatus(tr("Character assets could not be loaded: %1").arg(*err), false);
            return false;
        }
        m_assets = std::move(a);
        m_shapeModel.reset();
        bumpShapeRevision();
    }
    if (m_hairNamesSeen != CharacterAssets::hairStyleNames()) {
        m_hairNamesSeen = CharacterAssets::hairStyleNames();
        emit hairNamesChanged();
    }

    return true;
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

    if (!prepareAssets(&err)) return err;

    Params p;
    p.nativeHead = !m_assets->mhrArkit.empty();
    p.openEyes = m_openEyes;
    p.skinTexture = m_skinTexture;
    p.hairStyle = m_hairStyle;
    p.hairProcedural = m_hairParams;
    p.hairFit = m_hairFit;
    CharacterGen::parseHairColour(m_hairColour, &p.hairColour);
    p.bodyHair = float(m_bodyHair);
    p.proportions = Proportions{float(m_proportions[0]), float(m_proportions[1]), float(m_proportions[2]), float(m_proportions[3]), float(m_proportions[4]), float(m_proportions[5])};
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


// ---------------------------------------------------------------------------------------------------------------------------
// Hair Studio
// ---------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr const char* kStudioKey = "Character/hairStudio";

// A small front-view silhouette of a library hairstyle (data URI), cached: the browser's thumbnail.
QString thumbnailUri(const QString& id)
{
    static QHash<QString, QString> cache;
    const auto hit = cache.constFind(id);
    if (hit != cache.constEnd()) return hit.value();
    CharacterGen::Hair::ImportedHair h;
    if (!CharacterGen::Hair::Library::load(id, &h)) return QString();
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (const auto& p : h.pos) { lo[0] = std::min(lo[0], p.x); hi[0] = std::max(hi[0], p.x); lo[1] = std::min(lo[1], p.y); hi[1] = std::max(hi[1], p.y); }
    const float span = std::max(1e-6f, std::max(hi[0] - lo[0], hi[1] - lo[1]));
    const int S = 96;
    QImage img(S, S, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter pt(&img);
    pt.setRenderHint(QPainter::Antialiasing);
    pt.setPen(Qt::NoPen);
    pt.setBrush(QColor(120, 105, 95, 200));
    auto map = [&](const CharacterGen::Hair::V3& v) { return QPointF(4 + (v.x - lo[0]) / span * (S - 8), S - 4 - (v.y - lo[1]) / span * (S - 8)); };
    const size_t step = std::max<size_t>(1, h.idx.size() / 3 / 4000);
    for (size_t t = 0; t + 2 < h.idx.size(); t += 3 * step) pt.drawPolygon(QPolygonF{map(h.pos[h.idx[t]]), map(h.pos[h.idx[t + 1]]), map(h.pos[h.idx[t + 2]])});
    pt.end();
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    const QString uri = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
    cache.insert(id, uri);
    return uri;
}

}  // namespace

QString CharacterController::hairSource() const
{
    if (m_hairStyle.isEmpty()) return QStringLiteral("none");
    if (m_hairStyle.startsWith(QLatin1String("proc:"))) return QStringLiteral("procedural");
    if (m_hairStyle.startsWith(QLatin1String("lib:"))) return QStringLiteral("library");
    return QStringLiteral("bundle");
}

QVariantList CharacterController::hairParamSpecs() const
{
    QVariantList list;
    for (const CharacterGen::Hair::ParamSpec& s : CharacterGen::Hair::paramSpecs())
        list.append(QVariantMap{{"key", s.key}, {"label", s.label}, {"min", s.min}, {"max", s.max}, {"step", s.step}});
    return list;
}

QVariantMap CharacterController::hairParams() const
{
    QVariantMap m;
    for (const CharacterGen::Hair::ParamSpec& s : CharacterGen::Hair::paramSpecs()) m.insert(s.key, CharacterGen::Hair::getParam(m_hairParams, s.key));
    m.insert(QStringLiteral("symmetric"), CharacterGen::Hair::getParam(m_hairParams, QStringLiteral("symmetric")));
    return m;
}

QVariantMap CharacterController::hairFitParams() const { return CharacterGen::Hair::toJson(m_hairFit).toVariantMap(); }
QStringList CharacterController::hairPresetIds() const { return CharacterGen::Hair::presetIds(); }
QStringList CharacterController::savedHairPresets() const { return CharacterGen::Hair::Library::presetNames(); }
QString CharacterController::hairPresetLabel(const QString& id) const { return CharacterGen::Hair::presetLabel(id); }
QString CharacterController::hairFileFilter() const { return QStringLiteral("Hair models (*.fbx *.obj *.glb *.gltf *.dae *.ply);;All files (*)"); }

QVariantList CharacterController::hairLibrary() const
{
    QVariantList list;
    for (const CharacterGen::Hair::Library::Meta& m : CharacterGen::Hair::Library::list())
        list.append(QVariantMap{{"id", m.id}, {"name", m.name}, {"length", m.length}, {"texture", m.texture}, {"gender", m.gender}, {"style", m.style},
                                {"license", m.license}, {"author", m.author}, {"collection", m.collection}, {"source", m.source}, {"thumbnail", thumbnailUri(m.id)}});
    return list;
}

void CharacterController::hairChanged()
{
    emit hairStyleChanged();
    emit hairParamsChanged();
    saveHairStudioSettings();
}

void CharacterController::useProceduralHair(const QString& presetId)
{
    CharacterGen::Hair::Params p;
    if (CharacterGen::Hair::isPreset(presetId)) p = CharacterGen::Hair::presetParams(presetId);
    else if (!CharacterGen::Hair::Library::loadPreset(presetId, &p)) { m_hairStudioStatus = tr("Unknown hair preset '%1'").arg(presetId); emit hairLibraryChanged(); return; }
    m_hairParams = p;
    m_hairStyle = QStringLiteral("proc:") + presetId;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("character hair procedural %1").arg(presetId));
    hairChanged();
}

void CharacterController::useLibraryHair(const QString& id)
{
    CharacterGen::Hair::ImportedHair h;
    CharacterGen::Hair::Library::Meta meta;
    // load first: a failed load must leave the current style AND its fit as they were
    if (!CharacterGen::Hair::Library::load(id, &h, &meta)) { m_hairStudioStatus = tr("The hairstyle '%1' is missing or damaged.").arg(id); emit hairLibraryChanged(); return; }
    m_hairFit = meta.fit.isEmpty() ? CharacterGen::Hair::FitAdjust() : CharacterGen::Hair::fitAdjustFromJson(meta.fit);
    m_hairStyle = QStringLiteral("lib:") + id;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("character hair library"));
    hairChanged();
}

void CharacterController::clearHair()
{
    m_hairStyle.clear();
    hairChanged();
}

void CharacterController::setHairParam(const QString& key, double value)
{
    if (!CharacterGen::Hair::setParam(m_hairParams, key, value)) return;
    emit hairParamsChanged();
    saveHairStudioSettings();
}

void CharacterController::setHairFitParam(const QString& key, double value)
{
    QJsonObject o = CharacterGen::Hair::toJson(m_hairFit);
    static const QStringList numeric{"tx", "ty", "tz", "rx", "ry", "rz", "scale", "snug", "clearance"};
    if (key == QLatin1String("flip")) o["flipFront"] = value != 0.0;
    else if (numeric.contains(key)) o[key] = value;
    else return;
    m_hairFit = CharacterGen::Hair::fitAdjustFromJson(o);
    emit hairParamsChanged();
    saveHairStudioSettings();
}

void CharacterController::resetHairParams()
{
    const QString id = m_hairStyle.startsWith(QLatin1String("proc:")) ? m_hairStyle.mid(5) : QString();
    CharacterGen::Hair::Params p;
    if (CharacterGen::Hair::isPreset(id)) p = CharacterGen::Hair::presetParams(id);
    else if (!id.isEmpty()) CharacterGen::Hair::Library::loadPreset(id, &p);
    m_hairParams = p;
    emit hairParamsChanged();
    saveHairStudioSettings();
}

void CharacterController::resetHairFit()
{
    m_hairFit = CharacterGen::Hair::FitAdjust();
    emit hairParamsChanged();
    saveHairStudioSettings();
}

bool CharacterController::saveHairPreset(const QString& name)
{
    QString err;
    if (CharacterGen::Hair::isPreset(name.trimmed())) {   // the browser lists a saved preset under the same key as the built-in one: the built-in would always win
        m_hairStudioStatus = tr("'%1' is a built-in hair preset. Choose another name.").arg(name.trimmed());
        emit hairLibraryChanged();
        return false;
    }
    CharacterGen::Hair::Params p = m_hairParams;
    p.preset = name;
    if (!CharacterGen::Hair::Library::savePreset(name, p, &err)) { m_hairStudioStatus = err; emit hairLibraryChanged(); return false; }
    m_hairStudioStatus = tr("Saved hair preset '%1'.").arg(name);
    emit hairLibraryChanged();
    return true;
}

bool CharacterController::deleteHairPreset(const QString& name)
{
    const bool ok = CharacterGen::Hair::Library::removePreset(name);
    emit hairLibraryChanged();
    return ok;
}

QString CharacterController::importHair(const QString& path, const QString& author, const QString& license, const QString& attribution)
{
    CharacterGen::Hair::Library::ImportOptions opt;
    opt.author = author;
    opt.license = license;
    opt.attribution = attribution;
    const QString local = path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path;
    const auto r = QFileInfo(local).isDir() ? CharacterGen::Hair::Library::importFolder(local, opt) : CharacterGen::Hair::Library::importFile(local, opt);
    m_hairStudioStatus = tr("Hair import: %1 found, %2 added, %3 already in the library, %4 failed.%5")
                             .arg(r.found).arg(r.added).arg(r.duplicates).arg(r.failed)
                             .arg(license.isEmpty() ? tr(" No licence was entered: check the source before sharing characters that use it.") : QString());
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"), QStringLiteral("character hair import added=%1").arg(r.added));
    emit hairLibraryChanged();
    return m_hairStudioStatus;
}

bool CharacterController::removeLibraryHair(const QString& id)
{
    // remove first: the selection is only cleared once the hairstyle really is gone (a failed removal keeps both the entry and the selection)
    const bool ok = CharacterGen::Hair::Library::remove(id);
    if (ok) {
        if (m_hairStyle == QLatin1String("lib:") + id) { m_hairStyle.clear(); hairChanged(); }
        m_hairStudioStatus = tr("Removed the hairstyle '%1' from the library.").arg(id);
    } else {
        m_hairStudioStatus = tr("The hairstyle '%1' could not be removed (is the hair library folder writable?).").arg(id);   // shown by the panel
    }
    emit hairLibraryChanged();
    return ok;
}

QString CharacterController::duplicateHair(const QString& name)
{
    if (name.trimmed().isEmpty()) return QString();
    if (hairSource() == QLatin1String("procedural")) {
        if (!saveHairPreset(name)) return QString();
        m_hairStyle = QStringLiteral("proc:") + name;
        hairChanged();
        return m_hairStyle;
    }
    if (hairSource() == QLatin1String("library")) {
        CharacterGen::Hair::ImportedHair h;
        CharacterGen::Hair::Library::Meta meta;
        if (!CharacterGen::Hair::Library::load(m_hairStyle.mid(4), &h, &meta)) return QString();
        // a copy has to differ in geometry to pass the duplicate test: nudge one vertex by a tenth of a millimetre-quantum
        if (!h.pos.empty()) h.pos[0].x += 0.0002f;
        meta.name = name;
        meta.fit = CharacterGen::Hair::toJson(m_hairFit);
        QString id, dup, err;
        if (!CharacterGen::Hair::Library::save(h, meta, &id, &dup, &err)) return QString();
        if (id.isEmpty()) id = dup;                 // this exact copy already exists: select it instead of doing nothing
        if (id.isEmpty()) return QString();
        m_hairStyle = QStringLiteral("lib:") + id;
        emit hairLibraryChanged();
        hairChanged();
        return m_hairStyle;
    }
    return QString();
}

void CharacterController::saveHairStudioSettings() const
{
    CharacterGen::Hair::Library::StudioConfig c;
    c.style = m_hairStyle;
    c.procedural = m_hairParams;
    c.fit = m_hairFit;
    c.colour = m_hairColour;
    QSettings().setValue(QLatin1String(kStudioKey), QString::fromUtf8(QJsonDocument(CharacterGen::Hair::Library::toJson(c)).toJson(QJsonDocument::Compact)));
}

void CharacterController::loadHairStudioSettings()
{
    CharacterGen::Hair::Library::StudioConfig c;
    const QByteArray raw = QSettings().value(QLatin1String(kStudioKey)).toString().toUtf8();
    if (raw.isEmpty() || !CharacterGen::Hair::Library::fromJson(QJsonDocument::fromJson(raw).object(), &c)) return;
    m_hairStyle = c.style;
    m_hairParams = c.procedural;
    m_hairFit = c.fit;
    CharacterGen::Rgb colour;
    if (CharacterGen::parseHairColour(c.colour, &colour)) m_hairColour = CharacterGen::hairColourHex(colour);   // validated: a damaged value keeps the default
}

namespace {

struct Tags { QString length, texture, gender, style; };

Tags tagsForProcedural(const QString& id)
{
    static const QHash<QString, Tags> t = {
        {"short_straight", {"short", "straight", "unisex", "crop"}},         {"medium_layered", {"medium", "straight", "feminine", "layered"}},
        {"long_straight", {"long", "straight", "feminine", "long"}},         {"wavy", {"medium", "wavy", "feminine", "wavy"}},
        {"curly", {"medium", "curly", "unisex", "curls"}},                   {"bob", {"short", "straight", "feminine", "bob"}},
        {"ponytail", {"long", "straight", "feminine", "ponytail"}},          {"short_side_part", {"short", "straight", "masculine", "side part"}},
        {"pixie", {"short", "straight", "feminine", "pixie"}},               {"buzz", {"short", "straight", "masculine", "fade"}},
        {"afro", {"medium", "coily", "unisex", "afro"}},                     {"curly_bob", {"short", "curly", "feminine", "bob"}},
        {"shag", {"medium", "wavy", "unisex", "layered"}},                   {"long_wavy", {"long", "wavy", "feminine", "wavy"}},
        {"high_ponytail", {"long", "straight", "feminine", "ponytail"}},     {"low_ponytail", {"long", "straight", "unisex", "ponytail"}},
        {"side_swept_long", {"long", "straight", "feminine", "long"}}};
    return t.value(id, Tags{"medium", "straight", "unisex", QString()});
}

// The MakeHuman bundle styles carry no metadata: read what the name says (short02, bob01, afro01, ponytail02, braid01 ...).
Tags tagsForBundleName(const QString& name)
{
    const QString n = name.toLower();
    Tags t{"medium", "straight", "unisex", QString()};
    for (const char* k : {"short", "long", "bob", "pixie", "afro", "ponytail", "braid", "bun", "mohawk"}) {
        if (!n.contains(QLatin1String(k))) continue;
        t.style = QString::fromLatin1(k);
        break;
    }
    if (n.contains(QLatin1String("short")) || n.contains(QLatin1String("bob")) || n.contains(QLatin1String("pixie"))) t.length = QStringLiteral("short");
    if (n.contains(QLatin1String("long")) || n.contains(QLatin1String("ponytail")) || n.contains(QLatin1String("braid"))) t.length = QStringLiteral("long");
    if (n.contains(QLatin1String("afro"))) t.texture = QStringLiteral("coily");
    return t;
}

bool matchesFilter(const QString& filter, const QString& value) { return filter.isEmpty() || filter == QLatin1String("all") || filter == value; }

}  // namespace

QVariantList CharacterController::hairBrowser(const QString& kind, const QString& length, const QString& texture, const QString& gender, const QString& style) const
{
    QVariantList out;
    const QString needle = style.trimmed().toLower();
    auto add = [&](const QString& key, const QString& k, const QString& name, const Tags& t, const QString& thumb, const QString& license, const QString& author) {
        if (!matchesFilter(kind, k) || !matchesFilter(length, t.length) || !matchesFilter(texture, t.texture) || !matchesFilter(gender, t.gender)) return;
        if (!needle.isEmpty() && !t.style.toLower().contains(needle) && !name.toLower().contains(needle)) return;
        out.append(QVariantMap{{"key", key}, {"kind", k}, {"name", name}, {"length", t.length}, {"texture", t.texture}, {"gender", t.gender}, {"style", t.style},
                               {"thumbnail", thumb}, {"license", license}, {"author", author}, {"current", m_hairStyle == key}});
    };
    for (const QString& n : hairNames()) add(n, QStringLiteral("builtin"), n, tagsForBundleName(n), QString(), QStringLiteral("CC0"), QStringLiteral("MakeHuman"));
    for (const QString& id : CharacterGen::Hair::presetIds()) add(QStringLiteral("proc:") + id, QStringLiteral("procedural"), CharacterGen::Hair::presetLabel(id), tagsForProcedural(id), QString(), QString(), QString());
    for (const QString& n : CharacterGen::Hair::Library::presetNames()) add(QStringLiteral("proc:") + n, QStringLiteral("procedural"), n, Tags{"medium", "straight", "unisex", QStringLiteral("saved")}, QString(), QString(), QString());
    if (matchesFilter(kind, QStringLiteral("imported")))   // the thumbnail of an entry may have to be rendered: not for entries the filter drops anyway
        for (const CharacterGen::Hair::Library::Meta& m : CharacterGen::Hair::Library::list())
            add(QStringLiteral("lib:") + m.id, QStringLiteral("imported"), m.name, Tags{m.length, m.texture, m.gender, m.style}, thumbnailUri(m.id), m.license, m.author);
    return out;
}

void CharacterController::selectHair(const QString& key)
{
    if (key.startsWith(QLatin1String("proc:"))) useProceduralHair(key.mid(5));
    else if (key.startsWith(QLatin1String("lib:"))) useLibraryHair(key.mid(4));
    else { m_hairStyle = key; hairChanged(); }
}
