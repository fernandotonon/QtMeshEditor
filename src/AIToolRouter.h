#ifndef AITOOLROUTER_H
#define AITOOLROUTER_H

// Lexical tool router for the AI agent (#1002 follow-up): BM25 over the MCP
// tool docs, fed by an INTENT LEXICON that translates what the user means
// into the words the tool docs actually use.
//
//   "create a f22 raptor scene"  → +generate +mesh +image +prompt +3d
//   "make it green"              → +material +diffuse +colour +apply
//   "rig and skin the wolf"      → +rig +skeleton +skin +weights
//
// BM25 alone knows only the tool vocabulary ("diffuse colour [R,G,B]");
// the lexicon bridges English user vocabulary ("green"). Other languages
// and odd paraphrases are handled one level up: when a route has little
// signal (`Route::confident` false), AIAgentManager asks the loaded LLM for
// a few English operation keywords and routes on those — any language, no
// extra model. Zero dependencies, microseconds per request; an embedding
// router stays a later option if the benchmark (AIToolRouter_test) shows
// misses this cannot cover.
//
// Output: ranked capabilities (for the planner's capability pick) and a
// per-request tool shortlist (so a 26-tool capability contributes only its
// relevant tools to the prompt — the actual context-window win).

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class AIToolRouter
{
public:
    struct ToolDoc {
        QString name;
        QString capability;
        QString text;   // description + parameter docs
    };
    struct ScoredTool { QString name; QString capability; double score; };
    struct Route {
        QStringList capabilities;      // best first; never empty for a non-empty request
        QStringList tools;             // all tools of the chosen capabilities that scored, best first
        QVector<ScoredTool> scores;    // every tool with a positive score, best first
        QStringList expandedTerms;     // what the lexicon added (for the trace)
        double bestScore = 0.0;        // top tool score
        bool confident = false;        // enough lexical signal to skip the LLM intent step
    };

    AIToolRouter() = default;
    explicit AIToolRouter(const QVector<ToolDoc>& docs);

    bool isEmpty() const { return m_docs.isEmpty(); }

    /// Rank capabilities and tools for a request. `maxCapabilities` caps the
    /// pick (the planner can still ask for more); "scene" is always kept
    /// when present since its inspection tools are needed everywhere.
    Route route(const QString& request, int maxCapabilities = 4) const;

    /// Tools of `capability` worth showing for this request: the ones that
    /// scored, plus every tool when the capability is small (<= keepAll) or
    /// nothing in it scored (no signal → show all rather than none).
    QStringList shortlist(const Route& route, const QString& capability, int keepAll = 8, int maxTools = 10) const;

    // ---- pure helpers (unit-tested) ----
    /// Lowercase word tokens with a light stemmer (plural/-ing/-ed/-tion),
    /// stopwords removed; "auto_rig" → {auto, rig}.
    static QStringList tokenize(const QString& text);
    /// Intent lexicon: extra English query terms implied by the request's
    /// words, each with a weight (generic verbs like "make" expand weakly,
    /// specific words like "green" strongly). Deterministic, table-driven.
    static QHash<QString, double> expandIntentWeighted(const QStringList& queryTokens);
    static QStringList expandIntent(const QStringList& queryTokens);

private:
    struct Indexed {
        ToolDoc doc;
        QHash<QString, int> tf;
        int length = 0;
    };
    QVector<Indexed> m_docs;
    QHash<QString, int> m_df;   // term → number of docs containing it
    double m_avgLen = 1.0;
    double bm25(const Indexed& d, const QHash<QString, double>& weightedQuery) const;
};

#endif // AITOOLROUTER_H
