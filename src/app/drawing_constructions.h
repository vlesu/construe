// Constructions on a drawing sheet (spec/app-ux.md §8б «Constructions», drawing.md §6.29, FX14-DRW-006): placement of a
// segment, a circle, an arc, a pitch circle through hole centers and an axis line with points snapped to the view model
// (vertices, circle centers, edge middles and points) or free; selection, dragging of free points, deletion, the
// construction properties block (line type, radius).
#pragma once
#include "app/drawing_view.h"

#include "api/session.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class ConstructionTool : public QObject, public DrawingView::Tool {
    Q_OBJECT
public:
    struct Hooks {
        std::function<api::Session&()> session;
        std::function<std::string()> currentSheet;
        std::function<void(const QString&, bool)> toast;  // notification (error — true)
        std::function<void(const QString&)> status;       // status bar hint; empty — clear
        std::function<void()> selected;                   // a construction selected: the window drops other selections
        std::function<void(bool)> placingChanged;         // placement mode switched on / off
        std::function<bool()> busy;                       // another sheet mode is on (dimensions): presses are not taken
    };
    ConstructionTool(DrawingView* view, Hooks hooks, QObject* parent = nullptr);

    QWidget* bar() const { return bar_; }
    // Placement of a construction of kind (line | circle | arc | pitchCircle | axis); empty — off.
    void setPlacing(const std::string& kind);
    const std::string& placing() const { return kind_; }
    const std::string& selectedConstruction() const { return selected_; }
    void select(const std::string& id);
    void refresh();   // the document or the sheet changed
    bool escape();    // Esc: drop the picked points, leave the mode, clear the selection; false — nothing to cancel
    // Construction under a sheet point (its lines within PICK_CURVE_PX); empty — none.
    std::string constructionAt(const Point2& at) const;
    size_t pickedCount() const { return points_.size(); }  // points picked in the placement (for tests)
    // Kind of the snap under a sheet point in the placement: vertex | center | edge | free | "" (no view) — for tests.
    std::string snapKindAt(const Point2& at) const;

    // DrawingView::Tool
    bool press(const Point2& at) override;
    void move(const Point2& at, bool leftDown) override;
    void release(const Point2& at) override;
    bool key(int key) override;
    void paint(QPainter& p) override;
    std::string hiddenRole() const override { return dragging_ ? "cons:" + selected_ : std::string(); }

private:
    struct Snap {
        std::string view;
        doc::ConstructionPoint point;  // xy in view coordinates and the binding
        Point2 sheet{0, 0};            // the snapped point on the sheet
        std::vector<drawing::Primitive> highlight;  // the snapped model entity
    };
    DrawingView* view_;
    Hooks h_;
    QWidget* bar_ = nullptr;
    QComboBox* line_ = nullptr;
    QLineEdit* radius_ = nullptr;
    QLabel* info_ = nullptr;
    std::string kind_;  // placement kind; empty — not placing
    std::vector<Snap> points_;
    std::optional<Snap> hover_;
    std::vector<drawing::Primitive> preview_;
    std::string selected_;
    std::vector<Point2> selPoints_;  // effective points of the selected construction on the sheet
    bool pressed_ = false, dragging_ = false;
    int dragPoint_ = -1;
    Point2 pressAt_{0, 0};
    mutable std::map<std::string, std::vector<drawing::ViewEntity>> entities_;

    std::shared_ptr<const doc::Document> document() const;
    const doc::Sheet* sheet(const doc::Document& d) const;
    const doc::SheetConstruction* byId(const doc::Document& d, const std::string& id) const;
    const std::vector<drawing::ViewEntity>& entitiesOf(const std::string& viewId) const;
    const drawing::ViewPlacement* placementOf(const std::string& viewId) const;
    std::optional<Snap> snapAt(const Point2& at) const;
    doc::SheetConstruction candidate(const std::optional<Snap>& extra) const;
    size_t requiredPoints() const;  // points that finish the placement; 0 — finished by Enter (pitch circle)
    void updatePreview();
    void finish();
    void resetPoints();
    void refreshBar();
    void prompt();
    double tolMm(int px) const;
};

}  // namespace construe::app
