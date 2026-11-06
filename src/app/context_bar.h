// Context bar near the cursor (app-ux §7.2): command set by selection (a pure function over the §7.2 table) and
// a widget — a row of icons at the click point; opacity by cursor distance; hidden when the cursor leaves,
// on Esc, on a click into empty space and when a command is executed.
#pragma once

#include <QFrame>
#include <QPoint>
#include <QString>

#include <string>
#include <vector>

class QGraphicsOpacityEffect;

namespace construe::app {

struct ContextSelection {
    // CylinderFace — one cylindrical body face in the view: its diameter tolerance (FX12-PMI-004).
    enum class Kind { None, Plane, Sketch, Feature, SketchEntities, SketchConstraint, SketchDimension, CylinderFace };
    Kind kind = Kind::None;
    std::vector<std::string> constraints;  // SketchEntities: applicable constraints (api::sketchops::applicableConstraints)
    bool diametral = false;                // SketchDimension: diameter/radius-from-axis can be toggled (S2-SK-003)
    bool weak = false;                     // SketchDimension: a weak dimension — no Delete / Reference (FX9-SK-002)
};

// Bar commands (§7.2), at most CONTEXT_BAR_MAX_ICONS: sketch, normalTo, extrude, revolve, editSketch, editFeature,
// suppress, delete, constraint_<type>, dimension, construction, editValue, toggleDriven, pmiFit.
std::vector<QString> contextCommands(const ContextSelection& s);
// Command name for the tooltip (with the shortcut, if any).
QString contextCommandLabel(const QString& command);

class ContextBar : public QFrame {
    Q_OBJECT
public:
    explicit ContextBar(QWidget* parent);
    // Show commands at point at (parent coordinates) offset by CONTEXT_BAR_OFFSET_PX.
    void showAt(const std::vector<QString>& commands, QPoint at);
    // Opacity by distance from cursor (parent coordinates) to the bar; beyond FAR — hide.
    void updateFade(QPoint cursor);
    std::vector<QString> commands() const { return commands_; }

signals:
    void triggered(const QString& command);

private:
    std::vector<QString> commands_;
    QGraphicsOpacityEffect* opacity_ = nullptr;
};

}  // namespace construe::app
