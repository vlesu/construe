#include "app/pmi_panel.h"
#include "app/panel_header.h"

#include "api/session.h"
#include "app/messages.h"
#include "app/selection_field_widget.h"
#include "app/symbol_field.h"
#include "app/theme.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace construe::app {

namespace {

const char* kTitles[][2] = {{"roughness", "ui.common.roughness"},
                            {"fit", "ui.common.sizeTolerance"},
                            {"geometric", "ui.pmi.geometricTolerance"},
                            {"datum", "ui.common.datum"},
                            {"rest", "ui.pmi.roughnessOfOtherSurfaces"}};

QString titleOf(const std::string& type) {
    for (const auto& t : kTitles)
        if (type == t[0]) return QString::fromUtf8(txt(t[1]));
    return {};
}

void selectData(QComboBox* c, const QString& data) {
    const int i = c->findData(data);
    c->setCurrentIndex(i >= 0 ? i : 0);
}

}  // namespace

AnnotationPanel::AnnotationPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("annotationPanel");
    auto* col = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(QString(), PanelHeader::CommandButtons{"pmiOk", "pmiCancel"});
    title_ = head->title();
    col->addWidget(head);
    auto* top = new QFormLayout;
    top_ = top;
    face_ = new SelectionFieldWidget("pmiFace", txt("ui.pmi.clickAFaceInThe"), this);
    face_->setRequired(true);
    face_->addTo(top, txt("ui.common.face"));
    connect(face_, &SelectionFieldWidget::removed, this, [this](int) {  // ✕ empties the field (§5.5)
        def_.erase("face");
        def_.erase("at");
        syncFace({});
        emit changed();
    });
    col->addLayout(top);
    // Set in the drawing (FX9-DRW-009): a sheet mark only.
    local_ = new QCheckBox(txt("ui.drawingDim.setInDrawing"));
    local_->setObjectName("pmiLocal");
    local_->hide();
    col->addWidget(local_);
    connect(local_, &QCheckBox::toggled, this, [this] {
        syncLocal();
        emit changed();
    });
    // Roughness (pmi.md §2.1).
    roughness_ = new QWidget;
    auto* rf = new QFormLayout(roughness_);
    roughForm_ = rf;
    rf->setContentsMargins(0, 0, 0, 0);
    sign_ = new QComboBox;
    sign_->setObjectName("pmiSign");
    sign_->addItem(txt("ui.pmi.machiningMethodNotSpecified"), "any");
    sign_->addItem(txt("ui.pmi.materialRemovalRequired"), "removal");
    sign_->addItem(txt("ui.pmi.materialRemovalProhibited"), "noRemoval");
    rf->addRow(txt("ui.pmi.symbol"), sign_);
    parameter_ = new QComboBox;
    parameter_->setObjectName("pmiParameter");
    parameter_->addItem("—", "");
    for (const char* p : {"Ra", "Rz", "Rmax"}) parameter_->addItem(p, p);
    rf->addRow(txt("ui.pmi.parameter"), parameter_);
    value_ = new QComboBox;
    value_->setObjectName("pmiValue");
    value_->setEditable(true);
    for (const char* v : {"0,025", "0,05", "0,1", "0,2", "0,4", "0,8", "1,6", "3,2", "6,3", "12,5", "25", "50", "100"}) value_->addItem(v);
    rf->addRow(txt("ui.pmi.valueM"), value_);
    method_ = new SymbolLineEdit;  // an inscription text — with the standard symbols (FX14-UI-001)
    method_->setObjectName("pmiMethod");
    rf->addRow(txt("ui.pmi.machiningMethod"), method_);
    lay_ = new QComboBox;
    lay_->setObjectName("pmiLay");
    lay_->addItem("—", "");
    for (const auto& [k, t] : std::vector<std::pair<const char*, const char*>>{{"parallel", txt("ui.pmi.parallel")}, {"perpendicular", txt("ui.pmi.perpendicular")},
                                                                               {"crossed", txt("ui.pmi.crossed")}, {"multi", txt("ui.pmi.multiDirectional")},
                                                                               {"circular", txt("ui.pmi.circular")}, {"radial", txt("ui.pmi.radial")},
                                                                               {"particulate", txt("ui.pmi.particulate")}})
        lay_->addItem(QString::fromStdString(pmi::laySymbol(k)) + "  " + QString::fromUtf8(t), k);
    rf->addRow(txt("ui.pmi.layDirection"), lay_);
    col->addWidget(roughness_);
    // Size tolerance (pmi.md §2.2).
    tol_ = new ToleranceEditor;
    col->addWidget(tol_);
    // Geometric tolerance (form and position) (pmi.md §2.3).
    geometric_ = new QWidget;
    auto* gf = new QFormLayout(geometric_);
    gf->setContentsMargins(0, 0, 0, 0);
    characteristic_ = new QComboBox;
    characteristic_->setObjectName("pmiCharacteristic");
    for (const auto& c : pmi::characteristics()) characteristic_->addItem(QString::fromStdString(pmi::characteristicName(c)), QString::fromStdString(c));
    gf->addRow(txt("ui.pmi.toleranceType"), characteristic_);
    tolValue_ = new QLineEdit("0,05");
    tolValue_->setObjectName("pmiTolValue");
    gf->addRow(txt("ui.pmi.valueMm"), tolValue_);
    diameter_ = new QCheckBox(txt("ui.pmi.diameterTolerance"));
    diameter_->setObjectName("pmiDiameter");
    gf->addRow(diameter_);
    material_ = new QComboBox;
    material_->setObjectName("pmiMaterial");
    material_->addItem("—", "");
    material_->addItem(txt("ui.pmi.maximumMaterial"), "M");
    material_->addItem(txt("ui.pmi.leastMaterial"), "L");
    gf->addRow(txt("ui.pmi.materialCondition"), material_);
    auto* drow = new QWidget;
    auto* dl = new QHBoxLayout(drow);
    dl->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 3; ++i) {
        auto* c = new QComboBox;
        c->setObjectName(QString("pmiDatum%1").arg(i + 1));
        datums_.push_back(c);
        dl->addWidget(c);
        connect(c, &QComboBox::activated, this, [this] { emit changed(); });
    }
    gf->addRow(txt("ui.pmi.datums"), drow);
    col->addWidget(geometric_);
    // Datum (pmi.md §2.4).
    datum_ = new QWidget;
    auto* bf = new QFormLayout(datum_);
    bf->setContentsMargins(0, 0, 0, 0);
    letter_ = new QLineEdit;
    letter_->setObjectName("pmiLetter");
    letter_->setMaxLength(2);
    bf->addRow(txt("ui.pmi.letter"), letter_);
    col->addWidget(datum_);
    status_ = new QLabel;
    status_->setObjectName("pmiStatus");
    status_->setWordWrap(true);
    status_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
    col->addWidget(status_);
    col->addStretch(1);
    connect(characteristic_, &QComboBox::activated, this, [this] {
        syncCharacteristic();
        emit changed();
    });
    for (QComboBox* c : {sign_, parameter_, lay_, material_}) connect(c, &QComboBox::activated, this, [this] { emit changed(); });
    connect(sign_, &QComboBox::activated, this, [this] { syncRest(); });
    connect(tol_, &ToleranceEditor::changed, this, [this] { emit changed(); });
    connect(head, &PanelHeader::accepted, this, &AnnotationPanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &AnnotationPanel::cancelled);
    head->acceptOnEnter(this);  // Enter in a field — ✓ (§2.1д)
}

// Rest mode (FX5-PMI-001): «— not set —» disables the parameter and the value.
void AnnotationPanel::syncRest() {
    const bool set = !restMode() || !sign_->currentData().toString().isEmpty();
    parameter_->setEnabled(set);
    value_->setEnabled(set);
}

void AnnotationPanel::syncCharacteristic() {
    const std::string c = characteristic_->currentData().toString().toStdString();
    diameter_->setEnabled(pmi::allowsDiameter(c));
    material_->setEnabled(pmi::allowsMaterial(c));
    for (auto* d : datums_) d->setEnabled(pmi::allowsDatums(c));
}

void AnnotationPanel::start(const std::string& type, const std::optional<std::string>& editing, const doc::Json& def,
                            const std::vector<std::string>& letters, const QString& subject) {
    type_ = type;
    editing_ = editing;
    def_ = def;
    title_->setText(titleOf(type));
    syncFace(subject);
    const bool rest = type == "rest";
    // Rest mode: a part property, not a face annotation — no face, machining method or lay (app-ux §5.12).
    face_->setFieldVisible(!rest);
    roughForm_->setRowVisible(method_, !rest);
    roughForm_->setRowVisible(lay_, !rest);
    const int notSet = sign_->findData("");
    if (rest && notSet < 0) sign_->insertItem(0, QString::fromUtf8(txt("ui.pmi.notSet")), "");
    if (!rest && notSet >= 0) sign_->removeItem(notSet);
    roughness_->setVisible(type == "roughness" || rest);
    tol_->setVisible(type == "fit");
    geometric_->setVisible(type == "geometric");
    datum_->setVisible(type == "datum");
    status_->clear();
    setDrawingLocal(false, true, true);
    if (type == "roughness") {
        selectData(sign_, QString::fromStdString(def.value("sign", std::string("removal"))));
        selectData(parameter_, QString::fromStdString(def.value("parameter", std::string(editing ? "" : "Ra"))));
        value_->setCurrentText(def.contains("value") ? QString::fromStdString(pmi::formatNumber(def["value"].get<double>(), pmi::ROUGHNESS_DECIMALS)) : "1,6");
        method_->setText(QString::fromStdString(def.value("method", std::string())));
        selectData(lay_, QString::fromStdString(def.value("lay", std::string())));
    }
    if (rest) {  // not set — defaults «Material removal required», Ra 6,3
        const bool set = def.is_object() && def.contains("sign");
        selectData(sign_, QString::fromStdString(set ? def["sign"].get<std::string>() : std::string("removal")));
        selectData(parameter_, QString::fromStdString(set ? def.value("parameter", std::string()) : std::string("Ra")));
        value_->setCurrentText(set && def.contains("value") ? QString::fromStdString(pmi::formatNumber(def["value"].get<double>(), pmi::ROUGHNESS_DECIMALS))
                                                            : "6,3");
    }
    syncRest();
    if (type == "geometric") {
        selectData(characteristic_, QString::fromStdString(def.value("characteristic", std::string("flatness"))));
        tolValue_->setText(def.contains("value") ? QString::fromStdString(pmi::formatNumber(def["value"].get<double>(), pmi::DEV_DECIMALS)) : "0,05");
        diameter_->setChecked(def.value("diameter", false));
        selectData(material_, QString::fromStdString(def.value("material", std::string())));
        for (size_t i = 0; i < datums_.size(); ++i) {
            QComboBox* c = datums_[i];
            c->clear();
            c->addItem("", "");
            for (const auto& l : letters) c->addItem(QString::fromStdString(l), QString::fromStdString(l));
            const std::string cur = def.contains("datums") && i < def["datums"].size() ? def["datums"][i].get<std::string>() : std::string();
            if (!cur.empty() && c->findData(QString::fromStdString(cur)) < 0) c->addItem(QString::fromStdString(cur), QString::fromStdString(cur));
            selectData(c, QString::fromStdString(cur));
        }
        syncCharacteristic();
    }
    if (type == "datum") letter_->setText(QString::fromStdString(def.value("letter", std::string())));
}

void AnnotationPanel::syncFace(const QString& subject) {
    std::vector<SelectionItem> items;
    if (def_.is_object() && def_.contains("face") && def_["face"].is_string()) {
        const std::string face = def_["face"];
        items.push_back({faceLabeler_ ? faceLabeler_(face) : QString::fromStdString(face), {{PickTarget::Kind::Face, {}, face}}, {}});
    }
    face_->setSelection(std::move(items), def_.is_object() && def_.contains("face") ? QString() : subject);
}

void AnnotationPanel::setFace(const std::string& face, const std::optional<std::array<double, 3>>& at) {
    def_.erase("dimension");
    def_["face"] = face;
    if (at) def_["at"] = doc::Json::array({(*at)[0], (*at)[1], (*at)[2]});
    else def_.erase("at");
    syncFace({});
    emit changed();
}

std::optional<doc::Json> AnnotationPanel::definition(QString* error) const {
    if (restMode()) {  // restRoughness (document.md §2.4)
        const QString sign = sign_->currentData().toString();
        if (sign.isEmpty()) return doc::Json(nullptr);
        doc::Json r = {{"sign", sign.toStdString()}};
        if (const QString p = parameter_->currentData().toString(); !p.isEmpty()) {
            const auto v = parseDecimal(value_->currentText());
            if (!v || *v <= 0) {
                if (error) *error = text("ui.pmi.badNumber", {{"name", txt("ui.common.value")}});
                return std::nullopt;
            }
            r["parameter"] = p.toStdString();
            r["value"] = *v;
        }
        return r;
    }
    doc::Json d = doc::Json::object();
    d["type"] = type_;
    if (def_.contains("dimension")) d["dimension"] = def_["dimension"];
    if (def_.contains("face")) d["face"] = def_["face"];
    if (def_.contains("face") && def_.contains("at")) d["at"] = def_["at"];  // click point (§5.12)
    if (type_ == "roughness") {
        d["sign"] = sign_->currentData().toString().toStdString();
        const QString p = parameter_->currentData().toString();
        if (!p.isEmpty()) {
            const auto v = parseDecimal(value_->currentText());
            if (!v || *v <= 0) {
                if (error) *error = text("ui.pmi.badNumber", {{"name", txt("ui.common.value")}});
                return std::nullopt;
            }
            d["parameter"] = p.toStdString();
            d["value"] = *v;
        }
        if (!method_->text().trimmed().isEmpty()) d["method"] = method_->text().trimmed().toStdString();
        if (!lay_->currentData().toString().isEmpty()) d["lay"] = lay_->currentData().toString().toStdString();
    } else if (type_ == "fit") {
        auto t = tol_->tolerance(error);
        if (!t) return std::nullopt;
        for (auto it = t->begin(); it != t->end(); ++it) d[it.key()] = it.value();
    } else if (type_ == "geometric") {
        const std::string c = characteristic_->currentData().toString().toStdString();
        d["characteristic"] = c;
        const auto v = parseDecimal(tolValue_->text());
        if (!v || *v <= 0) {
            if (error) *error = text("ui.pmi.badNumber", {{"name", txt("ui.common.value")}});
            return std::nullopt;
        }
        d["value"] = *v;
        if (pmi::allowsDiameter(c) && diameter_->isChecked()) d["diameter"] = true;
        if (pmi::allowsMaterial(c) && !material_->currentData().toString().isEmpty()) d["material"] = material_->currentData().toString().toStdString();
        if (pmi::allowsDatums(c)) {
            doc::Json ds = doc::Json::array();
            for (auto* cb : datums_)
                if (!cb->currentData().toString().isEmpty()) ds.push_back(cb->currentData().toString().toStdString());
            if (!ds.empty()) d["datums"] = ds;
        }
    } else if (type_ == "datum") {
        d["letter"] = letter_->text().trimmed().toUpper().toStdString();
    }
    return d;
}

void AnnotationPanel::setStatus(const QString& error) { status_->setText(error); }

void AnnotationPanel::setDrawingLocal(bool shown, bool checked, bool enabled) {
    const QSignalBlocker b(local_);
    local_->setVisible(shown);
    local_->setChecked(checked);
    local_->setEnabled(enabled);
    syncLocal();
}

// Values of a model mark without «Set in drawing» come from the model: not edited in the drawing.
void AnnotationPanel::syncLocal() {
    const bool editable = local_->isHidden() || local_->isChecked();
    for (QWidget* w : {roughness_, geometric_, datum_}) w->setEnabled(editable);
}

}  // namespace construe::app
