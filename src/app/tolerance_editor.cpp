#include "app/tolerance_editor.h"

#include "api/session.h"
#include "app/dialog_buttons.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QCompleter>
#include <QFormLayout>
#include <QStandardItemModel>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {

QString mmText(double v) { return QString::fromStdString(pmi::formatNumber(v, pmi::DEV_DECIMALS)); }

}  // namespace

ToleranceEditor::ToleranceEditor(QWidget* parent) : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    kind_ = new QComboBox;
    kind_->setObjectName("tolKind");
    kind_->addItem(txt("ui.common.toleranceField"), "field");
    kind_->addItem(txt("ui.common.deviations"), "deviations");
    form->addRow(txt("ui.common.tolerance"), kind_);
    fieldRow_ = new QWidget;
    auto* fl = new QHBoxLayout(fieldRow_);
    fl->setContentsMargins(0, 0, 0, 0);
    field_ = new QComboBox;
    field_->setObjectName("tolField");
    field_->setEditable(true);
    field_->completer()->setCaseSensitivity(Qt::CaseSensitive);  // h11 ≠ H11 (shaft vs. hole)
    for (const char* f : {"H6", "H7", "H8", "H9", "H11", "JS7", "K7", "M7", "N7", "P7", "F7", "F8", "E9", "D10", "h6", "h7", "h8", "h9",
                          "h11", "js6", "k6", "m6", "n6", "p6", "r6", "s6", "f7", "g6", "e8", "d9"})
        field_->addItem(f);
    field_->setToolTip(txt("ui.tolerance.iso286ToleranceFieldFundamental"));
    fl->addWidget(field_, 1);
    show_ = new QCheckBox(txt("ui.tolerance.showDeviations"));
    show_->setObjectName("tolShowDeviations");
    fl->addWidget(show_);
    form->addRow(txt("ui.tolerance.field"), fieldRow_);
    devRow_ = new QWidget;
    auto* dl = new QFormLayout(devRow_);
    dl->setContentsMargins(0, 0, 0, 0);
    upper_ = new QLineEdit("0");
    upper_->setObjectName("tolUpper");
    lower_ = new QLineEdit("0");
    lower_->setObjectName("tolLower");
    dl->addRow(txt("ui.tolerance.upperMm"), upper_);
    dl->addRow(txt("ui.tolerance.lowerMm"), lower_);
    form->addRow(devRow_);
    preview_ = new QLabel;
    preview_->setObjectName("tolPreview");
    preview_->setWordWrap(true);
    form->addRow(preview_);
    connect(kind_, &QComboBox::activated, this, [this] {
        syncKind();
        updatePreview();
        emit changed();
    });
    connect(field_, &QComboBox::currentTextChanged, this, [this] {
        fieldTouched_ = true;
        updatePreview();
        emit changed();
    });
    connect(show_, &QCheckBox::toggled, this, [this] {
        updatePreview();
        emit changed();
    });
    for (QLineEdit* e : {upper_, lower_})
        connect(e, &QLineEdit::textChanged, this, [this] {
            updatePreview();
            emit changed();
        });
    syncKind();
}

void ToleranceEditor::syncKind() {
    const bool field = kind_->currentData().toString() == "field";
    fieldRow_->setVisible(field);
    devRow_->setVisible(!field);
}

void ToleranceEditor::load(const doc::Json& fit, bool sizeAllowed, std::optional<double> nominal, const QString& label) {
    {
        const QSignalBlocker b1(kind_), b2(field_), b3(show_), b4(upper_), b5(lower_);
        auto* model = qobject_cast<QStandardItemModel*>(kind_->model());
        if (model) model->item(0)->setEnabled(sizeAllowed);
        const bool field = fit.contains("field") || (!fit.contains("upper") && sizeAllowed);
        kind_->setCurrentIndex(field ? 0 : 1);
        field_->setCurrentText(QString::fromStdString(fit.value("field", std::string("H7"))));
        show_->setChecked(fit.value("showDeviations", false));
        upper_->setText(mmText(fit.value("upper", 0.0)));
        lower_->setText(mmText(fit.value("lower", 0.0)));
    }
    fieldTouched_ = fit.contains("field");
    nominal_ = nominal;
    label_ = label;
    syncKind();
    updatePreview();
}

void ToleranceEditor::setNominal(std::optional<double> nominal, const QString& label) {
    nominal_ = nominal;
    label_ = label;
    updatePreview();
}

void ToleranceEditor::suggestField(bool hole) {
    if (fieldTouched_) return;
    const QSignalBlocker b(field_);
    field_->setCurrentText(hole ? "H7" : "h6");
    updatePreview();
}

std::optional<doc::Json> ToleranceEditor::tolerance(QString* error) const {
    doc::Json t = doc::Json::object();
    if (kind_->currentData().toString() == "field") {
        const std::string f = field_->currentText().trimmed().toStdString();
        if (auto p = pmi::parseField(f); !p) {
            if (error) *error = errorText(p.error());
            return std::nullopt;
        }
        t["field"] = f;
        if (show_->isChecked()) t["showDeviations"] = true;
        return t;
    }
    const auto up = parseDecimal(upper_->text()), lo = parseDecimal(lower_->text());
    if (!up || !lo || *up <= *lo) {
        if (error) *error = txt("ui.tolerance.theUpperDeviationMustBe");
        return std::nullopt;
    }
    t["upper"] = *up;
    t["lower"] = *lo;
    return t;
}

void ToleranceEditor::updatePreview() {
    QString err;
    const auto t = tolerance(&err);
    if (!t) {
        preview_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
        preview_->setText(err);
        return;
    }
    std::optional<pmi::Deviations> d;
    if (t->contains("field")) {
        if (nominal_) {
            auto r = api::Session::fitDeviations((*t)["field"].get<std::string>(), *nominal_);
            if (!r) {
                preview_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
                preview_->setText(errorText(r.error()));
                return;
            }
            d = *r;
        }
    } else {
        d = pmi::Deviations{(*t)["upper"].get<double>(), (*t)["lower"].get<double>()};
    }
    preview_->setStyleSheet(QString());
    QString text = label_ + QString::fromStdString(pmi::toleranceSuffix(*t, d));
    if (d) text += QString(": %1 / %2").arg(QString::fromStdString(pmi::formatDeviation(d->upper)), QString::fromStdString(pmi::formatDeviation(d->lower)));
    preview_->setText(text);
}

// ---------------------------------------------------------------- tolerance box of a sketch dimension

ToleranceBox::ToleranceBox(QWidget* parent) : PopupBox("toleranceBox", parent) {
    auto* col = new QVBoxLayout(this);
    auto* title = new QLabel(txt("ui.common.sizeTolerance"));
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    col->addWidget(title);
    editor_ = new ToleranceEditor;
    col->addWidget(editor_);
    error_ = new QLabel;
    error_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
    error_->setWordWrap(true);
    col->addWidget(error_);
    auto* buttons = new DialogButtons("tolOk", "tolCancel");
    ok_ = buttons->ok();
    cancel_ = buttons->cancel();
    remove_ = buttons->addAction(txt("ui.tolerance.remove"), "tolRemove");
    col->addWidget(buttons);
    connect(buttons, &DialogButtons::accepted, this, [this] { accept(); });
    connect(remove_, &QPushButton::clicked, this, [this] {
        hide();
        emit removed();
    });
    connect(buttons, &DialogButtons::cancelled, this, [this] { hide(); });
    hide();
}

void ToleranceBox::open(const QPoint& at, const doc::Json& fit, bool exists, bool sizeAllowed, std::optional<double> nominal,
                        const QString& label) {
    editor_->load(fit, sizeAllowed, nominal, label);
    remove_->setVisible(exists);
    error_->clear();
    openAt(at);
    editor_->setFocus();
}

void ToleranceBox::accept() {
    QString err;
    const auto t = editor_->tolerance(&err);
    if (!t) {
        error_->setText(err);
        return;
    }
    hide();
    emit accepted(*t);
}

}  // namespace construe::app
