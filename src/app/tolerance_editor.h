// Dimension tolerance editor (spec/app-ux.md §5.12, §6.6; pmi.md §2.2, S3-PMI-002): ISO 286 tolerance class or explicit
// deviations with a label preview; the tolerance box of a sketch dimension at the dimension label.
#pragma once

#include "app/expr_input.h"  // parseDecimal
#include "app/popup_box.h"
#include "doc/json_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

#include <optional>

namespace construe::app {

class ToleranceEditor : public QWidget {
    Q_OBJECT
public:
    explicit ToleranceEditor(QWidget* parent = nullptr);
    // fit: fit designation (field, showDeviations, upper, lower are used) or an empty object; sizeAllowed: tolerance
    // class field available (feature of size); nominal: the nominal for the preview; label: nominal label text («⌀10»).
    void load(const doc::Json& fit, bool sizeAllowed, std::optional<double> nominal, const QString& label);
    void setNominal(std::optional<double> nominal, const QString& label);
    // Insert the default class (H7 for a hole, h6 for a shaft) if the user has not changed it yet.
    void suggestField(bool hole);
    // Tolerance fields (field and showDeviations, or upper and lower); nullopt: invalid input, error gets the text.
    std::optional<doc::Json> tolerance(QString* error) const;

signals:
    void changed();

private:
    void syncKind();
    void updatePreview();
    QComboBox *kind_, *field_;
    QCheckBox* show_;
    QLineEdit *upper_, *lower_;
    QLabel* preview_;
    QWidget *fieldRow_, *devRow_;
    std::optional<double> nominal_;
    QString label_;
    bool fieldTouched_ = false;
};

// Sketch dimension tolerance box (§6.6): editor, «OK», «Remove», «Cancel»; non-modal, over the view.
class ToleranceBox : public PopupBox {
    Q_OBJECT
public:
    explicit ToleranceBox(QWidget* parent);
    void open(const QPoint& at, const doc::Json& fit, bool exists, bool sizeAllowed, std::optional<double> nominal, const QString& label);
    ToleranceEditor* editor() const { return editor_; }

signals:
    void accepted(const doc::Json& tolerance);
    void removed();

protected:
    void acceptBox() override { accept(); }
    void cancelBox() override { hide(); }

private:
    void accept();
    ToleranceEditor* editor_;
    QPushButton *ok_, *remove_, *cancel_;
    QLabel* error_;
};


}  // namespace construe::app
