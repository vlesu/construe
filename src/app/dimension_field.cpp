#include "app/dimension_field.h"

#include "api/session.h"
#include "app/messages.h"
#include "app/theme.h"
#include "app/tolerance_edit.h"
#include "pmi/pmi.h"

#include <QHBoxLayout>

namespace construe::app {

DimensionField::DimensionField(const std::string& param, QWidget* value, QWidget* parent)
    : QWidget(parent), param_(param), value_(value) {
    setObjectName("dimensionField");
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(2);
    l->addWidget(value, 1);
    tol_ = new ToleranceEdit;  // editable list of standard fields (FX13-TOL-001)
    tol_->setObjectName("dimensionTolerance");
    tol_->setPlaceholderText(txt("ui.sketch.tolerancePlaceholder"));
    tol_->setToolTip(txt("ui.feature.toleranceTip"));
    tol_->setFixedWidth(DIM_TOL_FIELD_W_PX);
    l->addWidget(tol_);
    setFocusProxy(value);
    connect(tol_, &QLineEdit::textChanged, this, [this] {
        check();
        emit toleranceEdited();
    });
    connect(tol_, &QLineEdit::returnPressed, this, [this] {
        if (valid_) emit acceptRequested();
    });
}

void DimensionField::setTolerance(const doc::Json* tolerance) {
    const QString t = tolerance ? QString::fromStdString(pmi::toleranceSuffix(*tolerance, std::nullopt)).trimmed() : QString();
    const QSignalBlocker b(tol_);
    tol_->setText(t);
    check();
}

std::optional<doc::Json> DimensionField::tolerance() const {
    auto r = api::Session::parseTolerance(tol_->text().toStdString());
    if (!r) return std::nullopt;
    return *r;
}

void DimensionField::check() {
    auto r = api::Session::parseTolerance(tol_->text().toStdString());
    valid_ = bool(r);
    error_ = valid_ ? QString() : errorText(r.error());
    tol_->setStyleSheet(valid_ ? QString() : QString("QLineEdit { border: 1px solid %1; }").arg(COLOR_ERROR.color.name()));
    tol_->setToolTip(valid_ ? txt("ui.feature.toleranceTip") : error_);
}

DimensionField* DimensionField::of(const QWidget* value) {
    return value ? qobject_cast<DimensionField*>(value->parentWidget()) : nullptr;
}

void markNotDimension(QWidget* field) { field->setProperty("construeNotDimension", true); }
bool isNotDimension(const QWidget* field) { return field && field->property("construeNotDimension").toBool(); }

}  // namespace construe::app
