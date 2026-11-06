// Texts on a drawing sheet (spec/app-ux.md §8б «Texts», drawing.md §6.23, FX7-DRW-006, FX9-DRW-006, FX9-DRW-007).
#include "app/drawing_texts.h"

#include "app/expr_input.h"
#include "app/messages.h"
#include "app/theme.h"
#include "app/sheet_print.h"

#include "drawing/render.h"
#include "base/constants.h"
#include "base/tolerance.h"

#include <QFormLayout>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {

// GOST 2.304 font sizes (mm) offered in «Size»; any positive value can be typed.
const char* const TEXT_SIZES[] = {"1,8", "2,5", "3,5", "5", "7", "10", "14", "20"};

// Text extent in its own axes: x by glyph outlines, y from the baseline to the height (as a dimension label, §8б).
bool insideText(const drawing::Primitive& pr, const Point2& p, double margin) {
    const auto [x0, x1] = textExtentAlong(pr).value_or(std::pair(0.0, 0.7 * pr.height * double(pr.text.size())));
    const Point2 q = textLocal(pr, p);
    return q.x() >= x0 - margin && q.x() <= x1 + margin && q.y() >= -margin && q.y() <= pr.height + margin;
}

void fillText(QPainter& p, const DrawingView& v, const drawing::Primitive& pr, const Point2& off, const QColor& color) {
    const QPainterPath path = sheetTextPath(pr);
    if (!path.isEmpty()) p.fillPath(v.sheetTransform().map(path.translated(off.x(), off.y())), color);
}


QString numberText(double v) { return QString::number(v, 'g', 6).replace('.', ','); }

api::Json stylesJson(const std::vector<font::StyleSpan>& styles) {
    if (styles.empty()) return nullptr;  // no field
    api::Json a = api::Json::array();
    for (const auto& sp : styles) {
        api::Json o{{"from", sp.from}, {"to", sp.to}};
        if (sp.bold) o["bold"] = true;
        if (sp.italic) o["italic"] = true;
        if (sp.underline) o["underline"] = true;
        a.push_back(o);
    }
    return a;
}

// Paragraph alignments up to the last not "left" (S7F-DRW-002); all left — none.
std::vector<std::string> trimAligns(std::vector<std::string> a) {
    while (!a.empty() && (a.back().empty() || a.back() == "left")) a.pop_back();
    for (auto& x : a)
        if (x.empty()) x = "left";
    return a;
}

api::Json alignsJson(const std::vector<std::string>& aligns) {
    const auto a = trimAligns(aligns);
    if (a.empty()) return nullptr;  // no field
    return api::Json(a);
}

// Point p in the axes of a frame (origin — its top left corner, x — along the top edge, y — up).
Point2 frameLocal(const std::array<Point2, 4>& f, const Point2& p) {
    const Point2 ux = (f[1] - f[0]).norm() > 1e-12 ? Point2((f[1] - f[0]).normalized()) : Point2(1, 0);
    const Point2 uy(-ux.y(), ux.x());
    return Point2((p - f[0]).dot(ux), (p - f[0]).dot(uy));
}

}  // namespace

SheetTextTool::SheetTextTool(DrawingView* view, Hooks hooks, QObject* parent) : QObject(parent), view_(view), h_(std::move(hooks)) {
    // «Text» block of the title-block panel (app-ux §8б): content (the large field), font, size, rotation.
    bar_ = new QWidget;
    bar_->setObjectName("textBar");
    auto* l = new QFormLayout(bar_);
    l->setContentsMargins(0, 6, 0, 0);
    auto* hdr = new QLabel(txt("ui.common.text"));
    hdr->setObjectName("textPropsHeader");
    QFont hf = hdr->font();
    hf.setBold(true);
    hdr->setFont(hf);
    l->addRow(hdr);
    // The text with styles, growing with its lines (FX9-DRW-006, FX9-UI-002).
    contentField_ = new RichTextField("sheetTextContent", "sheetText");
    contentField_->enableAlignment();
    content_ = contentField_->edit();
    content_->setToolTip(txt("ui.drawingText.textLinesCtrlEnterOr"));
    content_->installEventFilter(this);
    l->addRow(contentField_);
    font_ = new QComboBox;
    font_->setObjectName("sheetTextFont");
    fillFonts();
    l->addRow(txt("ui.common.font"), font_);
    height_ = new QComboBox;
    height_->setObjectName("sheetTextHeight");
    height_->setEditable(true);
    height_->setInsertPolicy(QComboBox::NoInsert);
    for (const char* sz : TEXT_SIZES) height_->addItem(QString::fromUtf8(sz));
    l->addRow(txt("ui.drawingText.sizeMm"), height_);
    angle_ = new QLineEdit;
    angle_->setObjectName("sheetTextAngle");
    l->addRow(txt("ui.drawingText.rotation"), angle_);
    bar_->hide();
    connect(font_, &QComboBox::activated, this, [this] {
        const QString f = font_->currentData().toString();
        applyField(api::Json{{"font", f.isEmpty() ? api::Json(nullptr) : api::Json(f.toStdString())}});
    });
    auto applyHeight = [this] {
        const auto v = parseDecimal(height_->currentText());
        if (!v || *v <= 0) {
            h_.toast(text("ui.sheetText.badSize"), true);
            refreshBar();
            return;
        }
        const auto d = document();
        const doc::SheetText* t = d ? textById(*d, selected_) : nullptr;
        if (t && t->height != *v) applyField(api::Json{{"height", *v}});
    };
    connect(height_, &QComboBox::activated, this, applyHeight);
    connect(height_->lineEdit(), &QLineEdit::editingFinished, this, [this, applyHeight] {
        if (!height_->lineEdit()->isModified()) return;
        height_->lineEdit()->setModified(false);
        applyHeight();
    });
    connect(angle_, &QLineEdit::editingFinished, this, [this] {
        if (!angle_->isModified()) return;
        angle_->setModified(false);
        const auto v = parseDecimal(angle_->text());
        if (!v) {
            h_.toast(text("ui.sheetText.badAngle"), true);
            refreshBar();
            return;
        }
        applyField(api::Json{{"angle", *v == 0 ? api::Json(nullptr) : api::Json(*v)}});
    });
}

std::shared_ptr<const doc::Document> SheetTextTool::document() const { return h_.session().document().document; }

const doc::SheetText* SheetTextTool::textById(const doc::Document& d, const std::string& id) const {
    if (!d.active().isDrawing() || id.empty()) return nullptr;
    const int si = d.active().sheetIndex(h_.currentSheet());
    if (si < 0) return nullptr;
    const auto& sh = d.active().sheets[size_t(si)];
    const int ti = sh.textIndex(id);
    return ti < 0 ? nullptr : &sh.texts[size_t(ti)];
}

void SheetTextTool::setPlacing(bool on) {
    if (on == placing_) return;
    placing_ = on;
    if (on) select({});
    if (h_.placingChanged) h_.placingChanged(on);  // other modes off first: they clear the hint
    h_.status(on ? text("ui.status.placeText") : QString());
    view_->setCursor(on ? Qt::IBeamCursor : Qt::ArrowCursor);
}

void SheetTextTool::select(const std::string& id) {
    if (id == selected_) return;
    selected_ = id;
    refreshBar();
    if (!id.empty() && h_.selected) h_.selected();
    view_->update();
}

std::string SheetTextTool::textAt(const Point2& at) const {
    const double margin = safeDiv(PICK_CURVE_PX, view_->zoom());
    const auto& prims = view_->graphics().primitives;
    for (auto it = prims.rbegin(); it != prims.rend(); ++it)
        if (it->kind == drawing::PrimitiveKind::Text && it->role.rfind("text:", 0) == 0 && insideText(*it, at, margin))
            return it->role.substr(5);
    return {};
}

bool SheetTextTool::doubleClick(const Point2& at) {
    const std::string id = textAt(at);
    if (id.empty()) return false;
    const auto d = document();
    const doc::SheetText* t = d ? textById(*d, id) : nullptr;
    if (!t) return false;
    select(id);
    openEditor(Point2(t->position[0], t->position[1]), id, t->text, t->styles);
    return true;
}

void SheetTextTool::openEditor(const Point2& at, const std::string& id, const std::string& content,
                               const std::vector<font::StyleSpan>& styles, const Point2& box) {
    closeEditor();
    editId_ = id;
    editAt_ = at;
    editBox_ = box;
    editor_ = new RichTextField("sheetTextEditor", "sheetEditor", view_);
    editor_->setObjectName("sheetTextEditorBox");
    editor_->setAutoFillBackground(true);
    editor_->enableAlignment();
    editor_->setValue(content, styles);
    if (!id.empty())
        if (const auto d = document())
            if (const doc::SheetText* t = textById(*d, id)) editor_->setAligns(t->aligns);
    editor_->edit()->setToolTip(txt("ui.drawingText.ctrlEnterDoneEscCancel"));
    // Width: the frame, otherwise not narrower than the text on the screen (FX9-UI-002); top left — at the text.
    int width = SHEET_TEXT_EDITOR_MIN_W_PX;
    QPointF corner = view_->sheetToWidget(at) - QPointF(0, 24);
    const auto fr = id.empty() ? std::optional<std::array<Point2, 4>>() : frameOf(id);
    if (fr) {
        width = std::max(width, int(std::ceil(((*fr)[1] - (*fr)[0]).norm() * view_->zoom())) + 24);
        corner = view_->sheetToWidget((*fr)[0]);
    }
    if (box.x() > 0) width = std::max(SHEET_TEXT_EDITOR_MIN_W_PX, int(std::ceil(box.x() * view_->zoom())) + 24);
    editor_->setMinimumTextWidth(width);
    editor_->adjustSize();
    const int barH = editor_->height() - editor_->edit()->height();
    editor_->move(std::max(0, int(corner.x())), std::max(0, int(corner.y()) - barH));
    connect(editor_, &RichTextField::heightChanged, editor_, [e = editor_] { e->adjustSize(); });
    editor_->edit()->installEventFilter(this);
    editor_->show();
    editor_->raise();
    editor_->edit()->setFocus(Qt::OtherFocusReason);
    editor_->edit()->moveCursor(QTextCursor::End);
    h_.status(text("ui.status.editText"));
}

void SheetTextTool::closeEditor() {
    if (!editor_) return;
    RichTextField* e = std::exchange(editor_, nullptr);
    e->edit()->removeEventFilter(this);
    e->hide();
    e->deleteLater();
    editId_.clear();
    h_.status(QString());
}

void SheetTextTool::commitEditor() {
    if (!editor_) return;
    const auto [body, styles] = editor_->value();
    const auto aligns = trimAligns(editor_->aligns());
    const std::string id = editId_;
    const Point2 at = editAt_, box = editBox_;
    closeEditor();
    const bool blank = body.find_first_not_of(" \t\r\n") == std::string::npos;
    auto& s = h_.session();
    if (id.empty()) {  // a new text: an empty one is not created
        if (blank) return;
        api::Json def{{"position", {at.x(), at.y()}}, {"text", body}, {"height", drawing::SHEET_TEXT_DEFAULT_MM}};
        if (!styles.empty()) def["styles"] = stylesJson(styles);
        if (!aligns.empty()) def["aligns"] = alignsJson(aligns);
        if (box.x() > 0) def["boxWidth"] = box.x(), def["boxHeight"] = box.y();  // stretched frame (FX9-DRW-007)
        auto r = s.addSheetText(h_.currentSheet(), def);
        if (!r) h_.toast(errorText(r.error()), true);
        else select(*r);
        return;
    }
    const auto d = document();
    const doc::SheetText* t = d ? textById(*d, id) : nullptr;
    if (!t || (t->text == body && t->styles == styles && trimAligns(t->aligns) == aligns)) return;
    // A text emptied in place is deleted (an empty text is not allowed, document.md §2.4г).
    auto st = blank ? s.deleteSheetText(id)
                    : s.updateSheetText(id, api::Json{{"text", body}, {"styles", stylesJson(styles)}, {"aligns", alignsJson(aligns)}});
    if (!st) h_.toast(errorText(st.error()), true);
}

void SheetTextTool::applyField(const api::Json& def) {
    if (selected_.empty()) return;
    if (auto s = h_.session().updateSheetText(selected_, def); !s) {
        h_.toast(errorText(s.error()), true);
        refreshBar();
    }
}

bool SheetTextTool::eventFilter(QObject* o, QEvent* e) {
    // Ctrl+Enter and Esc belong to the text fields, not to the window shortcuts (they would take the key press).
    QObject* editor = editor_ ? editor_->edit() : nullptr;
    if (e->type() == QEvent::ShortcutOverride && (o == editor || o == content_)) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool done = (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier);
        if (done || (o == editor && k->key() == Qt::Key_Escape)) {
            e->accept();
            return true;
        }
    }
    if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool done = (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier);
        if (o == editor) {
            if (done) {
                commitEditor();
                return true;
            }
            if (k->key() == Qt::Key_Escape) {
                closeEditor();
                return true;
            }
        } else if (o == content_ && done) {
            content_->clearFocus();  // focus out applies (below)
            return true;
        }
    } else if (e->type() == QEvent::FocusOut) {
        // A popup (the symbols menu of the field, FX14-UI-001; the context menu) takes the focus for a while and gives
        // it back: the edit goes on.
        if (static_cast<QFocusEvent*>(e)->reason() == Qt::PopupFocusReason) return QObject::eventFilter(o, e);
        if (o == editor) {
            commitEditor();
        } else if (o == content_) {
            const auto [body, styles] = contentField_->value();
            const auto aligns = trimAligns(contentField_->aligns());
            const auto d = document();
            const doc::SheetText* t = d ? textById(*d, selected_) : nullptr;
            if (!t || (t->text == body && t->styles == styles && trimAligns(t->aligns) == aligns)) return QObject::eventFilter(o, e);
            if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
                h_.toast(text("ui.sheetText.empty"), true);
                refreshBar();
            } else {
                applyField(api::Json{{"text", body}, {"styles", stylesJson(styles)}, {"aligns", alignsJson(aligns)}});
            }
        }
    }
    return QObject::eventFilter(o, e);
}

void SheetTextTool::refresh() {
    const auto d = document();
    if (!selected_.empty() && (!d || !textById(*d, selected_))) selected_.clear();
    refreshBar();
    view_->update();
}

bool SheetTextTool::escape() {
    if (editor_) {
        closeEditor();
        return true;
    }
    if (placing_) {
        setPlacing(false);
        return true;
    }
    if (!selected_.empty()) {
        select({});
        return true;
    }
    return false;
}

bool SheetTextTool::press(const Point2& at) {
    if (editor_) commitEditor();
    if (placing_) {  // a click — a text at the point; stretching — a text in a frame (FX9-DRW-007)
        placePressed_ = true;
        placeFrom_ = placeTo_ = at;
        return true;
    }
    if (const int c = cornerAt(at); c >= 0) {  // a corner of the selected text's frame (FX9-DRW-007)
        corner_ = c;
        cornerFrame_ = *frameOf(selected_);
        cornerTo_ = at;
        return true;
    }
    const std::string id = textAt(at);
    if (id.empty()) return false;
    select(id);
    pressed_ = true;
    dragging_ = false;
    pressAt_ = at;
    dragDelta_ = Point2(0, 0);
    return true;
}

void SheetTextTool::move(const Point2& at, bool leftDown) {
    if (!leftDown) return;
    if (placePressed_) {
        placeTo_ = at;
        view_->update();
        return;
    }
    if (corner_ >= 0) {
        cornerTo_ = at;
        view_->update();
        return;
    }
    if (!pressed_) return;
    if (!dragging_ && (at - pressAt_).norm() * view_->zoom() > DRAG_THRESHOLD_PX) dragging_ = true;
    if (!dragging_) return;
    dragDelta_ = at - pressAt_;
    view_->update();
}

void SheetTextTool::release(const Point2& at) {
    if (placePressed_) {
        placePressed_ = false;
        setPlacing(false);
        const Point2 d = at - placeFrom_;
        if (std::abs(d.x()) * view_->zoom() > DRAG_THRESHOLD_PX && std::abs(d.y()) * view_->zoom() > DRAG_THRESHOLD_PX) {
            // The frame: its top left corner and size; the first baseline is SHEET_TEXT_FRAME_TOP·h below its top.
            const Point2 tl(std::min(at.x(), placeFrom_.x()), std::max(at.y(), placeFrom_.y()));
            const double h = drawing::SHEET_TEXT_DEFAULT_MM;
            openEditor(tl - Point2(0, drawing::SHEET_TEXT_FRAME_TOP * h), {}, {}, {}, Point2(std::abs(d.x()), std::abs(d.y())));
        } else {
            openEditor(placeFrom_, {}, {}, {});
        }
        view_->update();
        return;
    }
    if (corner_ >= 0) {
        const auto f = draggedFrame();
        corner_ = -1;
        view_->update();
        const auto doc = document();
        const doc::SheetText* t = doc ? textById(*doc, selected_) : nullptr;
        if (!t) return;
        // Width and height of the frame; the first baseline — SHEET_TEXT_FRAME_TOP·h below the top left corner.
        const double w = (f[1] - f[0]).norm(), hgt = (f[3] - f[0]).norm();
        const double a = t->angle * PI / 180;
        const Point2 down(std::sin(a), -std::cos(a));
        const Point2 pos = f[0] + drawing::SHEET_TEXT_FRAME_TOP * t->height * down;
        applyField(api::Json{{"position", {pos.x(), pos.y()}}, {"boxWidth", w}, {"boxHeight", hgt}});
        return;
    }
    pressed_ = false;
    if (!dragging_) return;
    dragging_ = false;
    const Point2 d = dragDelta_;
    dragDelta_ = Point2(0, 0);
    view_->update();
    const auto doc = document();
    const doc::SheetText* t = doc ? textById(*doc, selected_) : nullptr;
    if (!t) return;
    const Point2 pos = Point2(t->position[0], t->position[1]) + d;
    applyField(api::Json{{"position", {pos.x(), pos.y()}}});
}

std::optional<std::array<Point2, 4>> SheetTextTool::frameOf(const std::string& id) const {
    const auto d = document();
    const doc::SheetText* t = d ? textById(*d, id) : nullptr;
    if (!t) return std::nullopt;
    size_t lines = 0;
    double right = 0;  // a text without a frame: the right edge of its glyphs in the text axes
    const double a = t->angle * PI / 180, c = std::cos(a), sn = std::sin(a);
    const Point2 o(t->position[0], t->position[1]);
    for (const auto& pr : view_->graphics().primitives) {
        if (pr.kind != drawing::PrimitiveKind::Text || pr.role != "text:" + id) continue;
        ++lines;
        if (t->boxWidth > 0) continue;
        if (auto ol = api::Session::textOutline(pr))
            for (const auto& cont : ol->contours)
                for (const auto& cv : cont)
                    for (const Point2& v : {cv.a, cv.b}) right = std::max(right, c * (v.x() - o.x()) + sn * (v.y() - o.y()));
    }
    doc::SheetText f = *t;
    if (f.boxWidth <= 0) f.boxWidth = std::max(right, t->height);
    return api::Session::sheetTextFrame(f, lines);
}

int SheetTextTool::cornerAt(const Point2& at) const {
    if (selected_.empty() || editor_) return -1;
    const auto f = frameOf(selected_);
    if (!f) return -1;
    const double r = safeDiv(TEXT_FRAME_HANDLE_PX, view_->zoom());
    for (int i = 0; i < 4; ++i)
        if (std::abs((*f)[size_t(i)].x() - at.x()) <= r && std::abs((*f)[size_t(i)].y() - at.y()) <= r) return i;
    return -1;
}

std::array<Point2, 4> SheetTextTool::draggedFrame() const {
    // In the frame axes: the opposite corner stays, the dragged one follows the cursor (width ≥ a character).
    const auto& f0 = cornerFrame_;
    const Point2 opp = frameLocal(f0, f0[size_t((corner_ + 2) % 4)]);
    const Point2 cur = frameLocal(f0, cornerTo_);
    const doc::SheetText* t = nullptr;
    const auto d = document();
    if (d) t = textById(*d, selected_);
    const double minW = t ? t->height : 1;
    double x0 = std::min(opp.x(), cur.x()), x1 = std::max(opp.x(), cur.x());
    double y0 = std::min(opp.y(), cur.y()), y1 = std::max(opp.y(), cur.y());
    if (x1 - x0 < minW) (cur.x() < opp.x() ? x0 : x1) = cur.x() < opp.x() ? x1 - minW : x0 + minW;
    if (y1 - y0 < minW) (cur.y() < opp.y() ? y0 : y1) = cur.y() < opp.y() ? y1 - minW : y0 + minW;
    const Point2 ux = (f0[1] - f0[0]).norm() > 1e-12 ? Point2((f0[1] - f0[0]).normalized()) : Point2(1, 0);
    const Point2 uy(-ux.y(), ux.x());
    auto at = [&](double x, double y) { return Point2(f0[0] + x * ux + y * uy); };
    return {at(x0, y1), at(x1, y1), at(x1, y0), at(x0, y0)};
}

bool SheetTextTool::key(int k) {
    if (editor_ || selected_.empty()) return false;
    if (k == Qt::Key_Delete) {
        const std::string id = selected_;
        select({});
        if (auto s = h_.session().deleteSheetText(id); !s) h_.toast(errorText(s.error()), true);
        return true;
    }
    return false;
}

void SheetTextTool::paint(QPainter& p) {
    auto quad = [&](const std::array<Point2, 4>& f, bool handles) {
        QPolygonF poly;
        for (const auto& c : f) poly << view_->sheetToWidget(c);
        p.setPen(QPen(COLOR_SELECTED.color, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(poly);
        if (!handles) return;
        p.setPen(QPen(COLOR_SELECTED.color, 1));
        p.setBrush(Qt::white);
        const double r = TEXT_FRAME_HANDLE_PX / 2.0;
        for (const QPointF& c : poly) p.drawRect(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r));
    };
    if (placePressed_ && (placeTo_ - placeFrom_).norm() > 0) {  // the frame being stretched (FX9-DRW-007)
        const Point2 a = placeFrom_, b = placeTo_;
        quad({Point2(std::min(a.x(), b.x()), std::max(a.y(), b.y())), Point2(std::max(a.x(), b.x()), std::max(a.y(), b.y())),
              Point2(std::max(a.x(), b.x()), std::min(a.y(), b.y())), Point2(std::min(a.x(), b.x()), std::min(a.y(), b.y()))},
             false);
    }
    if (selected_.empty()) return;
    for (const auto& pr : view_->graphics().primitives)
        if (pr.kind == drawing::PrimitiveKind::Text && pr.role == "text:" + selected_) fillText(p, *view_, pr, dragDelta_, COLOR_SELECTED.color);
    if (dragging_ || editor_) return;
    if (corner_ >= 0) {
        quad(draggedFrame(), true);
    } else if (const auto f = frameOf(selected_)) {
        quad(*f, true);
    }
}

void SheetTextTool::fillFonts() {
    const QSignalBlocker b(font_);
    font_->clear();
    font_->addItem(QString::fromUtf8(txt("ui.drawingText.asInDrawing")), QString());
    for (const auto& n : api::Session::fonts()) font_->addItem(QString::fromStdString(n), QString::fromStdString(n));
}

void SheetTextTool::refreshBar() {
    const auto d = document();
    const doc::SheetText* t = d ? textById(*d, selected_) : nullptr;
    bar_->setVisible(t != nullptr);
    if (!t) return;
    const QSignalBlocker b1(font_), b2(height_);
    if (!content_->hasFocus()) {
        contentField_->setValue(t->text, t->styles);
        contentField_->setAligns(t->aligns);
    }
    // The font list — the library's (shipped fonts and the user's folders, FX10-DRW-004), refreshed on each show; a font
    // not in the list (an older "gost" / "iso" choice, a missing file) is shown as its own item and not lost.
    fillFonts();
    int fi = font_->findData(QString::fromStdString(t->font));
    if (fi < 0) {
        font_->addItem(t->font == "gost" ? QString::fromUtf8(txt("ui.drawingText.gostTypeB")) : t->font == "iso" ? QString("ISO")
                                                                                                         : QString::fromStdString(t->font),
                       QString::fromStdString(t->font));
        fi = font_->count() - 1;
    }
    font_->setCurrentIndex(fi);
    if (!height_->lineEdit()->hasFocus()) {
        // The list item follows the value too: a stale item of the previous text would make Enter on the same size
        // apply nothing (the combo box sees no change).
        const QString h = numberText(t->height);
        height_->setCurrentIndex(height_->findText(h));
        height_->setEditText(h);
    }
    if (!angle_->hasFocus()) angle_->setText(numberText(t->angle));
}

}  // namespace construe::app
