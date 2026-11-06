// Feature dimension field (spec/app-ux.md §5.6а, FX12-PMI-007): the one input of every dimension of a feature panel —
// the value (an expression field, or another value widget such as the hole size list) and, right of it, the optional
// tolerance field. The tolerance is written as in the sketch dimension field (FX12-PMI-004, pmi.md §5.4: «h6»,
// «H7(+0,015)», «±0,1», «+0,1/−0,05»); empty — no tolerance. It is stored in the feature parameters (document.md §2.6,
// `tolerances`) under the parameter path `param`.
#pragma once

#include "app/expr_field.h"
#include "doc/document.h"

#include <QLineEdit>
#include <QWidget>

#include <optional>
#include <string>

namespace construe::app {

class DimensionField : public QWidget {
    Q_OBJECT
public:
    // value — the value widget (reparented into this field); param — the parameter path in the definition
    // ("end.depth", "radius", "directions.0.spacing", …; document.md §2.6).
    DimensionField(const std::string& param, QWidget* value, QWidget* parent = nullptr);

    const std::string& param() const { return param_; }
    void setParam(const std::string& param) { param_ = param; }
    QWidget* valueWidget() const { return value_; }
    ExprField* expr() const { return qobject_cast<ExprField*>(value_); }
    QLineEdit* toleranceEdit() const { return tol_; }

    // Show a stored tolerance (nullptr — none) in the record of pmi.toleranceSuffix without deviations (§5.6а).
    void setTolerance(const doc::Json* tolerance);
    // Parsed tolerance of the field (pmi.parseToleranceText): nullopt — empty field; an invalid record — nullopt and
    // valid() == false.
    std::optional<doc::Json> tolerance() const;
    bool toleranceValid() const { return valid_; }
    QString toleranceError() const { return error_; }

    // The widget is a dimension field of a feature panel: value is wrapped (the rule of the panel catalog test, §5.6а).
    static DimensionField* of(const QWidget* value);

signals:
    void toleranceEdited();  // the text changed (the panel re-checks OK)
    void acceptRequested();  // Enter in the tolerance field

private:
    void check();
    std::string param_;
    QWidget* value_;
    QLineEdit* tol_;
    bool valid_ = true;
    QString error_;
};

// Mark an expression field of a feature panel as not a dimension (a count, the K factor, …; app-ux §5.6а): the panel
// catalog test accepts it without a tolerance field.
void markNotDimension(QWidget* field);
bool isNotDimension(const QWidget* field);

}  // namespace construe::app
