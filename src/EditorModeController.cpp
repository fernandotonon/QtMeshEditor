#include "EditorModeController.h"

#include "EditModeController.h"
#include "SentryReporter.h"

#include <QJSEngine>
#include <QVariantMap>

EditorModeController* EditorModeController::m_pSingleton = nullptr;

EditorModeController::EditorModeController(QObject* parent)
    : QObject(parent)
{
    auto* edit = editController();
    connect(edit, &EditModeController::editModeChanged,
            this, &EditorModeController::syncFromEditMode);
    connect(edit, &EditModeController::selectionModeChanged,
            this, [this]() {
        if (m_currentMode == EditMode)
            setStatusText(editController()->modeLabel());
    });
    connect(edit, &EditModeController::selectionStateChanged,
            this, &EditorModeController::refreshEditAvailability);

    m_currentMode = edit->isEditModeActive() ? EditMode : ObjectMode;
    if (m_currentMode == EditMode)
        setStatusText(edit->modeLabel());
    else
        setStatusText(QStringLiteral("Object mode"));
}

EditorModeController* EditorModeController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new EditorModeController();
    return m_pSingleton;
}

EditorModeController* EditorModeController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void EditorModeController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

void EditorModeController::setCurrentMode(int mode)
{
    requestMode(mode);
}

void EditorModeController::requestMode(int mode)
{
    if (mode < ObjectMode || mode > ValidationMode)
        return;

    setModeInternal(static_cast<Mode>(mode), true);
}

void EditorModeController::toggleObjectEditMode()
{
    setModeInternal(editController()->isEditModeActive() ? ObjectMode : EditMode, true);
}

QString EditorModeController::modeName() const
{
    return modeNameFor(static_cast<int>(m_currentMode));
}

QString EditorModeController::modeNameFor(int mode) const
{
    switch (static_cast<Mode>(mode)) {
    case ObjectMode: return QStringLiteral("Object");
    case EditMode: return QStringLiteral("Edit");
    case AnimationMode: return QStringLiteral("Animation");
    case MaterialMode: return QStringLiteral("Material");
    case ValidationMode: return QStringLiteral("Validation");
    }
    return QStringLiteral("Object");
}

QString EditorModeController::modeTooltipFor(int mode) const
{
    switch (static_cast<Mode>(mode)) {
    case ObjectMode: return QStringLiteral("Object placement and scene organization");
    case EditMode: return QStringLiteral("Edit mesh components");
    case AnimationMode: return QStringLiteral("Animation playback and clip tools");
    case MaterialMode: return QStringLiteral("Material assignment and editing");
    case ValidationMode: return QStringLiteral("Mesh validation and scan tools");
    }
    return QStringLiteral("Object placement and scene organization");
}

QVariantList EditorModeController::availableModes() const
{
    QVariantList modes;
    const QList<Mode> orderedModes = {
        ObjectMode,
        EditMode,
        AnimationMode,
        MaterialMode,
        ValidationMode
    };

    for (Mode mode : orderedModes) {
        QVariantMap entry;
        entry.insert(QStringLiteral("mode"), static_cast<int>(mode));
        entry.insert(QStringLiteral("label"), modeNameFor(static_cast<int>(mode)));
        entry.insert(QStringLiteral("tip"), modeTooltipFor(static_cast<int>(mode)));
        modes.push_back(entry);
    }

    return modes;
}

bool EditorModeController::editModeAvailable() const
{
    auto* edit = editController();
    return edit->isEditModeActive() || edit->canEnterEditMode();
}

void EditorModeController::syncFromEditMode()
{
    auto* edit = editController();
    if (edit->isEditModeActive()) {
        // setModeInternal already refreshes the status text via
        // edit->modeLabel() when entering EditMode, so don't double-set it.
        setModeInternal(EditMode, false);
    } else if (m_currentMode == EditMode) {
        // setModeInternal sets status text to "Object mode" here too.
        setModeInternal(ObjectMode, false);
    }
    emit editModeAvailabilityChanged();
}

void EditorModeController::refreshEditAvailability()
{
    emit editModeAvailabilityChanged();
}

void EditorModeController::setModeInternal(Mode mode, bool driveEditController)
{
    auto* edit = editController();

    if (driveEditController) {
        if (mode == EditMode) {
            if (!edit->isEditModeActive() && !edit->enterEditMode()) {
                setStatusText(QStringLiteral("Select one mesh to enter Edit mode"));
                emit editModeAvailabilityChanged();
                return;
            }
        } else if (edit->isEditModeActive()) {
            edit->exitEditMode(true);
        }
    }

    if (m_currentMode == mode) {
        if (mode == EditMode)
            setStatusText(edit->modeLabel());
        return;
    }

    m_currentMode = mode;
    SentryReporter::addBreadcrumb(
        "ui.mode",
        QStringLiteral("Editor mode: %1").arg(modeName()));
    emit modeChanged();

    if (mode == EditMode)
        setStatusText(edit->modeLabel());
    else
        setStatusText(QStringLiteral("%1 mode").arg(modeName()));
}

void EditorModeController::setStatusText(const QString& text)
{
    if (m_statusText == text)
        return;

    m_statusText = text;
    emit statusTextChanged();
}

EditModeController* EditorModeController::editController() const
{
    return EditModeController::instance();
}
