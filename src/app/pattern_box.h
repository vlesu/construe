// Sketch pattern box (spec/app-ux.md §6.3 «Sketch pattern», S4-SKT-001, S4-SKT-002, S4-SKT-005):
// count, step and axis direction of a linear pattern (the direction may be measured from a sketch segment, S4-SKT-006),
// or count and angle of a circular one; all fields are expressions with document variables. The sketcher draws the
// preview from values().
#pragma once

#include "app/expr_field.h"
#include "app/popup_box.h"
#include "doc/json_text.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <string>

namespace construe::app {

class PatternBox : public PopupBox {
    Q_OBJECT
public:
    // Fields are expression texts in document units (a number without a unit is in document units).
    struct Values {
        bool circular = false;
        QString count1 = "3", count2 = "1";
        QString step1, angle1 = "0", step2, angle2 = "90";
        std::string line;               // linear: axis segment ID (directions are measured from it), empty — none
        std::string line2;              // linear: segment of the second axis (FX12-FEAT-001), empty — as the first
        std::string center = "origin";  // circular: point ID or origin
        QString count = "6";
        QString angle = "360";
    };
    explicit PatternBox(QWidget* parent);
    // Open at point at (parent coordinates); editing: edit an existing pattern («Unlink» is visible).
    void open(const QPoint& at, const Values& v, bool editing);
    Values values() const;
    // Parameters of the linearPattern / circularPattern group (sketch.md §2.6.3).
    doc::Json params() const;
    std::string type() const { return values_.circular ? "circularPattern" : "linearPattern"; }
    // Highlight the error field by parameter name (name of the API_INVALID_ARGUMENT error); empty clears the highlight.
    void markError(const std::string& name);
    // Axis segment picked in the view (S4-SKT-006); empty — none. Ends the pick mode.
    void setLine(const std::string& line);
    bool pickingLine() const;
    // The same for the second axis («По отрезку» of «Направление 2», FX12-FEAT-001).
    void setLine2(const std::string& line);
    bool pickingLine2() const;

signals:
    void changed();
    void accepted();
    void cancelled();
    void unlinkRequested();
    void linePickRequested(bool on);  // «By line» pressed / released

protected:
    void acceptBox() override { emit accepted(); }
    void cancelBox() override { emit cancelled(); }

private:
    Values values_;
    ExprField *count1_, *count2_, *count_;
    ExprField *step1_, *angle1_, *step2_, *angle2_, *angle_;
    QPushButton *ok_, *cancel_, *unlink_, *line_, *line2_;
    void updateLineButton();
    QLabel* title_;
    QFormLayout* form_;
};

}  // namespace construe::app
