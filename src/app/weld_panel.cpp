#include "app/weld_panel.h"
#include "app/panel_header.h"

#include "app/app_model.h"
#include "app/feature_panel.h"
#include "app/messages.h"
#include "pmi/weld.h"
#include "api/session.h"

#include <QFormLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace construe::app {

namespace {

QString jointName(const std::string& j) {
    if (j == "butt") return txt("ui.weld.butt");
    if (j == "corner") return txt("ui.weld.corner");
    if (j == "tee") return txt("ui.weld.tee");
    if (j == "lap") return txt("ui.weld.lap");
    return QString::fromStdString(j);
}

// Standard and type of the weld type table (weld.md §2.2; the table — by the weldTypes query); nullptr — none.
const doc::Json* findStandard(const std::string& id) {
    for (const auto& s : api::Session::weldTypes()["standards"])
        if (s["id"] == id) return &s;
    return nullptr;
}

const doc::Json* findType(const std::string& standard, const std::string& type) {
    const doc::Json* s = findStandard(standard);
    if (!s) return nullptr;
    for (const auto& t : (*s)["types"])
        if (t["type"] == type) return &t;
    return nullptr;
}

}  // namespace

WeldPanel::WeldPanel(AppModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName("weldPanel");
    auto* col = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(txt("ui.common.weld"), PanelHeader::CommandButtons{"weldOk", "weldCancel"});
    col->addWidget(head);
    auto* form = new QFormLayout;
    col->addLayout(form);
    facesLabel_ = new SelectionFieldWidget("weldFaces", text("ui.weld.pickFaces"), this);  // FX8-WLD-001: faces of the welded parts
    facesLabel_->setManyFormat(txt("ui.common.facesN"));
    facesLabel_->setRequired(true);
    facesLabel_->addTo(form, txt("ui.common.faces"));
    faceItems_ = facesLabel_->items();
    connect(facesLabel_, &SelectionFieldWidget::removed, this, [this](int i) {
        if (i < int(faces_.size())) toggleFace(faces_[size_t(i)]);
    });
    edgesLabel_ = new SelectionFieldWidget("weldEdges", txt("ui.weld.pickEdges"), this);
    edgesLabel_->setManyFormat(txt("ui.common.edgesN"));
    edgesLabel_->addTo(form, txt("ui.common.edges"));
    edgeItems_ = edgesLabel_->items();
    connect(edgesLabel_, &SelectionFieldWidget::removed, this, [this](int i) {
        if (i < int(edges_.size())) toggleEdge(edges_[size_t(i)]);
    });
    standard_ = new QComboBox;
    standard_->setObjectName("weldStandard");
    for (const auto& s : api::Session::weldTypes()["standards"])
        standard_->addItem(QString::fromStdString(s["designation"].get<std::string>()), QString::fromStdString(s["id"].get<std::string>()));
    form->addRow(txt("ui.common.standard"), standard_);
    type_ = new QComboBox;
    type_->setObjectName("weldType");
    form->addRow(txt("ui.common.type"), type_);
    process_ = new QComboBox;
    process_->setObjectName("weldProcess");
    processLabel_ = new QLabel(txt("ui.weld.process"));
    form->addRow(processLabel_, process_);
    leg_ = new ExprField;
    leg_->setChecker(&model_.session(), expr::LENGTH);
    leg_->setObjectName("weldLeg");
    legLabel_ = new QLabel(txt("ui.weld.leg"));
    form->addRow(legLabel_, leg_);
    side_ = new QComboBox;
    side_->setObjectName("weldSide");
    side_->addItem(txt("ui.weld.arrowSide"), QString());
    side_->addItem(txt("ui.weld.otherSide"), QString("back"));
    form->addRow(txt("ui.weld.side"), side_);
    allAround_ = new QCheckBox(txt("ui.weld.allAround"));
    allAround_->setObjectName("weldAllAround");
    field_ = new QCheckBox(txt("ui.weld.fieldWeld"));
    field_->setObjectName("weldField");
    removeReinforcement_ = new QCheckBox(txt("ui.weld.removeReinforcement"));
    removeReinforcement_->setObjectName("weldRemoveReinforcement");
    smooth_ = new QCheckBox(txt("ui.weld.smoothTransition"));
    smooth_->setObjectName("weldSmooth");
    intermittent_ = new QCheckBox(txt("ui.weld.intermittent"));
    intermittent_->setObjectName("weldIntermittent");
    for (QCheckBox* c : {allAround_, field_, removeReinforcement_, smooth_, intermittent_}) col->addWidget(c);
    auto* form2 = new QFormLayout;
    col->addLayout(form2);
    length_ = new ExprField;
    length_->setChecker(&model_.session(), expr::LENGTH);
    length_->setText("50");
    length_->setObjectName("weldLength");
    lengthLabel_ = new QLabel(txt("ui.weld.segmentLength"));
    form2->addRow(lengthLabel_, length_);
    pitch_ = new ExprField;
    pitch_->setChecker(&model_.session(), expr::LENGTH);
    pitch_->setText("100");
    pitch_->setObjectName("weldPitch");
    pitchLabel_ = new QLabel(txt("ui.common.pitch"));
    form2->addRow(pitchLabel_, pitch_);
    pattern_ = new QComboBox;
    pattern_->setObjectName("weldPattern");
    pattern_->addItem(txt("ui.weld.chain"), QString("chain"));
    pattern_->addItem(txt("ui.weld.staggered"), QString("staggered"));
    patternLabel_ = new QLabel(txt("ui.weld.arrangement"));
    form2->addRow(patternLabel_, pattern_);
    label_ = new QLabel;
    label_->setObjectName("weldLabel");
    label_->setWordWrap(true);
    auto* form3 = new QFormLayout;
    col->addLayout(form3);
    form3->addRow(txt("ui.common.designation"), label_);
    col->addStretch(1);
    auto changedNow = [this] {
        sync();
        emit changed();
    };
    connect(standard_, &QComboBox::activated, this, [this, changedNow] {
        fillTypes();
        changedNow();
    });
    for (QComboBox* c : {type_, process_, side_, pattern_}) connect(c, &QComboBox::activated, this, changedNow);
    for (ExprField* e : {leg_, length_, pitch_}) connect(e, &QLineEdit::textEdited, this, changedNow);
    for (QCheckBox* c : {allAround_, field_, removeReinforcement_, smooth_, intermittent_}) connect(c, &QCheckBox::toggled, this, changedNow);
    connect(head, &PanelHeader::accepted, this, &WeldPanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &WeldPanel::cancelled);
    head->acceptOnEnter(this);  // Enter in a field — ✓ (§2.1д)
}

// Types and processes of the current standard (app-ux §8а); keeps the chosen type if the standard has it.
void WeldPanel::fillTypes() {
    const QString keepType = type_->currentData().toString(), keepProcess = process_->currentData().toString();
    const QSignalBlocker b1(type_), b2(process_);
    type_->clear();
    process_->clear();
    const doc::Json* s = findStandard(standard_->currentData().toString().toStdString());
    if (!s) return;
    for (const auto& t : (*s)["types"]) {
        const QString code = QString::fromStdString(t["type"].get<std::string>());
        type_->addItem(code + QString::fromUtf8(" — ") + jointName(t["joint"].get<std::string>()) + ", " +
                           QString::fromStdString(t["description"].get<std::string>()),
                       code);
    }
    process_->addItem(QString::fromUtf8("—"), QString());
    for (const auto& p : (*s)["processes"]) {
        const QString code = QString::fromStdString(p["code"].get<std::string>());
        process_->addItem(code + QString::fromUtf8(" — ") + QString::fromStdString(p["name"].get<std::string>()), code);
    }
    type_->setCurrentIndex(std::max(0, type_->findData(keepType)));
    process_->setCurrentIndex(std::max(0, process_->findData(keepProcess)));
}

void WeldPanel::start(const std::optional<std::string>& editing, const doc::Json& weld) {
    editing_ = editing;
    faces_.clear();
    edges_.clear();
    if (weld.contains("faces"))
        for (const auto& e : weld["faces"]) faces_.push_back(e.get<std::string>());
    if (weld.contains("edges"))
        for (const auto& e : weld["edges"]) edges_.push_back(e.get<std::string>());
    {
        const QSignalBlocker b1(standard_), b2(type_), b3(process_), b4(side_), b5(pattern_);
        const QSignalBlocker c1(allAround_), c2(field_), c3(removeReinforcement_), c4(smooth_), c5(intermittent_);
        const std::string standard = weld.value("standard", std::string());
        standard_->setCurrentIndex(std::max(0, standard_->findData(QString::fromStdString(standard))));
        type_->clear();
        fillTypes();
        if (weld.contains("type")) {
            type_->setCurrentIndex(std::max(0, type_->findData(QString::fromStdString(weld["type"].get<std::string>()))));
        } else {  // new weld: the first tee type, otherwise the first one (app-ux §8а)
            const doc::Json* s = findStandard(standard_->currentData().toString().toStdString());
            int idx = 0;
            if (s)
                for (size_t i = 0; i < (*s)["types"].size(); ++i)
                    if ((*s)["types"][i]["joint"] == "tee") {
                        idx = int(i);
                        break;
                    }
            type_->setCurrentIndex(idx);
        }
        process_->setCurrentIndex(std::max(0, process_->findData(QString::fromStdString(weld.value("process", std::string())))));
        leg_->setText(QString::fromStdString(weld.value("leg", std::string(editing ? "" : "5"))));
        side_->setCurrentIndex(weld.value("side", std::string()) == "back" ? 1 : 0);
        allAround_->setChecked(weld.value("allAround", false));
        field_->setChecked(weld.value("field", false));
        removeReinforcement_->setChecked(weld.value("removeReinforcement", false));
        smooth_->setChecked(weld.value("smooth", false));
        const bool im = weld.contains("intermittent");
        intermittent_->setChecked(im);
        length_->setText(im ? QString::fromStdString(weld["intermittent"]["length"].get<std::string>()) : QString("50"));
        pitch_->setText(im ? QString::fromStdString(weld["intermittent"]["pitch"].get<std::string>()) : QString("100"));
        pattern_->setCurrentIndex(im && weld["intermittent"]["pattern"] == "staggered" ? 1 : 0);
    }
    if (leg_->text().isEmpty() && !editing) leg_->setText("5");
    sync();
}

void WeldPanel::toggleFace(const std::string& ref) {
    edges_.clear();
    auto it = std::find(faces_.begin(), faces_.end(), ref);
    if (it != faces_.end()) faces_.erase(it);
    else faces_.push_back(ref);
    sync();
    emit changed();
}

void WeldPanel::toggleEdge(const std::string& ref) {
    faces_.clear();
    auto it = std::find(edges_.begin(), edges_.end(), ref);
    if (it != edges_.end()) edges_.erase(it);
    else edges_.push_back(ref);
    sync();
    emit changed();
}

const doc::Json* WeldPanel::currentType() const {
    return findType(standard_->currentData().toString().toStdString(), type_->currentData().toString().toStdString());
}

void WeldPanel::sync() {
    auto items = [this](const std::vector<std::string>& refs) {
        std::vector<SelectionItem> out;
        for (const auto& r : refs) out.push_back({labeler_ ? labeler_(r) : QString::fromStdString(r), {}, r});
        return out;
    };
    facesLabel_->setSelection(items(faces_));
    edgesLabel_->setSelection(items(edges_));
    // The references themselves — in the tooltip (diagnostics of an assembly reference).
    auto refsTip = [](const std::vector<std::string>& refs) {
        QStringList out;
        for (const auto& r : refs) out << QString::fromStdString(r);
        return out.join("\n");
    };
    facesLabel_->setToolTip(refsTip(faces_));
    edgesLabel_->setToolTip(refsTip(edges_));
    const doc::Json* t = currentType();
    const std::string leg = t ? (*t)["leg"].get<std::string>() : std::string("none");
    leg_->setVisible(leg != "none");
    legLabel_->setVisible(leg != "none");
    const doc::Json* s = findStandard(standard_->currentData().toString().toStdString());
    const bool hasProcesses = s && !(*s)["processes"].empty();
    process_->setVisible(hasProcesses);
    processLabel_->setVisible(hasProcesses);
    const bool im = intermittent_->isChecked();
    for (QWidget* w : std::initializer_list<QWidget*>{length_, pitch_, pattern_, lengthLabel_, pitchLabel_, patternLabel_}) w->setVisible(im);
    const bool twoSided = t && (*t)["sides"].get<int>() == 2;
    if (!twoSided && pattern_->currentIndex() != 0) {
        const QSignalBlocker b(pattern_);
        pattern_->setCurrentIndex(0);
    }
    pattern_->setEnabled(twoSided);  // staggered — only two-sided types (weld.md §3)
    label_->setText(labelText());
}

doc::Json WeldPanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json w = doc::Json::object();
    w["standard"] = standard_->currentData().toString().toStdString();
    w["type"] = type_->currentData().toString().toStdString();
    const doc::Json* s = findStandard(w["standard"].get<std::string>());
    if (s && !(*s)["processes"].empty() && !process_->currentData().toString().isEmpty()) w["process"] = process_->currentData().toString().toStdString();
    const doc::Json* t = currentType();
    const std::string legRule = t ? (*t)["leg"].get<std::string>() : std::string("none");
    const std::string leg = leg_->expression();
    if (legRule == "required" || (legRule == "optional" && !leg.empty())) w["leg"] = leg;
    if (!faces_.empty() || edges_.empty()) {
        w["faces"] = doc::Json::array();
        for (const auto& e : faces_) w["faces"].push_back(e);
    } else {
        w["edges"] = doc::Json::array();
        for (const auto& e : edges_) w["edges"].push_back(e);
    }
    if (side_->currentIndex() == 1) w["side"] = "back";
    if (allAround_->isChecked()) w["allAround"] = true;
    if (field_->isChecked()) w["field"] = true;
    if (intermittent_->isChecked())
        w["intermittent"] = doc::Json{{"length", length_->expression()},
                                      {"pitch", pitch_->expression()},
                                      {"pattern", pattern_->currentData().toString().toStdString()}};
    if (removeReinforcement_->isChecked()) w["removeReinforcement"] = true;
    if (smooth_->isChecked()) w["smooth"] = true;
    return w;
}

// Designation line by the current fields (app-ux §8а): numbers as typed.
QString WeldPanel::labelText() const {
    const doc::Json w = definition();
    pmi::WeldTexts texts;
    if (w.contains("leg")) texts.leg = leg_->text().trimmed().toStdString();
    if (w.contains("intermittent")) {
        texts.length = length_->text().trimmed().toStdString();
        texts.pitch = pitch_->text().trimmed().toStdString();
    }
    return QString::fromStdString(pmi::weldLabelText(pmi::weldLabel(w, texts)));
}

}  // namespace construe::app
