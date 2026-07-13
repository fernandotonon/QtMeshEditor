#include "FaceCapMapper.h"

#include "FaceCapCanonicalData.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace FaceCapMapper {

namespace {

const QHash<QString, QString>& aliasTable()
{
    // normalized third-party name -> normalized canonical name. Only entries
    // the side-expansion + normalization pass can NOT already resolve.
    static const QHash<QString, QString> table = {
        // CC/iClone
        {QStringLiteral("mouthopen"), QStringLiteral("jawopen")},
        {QStringLiteral("browraiseinnerleft"), QStringLiteral("browinnerup")},
        {QStringLiteral("browraiseinnerright"), QStringLiteral("browinnerup")},
        {QStringLiteral("browraiseouterleft"), QStringLiteral("browouterupleft")},
        {QStringLiteral("browraiseouterright"), QStringLiteral("browouterupright")},
        {QStringLiteral("browdropleft"), QStringLiteral("browdownleft")},
        {QStringLiteral("browdropright"), QStringLiteral("browdownright")},
        {QStringLiteral("eyeswideleft"), QStringLiteral("eyewideleft")},
        {QStringLiteral("eyeswideright"), QStringLiteral("eyewideright")},
        // common shorthand
        {QStringLiteral("blinkleft"), QStringLiteral("eyeblinkleft")},
        {QStringLiteral("blinkright"), QStringLiteral("eyeblinkright")},
        {QStringLiteral("smileleft"), QStringLiteral("mouthsmileleft")},
        {QStringLiteral("smileright"), QStringLiteral("mouthsmileright")},
    };
    return table;
}

// "Mouth_Smile_L" -> "mouth smile left" token stream -> normalized join.
QString expandSideTokens(const QString& name)
{
    // split on separator characters AND camelCase boundaries
    QStringList tokens;
    QString current;
    for (const QChar c : name) {
        if (c == QLatin1Char('_') || c == QLatin1Char('-')
            || c == QLatin1Char('.') || c.isSpace()) {
            if (!current.isEmpty()) tokens << current;
            current.clear();
        } else if (c.isUpper() && !current.isEmpty()
                   && current.back().isLower()) {
            tokens << current;
            current = c;
        } else {
            current += c;
        }
    }
    if (!current.isEmpty()) tokens << current;
    if (!tokens.isEmpty()) {
        const QString last = tokens.last().toLower();
        if (last == QLatin1String("l"))
            tokens.last() = QStringLiteral("left");
        else if (last == QLatin1String("r"))
            tokens.last() = QStringLiteral("right");
    }
    return tokens.join(QString()).toLower();
}

}  // namespace

QString normalizedName(const QString& name)
{
    return expandSideTokens(name);
}

Mapping build(const QStringList& meshTargetNames, const QString& overrideJsonPath)
{
    Mapping mapping;

    QHash<QString, QString> overrideMap;  // canonical name -> mesh name
    QSet<QString> ignore;
    if (!overrideJsonPath.isEmpty()) {
        QFile f(overrideJsonPath);
        if (!f.open(QIODevice::ReadOnly)) {
            mapping.error = QStringLiteral("cannot open mapping override: %1")
                                .arg(overrideJsonPath);
        } else {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
            if (doc.isNull()) {
                mapping.error = QStringLiteral("mapping override parse error: %1")
                                    .arg(parseError.errorString());
            } else {
                const QJsonObject root = doc.object();
                const QJsonObject map = root.value(QLatin1String("map")).toObject();
                for (auto it = map.begin(); it != map.end(); ++it)
                    overrideMap.insert(it.key(), it.value().toString());
                const QJsonArray ign = root.value(QLatin1String("ignore")).toArray();
                for (const auto& v : ign)
                    ignore.insert(v.toString());
            }
        }
    }

    // normalized mesh name -> exact mesh name (first wins on collision)
    QHash<QString, QString> meshByNorm;
    for (const QString& mesh : meshTargetNames) {
        const QString norm = normalizedName(mesh);
        if (!meshByNorm.contains(norm))
            meshByNorm.insert(norm, mesh);
    }

    QSet<QString> usedMeshTargets;
    for (int i = 0; i < FaceCap::kBlendshapeCount; ++i) {
        const QString canonical = QString::fromLatin1(FaceCap::kBlendshapeNames[i]);
        if (canonical == QLatin1String("_neutral"))
            continue;  // rest weight; no mesh equivalent by design
        if (ignore.contains(canonical)) {
            mapping.ignored << canonical;
            continue;
        }
        QString meshName;
        if (overrideMap.contains(canonical)) {
            const QString target = overrideMap.value(canonical);
            if (meshTargetNames.contains(target)) {
                meshName = target;
            } else {
                mapping.error = mapping.error.isEmpty()
                    ? QStringLiteral("override target '%1' (for %2) not on mesh")
                          .arg(target, canonical)
                    : mapping.error;
            }
        }
        if (meshName.isEmpty()) {
            const QString norm = normalizedName(canonical);
            meshName = meshByNorm.value(norm);
        }
        if (meshName.isEmpty()) {
            // alias table: some mesh naming convention that normalizes to a
            // different string but means this canonical channel
            const QString canonNorm = normalizedName(canonical);
            for (auto it = meshByNorm.begin(); it != meshByNorm.end(); ++it) {
                if (aliasTable().value(it.key()) == canonNorm) {
                    meshName = it.value();
                    break;
                }
            }
        }
        if (meshName.isEmpty()) {
            mapping.unmatchedCanonical << canonical;
        } else {
            mapping.channels.append({i, meshName});
            usedMeshTargets.insert(meshName);
        }
    }

    for (const QString& mesh : meshTargetNames)
        if (!usedMeshTargets.contains(mesh))
            mapping.unmatchedMesh << mesh;

    return mapping;
}

}  // namespace FaceCapMapper
