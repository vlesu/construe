#include "app/text_box.h"

#include "app/app_model.h"
#include "app/dialog_buttons.h"
#include "app/expr_input.h"
#include "app/messages.h"
#include "app/theme.h"
#include "drawing/render.h"
#include "font/font.h"

#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include <array>
#include <cmath>

namespace construe::app {

std::vector<std::vector<Point2>> textPolylines(const sketch::Entity& e, const Point2& anchor) {
    std::vector<std::vector<Point2>> out;
    auto f = font::loadFont(e.font);
    if (!f) return out;
    const auto lay = font::layoutText(**f, e.text, e.height, anchor, e.angle, e.styles);
    // A contour is one polyline in traversal order: curves are oriented as on the sheet (drawing.md §3.1
    // «Glyph contour traversal», orientContour): an arc traversed clockwise goes clockwise,
    // a segment from its traversal start; otherwise the polyline "jumped" between arc ends with extra segments
    // (app-ux §6.11в, FX1-SKG-007).
    for (const auto& contour : lay.contours) out.push_back(contourPolyline(contour));
    return out;
}

std::vector<Point2> contourPolyline(const std::vector<font::Curve2>& contour) {
    std::vector<Point2> pl;
    for (const auto& k : drawing::orientContour(contour)) {
        if (!k.arc) {
            if (pl.empty()) pl.push_back(k.from);
            pl.push_back(k.to);
            continue;
        }
        const Point2 a = k.from - k.center, b = k.to - k.center;
        const double r = a.norm(), a0 = std::atan2(a.y(), a.x());
        double sw = std::atan2(a.x() * b.y() - a.y() * b.x(), a.dot(b));  // from `from` to `to` CCW, (−π, π]
        if (k.ccw && sw < 0) sw += 2 * 3.14159265358979323846;
        if (!k.ccw && sw > 0) sw -= 2 * 3.14159265358979323846;
        for (int i = pl.empty() ? 0 : 1; i <= 8; ++i) pl.push_back(k.center + r * Point2(std::cos(a0 + sw * i / 8), std::sin(a0 + sw * i / 8)));
        pl.back() = k.to;
    }
    return pl;
}

TextBox::TextBox(AppModel& model, QWidget* parent) : PopupBox("textEditor", parent), model_(model) {
    auto* col = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    col->addLayout(form);
    // The text with style buttons above it and a growing height (FX8-SK-007, FX9-UI-002); the box follows its height.
    field_ = new RichTextField("textValue", "text");
    field_->setMinimumTextWidth(TEXT_EDIT_MIN_WIDTH_PX);
    text_ = field_->edit();
    text_->installEventFilter(this);
    connect(field_, &RichTextField::heightChanged, this, [this] {
        if (isVisible()) placeAt(pos());
    });
    form->addRow(txt("ui.common.text"), field_);
    font_ = new QComboBox;
    font_->setObjectName("textFont");
    form->addRow(txt("ui.common.font"), font_);
    height_ = new QLineEdit;
    height_->setObjectName("textHeight");
    form->addRow(txt("ui.common.height"), height_);
    angle_ = new QLineEdit;
    angle_->setObjectName("textAngle");
    form->addRow(txt("ui.common.angle"), angle_);
    auto* buttons = new DialogButtons("textOk", QString());  // OK only: Esc closes the box
    ok_ = buttons->ok();
    ok_->setToolTip("OK (Ctrl+Enter)");
    col->addWidget(buttons);
    connect(buttons, &DialogButtons::accepted, this, [this] { accept(); });
    // Enter in the height and angle fields reaches the box (PopupBox) and accepts it once.
    hide();
}

bool TextBox::eventFilter(QObject* o, QEvent* e) {
    // Ctrl+Enter and Esc belong to the text field (Ctrl+B/I/U — RichTextField), not to the window shortcuts (Ctrl+Enter there exits the
    // sketch): the shortcut override is accepted, the key press comes below.
    if (o == text_ && e->type() == QEvent::ShortcutOverride) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool enter = k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter;
        const bool ctrl = k->modifiers() & Qt::ControlModifier;
        if (ctrl && enter) {
            e->accept();
            return true;
        }
        if (k->key() == Qt::Key_Escape) {  // closes the box here: the key press may not reach the field
            e->accept();
            cancelBox();
            return true;
        }
    }
    if (o == text_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool enter = k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter;
        if (enter && (k->modifiers() & Qt::ControlModifier)) {  // Ctrl+Enter — done (Enter — a new line)
            accept();
            return true;
        }
        if (k->key() == Qt::Key_Escape) {
            cancelBox();
            return true;
        }
    }
    return PopupBox::eventFilter(o, e);
}

void TextBox::open(const QPoint& at, const Values& v) {
    const auto u = model_.session().units();
    font_->clear();
    for (const auto& n : api::Session::fonts()) font_->addItem(QString::fromStdString(n));
    int fi = font_->findText(QString::fromStdString(v.font));
    if (fi < 0 && !v.font.empty()) {  // text font not listed (excluded or not in the library, font.md §2а): not lost
        font_->addItem(QString::fromStdString(v.font));
        fi = font_->count() - 1;
    }
    if (fi < 0) fi = font_->findText(QString::fromStdString(api::Session::defaultFont()));
    font_->setCurrentIndex(std::max(fi, 0));
    field_->setValue(v.text, v.styles);
    height_->setText(formatNumber(v.heightMm / expr::lengthFactor(u.length)));
    angle_->setText(formatNumber(v.angleRad / expr::angleFactor(u.angle)));
    for (QWidget* f : {static_cast<QWidget*>(text_), static_cast<QWidget*>(height_), static_cast<QWidget*>(angle_)}) markFieldError(f, false);
    openAt(at + QPoint(12, 12));
    field_->fitHeight();
    text_->setFocus();
    text_->selectAll();
}

void TextBox::placeAt(QPoint p) {
    adjustSize();
    moveInside(this, p);
}

void TextBox::accept() {
    const auto u = model_.session().units();
    bool ok = true;
    Values v;
    auto [plain, spans] = textWithStyles(*text_->document());
    v.text = plain;
    v.styles = spans;
    v.font = font_->currentText().toStdString();
    if (QString::fromStdString(plain).trimmed().isEmpty()) markFieldError(text_, true), ok = false;
    const auto hv = parseDecimal(height_->text()), av = parseDecimal(angle_->text());
    const bool okH = hv.has_value(), okA = av.has_value();
    const double h = hv.value_or(0) * expr::lengthFactor(u.length);
    const double a = av.value_or(0) * expr::angleFactor(u.angle);
    if (!okH || !(h > 0)) markFieldError(height_, true), ok = false;
    if (!okA) markFieldError(angle_, true), ok = false;
    if (!ok) return;
    v.heightMm = h;
    v.angleRad = a;
    hide();
    emit accepted(v);
}

void TextBox::cancelBox() {
    hide();
    if (parentWidget()) parentWidget()->setFocus();
}

}  // namespace construe::app
