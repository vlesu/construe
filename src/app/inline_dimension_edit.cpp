// In-place dimension value input field (app/inline_dimension_edit.h).
#include "app/inline_dimension_edit.h"

#include "api/sketch_ops.h"
#include "app/messages.h"
#include "app/theme.h"
#include "app/tolerance_edit.h"
#include "pmi/pmi.h"

#include <QEvent>
#include <QKeyEvent>

namespace construe::app {

InlineDimensionEdit::InlineDimensionEdit(api::Session& session, QWidget& host, QObject* parent)
    : QObject(parent), session_(session), host_(host) {}

void InlineDimensionEdit::open(const std::string& cid, const std::string& original, bool angle, const QRectF& under,
                               const std::optional<std::array<double, 2>>& weakLabel) {
    finish(false);
    weak_ = sketch::isWeakId(cid);
    weakLabel_ = weakLabel;
    constraint_ = cid;
    original_ = original;
    angle_ = angle;
    // A dimension of an offset contour (sketch.md §2.6.5, S7F-SK-005): the field shows and changes the signed distance
    // of the whole group.
    offsetGroup_.clear();
    QString shownOverride;
    {
        const auto docPtr = session_.document().document;
        const auto ctx = session_.editContext().sketch;
        const auto f = ctx && !weak_ ? docPtr->active().find(*ctx) : nullptr;
        const auto* g = f && f->sketchData() ? f->sketchData()->groupOfMember(cid) : nullptr;
        if (g && g->type == "offset")
            if (const auto it = g->params.find("distance"); it != g->params.end()) {
                offsetGroup_ = g->id;
                original_ = it->second.text;
                shownOverride = displayExpr(original_, docPtr->lengthUnit);
            }
    }
    auto* e = new ExprField(&host_);
    e->setObjectName("dimensionField");
    e->setEscapeReverts(false);  // Esc closes the field (§5.6, §6.9)
    e->setChecker(&session_, angle ? expr::ANGLE : expr::LENGTH);
    e->setFixedWidth(DIM_FIELD_W_PX);
    {
        // The value is shown as positive (app-ux §6.6, FX1-SKG-003): a negative one is shown without the minus.
        const auto docPtr = session_.document().document;
        std::string shown = original_;
        if (auto q = session_.checkExpression(original_, angle ? expr::ANGLE : expr::LENGTH); q && q->value < 0) {
            const QString t = QString::fromStdString(original_).trimmed();
            shown = (t.startsWith('-') ? t.mid(1).trimmed() : "-(" + t + ")").toStdString();
        }
        shownText_ = offsetGroup_.empty() ? displayExpr(shown, angle ? docPtr->angleUnit : docPtr->lengthUnit) : shownOverride;
        e->setText(shownText_);
    }
    e->move(int(under.center().x() - DIM_FIELD_W_PX / 2.0), int(under.bottom() + 2));
    e->show();
    // Tolerance field (FX12-PMI-004): a regular dimension of a type admitting a tolerance (pmi.md §2.2).
    tolExisting_.reset();
    sketch_.clear();
    tolShown_.clear();
    if (!weak_ && offsetGroup_.empty()) {
        const auto docPtr = session_.document().document;
        const auto ctx = session_.editContext().sketch;
        const auto f = ctx ? docPtr->active().find(*ctx) : nullptr;
        const auto* c = f && f->sketchData() ? f->sketchData()->findConstraint(cid) : nullptr;
        if (c && pmi::toleranceDimension(c->type)) {
            sketch_ = *ctx;
            for (const auto& a : docPtr->active().annotations)
                if (a.value("type", std::string()) == "fit" && a.contains("dimension") && a["dimension"].value("sketch", std::string()) == sketch_ &&
                    a["dimension"].value("constraint", std::string()) == cid) {
                    tolExisting_ = a["id"].get<std::string>();
                    tolShown_ = QString::fromStdString(pmi::toleranceSuffix(a, std::nullopt)).trimmed();
                }
            auto* t = new ToleranceEdit(&host_);  // editable list of standard fields (FX13-TOL-001)
            t->setObjectName("dimensionTolerance");
            t->setPlaceholderText(txt("ui.sketch.tolerancePlaceholder"));
            t->setFixedWidth(DIM_TOL_FIELD_W_PX);
            t->setText(tolShown_);
            t->move(e->x() + DIM_FIELD_W_PX + 2, e->y());
            t->show();
            t->installEventFilter(this);
            connect(t, &QLineEdit::returnPressed, this, [this] { finish(true); });
            connect(t, &QLineEdit::textEdited, t, [t] { t->setStyleSheet({}); });
            tol_ = t;
        }
    }
    e->setFocus();
    e->selectAll();
    edit_ = e;
    connect(e, &QLineEdit::returnPressed, this, [this] { finish(true); });
    connect(e, &ExprField::enterRejected, this, &InlineDimensionEdit::hint);
    e->installEventFilter(this);
}

void InlineDimensionEdit::closeFields() {
    if (edit_) {
        edit_->hide();
        edit_->deleteLater();
    }
    if (tol_) {
        tol_->hide();
        tol_->deleteLater();
    }
    edit_ = nullptr;
    tol_ = nullptr;
}

bool InlineDimensionEdit::finish(bool apply) {
    if (!edit_) return true;
    ExprField* e = edit_;
    // Tolerance (FX12-PMI-004): an invalid record keeps both fields open with an error frame on the tolerance.
    bool tolChanged = false;
    std::optional<doc::Json> tolNew;
    if (apply && tol_ && tol_->text().trimmed() != tolShown_) {
        auto r = api::Session::parseTolerance(tol_->text().toStdString());
        if (!r) {
            tol_->setStyleSheet(QString("QLineEdit { border: 1px solid %1; }").arg(COLOR_ERROR.color.name()));
            tol_->setFocus();
            emit hint(errorText(r.error()));
            return false;
        }
        tolChanged = true;
        tolNew = *r;
    }
    // Decimal comma, leading "=", a number without a unit — in document units, stored as is (§5.6).
    std::string value = e->expression();
    double entered = 0;
    if (apply && !value.empty()) {
        // Invalid expression: Enter does not accept it, the field stays open with an error frame (§5.6).
        auto check = session_.checkExpression(value, angle_ ? expr::ANGLE : expr::LENGTH);
        if (!check) {
            e->check();
            emit hint(errorText(check.error()));
            return false;
        }
        entered = check->value;
    }
    const QString enteredText = e->text().trimmed();
    closeFields();
    host_.setFocus();
    if (apply && weak_ && !value.empty()) {
        // Weak dimension → regular (§6.6а, S4-WEAK-002); value unchanged — use the current one (measured, signed).
        std::optional<Point2> label;
        if (weakLabel_) label = Point2((*weakLabel_)[0], (*weakLabel_)[1]);
        const bool same = enteredText == shownText_;
        auto r = session_.sketchStrengthen(constraint_, same ? std::nullopt : std::optional<std::string>(value), label);
        if (!r) emit error(errorText(r.error(), session_.document().document.get()));
        emit applied();
        return true;
    }
    const bool valueChanged = apply && !value.empty() && enteredText != shownText_;
    if (!valueChanged && !tolChanged) return true;
    // Value and tolerance — one undo step (an offset group keeps its own label).
    session_.beginGroup(offsetGroup_.empty() ? "undo.group.dimensionValue" : "undo.group.offset");
    if (valueChanged) applyValue(value, entered);
    if (tolChanged) {
        Status r = okStatus();
        if (!tolNew) {
            if (tolExisting_) r = session_.deleteAnnotation(*tolExisting_);
        } else {
            doc::Json def = {{"type", "fit"}, {"dimension", {{"sketch", sketch_}, {"constraint", constraint_}}}};
            for (auto it = tolNew->begin(); it != tolNew->end(); ++it) def[it.key()] = it.value();
            if (tolExisting_) r = session_.updateAnnotation(*tolExisting_, def);
            else if (auto a = session_.addAnnotation(def); !a) r = a.error();
        }
        if (!r) emit error(errorText(r.error(), session_.document().document.get()));
    }
    session_.endGroup();
    emit applied();
    return true;
}

void InlineDimensionEdit::applyValue(const std::string& value, double entered) {
    if (!offsetGroup_.empty()) {  // the whole offset contour: one signed value (S7F-SK-005)
        const auto docPtr = session_.document().document;
        const auto ctx = session_.editContext().sketch;
        const auto f = ctx ? docPtr->active().find(*ctx) : nullptr;
        const auto* g = f && f->sketchData() ? f->sketchData()->findGroup(offsetGroup_) : nullptr;
        if (g) {
            const std::vector<std::string> sources = g->sources;
            session_.beginGroup("undo.group.offset");
            auto r = session_.sketchGroupUpdate(offsetGroup_, sources, doc::Json{{"distance", value}});
            session_.endGroup();
            if (!r) emit error(r.error().code == "API_INVALID_ARGUMENT" ? text("ui.offset.impossible") : errorText(r.error(), docPtr.get()));
        }
        return;
    }
    // Negative value — geometry flips to the other side, the absolute value is stored (app-ux §6.9, FX1-SKG-003).
    doc::Json changes = doc::Json::array({{{"id", constraint_}, {"value", value}}});
    {
        const auto docPtr = session_.document().document;
        const auto ctx = session_.editContext().sketch;
        const auto f = ctx ? docPtr->active().find(*ctx) : nullptr;
        const auto* c = f && f->sketchData() ? f->sketchData()->findConstraint(constraint_) : nullptr;
        if (c) {
            double current = 1;
            if (auto q = session_.checkExpression(original_, angle_ ? expr::ANGLE : expr::LENGTH)) current = q->value;
            changes = api::sketchops::dimensionValueChanges(*c, current, value, entered);
        }
    }
    auto r = session_.sketchUpdate(changes);
    if (!r) emit error(errorText(r.error(), session_.document().document.get()));
}

bool InlineDimensionEdit::eventFilter(QObject* o, QEvent* ev) {
    if ((o == edit_ || (tol_ && o == tol_)) && ev->type() == QEvent::KeyPress) {
        const int k = static_cast<QKeyEvent*>(ev)->key();
        if (k == Qt::Key_Escape) {
            finish(false);
            return true;
        }
        // Tab / Shift+Tab — between the value and the tolerance (FX12-PMI-004).
        if ((k == Qt::Key_Tab || k == Qt::Key_Backtab) && tol_) {
            QLineEdit* next = o == edit_ ? static_cast<QLineEdit*>(tol_) : static_cast<QLineEdit*>(edit_);
            next->setFocus();
            next->selectAll();
            return true;
        }
    }
    return QObject::eventFilter(o, ev);
}

}  // namespace construe::app
