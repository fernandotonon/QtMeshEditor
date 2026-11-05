#include "HairLibrary.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace CharacterGen {
namespace Hair {
namespace Library {

namespace {

constexpr quint32 kMagic = 0x484D4851;   // "QHMH"
constexpr quint32 kVersion = 1;

QString importedDir() { return dir() + QStringLiteral("/imported"); }
QString entryDir(const QString& id) { return importedDir() + QLatin1Char('/') + id; }
// A removed hairstyle is moved here, not deleted: the undo history of the editor may still refer to it (undoing the edit that followed a
// removal rebuilds the earlier character). The trash is emptied at the next start (purgeTrash).
QString trashDir() { return dir() + QStringLiteral("/trash"); }
QString trashEntryDir(const QString& id) { return trashDir() + QLatin1Char('/') + id; }
QString resolveEntryDir(const QString& id)
{
    if (QFileInfo::exists(entryDir(id) + QStringLiteral("/hair.qmh"))) return entryDir(id);
    if (QFileInfo::exists(trashEntryDir(id) + QStringLiteral("/hair.qmh"))) return trashEntryDir(id);
    return entryDir(id);
}

bool validId(const QString& id) { return !id.isEmpty() && id.size() < 120 && !id.startsWith(QLatin1Char('.')) && !id.contains(QLatin1Char('/')) && !id.contains(QLatin1Char('\\')) && !id.contains(QStringLiteral("..")); }

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) return false;
    return f.commit();
}

QByteArray encode(const ImportedHair& h)
{
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_0);
    s.setFloatingPointPrecision(QDataStream::SinglePrecision);
    s << kMagic << kVersion << quint32(h.pos.size()) << quint32(h.idx.size());
    for (const V3& p : h.pos) s << p.x << p.y << p.z;
    for (size_t i = 0; i < h.pos.size() * 2; ++i) s << (i < h.uv.size() ? h.uv[i] : 0.0f);
    for (uint32_t i : h.idx) s << quint32(i);
    return out;
}

bool decode(const QByteArray& bytes, ImportedHair* h)
{
    QDataStream s(bytes);
    s.setVersion(QDataStream::Qt_6_0);
    s.setFloatingPointPrecision(QDataStream::SinglePrecision);
    quint32 magic = 0, ver = 0, nv = 0, ni = 0;
    s >> magic >> ver >> nv >> ni;
    if (s.status() != QDataStream::Ok || magic != kMagic || ver != kVersion || nv > 5000000 || ni > 30000000 || ni % 3 != 0) return false;
    h->pos.resize(nv);
    h->uv.resize(size_t(nv) * 2);
    h->idx.resize(ni);
    for (V3& p : h->pos) s >> p.x >> p.y >> p.z;
    for (float& f : h->uv) s >> f;
    for (uint32_t& i : h->idx) { quint32 v; s >> v; i = v; }
    if (s.status() != QDataStream::Ok) return false;
    for (uint32_t i : h->idx) if (i >= nv) return false;
    for (const V3& p : h->pos) if (!finite(p)) return false;
    return true;
}

}  // namespace

QJsonObject Meta::toJson() const
{
    return QJsonObject{{"schema", "qtmesh-hair-entry-v1"}, {"id", id}, {"name", name}, {"kind", kind}, {"author", author}, {"source", source},
                       {"license", license}, {"attribution", attribution}, {"collection", collection}, {"length", length}, {"texture", texture},
                       {"gender", gender}, {"style", style}, {"hash", geometryHash}, {"triangles", triangles}, {"fit", fit}};
}

Meta Meta::fromJson(const QJsonObject& o)
{
    Meta m;
    m.id = o.value("id").toString(); m.name = o.value("name").toString(); m.kind = o.value("kind").toString(QStringLiteral("imported"));
    m.author = o.value("author").toString(); m.source = o.value("source").toString(); m.license = o.value("license").toString();
    m.attribution = o.value("attribution").toString(); m.collection = o.value("collection").toString();
    m.length = o.value("length").toString(); m.texture = o.value("texture").toString(); m.gender = o.value("gender").toString();
    m.style = o.value("style").toString(); m.geometryHash = o.value("hash").toString(); m.triangles = o.value("triangles").toInt();
    m.fit = o.value("fit").toObject();
    return m;
}

QJsonObject toJson(const StudioConfig& c)
{
    return QJsonObject{{"schema", "qtmesh-hair-studio-v1"}, {"style", c.style}, {"procedural", Hair::toJson(c.procedural)}, {"fit", Hair::toJson(c.fit)}, {"colour", c.colour}};
}

bool fromJson(const QJsonObject& o, StudioConfig* out)
{
    if (!out || o.value("schema").toString() != QLatin1String("qtmesh-hair-studio-v1")) return false;
    StudioConfig c;
    c.style = o.value("style").toString();
    c.colour = o.value("colour").toString();
    c.fit = fitAdjustFromJson(o.value("fit").toObject());
    if (o.value("procedural").isObject()) {
        if (!Hair::fromJson(o.value("procedural").toObject(), &c.procedural)) return false;
    } else if (c.style.startsWith(QLatin1String("proc:"))) {
        // no parameters stored: the style id names them (a built-in preset, or a saved one) instead of silently generating default hair
        const QString id = c.style.mid(5);
        if (Hair::isPreset(id)) c.procedural = Hair::presetParams(id);
        else if (!loadPreset(id, &c.procedural)) return false;
    }
    *out = c;
    return true;
}

bool saveConfigFile(const QString& path, const StudioConfig& c) { return writeFile(path, QJsonDocument(toJson(c)).toJson()); }

bool loadConfigFile(const QString& path, StudioConfig* out)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && fromJson(QJsonDocument::fromJson(f.readAll()).object(), out);
}

QString dir()
{
    const QByteArray env = qgetenv("QTMESH_HAIR_LIBRARY_DIR");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/hair_library");
}

QString geometryHash(const ImportedHair& h)
{
    // quantise, then hash the SORTED set of triangles (each triangle = its three quantised corners, sorted): independent of vertex /
    // triangle order, so the same hairstyle re-exported by another tool still collides.
    auto q = [](const V3& p) { return QStringLiteral("%1,%2,%3").arg(qlonglong(std::llround(p.x * 10000.0f))).arg(qlonglong(std::llround(p.y * 10000.0f))).arg(qlonglong(std::llround(p.z * 10000.0f))); };
    QStringList tris;
    for (size_t t = 0; t + 2 < h.idx.size(); t += 3) {
        if (h.idx[t] >= h.pos.size() || h.idx[t + 1] >= h.pos.size() || h.idx[t + 2] >= h.pos.size()) continue;
        QStringList c{q(h.pos[h.idx[t]]), q(h.pos[h.idx[t + 1]]), q(h.pos[h.idx[t + 2]])};
        c.sort();
        tris << c.join(QLatin1Char('|'));
    }
    tris.sort();
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const QString& s : tris) hash.addData(s.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

QString makeId(const QString& name, const QString& hash)
{
    QString base = name.toLower();
    base.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    base = base.left(40);
    while (base.startsWith(QLatin1Char('_'))) base.remove(0, 1);
    while (base.endsWith(QLatin1Char('_'))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("hair");
    return base + QLatin1Char('_') + hash.left(8);
}

void inferTags(const ImportedHair& h, Meta* meta)
{
    if (!meta || h.pos.empty()) return;
    V3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (const V3& p : h.pos) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const float width = std::max(1e-6f, hi.x - lo.x), height = hi.y - lo.y;
    const float ratio = height / width;
    const QString lname = meta->name.toLower();
    if (meta->length.isEmpty()) meta->length = ratio < 0.75f ? QStringLiteral("short") : (ratio < 1.35f ? QStringLiteral("medium") : QStringLiteral("long"));
    if (meta->texture.isEmpty()) {
        meta->texture = lname.contains(QStringLiteral("curl")) ? QStringLiteral("curly") : lname.contains(QStringLiteral("coil")) || lname.contains(QStringLiteral("afro")) ? QStringLiteral("coily")
                        : lname.contains(QStringLiteral("wav")) ? QStringLiteral("wavy") : QStringLiteral("straight");
    }
    if (meta->gender.isEmpty()) meta->gender = lname.contains(QStringLiteral("woman")) || lname.contains(QStringLiteral("female")) ? QStringLiteral("feminine")
                                              : lname.contains(QStringLiteral("man")) || lname.contains(QStringLiteral("male")) ? QStringLiteral("masculine") : QStringLiteral("unisex");
    if (meta->style.isEmpty())
        for (const char* k : {"ponytail", "bun", "bob", "pixie", "afro", "fade", "braid", "bangs", "undercut"})
            if (lname.contains(QLatin1String(k))) { meta->style = QString::fromLatin1(k); break; }
}

QList<Meta> list()
{
    QList<Meta> out;
    const QDir d(importedDir());
    for (const QString& id : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile f(entryDir(id) + QStringLiteral("/meta.json"));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) continue;
        Meta m = Meta::fromJson(doc.object());
        m.id = id;
        out.append(m);
    }
    std::sort(out.begin(), out.end(), [](const Meta& a, const Meta& b) { return a.name.toLower() < b.name.toLower(); });
    return out;
}

bool load(const QString& id, ImportedHair* hair, Meta* meta)
{
    if (!hair || !validId(id)) return false;
    const QString entry = resolveEntryDir(id);
    QFile mf(entry + QStringLiteral("/meta.json"));
    QFile gf(entry + QStringLiteral("/hair.qmh"));
    if (!mf.open(QIODevice::ReadOnly) || !gf.open(QIODevice::ReadOnly)) return false;
    ImportedHair h;
    if (!decode(gf.readAll(), &h)) return false;
    const QJsonDocument metaDoc = QJsonDocument::fromJson(mf.readAll());
    if (!metaDoc.isObject()) return false;       // a truncated or damaged meta.json is a damaged entry, like in list()
    const Meta m = Meta::fromJson(metaDoc.object());
    h.id = id;
    h.name = m.name;
    QFile tf(entry + QStringLiteral("/texture.png"));
    if (tf.open(QIODevice::ReadOnly)) h.texturePng = tf.readAll();
    *hair = std::move(h);
    if (meta) { *meta = m; meta->id = id; }
    return true;
}

bool save(const ImportedHair& hair, Meta meta, QString* newId, QString* duplicateOf, QString* error)
{
    if (newId) newId->clear();
    if (hair.pos.empty() || hair.idx.size() < 3) { if (error) *error = QStringLiteral("empty hairstyle"); return false; }
    // what decode() would refuse must not be registered (an entry load() cannot read, and a re-import that reports a duplicate)
    if (hair.pos.size() > 5000000 || hair.idx.size() > 30000000 || hair.idx.size() % 3 != 0) { if (error) *error = QStringLiteral("hairstyle exceeds the library format limits"); return false; }
    for (const V3& p : hair.pos)
        if (!finite(p)) { if (error) *error = QStringLiteral("hairstyle has non-finite vertex positions"); return false; }
    meta.geometryHash = geometryHash(hair);
    for (const Meta& e : list())
        if (e.geometryHash == meta.geometryHash) {
            if (duplicateOf) *duplicateOf = e.id;
            return true;
        }
    if (meta.name.isEmpty()) meta.name = hair.name.isEmpty() ? QStringLiteral("Hairstyle") : hair.name;
    meta.id = makeId(meta.name, meta.geometryHash);
    meta.triangles = hair.triangles();
    inferTags(hair, &meta);
    if (!QDir().mkpath(entryDir(meta.id))) { if (error) *error = QStringLiteral("cannot create %1").arg(entryDir(meta.id)); return false; }
    bool ok = writeFile(entryDir(meta.id) + QStringLiteral("/hair.qmh"), encode(hair));
    if (ok && !hair.texturePng.isEmpty()) ok = writeFile(entryDir(meta.id) + QStringLiteral("/texture.png"), hair.texturePng);
    if (ok) ok = writeFile(entryDir(meta.id) + QStringLiteral("/meta.json"), QJsonDocument(meta.toJson()).toJson());
    if (!ok) { if (error) *error = QStringLiteral("cannot write the library entry"); QDir(entryDir(meta.id)).removeRecursively(); return false; }
    if (newId) *newId = meta.id;
    return true;
}

bool remove(const QString& id)
{
    if (!validId(id) || !QFileInfo::exists(entryDir(id))) return false;
    QDir().mkpath(trashDir());
    QDir(trashEntryDir(id)).removeRecursively();                       // an older copy of the same id
    if (QDir().rename(entryDir(id), trashEntryDir(id))) return true;
    return QDir(entryDir(id)).removeRecursively();                     // could not move it: delete it
}

void purgeTrash() { QDir(trashDir()).removeRecursively(); }

bool updateMeta(const Meta& meta)
{
    if (!validId(meta.id) || !QFileInfo::exists(entryDir(meta.id) + QStringLiteral("/hair.qmh"))) return false;
    return writeFile(entryDir(meta.id) + QStringLiteral("/meta.json"), QJsonDocument(meta.toJson()).toJson());
}

// ---- presets ----------------------------------------------------------------------------------------------------------------

namespace {
// One file per distinct name: the readable stem alone made "Long/curls" and "Long?curls" the same file (the second silently overwrote the
// first), so a hash of the exact name is part of the file name. The real name is stored inside the file (`preset`).
QString presetFile(const QString& name)
{
    const QString key = name.trimmed();
    QString stem = key;
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_\\- ]+")), QStringLiteral("_"));
    stem = stem.trimmed().left(40);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return dir() + QStringLiteral("/presets/") + stem + QLatin1Char('-') + hash + QStringLiteral(".json");
}
}  // namespace

QStringList presetNames()
{
    QStringList names;
    for (const QString& f : QDir(dir() + QStringLiteral("/presets")).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile file(dir() + QStringLiteral("/presets/") + f);
        QString name;
        if (file.open(QIODevice::ReadOnly)) name = QJsonDocument::fromJson(file.readAll()).object().value("preset").toString().trimmed();
        names << (name.isEmpty() ? f.left(f.size() - 5) : name);
    }
    names.removeDuplicates();
    return names;
}

bool savePreset(const QString& name, const Params& p, QString* error)
{
    if (name.trimmed().isEmpty()) { if (error) *error = QStringLiteral("a preset needs a name"); return false; }
    QDir().mkpath(dir() + QStringLiteral("/presets"));
    Params q = p;
    q.preset = name.trimmed();      // the stored name is the real one (the file name is only a readable key)
    return writeFile(presetFile(name), QJsonDocument(toJson(q)).toJson());
}

bool loadPreset(const QString& name, Params* p, QString* error)
{
    QFile f(presetFile(name));
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = QStringLiteral("no such preset"); return false; }
    return fromJson(QJsonDocument::fromJson(f.readAll()).object(), p, error);
}

bool removePreset(const QString& name) { return QFile::remove(presetFile(name)); }

}  // namespace Library
}  // namespace Hair
}  // namespace CharacterGen
