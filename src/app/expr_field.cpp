// Expression field (app/expr_field.h).
#include "app/expr_field.h"

#include "app/expr_input.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSet>

namespace construe::app {

namespace {
QSet<const ExprField*>& fields() {
    static QSet<const ExprField*> all;
    return all;
}
}  // namespace

ExprField::ExprField(QWidget* parent) : QLineEdit(parent), value_(new QLabel(this)) {
    fields().insert(this);
    value_->setObjectName("exprValue");
    value_->setAttribute(Qt::WA_TransparentForMouseEvents);
    value_->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(COLOR_DISABLED_TEXT.color.name()));
    value_->hide();
    typed_.setSingleShot(true);
    typed_.setInterval(EXPR_CHECK_DEBOUNCE_MS);
    set_.setSingleShot(true);
    set_.setInterval(0);
    connect(&typed_, &QTimer::timeout, this, [this] { check(); });
    connect(&set_, &QTimer::timeout, this, [this] { check(); });
    // textChanged comes first, textEdited right after it for a typed change: a typed change waits for the debounce.
    connect(this, &QLineEdit::textChanged, this, [this] {
        if (session_ && !typed_.isActive()) set_.start();
    });
    connect(this, &QLineEdit::textEdited, this, [this] {
        set_.stop();
        if (session_) typed_.start();
    });
}

ExprField::~ExprField() { fields().remove(this); }

void ExprField::setChecker(const api::Session* session, std::optional<expr::Dim> dim) {
    session_ = session;
    dim_ = dim;
    check();
}

void ExprField::setDim(std::optional<expr::Dim> dim) {
    if (dim_ == dim) return;
    dim_ = dim;
    check();
}

std::string ExprField::expression() const { return normalizeExprInput(text()); }

bool ExprField::check() {
    typed_.stop();
    set_.stop();
    if (!session_) {  // the owner marks errors (markError): the frame is left as it is
        valid_ = true;
        emit checked(true);
        return true;
    }
    const std::string e = expression();
    if (allowEmpty_ && e.empty()) {
        valid_ = true;
        setStyleSheet({});
        setToolTip({});
        showValue({});
        emit checked(true);
        return true;
    }
    auto q = session_->checkExpression(e, dim_);
    valid_ = bool(q);
    if (q) {
        setStyleSheet({});
        setToolTip({});
        showValue(isPlainNumber(e) ? QString() : "= " + formatNumber(q->valueInDocUnits));
    } else {
        showValue({});
        setStyleSheet(QString("QLineEdit { border: 1px solid %1; }").arg(COLOR_ERROR.color.name()));
        setToolTip(errorText(q.error()));
    }
    emit checked(valid_);
    return valid_;
}

void ExprField::refresh() {
    if (!checkPending()) check();
}

void ExprField::setErrorText(const QString& text) {
    valid_ = false;
    showValue({});
    setStyleSheet(QString("QLineEdit { border: 1px solid %1; }").arg(COLOR_ERROR.color.name()));
    setToolTip(text);
}

bool ExprField::anyCheckPending() {
    for (const ExprField* f : fields())
        if (f->checkPending()) return true;
    return false;
}

std::vector<QTimer*> ExprField::pendingCheckTimersForTest() {
    std::vector<QTimer*> v;
    for (const ExprField* f : fields())
        for (const QTimer* t : {&f->typed_, &f->set_})
            if (t->isActive()) v.push_back(const_cast<QTimer*>(t));
    return v;
}

void ExprField::keyPressEvent(QKeyEvent* e) {
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && session_ && !check()) {
        e->accept();  // an invalid expression: Enter does not accept, the field keeps the focus (§5.6)
        emit enterRejected(toolTip());
        return;
    }
    if (e->key() == Qt::Key_Escape && escapeReverts_ && text() != focusText_) {
        setText(focusText_);  // Esc returns the text before editing; the next Esc goes up (§5.6, §6.13)
        selectAll();
        e->accept();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

void ExprField::focusInEvent(QFocusEvent* e) {
    QLineEdit::focusInEvent(e);
    focusText_ = text();
    // All text selected on getting the focus (§5.6); by a click — on the release, unless the click dragged a selection.
    if (e->reason() == Qt::MouseFocusReason) selectOnRelease_ = true;
    else selectAll();
}

void ExprField::mouseReleaseEvent(QMouseEvent* e) {
    QLineEdit::mouseReleaseEvent(e);
    if (selectOnRelease_ && !hasSelectedText()) selectAll();
    selectOnRelease_ = false;
}

void ExprField::resizeEvent(QResizeEvent* e) {
    QLineEdit::resizeEvent(e);
    layoutValue();
}

void ExprField::showValue(const QString& text) {
    value_->setText(text);
    value_->setVisible(!text.isEmpty());
    layoutValue();
}

void ExprField::layoutValue() {
    if (value_->isHidden()) {
        setTextMargins(0, 0, 0, 0);
        return;
    }
    value_->adjustSize();
    const int w = value_->sizeHint().width();
    value_->setGeometry(width() - w - 4, 0, w, height());
    setTextMargins(0, 0, w + 4, 0);
}

}  // namespace construe::app
