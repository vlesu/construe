// Texts on a drawing sheet (spec/app-ux.md §8б «Texts», drawing.md §6.23, FX7-DRW-006): placement by a
// click or by stretching a frame (FX9-DRW-007), editing in place (a text field over the sheet) or in the large field of
// the «Text» block — both with styles (FX9-DRW-006), font, size, rotation, selection, dragging, the frame
// corners, deletion.
#pragma once
#include "app/drawing_view.h"

#include "api/session.h"
#include "app/rich_text_field.h"

#include <QComboBox>
#include <QLineEdit>
#include <QObject>
#include <QWidget>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class SheetTextTool : public QObject, public DrawingView::Tool {
    Q_OBJECT
public:
    struct Hooks {
        std::function<api::Session&()> session;
        std::function<std::string()> currentSheet;
        std::function<void(const QString&, bool)> toast;  // notification (error — true)
        std::function<void(const QString&)> status;       // status bar hint; empty — clear
        std::function<void()> selected;                   // a text selected: the window drops other selections
        std::function<void(bool)> placingChanged;         // placement mode switched on / off
    };
    SheetTextTool(DrawingView* view, Hooks hooks, QObject* parent = nullptr);

    QWidget* bar() const { return bar_; }
    // Placement mode (ribbon «Text»): the next click on the sheet opens the editor of a new text there.
    void setPlacing(bool on);
    bool placing() const { return placing_; }
    const std::string& selectedText() const { return selected_; }
    void select(const std::string& id);
    // Text under a sheet point (extent of its lines); empty — none.
    std::string textAt(const Point2& at) const;
    // Double click: editing the text under the point in place; false — no text there.
    bool doubleClick(const Point2& at);
    // The in-place editor (nullptr — closed); for tests.
    QTextEdit* editor() const { return editor_ ? editor_->edit() : nullptr; }
    RichTextField* editorField() const { return editor_; }
    // Frame of a text on the sheet (FX9-DRW-007): corners top left, top right, bottom right, bottom left; a text without
    // a frame — the extent of its lines; empty — no such text.
    std::optional<std::array<Point2, 4>> frameOf(const std::string& id) const;
    void refresh();  // the document or the sheet changed
    void fillFonts();  // «Font»: as in the drawing, then the library fonts (FX10-DRW-004)
    bool escape();   // Esc: close the editor, leave the mode, clear the selection; false — nothing to cancel

    // DrawingView::Tool
    bool press(const Point2& at) override;
    void move(const Point2& at, bool leftDown) override;
    void release(const Point2& at) override;
    bool key(int key) override;
    void paint(QPainter& p) override;
    std::string hiddenRole() const override { return dragging_ || corner_ >= 0 ? "text:" + selected_ : std::string(); }

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    DrawingView* view_;
    Hooks h_;
    QWidget* bar_ = nullptr;
    RichTextField* contentField_ = nullptr;
    QTextEdit* content_ = nullptr;
    QComboBox* font_ = nullptr;
    QComboBox* height_ = nullptr;
    QLineEdit* angle_ = nullptr;
    bool placing_ = false;
    std::string selected_;
    // In-place editor: new text at a point (editId_ empty; with a frame editBox_ — FX9-DRW-007) or editing editId_.
    RichTextField* editor_ = nullptr;
    std::string editId_;
    Point2 editAt_{0, 0}, editBox_{0, 0};
    bool pressed_ = false, dragging_ = false;
    Point2 pressAt_{0, 0}, dragDelta_{0, 0};
    // Placement by stretching a frame (FX9-DRW-007): the press point and the current corner.
    bool placePressed_ = false;
    Point2 placeFrom_{0, 0}, placeTo_{0, 0};
    // Dragging a frame corner (FX9-DRW-007): its index (−1 — none), the frame at the press, the current point.
    int corner_ = -1;
    std::array<Point2, 4> cornerFrame_{};
    Point2 cornerTo_{0, 0};

    std::shared_ptr<const doc::Document> document() const;
    const doc::SheetText* textById(const doc::Document& d, const std::string& id) const;
    void openEditor(const Point2& at, const std::string& id, const std::string& text, const std::vector<font::StyleSpan>& styles,
                    const Point2& box = Point2(0, 0));
    int cornerAt(const Point2& at) const;
    // The new frame when corner_ is dragged to cornerTo_: corners as frameOf.
    std::array<Point2, 4> draggedFrame() const;
    void commitEditor();
    void closeEditor();
    void refreshBar();
    void applyField(const api::Json& def);
};

}  // namespace construe::app
