#include "AIToolRouter.h"

#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {

const QSet<QString>& stopwords()
{
    static const QSet<QString> s = {
        "a", "an", "the", "and", "or", "of", "to", "in", "on", "for", "with", "it", "its", "is", "are", "be",
        "this", "that", "as", "at", "by", "from", "into", "then", "than", "so", "me", "my", "please", "can",
        "you", "we", "i", "do", "does", "not", "no", "yes", "if", "when", "use", "using", "used", "one",
        "all", "any", "each", "every", "also", "just", "only", "new", "current", "given", "optional",
        "default", "value", "values", "name", "names", "e", "g", "etc",
    };
    return s;
}

QString stem(QString w)
{
    // Light English stemmer: enough to make "rigging" meet "rig" and
    // "materials" meet "material"; deliberately conservative.
    if (w.size() > 5 && w.endsWith(QLatin1String("ation"))) return w.left(w.size() - 5) + QLatin1String("ate");
    if (w.size() > 4 && w.endsWith(QLatin1String("ing")))   { w.chop(3); if (w.size() > 2 && w[w.size()-1] == w[w.size()-2]) w.chop(1); return w; }
    if (w.size() > 4 && w.endsWith(QLatin1String("ies")))   return w.left(w.size() - 3) + QLatin1String("y");
    if (w.size() > 3 && w.endsWith(QLatin1String("ed")))    { w.chop(2); return w; }
    if (w.size() > 3 && w.endsWith(QLatin1String("es")) && !w.endsWith(QLatin1String("ses"))) { w.chop(2); return w; }
    if (w.size() > 3 && w.endsWith(QLatin1String("s")) && !w.endsWith(QLatin1String("ss"))) { w.chop(1); }
    return w;
}

// Both queries and docs pass through this, so the stem only has to be
// CONSISTENT, not linguistically right: dropping a trailing 'e' makes
// dance/dancing, create/creating and image/images agree.
QString canonical(QString w)
{
    w = stem(w);
    if (w.size() > 4 && w.endsWith('e')) w.chop(1);
    return w;
}

// intent word → tool-vocabulary terms, with a weight relative to the user's
// own words (1.0). Generic verbs expand WEAKLY: "make it green" must route
// to materials, not to "make → generate".
struct Intent { const char* trigger; const char* terms; double weight; };
const Intent kIntents[] = {
    // creating something that is not a primitive → generation (weak: many
    // requests say "make"/"create" and mean something else)
    {"create",    "generate mesh image prompt", 0.35},
    {"make",      "generate mesh image prompt", 0.25},
    {"build",     "generate mesh image prompt", 0.35},
    {"design",    "generate mesh image prompt", 0.5},
    {"generate",  "generate mesh image prompt", 0.6},
    {"scene",     "scene info", 0.4},
    {"character", "generate mesh image prompt", 0.5},
    {"creature",  "generate mesh image prompt", 0.6},
    {"monster",   "generate mesh image prompt", 0.6},
    {"vehicle",   "generate mesh image prompt", 0.5},
    {"car",       "generate mesh image prompt", 0.5},
    {"jet",       "generate mesh image prompt", 0.6},
    {"plane",     "generate mesh image prompt", 0.4},
    {"dragon",    "generate mesh image prompt", 0.6},
    // appearance → materials
    {"colour",    "material diffuse apply create colour", 0.80},
    {"color",     "material diffuse apply create colour", 0.80},
    {"red",       "material diffuse apply create colour", 0.80},
    {"green",     "material diffuse apply create colour", 0.80},
    {"blue",      "material diffuse apply create colour", 0.80},
    {"yellow",    "material diffuse apply create colour", 0.80},
    {"white",     "material diffuse apply create colour", 0.80},
    {"black",     "material diffuse apply create colour", 0.80},
    {"gold",      "material diffuse specular shininess apply", 0.80},
    {"shiny",     "material specular shininess apply", 0.80},
    {"glossy",    "material specular shininess apply", 0.80},
    {"metal",     "material specular shininess apply metallic", 0.80},
    {"metallic",  "material specular shininess apply metallic", 0.80},
    {"matte",     "material specular shininess apply", 0.80},
    {"paint",     "material diffuse apply paint", 0.50},
    {"recolour",  "material diffuse apply", 0.80},
    {"recolor",   "material diffuse apply", 0.80},
    {"texture",   "texture set material diffuse map", 0.50},
    // rigging
    {"rig",       "rig skeleton skin weight auto template", 0.80},
    {"skeleton",  "bone skin rig", 0.40},   // weak: "remove the skeleton" must not become auto_rig
    {"bone",      "rig skeleton skin weight", 0.80},
    {"skin",      "skin weight rig skeleton", 0.80},
    {"weight",    "skin weight", 0.80},
    // transforms
    {"bigger",    "transform scale node", 0.80},
    {"larger",    "transform scale node", 0.80},
    {"smaller",   "transform scale node", 0.80},
    {"twice",     "transform scale node", 0.80},
    {"half",      "transform scale node", 0.80},
    {"scale",     "transform scale node", 0.50},
    {"resize",    "transform scale node", 0.80},
    {"move",      "transform position node", 0.50},
    {"left",      "transform position node", 0.35},
    {"right",     "transform position node", 0.35},
    {"up",        "transform position node", 0.35},
    {"down",      "transform position node", 0.35},
    {"forward",   "transform position node", 0.35},
    {"rotate",    "transform rotation node", 0.50},
    {"turn",      "transform rotation node", 0.50},
    {"spin",      "node animation clip rotation", 0.80},
    // files
    {"export",    "export mesh output path file", 0.80},
    {"save",      "export save scene output path file", 0.50},
    {"glb",       "export mesh output path", 0.80},
    {"fbx",       "export mesh output path", 0.80},
    {"obj",       "load mesh file path", 0.80},
    {"load",      "load mesh file path", 0.50},
    {"open",      "load open mesh scene file path", 0.50},
    {"import",    "load mesh file path", 0.80},
    {"file",      "load export mesh file path", 0.50},
    {"folder",    "list search files directory", 0.50},
    // view
    {"screenshot","screenshot camera viewport", 0.80},
    {"show",      "screenshot camera viewport", 0.50},
    {"look",      "camera look viewport", 0.50},
    {"render",    "screenshot camera viewport", 0.80},
    {"zoom",      "camera viewport frame", 0.80},
    {"camera",    "camera viewport frame", 0.80},
    // animation / motion
    {"animate",   "animation generate motion clip play", 0.80},
    {"animation", "animation clip play keyframe", 0.80},
    {"walk",      "generate motion animation prompt", 0.80},
    {"run",       "generate motion animation prompt", 0.80},
    {"dance",     "generate motion animation prompt", 0.80},
    {"idle",      "generate motion animation prompt", 0.80},
    {"jump",      "generate motion animation prompt", 0.80},
    {"wave",      "generate motion animation prompt", 0.80},
    {"play",      "play animation", 0.50},
    // mesh ops
    {"lod",       "lod level detail generate", 0.80},
    {"decimate",  "decimate reduce triangle", 0.80},
    {"simplify",  "decimate reduce triangle simplify", 0.80},
    {"reduce",    "decimate reduce triangle lod", 0.50},
    {"polygon",   "decimate reduce triangle lod", 0.50},
    {"triangle",  "mesh info count", 0.50},
    {"retopo",    "retopology quad", 0.80},
    {"weld",      "weld vertex", 0.80},
    {"optimize",  "optimize vertex cache", 0.80},
    {"optimise",  "optimize vertex cache", 0.80},
    {"uv",        "uv unwrap", 0.80},
    {"unwrap",    "uv unwrap", 0.80},
    {"segment",   "segment part split", 0.80},
    {"part",      "segment part split explode", 0.50},
    {"split",     "segment part split", 0.50},
    {"explode",   "segment part explode", 0.80},
    {"delete",    "delete entity", 0.50},
    {"remove",    "delete remove entity", 0.50},
    {"duplicate", "duplicate entity", 0.80},
    {"copy",      "duplicate entity", 0.80},
    {"validate",  "validate mesh", 0.80},
    {"check",     "validate mesh info", 0.50},
    {"inspect",   "scene info mesh info", 0.50},
    {"info",      "scene info mesh info", 0.50},
    {"what",      "scene info mesh info", 0.35},
    {"which",     "scene info mesh info", 0.35},
    {"how",       "scene info mesh info", 0.35},
    {"many",      "mesh info count", 0.50},
    {"count",     "mesh info count", 0.50},
    {"light",     "light create lighting", 0.50},
    {"shadow",    "light lighting", 0.50},
    {"hdr",       "hdr environment", 0.80},
    {"pose",      "pose library apply", 0.80},
    {"morph",     "morph target weight", 0.80},
    {"face",      "face blendshape arkit", 0.80},
};

} // namespace

QStringList AIToolRouter::tokenize(const QString& text)
{
    static const QRegularExpression sep(R"([^\p{L}\p{N}]+)");
    QStringList out;
    for (const QString& raw : text.toLower().split(sep, Qt::SkipEmptyParts)) {
        if (raw.size() < 2 && !raw[0].isDigit()) continue;
        if (stopwords().contains(raw)) continue;
        out << canonical(raw);
    }
    return out;
}

QHash<QString, double> AIToolRouter::expandIntentWeighted(const QStringList& queryTokens)
{
    struct Entry { QStringList terms; double weight; };
    static QHash<QString, Entry> table;
    if (table.isEmpty()) {
        for (const Intent& i : kIntents) {
            Entry e; e.weight = i.weight;
            for (const QString& t : QString::fromLatin1(i.terms).split(' ', Qt::SkipEmptyParts)) e.terms << canonical(t);
            table.insert(canonical(QString::fromLatin1(i.trigger)), e);
        }
    }
    QHash<QString, double> extra;
    for (const QString& q : queryTokens) {
        const auto it = table.constFind(q);
        if (it == table.constEnd()) continue;
        for (const QString& t : it->terms) {
            if (queryTokens.contains(t)) continue;
            extra[t] = std::max(extra.value(t, 0.0), it->weight);
        }
    }
    return extra;
}

QStringList AIToolRouter::expandIntent(const QStringList& queryTokens)
{
    QStringList out = expandIntentWeighted(queryTokens).keys();
    std::sort(out.begin(), out.end());
    return out;
}

AIToolRouter::AIToolRouter(const QVector<ToolDoc>& docs)
{
    long long total = 0;
    for (const ToolDoc& d : docs) {
        Indexed ix;
        ix.doc = d;
        // the name's words count twice: "auto_rig" should win "rig" queries
        const QStringList toks = tokenize(d.name.split('_').join(' ') + ' ' + d.name.split('_').join(' ')
                                          + ' ' + d.capability.split('_').join(' ') + ' ' + d.text);
        for (const QString& t : toks) ix.tf[t]++;
        ix.length = static_cast<int>(toks.size());
        total += ix.length;
        for (auto it = ix.tf.begin(); it != ix.tf.end(); ++it) m_df[it.key()]++;
        m_docs.push_back(ix);
    }
    m_avgLen = m_docs.isEmpty() ? 1.0 : double(total) / double(m_docs.size());
}

double AIToolRouter::bm25(const Indexed& d, const QHash<QString, double>& weightedQuery) const
{
    constexpr double k1 = 1.2;
    constexpr double b  = 0.75;
    const double N = double(m_docs.size());
    double score = 0.0;
    for (auto it = weightedQuery.begin(); it != weightedQuery.end(); ++it) {
        const int tf = d.tf.value(it.key(), 0);
        if (tf == 0) continue;
        const int df = m_df.value(it.key(), 0);
        const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));
        const double norm = tf * (k1 + 1.0) / (tf + k1 * (1.0 - b + b * d.length / m_avgLen));
        score += it.value() * idf * norm;
    }
    return score;
}

AIToolRouter::Route AIToolRouter::route(const QString& request, int maxCapabilities) const
{
    Route r;
    const QStringList raw = tokenize(request);
    const QHash<QString, double> expanded = expandIntentWeighted(raw);
    r.expandedTerms = expanded.keys();
    std::sort(r.expandedTerms.begin(), r.expandedTerms.end());
    // The user's own words weigh 1.0; lexicon expansions carry their intent
    // weight, so they hit where the raw words cannot without drowning them.
    QHash<QString, double> query = expanded;
    for (const QString& w : raw) query[w] = 1.0;
    // "create/make/build <something no tool doc mentions>" — an f22, a raptor,
    // a goblin — is a request to GENERATE that thing: the unknown noun is
    // the strongest generation signal there is, so it lifts the (otherwise
    // weak) creation-verb expansion to full weight.
    {
        static const QStringList creationVerbs = {canonical("create"), canonical("make"), canonical("build"), canonical("design"), canonical("model")};
        static const QRegularExpression numberLike(QStringLiteral("^\\d+x?$"));
        bool creation = false, unknownNoun = false;
        for (const QString& w : raw) {
            if (creationVerbs.contains(w)) creation = true;
            else if (!m_df.contains(w) && !expanded.contains(w) && !numberLike.match(w).hasMatch()
                     && expandIntentWeighted({w}).isEmpty()) unknownNoun = true;
        }
        if (creation && unknownNoun)
            for (const QString& t : {canonical("generate"), canonical("mesh"), canonical("image"), canonical("prompt")})
                if (!raw.contains(t)) query[t] = std::max(query.value(t, 0.0), 0.9);
    }

    QHash<QString, double> capScore;
    for (const Indexed& d : m_docs) {
        const double s = bm25(d, query);
        if (s <= 0.0) continue;
        r.scores.push_back({d.doc.name, d.doc.capability, s});
        // capability = best tool + a little for breadth
        capScore[d.doc.capability] = std::max(capScore.value(d.doc.capability), s) + 0.15 * s;
    }
    std::stable_sort(r.scores.begin(), r.scores.end(), [](const ScoredTool& a, const ScoredTool& b) { return a.score > b.score; });

    QStringList caps = capScore.keys();
    std::stable_sort(caps.begin(), caps.end(), [&](const QString& a, const QString& b) { return capScore[a] > capScore[b]; });
    // keep only capabilities that are not far below the best (noise cut)
    if (!caps.isEmpty()) {
        const double best = capScore[caps.first()];
        QStringList kept;
        for (const QString& c : caps) if (capScore[c] >= 0.25 * best || kept.size() < 2) kept << c;
        caps = kept;
    }
    if (caps.size() > maxCapabilities) caps = caps.mid(0, maxCapabilities);
    bool haveScene = false;
    for (const Indexed& d : m_docs) if (d.doc.capability == QLatin1String("scene")) { haveScene = true; break; }
    if (haveScene && !caps.contains(QStringLiteral("scene"))) {
        if (caps.size() >= maxCapabilities) caps.removeLast();
        caps << QStringLiteral("scene");
    }
    r.capabilities = caps;
    for (const ScoredTool& t : r.scores) if (caps.contains(t.capability)) r.tools << t.name;
    r.bestScore = r.scores.isEmpty() ? 0.0 : r.scores.first().score;
    // Confident = a real request word (not only lexicon expansions) hit at
    // least one tool with a decent score. A request in another language has
    // no word in the English tool docs and lands here as not confident.
    QStringList rawHits;
    for (const Indexed& d : m_docs) for (const QString& w : raw) if (d.tf.contains(w) && !rawHits.contains(w)) rawHits << w;
    r.confident = !rawHits.isEmpty() && r.bestScore >= 1.0;
    return r;
}

QStringList AIToolRouter::shortlist(const Route& route, const QString& capability, int keepAll, int maxTools) const
{
    QStringList all;
    for (const Indexed& d : m_docs) if (d.doc.capability == capability) all << d.doc.name;
    if (all.size() <= keepAll) return all;
    QStringList scored;
    for (const ScoredTool& t : route.scores) {
        if (t.capability != capability) continue;
        scored << t.name;
        if (scored.size() >= maxTools) break;
    }
    // No signal inside this capability → show it whole rather than hide it.
    return scored.isEmpty() ? all : scored;
}
