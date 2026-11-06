// In-place dimension value input field (app-ux §6.9): opens below the dimension label; Enter — validate and
// write the value (an invalid expression is not accepted, the field stays open with an error frame), Esc — close
// without changes. Next to the value — the tolerance field (FX12-PMI-004, app-ux §6.9): the tolerance is part of the
// dimension; text as in the label («h6», «H7(+0,015)», «±0,1», «+0,1/−0,05», pmi.md §5.4), empty — no tolerance; Tab
// from the value goes to it, Enter in either field accepts both in one undo step.
#pragma once

#include "api/session.h"
#include "app/expr_field.h"

#include <QLineEdit>
#include <QObject>
#include <QPointer>
#include <QRectF>

#include <array>
#include <optional>
#include <string>

namespace construe::app {

class InlineDimensionEdit : public QObject {
    Q_OBJECT
public:
    InlineDimensionEdit(api::Session& session, QWidget& host, QObject* parent = nullptr);

    // Open the field for dimension constraintId: original — the stored expression, angle — angular dimension,
    // under — the label rectangle (the field is centered below it).
    // weakLabel — weak dimension (app-ux §6.6а, S4-WEAK-002): Enter makes it regular (sketchStrengthen) with the input
    // value and this label position, even if the value was not changed.
    void open(const std::string& constraintId, const std::string& original, bool angle, const QRectF& under,
              const std::optional<std::array<double, 2>>& weakLabel = std::nullopt);
    // Close: apply — accept the input. An invalid expression is not accepted: the field stays open, false.
    bool finish(bool apply);
    bool isOpen() const { return edit_ != nullptr; }
    ExprField* editor() const { return edit_; }
    QLineEdit* toleranceEditor() const { return tol_; }  // nullptr — the dimension admits no tolerance

signals:
    void applied();                     // value written to the document
    void hint(const QString& text);     // expression error (status bar)
    void error(const QString& text);    // command error

protected:
    bool eventFilter(QObject* o, QEvent* ev) override;

private:
    api::Session& session_;
    QWidget& host_;
    QPointer<ExprField> edit_;
    std::string constraint_;
    std::string original_;
    QString shownText_;  // displayed text (absolute value, FX1-SKG-003)
    bool angle_ = false;
    bool weak_ = false;
    std::string offsetGroup_;  // dimension of an `offset` group: the field edits the group distance (S7F-SK-005)
    std::optional<std::array<double, 2>> weakLabel_;
    QPointer<QLineEdit> tol_;               // tolerance field (FX12-PMI-004)
    QString tolShown_;                      // its initial text
    std::string sketch_;                    // sketch feature of the dimension
    std::optional<std::string> tolExisting_;  // ID of the dimension's `fit` annotation
    void closeFields();
    void applyValue(const std::string& value, double entered);  // the dimension value (or the offset group distance)
};

}  // namespace construe::app
