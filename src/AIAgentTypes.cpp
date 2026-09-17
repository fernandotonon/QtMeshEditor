#include "AIAgentTypes.h"

#include <QJsonDocument>
#include <array>
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
// observationFromToolResult — split into one helper per concern so each
// stays readable (and under Sonar's nesting/complexity gates).

namespace {

QString resultText(const QJsonObject& toolResult)
{
    QString text;
    const QJsonArray content = toolResult["content"].toArray();
    for (const QJsonValue& v : content) {
        const QJsonObject c = v.toObject();
        if (c["type"].toString() != QLatin1String("text") && !c.contains("text")) continue;
        if (!text.isEmpty()) text += '\n';
        text += c["text"].toString();
    }
    if (text.isEmpty() && !content.isEmpty())
        text = QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact));
    if (text.isEmpty() && toolResult.contains("error"))
        text = toolResult["error"].toString();
    return text;
}

void parseErrorStatus(const QJsonObject& toolResult, const QString& text, Observation& ob)
{
    const bool isError = toolResult["isError"].toBool()
        || text.trimmed().startsWith(QLatin1String("Error"), Qt::CaseInsensitive);
    ob.status = isError ? QStringLiteral("error") : QStringLiteral("success");
    if (!isError) return;
    ob.error = text.section('\n', 0, 0).trimmed();
    if (ob.error.isEmpty()) ob.error = QStringLiteral("tool reported an error");
}

// "<Label>: <number>" lines the info tools print.
void parseLabelledNumbers(const QString& text, Observation& ob)
{
    static const QRegularExpression kv(
        R"((?im)^\s*[-*]?\s*(vertices|triangles|faces|submeshes|materials|bones|animations|entities|scene nodes|joints|bone count|influences)\s*:\s*([0-9][0-9,\.]*))");
    auto it = kv.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString key = m.captured(1).toLower().replace(' ', '_');
        QString cleaned = m.captured(2);
        cleaned.remove(',');
        bool ok = false;
        const double d = cleaned.toDouble(&ok);
        if (ok && !ob.facts.contains(key)) ob.facts[key] = d;
    }
    static const QRegularExpression hasSkel(R"((?i)\b(has skeleton|skinned|rigged)\b\s*[:=]?\s*(true|yes|false|no))");
    const auto m = hasSkel.match(text);
    if (!m.hasMatch()) return;
    const QString v = m.captured(2).toLower();
    ob.facts["hasSkeleton"] = (v == QLatin1String("true") || v == QLatin1String("yes"));
}

// JSON payloads: lift a few well-known top-level keys and the error reason.
void parseJsonFacts(const QString& text, Observation& ob)
{
    const QString trimmed = text.trimmed();
    if (!trimmed.startsWith('{')) return;
    const QJsonObject j = QJsonDocument::fromJson(trimmed.toUtf8()).object();
    static const std::array<const char*, 11> kKeys = {"boneCount", "vertexCount", "triangleCount", "algorithm",
                                                      "template", "skinned", "applied", "fallbackReason",
                                                      "jointLabeling", "faceCount", "partCount"};
    for (const char* k : kKeys)
        if (j.contains(QLatin1String(k))) ob.facts[QLatin1String(k)] = j[QLatin1String(k)];
    if (!j["isError"].toBool()) return;
    ob.status = QStringLiteral("error");
    if (!ob.error.isEmpty()) return;
    ob.error = j.contains("error") ? j["error"].toString() : j["message"].toString();
    if (ob.error.isEmpty()) ob.error = QStringLiteral("tool reported an error");
}

// Written files (a path token with a 3D/image extension on a created/exported/
// saved/... line) and created scene objects.
void parseArtifacts(const QString& text, Observation& ob)
{
    static const QRegularExpression created(R"((?im)^[^\n]*\b(?:created|exported|saved|loaded|wrote|written|generated)\b[^\n]*?((?:[A-Za-z]:)?[^\s'"()]+\.(?:glb|gltf|fbx|obj|mesh|dae|stl|ply|png|jpg|jpeg|json|abc|mp4)))");
    auto it = created.globalMatch(text);
    while (it.hasNext()) {
        const QString a = it.next().captured(1).trimmed();
        if (!a.isEmpty() && !ob.artifacts.contains(a)) ob.artifacts << a;
    }
    static const QRegularExpression createdName(R"((?im)^\s*(?:created|duplicated|applied|loaded)\b[^:\n]*?[:'"]\s*([A-Za-z0-9_\-\.]+))");
    it = createdName.globalMatch(text);
    while (it.hasNext()) {
        const QString a = it.next().captured(1).trimmed();
        if (!a.isEmpty() && !a.contains('.') && !ob.artifacts.contains(a)) ob.artifacts << a;
    }
}

void parseWarnings(const QString& text, Observation& ob)
{
    static const QRegularExpression warn(R"((?im)^\s*(?:warning|note|fallback)[^\n]*)");
    auto it = warn.globalMatch(text);
    while (it.hasNext()) ob.warnings << it.next().captured(0).trimmed().left(200);
    const QString fallback = ob.facts["fallbackReason"].toString();
    if (!fallback.isEmpty()) ob.warnings << QStringLiteral("fallback: %1").arg(fallback.left(160));
}

// One "facts" fragment for the summary line of a succeeded step.
QString factsFragment(const Observation& ob)
{
    QStringList bits;
    for (auto it = ob.facts.begin(); it != ob.facts.end(); ++it) {
        const QJsonValue v = it.value();
        if (v.isDouble()) bits << QStringLiteral("%1 %2").arg(QString::number(v.toDouble(), 'g', 10), it.key());
        else if (v.isBool()) bits << QStringLiteral("%1=%2").arg(it.key(), v.toBool() ? "yes" : "no");
        else if (v.isString() && !v.toString().isEmpty() && it.key() != QLatin1String("fallbackReason"))
            bits << QStringLiteral("%1=%2").arg(it.key(), v.toString().left(40));
    }
    if (!ob.artifacts.isEmpty()) bits << QStringLiteral("→ %1").arg(ob.artifacts.join(", "));
    return bits.join(", ");
}

const Observation* successObservationFor(const QVector<Observation>& observations, int stepIndex)
{
    for (const Observation& ob : observations)
        if (ob.stepIndex == stepIndex && ob.status == QLatin1String("success")) return &ob;
    return nullptr;
}

QString summaryHeadline(const Plan& plan, State finalState, int ok, int failed)
{
    const auto total = static_cast<int>(plan.steps.size());
    switch (finalState) {
    case State::Completed: return QStringLiteral("Done — %1 of %2 steps succeeded.").arg(ok).arg(total);
    case State::Cancelled: return QStringLiteral("Cancelled after %1 of %2 steps.").arg(ok + failed).arg(total);
    case State::Failed:    return QStringLiteral("Stopped — %1 of %2 steps succeeded, %3 failed.").arg(ok).arg(total).arg(failed);
    default:               return QStringLiteral("%1 of %2 steps done.").arg(ok).arg(total);
    }
}

} // namespace

Observation observationFromToolResult(int stepIndex, const QString& tool,
                                      const QJsonObject& toolResult)
{
    Observation ob;
    ob.stepIndex = stepIndex;
    ob.tool = tool;
    const QString text = resultText(toolResult);
    ob.raw = text;
    parseErrorStatus(toolResult, text, ob);
    parseLabelledNumbers(text, ob);
    parseJsonFacts(text, ob);
    parseArtifacts(text, ob);
    parseWarnings(text, ob);
    return ob;
}

QString summarize(const Plan& plan, const QVector<Observation>& observations, State finalState)
{
    int ok = 0;
    int failed = 0;
    int skipped = 0;
    for (const Step& s : plan.steps) {
        if (s.status == Step::Succeeded) ++ok;
        else if (s.status == Step::Failed) ++failed;
        else if (s.status == Step::Skipped) ++skipped;
    }
    QStringList lines{summaryHeadline(plan, finalState, ok, failed)};
    for (int i = 0; i < plan.steps.size(); ++i) {
        const Step& s = plan.steps[i];
        QString line = QStringLiteral("%1. %2 — %3").arg(i + 1).arg(s.tool, Step::statusName(s.status));
        const bool showError = (s.status == Step::Failed || s.status == Step::Repaired) && !s.error.isEmpty();
        if (showError) line += QStringLiteral(" (%1)").arg(s.error.left(120));
        if (const Observation* ob = successObservationFor(observations, i)) {
            const QString facts = factsFragment(*ob);
            if (!facts.isEmpty()) line += QStringLiteral(": ") + facts;
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
