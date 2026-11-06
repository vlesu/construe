// Dimensions on a drawing sheet (spec/app-ux.md §8б «Dimensions», drawing.md §6а, S3-DRW-006): placement
// mode with highlighting of view edges and vertices, preview, selection, dragging, deletion, dimension properties row.
#pragma once
#include "app/drawing_view.h"

#include "api/session.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QObject>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class DimensionTool : public QObject, public DrawingView::Tool {
    Q_OBJECT
public:
    struct Hooks {
        std::function<api::Session&()> session;
        std::function<std::string()> currentSheet;
        std::function<void(const QString&, bool)> toast;    // notification (error — true)
        std::function<void(const QString&)> status;         // status bar hint; empty — clear
        std::function<void()> selected;                     // dimension selected: window drops view selection, rows
        std::function<void(bool)> activeChanged;            // dimension mode switched on / off
    };
    DimensionTool(DrawingView* view, Hooks hooks, QObject* parent = nullptr);

    QWidget* bar() const { return bar_; }
    bool active() const { return active_; }
    void setActive(bool on);
    const std::string& selectedDimension() const { return selected_; }
    void select(const std::string& id);
    // The document or sheet changed: reset the view geometry cache, validate the selection, update the properties row.
    void refresh();
    // Esc (§6.13, §8б): reset selected objects, leave the mode, clear the selection; false — nothing to cancel.
    bool escape();
    // Context menu of a dimension on the sheet; false — no dimension under the point.
    bool contextMenu(const Point2& at, const QPoint& globalPos);
    // Dimension under a sheet point (lines within PICK_CURVE_PX, label extent); empty — none.
    std::string dimensionAt(const Point2& at) const;
    // Showing undimensioned model dimensions in blue (FX7-DRW-003, app-ux §8б): on — recomputed on every refresh().
    void setShowUndimensioned(bool on);
    bool showUndimensioned() const { return showUndimensioned_; }
    const std::vector<drawing::DimensionPreview>& undimensioned() const { return undimensioned_; }
    // Model marks not on the sheet, in blue with the dimensions (FX12-DRW-004): roughness, tolerances, datums, welds.
    const std::vector<drawing::MarkPreview>& unplacedMarks() const { return unplacedMarks_; }
    int unplacedMarkAt(const Point2& at) const;  // index under a sheet point; −1 — none
    // Index of the blue dimension under a sheet point (lines within PICK_CURVE_PX, label extent); −1 — none.
    int undimensionedAt(const Point2& at) const;
    // Double click (FX7-DRW-003, FX12-DRW-004): the blue dimension or mark under the point is placed on the sheet; false
    // — none there.
    bool adoptUndimensioned(const Point2& at);
    // Name of the view entity highlighted under the cursor (dimension mode); empty — none. For tests.
    std::string hoverName() const { return hover_ ? hover_->e.name : std::string(); }
    std::string hoverRef() const { return hover_ ? hover_->e.ref : std::string(); }  // its kind (FX9-DRW-001), for tests
    // The face whose line on a view is nearest to a sheet point within PICK_CURVE_PX (drawing.md §6а.7 ViewEntity.face;
    // a mark set in the drawing goes onto it, FX9-DRW-009): {view, face}; none — no such line.
    std::optional<std::pair<std::string, std::string>> faceAt(const Point2& at) const;

    // DrawingView::Tool
    bool press(const Point2& at) override;
    void move(const Point2& at, bool leftDown) override;
    void release(const Point2& at) override;
    bool key(int key) override;
    void paint(QPainter& p) override;
    std::string hiddenRole() const override { return dragging_ ? "dim:" + dragId_ : std::string(); }

private:
    struct Pick {
        std::string view;
        drawing::ViewEntity e;
    };
    DrawingView* view_;
    Hooks h_;
    QWidget* bar_ = nullptr;
    QComboBox* type_ = nullptr;
    QComboBox* tolerance_ = nullptr;  // showing the tolerance from the model (FX3-DRW-001)
    QCheckBox* break_ = nullptr;      // dimension line with a break (FX3-DRW-003)
    QCheckBox* local_ = nullptr;      // tolerance set in the drawing (FX9-DRW-009)
    QLineEdit* fit_ = nullptr;        // its tolerance field
    QLineEdit *prefix_ = nullptr, *text_ = nullptr, *suffix_ = nullptr;
    bool active_ = false;
    std::string selected_;
    // Placement: selected objects, hovered one, preview.
    std::vector<Pick> picks_;
    std::optional<Pick> hover_;
    std::vector<drawing::Primitive> preview_;
    std::optional<doc::SheetDimension> previewDim_;
    // Pressing a dimension outside the mode: dragging.
    bool pressed_ = false, dragging_ = false;
    Point2 pressAt_{0, 0};
    std::string dragId_;
    bool showUndimensioned_ = false;
    std::vector<drawing::DimensionPreview> undimensioned_;
    std::vector<drawing::MarkPreview> unplacedMarks_;
    void recomputeUndimensioned();
    // Geometry cache of the current sheet's views (viewEntities); reset by refresh().
    mutable std::map<std::string, std::vector<drawing::ViewEntity>> entities_;

    std::shared_ptr<const doc::Document> document() const;
    const doc::Sheet* sheet(const doc::Document& d) const;
    const doc::SheetDimension* dimensionById(const doc::Document& d, const std::string& id) const;
    const std::vector<drawing::ViewEntity>& entitiesOf(const std::string& viewId) const;
    std::optional<Pick> entityAt(const Point2& at) const;
    std::optional<doc::SheetDimension> fromPicks() const;
    void updatePreview(const Point2& at);
    void clickPlace(const Point2& at);
    void resetPicks();
    void refreshBar();
    void applyText(QLineEdit* e, const char* field);
    double tolMm(int px) const;
};

}  // namespace construe::app
