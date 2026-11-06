// Sketch text in the client (spec/app-ux.md §6.11в, S2-SK-013): contours for display and the text box at the cursor.
#pragma once

#include "base/math.h"
#include "font/font.h"
#include "sketch/data.h"
#include "app/popup_box.h"
#include "app/rich_text_field.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QToolButton>

#include <functional>
#include <string>
#include <vector>

namespace construe::app {

class AppModel;

// Text contours for display: polylines in sketch coordinates (arcs as segments); font not found: empty.
std::vector<std::vector<Point2>> textPolylines(const sketch::Entity& e, const Point2& anchor);
// Glyph contour as one polyline in traversal order (an arc is 8 segments; curves oriented by orientContour,
// drawing.md §3.1; app-ux §6.11в, FX1-SKG-007).
std::vector<Point2> contourPolyline(const std::vector<font::Curve2>& contour);

// Text box (non-modal, over the view): multi-line text with bold / italic / underline buttons, font, height, angle;
// OK, Ctrl+Enter in the text or Enter in the other fields emit accepted (Enter in the text — a new line), Esc closes.
class TextBox : public PopupBox {
    Q_OBJECT
public:
    struct Values {
        std::string text, font;
        double heightMm = 5, angleRad = 0;
        std::vector<font::StyleSpan> styles;
    };
    TextBox(AppModel& model, QWidget* parent);
    void open(const QPoint& at, const Values& v);
    QTextEdit* textField() const { return text_; }

signals:
    void accepted(const TextBox::Values& v);

protected:
    void acceptBox() override { accept(); }
    void cancelBox() override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void accept();
    void placeAt(QPoint p);  // moves the box (adjusted to its size) to p inside the view
    AppModel& model_;
    RichTextField* field_;
    QTextEdit* text_;
    QLineEdit *height_, *angle_;
    QComboBox* font_;
    QPushButton* ok_;
};

}  // namespace construe::app
