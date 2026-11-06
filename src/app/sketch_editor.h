// Sketcher: state machine of drawing, dimension and constraint tools, selection, dragging (spec/app-ux.md §6).
// Screen geometry — SketchGeometry; snaps and hit testing — sketch_picker; labels — SketchAnnotations;
// dimension input field — InlineDimensionEdit; the domain part of the tools — api::sketchops.
#pragma once

#include "api/session.h"
#include "api/sketch_ops.h"
#include "app/text_box.h"
#include "app/tolerance_editor.h"
#include "app/inline_dimension_edit.h"
#include "app/pattern_box.h"
#include "app/primitive_box.h"
#include "app/sketch_annotations.h"
#include "app/sketch_geometry.h"
#include "app/sketch_picker.h"
#include "app/snap.h"
#include "app/viewport.h"
#include "sketch/trim.h"

#include <QColor>
#include <QLineEdit>
#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::app {

class AppModel;

// Selection that preserves selection order (§3.6, §6.8: "first selected").
class OrderedSelection {
public:
    OrderedSelection() = default;
    OrderedSelection(std::initializer_list<std::string> ids) {
        for (const auto& i : ids) insert(i);
    }
    bool count(const std::string& id) const { return std::find(v_.begin(), v_.end(), id) != v_.end(); }
    bool insert(const std::string& id) {
        if (count(id)) return false;
        v_.push_back(id);
        return true;
    }
    bool erase(const std::string& id) {
        auto it = std::find(v_.begin(), v_.end(), id);
        if (it == v_.end()) return false;
        v_.erase(it);
        return true;
    }
    void clear() { v_.clear(); }
    bool empty() const { return v_.empty(); }
    size_t size() const { return v_.size(); }
    auto begin() const { return v_.begin(); }
    auto end() const { return v_.end(); }

private:
    std::vector<std::string> v_;
};

class SketchEditor : public QObject, public ViewportTool {
    Q_OBJECT
public:
    enum class Tool { Select, Point, Line, Rectangle, Circle, Dimension, Constraint, Arc3, ArcCenter, TangentArc, CenterRectangle, Trim, Project,
                      Polygon, Circle3, Mirror, Offset, SketchFillet, SketchChamfer, Pierce, Text, Construction,
                      PatternLinear, PatternCircular, Ellipse, Star, SplineControl, SplineThrough };

    SketchEditor(AppModel& model, Viewport& view, QObject* parent = nullptr);
    // The piece that a trim click under the cursor would remove (for tests and the hint).
    const std::optional<sketch::TrimPlan>& trimPreview() const { return trimPreview_; }
    // Rubber-band geometry in the sketch plane (app-ux §6.1): polylines in screen coordinates for cursor cur.
    std::vector<std::vector<QPointF>> rubberPolylines(const Point2& cur) const;
    // Constraint icons (§6.6): visibility (Shift+C) and the screen rectangle of a constraint's icon.
    bool constraintsShown() const { return showConstraints_; }
    void setConstraintsShown(bool on);
    std::optional<QRectF> glyphRect(const std::string& constraintId) const;

    void open(const std::string& featureId);
    void close();
    bool isOpen() const { return !feature_.empty(); }
    const std::string& feature() const { return feature_; }
    void refresh();  // after a revision change

    void setTool(Tool t, const std::string& constraintType = {});
    // Centerline — the «Line» tool with construction elements (§6.3).
    void setCenterlineTool() { setTool(Tool::Line); centerline_ = true; }
    // Q (§6.3): with curves selected — toggle "construction" on them, otherwise turn on/off the
    // «Construction geometry» mode (FX1-UI-005, FX7-SK-001).
    void toggleConstruction();
    // The single construction mode (§6.3, FX7-SK-001): while on, new curves and texts are construction ones, and without
    // a drawing tool (Tool::Construction) a click toggles an element; off when a sketch is opened.
    bool constructionMode() const { return drawConstruction_; }
    void setConstructionMode(bool on);
    Tool tool() const { return tool_; }
    // What the tool waits for, for the status line (S7F-UI-006); empty — «Select» waits for nothing.
    static QString toolHint(Tool t);
    // «Dimension» with the automatic selection of a just placed dimension (S7F-SK-002): its bar and selection are not
    // steps of the Esc ladder.
    bool dimensionAutoSelected() const { return tool_ == Tool::Dimension && dimAutoSelected_ && !selection_.empty(); }
    bool centerline() const { return centerline_; }  // the «Line» tool draws centerlines (§6.3)
    // Extent of the open sketch's geometry in its coordinates (§6.1, FX1-UI-010): points, circles, arcs, projections,
    // text outlines; no geometry — empty.
    std::optional<std::pair<Point2, Point2>> localBounds() const;
    // Polygon (app-ux §6.3, S1-SK-020): inscribed (second click — a vertex) or circumscribed (side midpoint).
    void setPolygonTool(bool inscribed);
    bool polygonInscribed() const { return polygonInscribed_; }
    int polygonSides() const { return polygonSides_; }
    // Star (§6.3, S4-SKT-003): the number of rays is the starRays field of the status bar.
    void setStarRays(int n);
    int starRays() const { return starRays_; }
    // Primitive dialog (§6.3 «Primitive dialog», S4-SKT-003): double click on a member of a polygon / star group.
    PrimitiveBox* primitiveBox() const { return primitiveBox_; }
    const std::string& primitiveEditing() const { return primitiveEditing_; }
    void setPolygonSides(int n);
    bool escape();   // Esc ladder (§6.13); false — nothing to do
    void deleteSelection();
    // Tangent arc: in a chain of segments — the next segment as an arc, otherwise the tool (§6.3; key T and icon).
    void startTangentArc();
    void applyConstraint(const std::string& type);  // apply to the selection (§6.8, method 2)
    void selectAll();
    // Context panel commands (§7.2): dimension the selection ("selection → command", §6.9), change a dimension value,
    // toggle reference/driving.
    void startDimensionFromSelection();
    void editDimension(const std::string& constraintId) { openInlineEdit(constraintId); }
    // Weak dimensions (§6.6а, S4-WEAK-001…003): IDs "w<N>" from the open sketch state; clicking a weak dimension opens
    // the value input field (sketchStrengthen), clicking a weak relation makes it regular right away.
    bool isWeak(const std::string& id) const;
    void strengthenWeak(const std::string& weakId);
    void toggleDriven(const std::string& constraintId);
    void toggleDiametral(const std::string& cid);  // diameter / radius from an axis (S2-SK-003); a weak one — made regular first
    // The dimension (regular or weak) can be switched to a diameter one (S2-SK-003, FX8-SK-001, FX9-SK-001): axes of
    // the revolve features of this sketch are counted too.
    bool diametralApplicable(const std::string& cid) const;
    // A dimension by ID: a regular one (document) or a weak one (SketchState.weak), else nullptr (FX9-SK-002).
    const sketch::Constraint* dimensionConstraint(const std::string& cid) const;
    // Mirror and offset of the selection (§6.3, S2-SK-009): then a click on the axis / on the offset side.
    void startMirror();
    void startOffset();
    // Offset group editing (§6.3, S7F-SK-005): its sources highlighted, the value field with its distance.
    void startOffsetEdit(const std::string& groupId, const QPointF& at);
    // Sketch pattern (§6.3 «Sketch pattern», S4-SKT-001, S4-SKT-002): selection → pattern dialog (circular — after
    // a click on the center); edit — double click on a pattern element; «Unlink» (S4-SKT-005) — selection.
    void startPattern(bool circular);
    void unlinkSelection();
    bool selectionHasGroupMember() const;  // the selection contains a member of a linked group (§7.5)
    PatternBox* patternBox() const { return patternBox_; }
    const std::string& patternEditing() const { return patternEditing_; }  // editing: group ID; otherwise empty
    // Pattern preview (screen polylines of generated curves); dialog closed — empty.
    std::vector<std::vector<QPointF>> patternPreview() const;
    // Direction snap the next click of the «Line» / «Centerline» tool would set (§6.4,
    // FX7-SK-011): "horizontal", "vertical", "parallel", "perpendicular", "tangent" or empty — shown at the cursor.
    QString directionHint() const;
    // Equality snap the next click would set (FX7-SK-003): ID of the circle / arc / line the drawn radius or length
    // becomes equal to; empty — none. Arc by 3 points (FX7-SK-002): the line / arc the arc would be tangent to.
    // raw — including hints dismissed by Esc (FX10-SK-005).
    QString equalHint(bool raw = false) const;
    QString arcTangentHint(bool raw = false) const;
    // Keys of the auto-constraint hints shown at the cursor (FX10-SK-005): "equal:<id>", "tangent:<id>",
    // "dir:<type>:<other>"; raw — including the dismissed ones. Esc dismisses the shown ones (§6.13 item 0).
    std::vector<std::string> shownHintKeys(bool raw = false) const;
    const std::set<std::string>& dismissedHints() const { return dismissedHints_.keys(); }
    // Dimension input while drawing (§6.3, S2-SK-010): field near the cursor; nullptr — closed.
    QLineEdit* drawInput() const { return drawInput_; }
    TextBox* textBox() const { return textBox_; }  // text dialog (§6.11в)
    ToleranceBox* toleranceBox() const { return toleranceBox_; }  // dimension tolerance dialog (§6.6)
    QString toolTipAt(const QPoint& at) const override;
    // Dimension label text with tolerance (§6.6) — for tests and hints.
    QString dimensionText(const std::string& constraintId) const;
    // Screen size of a point marker (app-ux §6.5, FX9-SK-007): a standalone point is larger than a curve end; the
    // emphasized (selected, hovered, picked) marker is larger still. By ID: 0 — not a point or a center (a cross).
    static double pointMarkerPx(bool standalone, bool emphasized);
    double pointMarkerPx(const std::string& pointId) const;
    // «Dimension» tool (§6.9, FX2-SK-002): objects picked by clicks and a preview of the dimension a click
    // at the place under the cursor would create (after a mouse pause of DIM_PREVIEW_DELAY_MS; disappears on movement).
    struct DimPreview {
        sketch::Constraint c;  // type, references, value, label position (label)
        QPointF label;         // screen
        // Picks with not-yet-projected body edges/vertices or axes (FX3-SK-001): sketch data and state with
        // temporary projections from which the dimension is built and drawn; empty — from the sketch itself.
        std::shared_ptr<const sketch::SketchData> data;
        std::shared_ptr<const api::SketchState> state;
    };
    const std::vector<std::string>& dimensionPicks() const { return picked_; }
    // The target highlighted under the cursor in «Dimension» / «Constraint» (FX9-SK-006): what a click would take;
    // empty — nothing.
    std::string toolHoverId() const;
    const std::optional<DimPreview>& dimensionPreview() const { return dimPreview_; }

protected:
    bool eventFilter(QObject* o, QEvent* ev) override;

public:
    // Sketch data with projection geometry (for commands that depend on the projection kind).
    sketch::SketchData dataWithProjections() const;
    // Ctrl+Z with an active tool: the unfinished element is discarded, the tool stays (§7.4).
    void dropPending() { resetToolState(); view_.update(); }
    bool inlineEditOpen() const { return inlineEdit_.isOpen(); }

    // State for the status bar (§6.10).
    QString dofText() const;
    QColor dofColor() const;

    // Test mode: screen coordinates.
    QPointF toScreen(const Point2& local) const;
    std::optional<Point2> toLocal(const QPointF& screen) const;
    std::optional<QPointF> screenPosOfEntity(const std::string& id) const;  // point — itself, curve — its middle
    std::optional<QRectF> labelRect(const std::string& constraintId) const;
    // Dimension and extension lines of a dimension on screen (test mode, FX4-SK-001); no geometry — empty.
    std::optional<DimPrims> dimensionPrims(const std::string& constraintId) const;
    // Dimension label size in sketch mm at the current view scale — for the «Fully define»
    // layout (core-api §4.6.2, FX4-SK-001): label rectangle height and character width.
    api::FullyDefineLayout dimensionLabelLayout() const;
    const OrderedSelection& selection() const { return selection_; }
    std::optional<api::SketchState> state() const { return state_; }
    QColor entityColor(const std::string& id) const;
    const char* entityColorToken(const std::string& id) const;
    QLineEdit* inlineEditor() const { return inlineEdit_.editor(); }
    QLineEdit* inlineToleranceEditor() const { return inlineEdit_.toleranceEditor(); }  // FX12-PMI-004

    // ViewportTool
    bool mousePress(QMouseEvent*) override;
    bool mouseMove(QMouseEvent*) override;
    bool mouseRelease(QMouseEvent*) override;
    bool mouseDoubleClick(QMouseEvent*) override;
    bool keyPress(QKeyEvent*) override;
    void paint(QPainter&) override;
    void leave() override;

signals:
    void statusChanged();
    void hint(const QString& text);
    void toolHintEnded(const QString& text);  // the tool hint `text` no longer applies (S7F-UI-006)
    void error(const QString& text);
    void notice(const QString& text, bool undoable);
    void madeDriven(const QString& text, const QString& constraintId);
    // Dimension displaced auto-constraints, stayed driving; restore — removed ones (for "Make reference").
    void yieldedAuto(const QString& text, const QString& constraintId, const QList<doc::Json>& restore);
    // Selection changed by a click or a box in the view (release point, view coordinates) — context panel (§7.2).
    void selectionPicked(const QPointF& at);
    // Esc found nothing to do on the ladder (§6.13 item 6): the window closes an unchanged sketch (FX10-SK-001).
    void escapeBottom();

private:
    const sketch::SketchData* data() const;
    // Data for display and hit testing (§6.6а): sketch data plus weak dimensions (driving = false, label from the
    // FX4-SK-001 layout) — a copy per state revision; no weak ones — data().
    const sketch::SketchData* displayData() const;
    mutable std::shared_ptr<sketch::SketchData> display_;
    mutable const sketch::SketchData* displayKey_ = nullptr;
    mutable uint64_t displayRev_ = 0;
    mutable bool buildingDisplay_ = false;
    QString toolHint_;  // the hint the current tool put into the status line (S7F-UI-006)
    // Sketch data with projection geometry (projection kinds by geometry, sketch.md §2.5) — for api::sketchops commands
    // Sketch axis near the cursor (app-ux §6.8, S1-SK-021): "sketch:x", "sketch:y" or empty.
    std::string axisAt(const QPointF& s) const;
    // Screen geometry and labels of the open sketch at the current moment (while dragging — from the
    // intermediate state).
    SketchGeometry geo() const;
    SketchAnnotations annotations() const;
    Point2 pointPos(const std::string& id) const { return geo().pointPos(id); }
    Snap snapAt(const QPointF& screen, bool suppress, const std::set<std::string>& exclude = {}) const;
    // ID of the projection of source for placing items: existing in the sketch or a new construction one in items.
    std::string projectionFor(doc::Json& items, const std::string& source) const;
    Hit hitAt(const QPointF& screen) const;
    // Displacement of auto-constraints by explicit ones (§6.4, S1-SK-016): added — IDs of just added constraints.
    // Returns the removed auto-constraints (type and references) — for undoing the "drive" choice (§6.9).
    std::vector<doc::Json> yieldAutoConstraints(const std::vector<std::string>& added);
    // Display names of the sketch's auto-constraints (for the «Auto relation removed» status message, FX8-SK-003).
    std::map<std::string, QString> autoConstraintNames() const;
    // Implicit body targets of the open sketch as snap bodies (bodyTargetsBodies: [0] — projections of edges and
    // vertices off the plane, FX8-SK-010; [1] — silhouettes and extremes, FX8-SK-002); none — nullptr.
    const std::vector<DrawBody>* bodyTargets() const;
    // Silhouette or extreme under the cursor for the dimension and constraint tools: source and «is a point».
    std::optional<std::pair<std::string, bool>> bodyTargetAt(const QPointF& s) const;
    // The object a click of «Dimension» or «Constraint» takes at s (§6.8, §6.9): app::toolTargetAt (sketch_picker) over
    // the candidates under the cursor. One choice for the hover highlight and the click (FX9-SK-006).
    ToolTarget toolTargetAt(const QPointF& s) const;
    ToolTarget toolHover_;
    mutable const void* targetsKey_ = nullptr;
    mutable std::shared_ptr<const std::vector<regen::BodyTarget>> targetsHold_;
    mutable std::vector<DrawBody> targetsBodies_;
    // Trim (app-ux §6.11): the curve under the cursor and the piece to remove.
    std::string trimCurveAt(const QPointF& screen) const;
    // Click with the «Project» tool on a body edge/vertex (app-ux §6.11а).
    void clickProject(const QPointF& s);
    // "Forward" tangent at the end of a chain (tangent arc, §6.3); no curve — nullopt.
    std::optional<Point2> chainTangent() const;

    void updateTrimPreview();
    void clickTrim(const QPointF& s);
    void clickMirror(const QPointF& s);
    void clickPatternCenter(const QPointF& s);
    void openPatternBox(const PatternBox::Values& v, const std::string& editing);
    void clickStar(const QPointF& s, bool suppress);
    bool openPrimitiveBox(const std::string& memberId, const QPointF& at);  // false — not a primitive member
    void primitiveAccepted();
    void primitiveClosed();
    // Create a primitive group and snaps to its members as one command group (§6.3); snap roles → snapshots.
    bool createPrimitive(const char* undoKey, const std::string& type, const doc::Json& params,
                         const std::vector<std::pair<std::string, Snap>>& snaps);
    PrimitiveBox* primitiveBox_ = nullptr;
    std::string primitiveEditing_;
    int starRays_ = 5;
    void patternAccepted();
    void patternClosed();
    PatternBox* patternBox_ = nullptr;
    std::string patternEditing_;
    void clickOffset(const QPointF& s);
    void clickCorner(const QPointF& s);
    std::string cornerPoint_;  // sketch fillet / chamfer: the selected corner point
    std::vector<std::string> pending_;  // selection for mirror / offset; offset — its sketch sources
    // Offset (§6.3, S7F-SK-005): offsetSide_ — the multiplier of the entered value (the sign of the group distance for
    // the chosen side); offsetContour_ — the value field of a contour is open (Ctrl+click adds / removes elements);
    // offsetEdges_ — body edges to project as sources on apply; offsetEditing_ — the edited offset group.
    int offsetSide_ = 1;
    bool offsetContour_ = false;
    std::vector<std::string> offsetEdges_;
    std::map<std::string, sketch::Projection> offsetEdgeGeometry_;  // their projections (preview)
    std::string offsetEditing_;
    void offsetChoice(const std::string& curve, const QPointF& s);  // «Line» / «Contour» menu at the click
    std::string offsetCurveAt(const QPointF& s) const;  // sketch curve or projected curve under the cursor
    void openOffsetValue(const QPointF& s, const QString& value, const char* hintKey);
    void toggleOffsetSource(const QPointF& s);
    void setOffsetEdges(std::vector<std::string> edges);
    void applyOffsetInput();
    // Sketch data with the offset's body edges as temporary projections; sources — the offset sources in it.
    sketch::SketchData offsetData(std::vector<std::string>& sources) const;
    std::vector<std::vector<QPointF>> offsetPreview(std::vector<std::vector<QPointF>>& edges) const;
    std::string lengthText(double mm) const;
    std::string angleText(double rad) const;

    bool commitItems(const doc::Json& items, const std::string& groupKey, std::map<std::string, std::string>* ids = nullptr);
    // Sizes of the sketch's lines, arcs and circles now (length, radius) — to see a placement collapse them (§6.4).
    std::map<std::string, double> curveSizes() const;
    void clickLine(const QPointF& s, bool suppress);
    void clickRectangle(const QPointF& s, bool suppress);
    void clickCircle(const QPointF& s, bool suppress);
    void clickArc3(const QPointF& s, bool suppress);
    void clickCenterRectangle(const QPointF& s, bool suppress);
    void clickPolygon(const QPointF& s, bool suppress);
    void clickCircle3(const QPointF& s, bool suppress);
    void clickEllipse(const QPointF& s, bool suppress);  // S4-SKT-004, app-ux §6.3
    void clickSpline(const QPointF& s, bool suppress);   // S4A-SPL-001, app-ux §6.3
    void finishSpline(bool closed);
    void clickArcCenter(const QPointF& s, bool suppress);
    void clickTangentArc(const QPointF& s, bool suppress);
    // Tangent arc from the end of a curve fromPoint (point ID) with tangent `tangent` to end.
    bool createTangentArc(const std::string& fromPoint, const std::string& fromCurve, const Point2& tangent, Point2 end,
                          const Snap& endSnap, std::string* newEnd);
    void clickPoint(const QPointF& s, bool suppress);
    void clickText(const QPointF& s, bool suppress);   // §6.11в
    void textAccepted(const TextBox::Values& v);
    void clickDimension(const QPointF& s);
    void clickConstraint(const QPointF& s);
    void clickConstructionMode(const QPointF& s);  // «Construction geometry» mode (§6.3, FX1-UI-005)
    // Toggle construction of curves (one undo step) and merge the duplicates it produced (FX7-SK-001).
    void setCurvesConstruction(const std::vector<std::string>& curves, bool construction);
    // Dimension from picked_ (§6.9): kindAt — the point whose position chooses the type (empty — default type),
    // labelAt — label position (screen).
    void createDimension(const std::optional<QPointF>& kindAt, const QPointF& labelAt);
    // Dimension preview (§6.9, FX2-SK-002): compute for the cursor; reset and restart the pause countdown.
    std::optional<DimPreview> computeDimensionPreview(const QPointF& at) const;
    void restartDimensionPreview();
    // Body edge/vertex labels in picked_ → IDs of construction projections (§6.9, М9); false — command error.
    bool resolveBodyPicks();
    bool resolveBodyPicks(std::vector<std::string>& ids);  // srce:/srcv: labels → projection IDs (§6.8, §6.9)
    void openInlineEdit(const std::string& constraintId);
    void finishInlineEdit(bool apply) { inlineEdit_.finish(apply); }
    std::optional<std::string> dimensionKind(const std::vector<std::string>& ids, const QPointF& at) const;
    void resetToolState();
    void clickPierce(const QPointF& s);
    std::string piercePoint_;  // piercing: the selected sketch point (§6.11б)
public:
    // Pierce (§6.11б): source picked outside the view (sketch in the tree) — "pierce:<sketch ID>".
    void pierceSource(const std::string& source);
private:
    void dimensionMenu(const std::string& constraintId, const QPoint& globalPos);
    void pointMenu(const std::string& pointId, const QPoint& globalPos);  // S4-WEAK-006
    void clickPatternLine(const QPointF& s);                               // S4-SKT-006
    void addPointWithSnap(doc::Json& items, const std::string& tempId, const Point2& p, const Snap& snap);
    // Position snap constraint (§6.4 items 1–3) for point tempId.
    void snapConstraint(doc::Json& items, const std::string& tempId, const Snap& snap) const;
    // Direction snap of segment a→b (§6.4 items 4–6): snapped b and the constraint (type, second entity).
    struct DirectionSnap {
        std::string type;    // horizontal | vertical | parallel | perpendicular | tangent | empty
        std::string other;   // woken segment / preceding arc
        Point2 end = Point2::Zero();
        std::string alsoTangent;  // H/V from an arc end coinciding with the tangent — also tangent to that arc (§6.4)
    };
    // The snap functions below skip the hints dismissed by Esc (FX10-SK-005) unless raw.
    DirectionSnap directionSnap(const Point2& a, const Point2& b, bool raw = false) const;
    // tolScale — the tolerance in units of the point snap one (an arc by 3 points: arc3RadiusSensitivity, FX10-SK-004).
    std::optional<api::sketchops::EqualMatch> equalSnap(bool radius, double value, bool raw = false, double tolScale = 1) const;  // FX7-SK-003
    std::vector<std::string> arc3Tangents(const Point2& a, const Point2& b, const Point2& m, bool raw = false) const;  // FX7-SK-002
    // Curves tangent to an arc with center c at its ends a, b snapped to curve ends (§6.4 tangency at a joint).
    std::vector<std::string> arcEndTangents(const Point2& c, const Point2& a, const Snap& sa, const Point2& b, const Snap& sb,
                                            bool raw = false) const;
    // The arc ending at point (a snap target) that segment direction dir enters smoothly (§6.4, FX10-SK-004).
    std::string arcEnteredAt(const std::string& point, const Point2& dir, bool raw = false) const;
    // Key of the direction hint of the «Line» tool (FX10-SK-005): "dir:<type>:<other>", "tangent:<arc>" or empty.
    std::string directionHintKey(bool raw) const;
    bool hintDismissed(const std::string& key) const { return dismissedHints_.contains(key); }
    // Esc on a shown auto-constraint hint dismisses it (§6.13 item 0, FX10-SK-005); false — no hint shown.
    bool dismissShownHints();
    // Hints dismissed by Esc until the next click of the tool or until the cursor leaves the hint (FX10-SK-005).
    snap::DismissedHints dismissedHints_;

    AppModel& model_;
    Viewport& view_;
    std::string feature_;
    std::optional<api::SketchState> state_;
    Tool tool_ = Tool::Select;
    std::string constraintType_;

    // Tool phases.
    std::vector<Point2> clicks_;
    bool polygonInscribed_ = true;
    int polygonSides_ = 6;
    QPointer<QLineEdit> drawInput_;
    TextBox* textBox_ = nullptr;
    ToleranceBox* toleranceBox_ = nullptr;
    std::string toleranceDim_;
    void openToleranceBox(const std::string& cid);
    void toleranceAccepted(const doc::Json& tol);
    void toleranceRemoved();
    Snap textSnap_;            // snap of the new text's point
    std::string textEditing_;  // text being edited (ID) or empty — a new one
    void openDrawInput(const QString& first);
    void applyDrawInput();
    std::vector<Snap> clickSnaps_;
    std::string chainEndPoint_;    // persistent ID of the previous chain segment's end
    std::string chainStartPoint_;  // persistent ID of the chain start
    std::string chainEndCurve_;    // curve the chain ends with (for the tangent arc)
    bool returnToLine_ = false;    // tangent arc from a chain of segments → then segment again
    bool centerline_ = false;      // the current segment is a centerline (construction)
    bool drawConstruction_ = false;  // the construction mode (§6.3, FX7-SK-001)
    bool dimAutoSelected_ = false;  // see dimensionAutoSelected()  // the «Dimension» selection is the just placed dimension (S7F-SK-002), not a click
    std::set<std::string> lastDrawn_;  // entities of the last element drawn by the current tool (FX7-SK-008)
    double arcSweep_ = 0;          // accumulated sweep angle around the center (arc center–start–end)
    std::optional<double> lastArcAngle_;
    std::optional<sketch::TrimPlan> trimPreview_;
    // "Woken" segment (§6.4 item 5): the previous chain segment or the segment under the cursor ≥ WAKE_HOVER_MS.
    std::string hoverLine_;
    qint64 hoverLineSince_ = 0;
    std::string wokenLine_;
    QElapsedTimer clock_;
    QPointF cursor_;
    bool cursorValid_ = false;
    bool ctrlHeld_ = false;
    Snap hoverSnap_;

    std::vector<std::string> picked_;  // for a dimension/constraint
    std::vector<QPointF> pickClicks_;  // click points for picked_ (dimension, §6.9)
    std::optional<DimPreview> dimPreview_;  // dimension preview (§6.9, FX2-SK-002)
    QTimer* dimPreviewTimer_ = nullptr;
    QPointF dimPreviewAt_;                  // where the mouse stopped (moving < DIM_PREVIEW_MOVE_PX does not reset)
    OrderedSelection selection_;
    Hit hover_;

    // Dragging.
    bool pressed_ = false;
    QPointF pressPos_;
    Hit pressHit_;
    bool dragging_ = false;
    bool boxing_ = false;  // selection box (§6.3)
    bool showConstraints_ = true;
    // Dragging a dimension label (§6.6, FX1-SKI-001): dimension ID, label position (sketch coordinates) before start
    // and the current one.
    std::string labelDrag_;
    Point2 labelDragFrom_ = Point2::Zero(), labelDragAt_ = Point2::Zero();
    void finishBox(bool add);
    std::optional<api::SketchState> dragState_;

    InlineDimensionEdit inlineEdit_;
};

}  // namespace construe::app
