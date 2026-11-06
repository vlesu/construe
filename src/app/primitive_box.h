// Primitive box (spec/app-ux.md §6.3 «Primitive box», S4-SKT-003): side count and kind of an N-gon,
// or ray count and diameters of a star; edits parameters of the linked polygon / star group (sketch.md §2.6.4).
#pragma once

#include "app/expr_field.h"
#include "app/popup_box.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include <string>

namespace construe::app {

class PrimitiveBox : public PopupBox {
    Q_OBJECT
public:
    struct Values {
        bool star = false;
        int count = 6;            // sides / rays
        bool inscribed = true;    // N-gon
        QString d1, d2;           // star: expressions
    };
    explicit PrimitiveBox(QWidget* parent);
    void open(const QPoint& at, const Values& v);
    Values values() const;
    // Highlight the error field by parameter name (sides, rays, d1, d2); empty clears it.
    void markError(const std::string& name);

signals:
    void accepted();
    void cancelled();
    void unlinkRequested();

protected:
    void acceptBox() override { emit accepted(); }
    void cancelBox() override { emit cancelled(); }

private:
    Values values_;
    QLabel* title_;
    QFormLayout* form_;
    QSpinBox* count_;
    QComboBox* kind_;
    ExprField *d1_, *d2_;
};

}  // namespace construe::app
