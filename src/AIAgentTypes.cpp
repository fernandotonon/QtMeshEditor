#include "AIAgentTypes.h"

#include <QJsonDocument>
#include <QRegularExpression>

namespace AIAgent {

QString Step::signature() const
{
    QJsonObject o{{"tool", tool}, {"arguments", arguments}};
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject Step::toJson() const
{
    return {
        {"tool", tool},
        {"arguments", arguments},
        {"why", why},
        {"status", statusName(status)},
        {"attempts", attempts},
        {"error", error},
    };
}

QJsonObject Observation::toJson(bool includeRaw) const
{
    QJsonObject o{
        {"step", stepIndex},
        {"tool", tool},
        {"status", status},
        {"artifacts", QJsonArray::fromStringList(artifacts)},
        {"facts", facts},
        {"warnings", QJsonArray::fromStringList(warnings)},
    };
    if (!error.isEmpty()) o["error"] = error;
    if (includeRaw) o["raw"] = raw;
    return o;
}

QString Observation::toPromptLine() const
{
    QJsonObject o{{"step", stepIndex + 1}, {"tool", tool}, {"status", status}};
    if (!facts.isEmpty())     o["facts"] = facts;
    if (!artifacts.isEmpty()) o["artifacts"] = QJsonArray::fromStringList(artifacts);
    if (!error.isEmpty())     o["error"] = error.left(200);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

int Plan::nextPendingIndex() const
{
    for (int i = 0; i < steps.size(); ++i)
        if (steps[i].status == Step::Pending) return i;
    return -1;
}

bool Plan::allDone() const
{
    for (const Step& s : steps)
        if (s.status == Step::Pending || s.status == Step::Running) return false;
    return true;
}

QJsonObject Plan::toJson() const
{
    QJsonArray arr;
    for (const Step& s : steps) arr.append(s.toJson());
    return {
        {"title", title},
        {"goal", goal},
        {"capabilities", QJsonArray::fromStringList(capabilities)},
        {"steps", arr},
    };
}

// ---------------------------------------------------------------------------

Observation observationFromToolResult(int stepIndex, const QString& tool,
                                      const QJsonObject& toolResult)
{
    Observation ob;
    ob.stepIndex = stepIndex;
    ob.tool = tool;

    QString text;
    const QJsonArray content = toolResult["content"].toArray();
    for (const QJsonValue& v : content) {
        const QJsonObject c = v.toObject();
        if (c["type"].toString() == QLatin1String("text") || c.contains("text")) {
            if (!text.isEmpty()) text += '\n';
            text += c["text"].toString();
        }
    }
    if (text.isEmpty() && !content.isEmpty())
        text = QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact));
    if (text.isEmpty() && toolResult.contains("error"))
        text = toolResult["error"].toString();
    ob.raw = text;

    const bool isError = toolResult["isError"].toBool()
        || text.trimmed().startsWith(QLatin1String("Error"), Qt::CaseInsensitive);
    ob.status = isError ? QStringLiteral("error") : QStringLiteral("success");
    if (isError) {
        ob.error = text.section('\n', 0, 0).trimmed();
        if (ob.error.isEmpty()) ob.error = QStringLiteral("tool reported an error");
    }

    // ---- facts: "<Label>: <number>" lines the info tools print ----
    static const QRegularExpression kv(
        R"((?im)^\s*[-*]?\s*(vertices|triangles|faces|submeshes|materials|bones|animations|entities|scene nodes|joints|bone count|influences)\s*:\s*([0-9][0-9,\.]*))");
    auto it = kv.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        QString key = m.captured(1).toLower().replace(' ', '_');
        const QString num = m.captured(2);
        QString cleaned = num; cleaned.remove(',');
        bool ok = false;
        const double d = cleaned.toDouble(&ok);
        if (ok && !ob.facts.contains(key)) ob.facts[key] = d;
    }
    // JSON payloads: lift a few well-known top-level keys
    if (text.trimmed().startsWith('{')) {
        const QJsonObject j = QJsonDocument::fromJson(text.trimmed().toUtf8()).object();
        for (const char* k : {"boneCount", "vertexCount", "triangleCount", "algorithm",
                              "template", "skinned", "applied", "fallbackReason",
                              "jointLabeling", "faceCount", "partCount"}) {
            if (j.contains(k)) ob.facts[QLatin1String(k)] = j[k];
        }
        if (j.contains("isError") && j["isError"].toBool()) ob.status = QStringLiteral("error");
    }
    static const QRegularExpression hasSkel(R"((?i)\b(has skeleton|skinned|rigged)\b\s*[:=]?\s*(true|yes|false|no))");
    if (const auto m = hasSkel.match(text); m.hasMatch()) {
        const QString v = m.captured(2).toLower();
        ob.facts["hasSkeleton"] = (v == QLatin1String("true") || v == QLatin1String("yes"));
    }

    // ---- artifacts: created scene objects and written files ----
    // A written/created file: a path token (no spaces) with a 3D/image extension,
    // on a line that says created/exported/saved/wrote/generated/loaded.
    static const QRegularExpression created(R"((?im)^[^\n]*\b(?:created|exported|saved|loaded|wrote|written|generated)\b[^\n]*?((?:[A-Za-z]:)?[^\s'"()]+\.(?:glb|gltf|fbx|obj|mesh|dae|stl|ply|png|jpg|jpeg|json|abc|mp4)))");
    it = created.globalMatch(text);
    while (it.hasNext()) {
        const QString a = it.next().captured(1).trimmed();
        if (!a.isEmpty() && !ob.artifacts.contains(a)) ob.artifacts << a;
    }
    static const QRegularExpression createdName(R"((?im)^\s*(?:created|duplicated|applied|loaded)\b[^:\n]*?[:'"]\s*([A-Za-z0-9_\-\.]+))");
    it = createdName.globalMatch(text);
    while (it.hasNext()) {
        const QString a = it.next().captured(1).trimmed();
        if (!a.isEmpty() && !ob.artifacts.contains(a) && !a.contains('.')) ob.artifacts << a;
    }

    // ---- warnings ----
    static const QRegularExpression warn(R"((?im)^\s*(?:warning|note|fallback)[^\n]*)");
    it = warn.globalMatch(text);
    while (it.hasNext()) ob.warnings << it.next().captured(0).trimmed().left(200);
    if (ob.facts.contains("fallbackReason") && !ob.facts["fallbackReason"].toString().isEmpty())
        ob.warnings << QStringLiteral("fallback: %1").arg(ob.facts["fallbackReason"].toString().left(160));

    return ob;
}

QString summarize(const Plan& plan, const QVector<Observation>& observations, State finalState)
{
    int ok = 0, failed = 0, skipped = 0;
    for (const Step& s : plan.steps) {
        if (s.status == Step::Succeeded) ++ok;
        else if (s.status == Step::Failed) ++failed;
        else if (s.status == Step::Skipped) ++skipped;
    }
    QString head;
    switch (finalState) {
    case State::Completed: head = QStringLiteral("Done — %1 of %2 steps succeeded.").arg(ok).arg(plan.steps.size()); break;
    case State::Cancelled: head = QStringLiteral("Cancelled after %1 of %2 steps.").arg(ok + failed).arg(plan.steps.size()); break;
    case State::Failed:    head = QStringLiteral("Stopped — %1 of %2 steps succeeded, %3 failed.").arg(ok).arg(plan.steps.size()).arg(failed); break;
    default:               head = QStringLiteral("%1 of %2 steps done.").arg(ok).arg(plan.steps.size()); break;
    }
    QStringList lines{head};
    for (int i = 0; i < plan.steps.size(); ++i) {
        const Step& s = plan.steps[i];
        QString line = QStringLiteral("%1. %2 — %3").arg(i + 1).arg(s.tool, Step::statusName(s.status));
        if (s.status == Step::Failed && !s.error.isEmpty()) line += QStringLiteral(" (%1)").arg(s.error.left(120));
        for (const Observation& ob : observations) {
            if (ob.stepIndex != i || ob.status != QLatin1String("success")) continue;
            QStringList bits;
            for (auto it = ob.facts.begin(); it != ob.facts.end(); ++it) {
                const QJsonValue v = it.value();
                if (v.isDouble()) bits << QStringLiteral("%1 %2").arg(QString::number(v.toDouble(), 'g', 10), it.key());
                else if (v.isBool()) bits << QStringLiteral("%1=%2").arg(it.key(), v.toBool() ? "yes" : "no");
                else if (v.isString() && !v.toString().isEmpty() && it.key() != QLatin1String("fallbackReason"))
                    bits << QStringLiteral("%1=%2").arg(it.key(), v.toString().left(40));
            }
            if (!ob.artifacts.isEmpty()) bits << QStringLiteral("→ %1").arg(ob.artifacts.join(", "));
            if (!bits.isEmpty()) line += QStringLiteral(": ") + bits.join(", ");
            break;
        }
        lines << line;
    }
    QStringList warnings;
    for (const Observation& ob : observations) warnings << ob.warnings;
    warnings.removeDuplicates();
    if (!warnings.isEmpty()) lines << QStringLiteral("Warnings: %1").arg(warnings.mid(0, 4).join("; "));
    if (skipped) lines << QStringLiteral("%1 step(s) skipped.").arg(skipped);
    return lines.join('\n');
}

} // namespace AIAgent
