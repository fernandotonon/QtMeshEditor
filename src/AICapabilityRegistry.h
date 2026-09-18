#ifndef AICAPABILITYREGISTRY_H
#define AICAPABILITYREGISTRY_H

// Capability registry + dynamic tool routing for the AI agent (#1002 / #1003).
//
// The MCP server exposes ~170 tools. A 3–8B orchestrator cannot hold that
// catalog in context, and the v1 chat hard-coded a 19-tool subset that
// silently excluded rigging, segmentation, generation, ... This registry
// groups every tool into a CAPABILITY (scene, materials, rigging, ...), each
// with a one-line description, so the planner first picks capabilities from
// a ~20-line index and only then sees the full per-tool docs of the picked
// ones. Tool docs are generated from the live MCP schema, so they cannot
// drift from what `callTool` accepts.
//
// It also owns the constrained tool-call protocol: `validateArguments`
// checks (and gently coerces) a planned call against the tool's JSON schema
// BEFORE it reaches the server, and `destructiveReason` names the calls
// that need a user confirmation unless trusted mode is on.
//
// Pure data — built from a QJsonArray, no Ogre, no MCPServer dependency —
// so it is fully unit-tested headless.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class AICapabilityRegistry
{
public:
    struct Capability {
        QString     id;           // "rigging"
        QString     title;        // "Rigging & skinning"
        QString     description;  // one line for the planner's index
        QStringList tools;        // tool names, in schema order
    };

    AICapabilityRegistry() = default;
    /// Build from `MCPServer::buildToolsList()` output (name/description/inputSchema).
    explicit AICapabilityRegistry(const QJsonArray& toolList);

    bool isEmpty() const { return m_tools.isEmpty(); }
    int  toolCount() const { return static_cast<int>(m_tools.size()); }

    QStringList capabilityIds() const;
    const Capability* capability(const QString& id) const;
    /// Capability id a tool belongs to ("other" when unmapped, empty when unknown).
    QString capabilityOf(const QString& tool) const;
    bool hasTool(const QString& tool) const { return m_tools.contains(tool); }
    QJsonObject schemaFor(const QString& tool) const;
    QStringList toolsFor(const QStringList& capabilityIds) const;

    // ---- prompt fragments ----
    /// The compact index: one line per capability (id, title, description, tool count).
    QString promptIndex() const;
    /// Full docs (description + params) for every tool of the given capabilities.
    QString promptToolsFor(const QStringList& capabilityIds) const;
    /// One tool's doc block: "- name: description\n    param: doc (type, required)".
    QString toolDoc(const QString& tool) const;

    // ---- routing ----
    /// Keyword heuristic: which capabilities a request most likely needs.
    /// Used to pre-narrow the planner's docs and as the fallback when the
    /// planner's own capability pick is unusable. Always non-empty for a
    /// non-empty request (falls back to "scene").
    QStringList routeByKeywords(const QString& request) const;

    // ---- constrained protocol (#1003) ----
    /// Validate `args` against the tool's input schema. Missing required →
    /// false. Type mismatches are coerced where unambiguous ("2" → 2,
    /// "true" → true, 3 → "3" for string params) and reported in `warnings`;
    /// impossible ones → false. Unknown properties are kept but warned.
    /// `out` receives the coerced arguments.
    bool validateArguments(const QString& tool, const QJsonObject& args,
                           QJsonObject* out, QString* error,
                           QStringList* warnings = nullptr) const;

    /// Non-empty when the call deletes data or overwrites a file, e.g.
    /// "deletes entity 'Cube'" / "overwrites /path/out.glb". Export/save
    /// tools are destructive only when the target exists (`fileExists`,
    /// injectable for tests).
    static QString destructiveReason(const QString& tool, const QJsonObject& args,
                                     const std::function<bool(const QString&)>& fileExists);
    static QString destructiveReason(const QString& tool, const QJsonObject& args);

    /// Read-only tools never open the undo group (get_*, list_*, screenshots ...).
    static bool isReadOnly(const QString& tool);

    /// Built-in tool → capability taxonomy (exposed for the tests).
    static const QHash<QString, QString>& taxonomy();

private:
    struct ToolInfo {
        QString name;
        QString description;
        QJsonObject schema;
    };
    QHash<QString, ToolInfo> m_tools;
    QVector<Capability>      m_capabilities;   // stable display order
    QHash<QString, int>      m_capIndex;
};

#endif // AICAPABILITYREGISTRY_H
