// Expression field (spec/app-ux.md §5.6, S7F-RF-006): the one input of every expression field — panels, popups at the
// cursor, the sketch dimension value field.
#pragma once

#include "api/session.h"
#include "expr/expr.h"

#include <QLabel>
#include <QLineEdit>
#include <QTimer>

#include <vector>

#include <optional>
#include <string>

namespace construe::app {

class ExprField : public QLineEdit {
    Q_OBJECT
public:
    explicit ExprField(QWidget* parent = nullptr);
    ~ExprField() override;

    // Check against the document with the expected dimension (nullopt — any); session nullptr — no check (the caller
    // reports errors, e.g. popups at the cursor).
    void setChecker(const api::Session* session, std::optional<expr::Dim> dim);
    void setDim(std::optional<expr::Dim> dim);
    // An empty field is valid (optional fields): no error frame.
    void setAllowEmpty(bool allow) { allowEmpty_ = allow; }
    // false — Esc is left to the owner (popups, the dimension field close at once, §5.6).
    void setEscapeReverts(bool on) { escapeReverts_ = on; }
    bool escapeReverts() const { return escapeReverts_; }

    // Normalized text as stored in the document (normalizeExprInput).
    std::string expression() const;
    // Check now, cancelling a pending typed check; true — valid (or no checker).
    bool check();
    // Check now unless a typed check is pending (panels call it when anything else changed).
    void refresh();
    bool valid() const { return valid_; }
    bool checkPending() const { return typed_.isActive() || set_.isActive(); }
    // Text changed since the field got the focus (Esc reverts it, §5.6).
    bool changedSinceFocus() const { return hasFocus() && text() != focusText_; }
    // Computed value shown at the right edge ("= 140"); empty — hidden.
    QString valueText() const { return !value_->isHidden() ? value_->text() : QString(); }
    // Error from the caller (e.g. a rejected command): frame and tooltip until the next check.
    void setErrorText(const QString& text);

    // Any expression field waits for its check (the idle signal, §14).
    static bool anyCheckPending();
    // Tests (§12.1, FX14-CI-002): the active check timers of all fields.
    static std::vector<QTimer*> pendingCheckTimersForTest();

signals:
    void checked(bool ok);
    // Enter with an invalid expression (not accepted, §5.6): the error text for the status bar.
    void enterRejected(const QString& error);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void showValue(const QString& text);
    void layoutValue();

    const api::Session* session_ = nullptr;
    std::optional<expr::Dim> dim_;
    bool allowEmpty_ = false;
    bool escapeReverts_ = true;
    bool valid_ = true;
    bool selectOnRelease_ = false;
    QString focusText_;
    QLabel* value_;
    QTimer typed_;  // EXPR_CHECK_DEBOUNCE_MS after the last key
    QTimer set_;    // 0 ms: a text set by the program (cancelled when the change was typed)
};

}  // namespace construe::app
