// Thread and hole panels (spec/app-ux.md §5.3и, §5.3з).
#include "app/feature_panel.h"
#include "app/flip_button.h"

#include <QStandardItemModel>

#include "app/app_model.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace construe::app {

// ---------------------------------------------------------------- thread (§5.3и, S2-FEAT-009)

ThreadPanel::ThreadPanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.thread"), parent, Profiles::Hidden) {
    setObjectName("threadPanel");
    faceLabel_ = addSelectionField("threadFace", txt("ui.common.face"), txt("ui.feature.selectACylindricalFace"), [this](int) {
        face_.clear();
        refit();
    });
    faceLabel_->setRequired(true);
    faceItems_ = faceLabel_->items();
    designation_ = new QComboBox;
    designation_->setObjectName("threadDesignation");  // designations matching the face (refit, FX2-FEAT-001)
    form_->addRow(txt("ui.common.designation"), designation_);
    custom_ = new QCheckBox(txt("ui.feature.customThread"));
    custom_->setObjectName("threadCustom");
    custom_->setToolTip(txt("ui.feature.customThreadPitchAndProfile"));
    form_->addRow(custom_);
    // Pitch and profile height are thread parameters, not sizes (the thread tolerance class — deferred, app-ux §5.6а).
    auto exprRow = [&](const char* name, ExprField*& field) {
        auto* w = new QWidget;
        auto* r = new QHBoxLayout(w);
        r->setContentsMargins(0, 0, 0, 0);
        field = newNumberField();
        field->setObjectName(name);
        r->addWidget(field, 1);
        connect(field, &QLineEdit::textEdited, this, &ThreadPanel::changed);
        return w;
    };
    auto* pw = exprRow("threadPitch", pitch_);
    form_->addRow(txt("ui.common.pitch"), pw);
    pitchLabel_ = form_->labelForField(pw);
    auto* hw = exprRow("threadHeight", height_);
    height_->setAllowEmpty(true);
    height_->setPlaceholderText(txt("ui.feature.perIso681"));
    form_->addRow(txt("ui.feature.profileHeight"), hw);
    heightLabel_ = form_->labelForField(hw);
    connect(custom_, &QCheckBox::toggled, this, &ThreadPanel::changed);
    full_ = new QCheckBox(txt("ui.feature.fullLength"));
    full_->setObjectName("threadFull");
    form_->addRow(full_);
    auto* row = new QHBoxLayout;
    length_ = newDimensionField("length");
    length_->setObjectName("threadLength");
    reverse_ = new FlipButton("threadReverse", txt("ui.feature.fromTheOtherEndOf2"));
    row->addWidget(dimRow(length_), 1);
    row->addWidget(reverse_);
    form_->addRow(txt("ui.feature.length"), row);
    info_ = new QLabel;
    info_->setObjectName("threadInfo");
    info_->setWordWrap(true);
    info_->setStyleSheet("color: gray;");
    form_->addRow(info_);
    connect(designation_, &QComboBox::currentIndexChanged, this, &ThreadPanel::changed);
    connect(full_, &QCheckBox::toggled, this, &ThreadPanel::changed);
    connect(length_, &QLineEdit::textEdited, this, &ThreadPanel::changed);
    connect(length_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    connect(reverse_, &QToolButton::toggled, this, &ThreadPanel::changed);
}

void ThreadPanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    face_.clear();
    byExpr_.reset();
    custom_->setChecked(false);
    pitch_->setText(formatNumber(1 / expr::lengthFactor(model_.session().units().length)));
    height_->clear();
    refit();
    full_->setChecked(true);
    length_->setText(formatNumber(10 / expr::lengthFactor(model_.session().units().length)));
    reverse_->setChecked(false);
    changed();
}

void ThreadPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Thread) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    face_ = def["face"];
    if (def.contains("height")) {  // free thread (FX7-PT-003): the fields of «Custom thread»
        custom_->setChecked(true);
        pitch_->setText(displayExpr(def["pitch"].get<std::string>(), d->lengthUnit));
        height_->setText(displayExpr(def["height"].get<std::string>(), d->lengthUnit));
        refit();
    } else if (def.contains("diameter")) {
        byExpr_ = doc::Json{{"diameter", def["diameter"]}};
        if (def.contains("pitch")) (*byExpr_)["pitch"] = def["pitch"];
        refit();
    } else {
        refit(def["designation"].get<std::string>());
    }
    full_->setChecked(!def.contains("length"));
    if (def.contains("length")) length_->setText(displayExpr(def["length"].get<std::string>(), d->lengthUnit));
    reverse_->setChecked(def.value("reverse", false));
    loadTargets(def.value("targets", std::vector<std::string>{}));
    changed();
}

bool ThreadPanel::pick(const PickTarget& t) {
    if (t.kind != PickTarget::Kind::Face) return false;
    setFace(t.name, t.body);
    return true;
}

std::vector<PickTarget> ThreadPanel::selectedTargets() const {
    if (face_.empty()) return {};
    return {{PickTarget::Kind::Face, {}, face_}};
}

void ThreadPanel::setFace(const std::string& name, const std::string& body) {
    face_ = name;
    refit();
    (void)body;
    changed();
}

bool ThreadPanel::canAccept() const {
    if (face_.empty()) return false;
    if (custom_->isChecked()) return customDiameter().has_value();
    return designation_->count() > 0;
}

std::optional<double> ThreadPanel::customDiameter() const {
    if (!fit_) return std::nullopt;
    const auto d = model_.session().document().document;
    auto P = model_.session().checkExpression(pitch_->expression(), expr::LENGTH);
    if (!P || !(P->value > 0)) return std::nullopt;
    return fit_->internal ? fit_->diameter + 1.0825 * P->value : fit_->diameter;
}

void ThreadPanel::refit(const std::string& prefer) {
    const QSignalBlocker b(designation_);
    designation_->clear();
    designation_->setEnabled(!byExpr_);
    fit_.reset();
    fitError_.reset();
    if (face_.empty()) return;
    auto r = model_.session().threadFit(face_, api::StateRef::final());
    if (!r) {
        fitError_ = r.error();
        return;
    }
    fit_ = *r;
    if (byExpr_) {  // the evaluated designation of the edited thread (S6-LIB-008); none — the diameter expression
        QString shown = QString::fromStdString((*byExpr_)["diameter"].get<std::string>());
        if (auto th = model_.session().threads(api::StateRef::final()); th && editing_)
            for (const auto& t : *th)
                if (t.feature == *editing_) shown = QString::fromStdString(t.designation);
        designation_->addItem(shown);
        designation_->setCurrentIndex(0);
        return;
    }
    for (const auto& o : fit_->options) designation_->addItem(QString::fromStdString(o.designation));
    int pick = fit_->suggested;
    if (!prefer.empty())
        if (const int i = designation_->findText(QString::fromStdString(prefer)); i >= 0) pick = i;
    if (pick >= 0) designation_->setCurrentIndex(pick);
}

namespace {
// Numbers in the thread hint (§5.3и): decimal comma; diameter up to UI_DECIMALS digits, range bound one digit rounded
// towards the inside of the range (dir: +1 up, −1 down).
QString mmText(double v) { return formatNumber(v).replace('.', ','); }
QString boundText(double v, int dir) {
    const double t = dir > 0 ? std::ceil(v * 10 - 1e-6) / 10 : std::floor(v * 10 + 1e-6) / 10;
    return QString::number(t, 'f', 1).replace('.', ',');
}
}  // namespace

void ThreadPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    faceLabel_->setSelection(face_.empty() ? std::vector<SelectionItem>{}
                                       : std::vector<SelectionItem>{{topoCaption(model_.session(), face_, false), {{PickTarget::Kind::Face, {}, face_}}, {}}});
    const bool custom = custom_->isChecked();
    custom_->setEnabled(!byExpr_);
    designation_->setEnabled(!byExpr_ && !custom);
    form_->setRowVisible(pitchLabel_, custom);
    form_->setRowVisible(heightLabel_, custom);
    if (custom) {
        const auto d = model_.session().document().document;
        showExprState(pitch_, expr::LENGTH);
        showExprState(height_, expr::LENGTH);
    }
    QString info;
    if (fitError_) {
        info = text("ui.thread.notCylinder");
    } else if (byExpr_) {
        info = text("ui.thread.byExpression", {{"diameter", QString::fromStdString((*byExpr_)["diameter"].get<std::string>())},
                                                {"pitch", byExpr_->contains("pitch") ? QString::fromStdString((*byExpr_)["pitch"].get<std::string>())
                                                                                     : text("ui.thread.coarsePitch")}});
    } else if (fit_ && fit_->options.empty() && !fit_->nearest.empty()) {
        const auto& n = fit_->nearest.front();
        QStringList near;
        for (const auto& x : fit_->nearest) near << QString::fromStdString(x.designation);
        info = text(fit_->internal ? "ui.thread.mismatchInternal" : "ui.thread.mismatchExternal",
                    {{"actual", mmText(fit_->diameter)},
                     {"thread", QString::fromStdString(n.designation)},
                     {"tapMin", boundText(n.tapMin, 1)},
                     {"tapMax", boundText(n.tapMax, -1)},
                     {"nomMin", boundText(n.nomMin, 1)},
                     {"nomMax", boundText(n.nomMax, -1)},
                     {"nearest", near.join(", ")}});
    } else if (fit_ && designation_->currentIndex() >= 0 && size_t(designation_->currentIndex()) < fit_->options.size() &&
               fit_->options[size_t(designation_->currentIndex())].nominal) {
        info = text("ui.thread.nominalNote", {{"drill", mmText(fit_->options[size_t(designation_->currentIndex())].tapDrill)}});
    }
    info_->setText(info);
    info_->setVisible(!info.isEmpty());
    dimRow(length_)->setEnabled(!full_->isChecked());
    showExprState(length_, expr::LENGTH);
}

doc::Json ThreadPanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json def = {{"type", "thread"}, {"face", face_}};
    if (custom_->isChecked()) {
        // Free thread (FX7-PT-003): the nominal diameter by the face, pitch and the optional profile height.
        def["diameter"] = formatNumber(customDiameter().value_or(0)).toStdString() + " mm";
        def["pitch"] = pitch_->expression();
        if (!height_->text().trimmed().isEmpty()) def["height"] = height_->expression();
    } else if (byExpr_) {
        def["diameter"] = (*byExpr_)["diameter"];
        if (byExpr_->contains("pitch")) def["pitch"] = (*byExpr_)["pitch"];
    } else {
        def["designation"] = designation_->currentText().toStdString();
    }
    if (!full_->isChecked()) def["length"] = length_->expression();
    if (reverse_->isChecked()) def["reverse"] = true;
    return def;
}

// ---------------------------------------------------------------- hole (§5.3з, S2-FEAT-008)

HolePanel::HolePanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.hole"), parent) {
    setObjectName("holePanel");
    if (auto* l = qobject_cast<QLabel*>(form_->labelForField(profiles_))) l->setText(txt("ui.feature.position"));
    kind_ = new QComboBox;
    kind_->setObjectName("holeKind");
    kind_->addItem(txt("ui.feature.simple"), "simple");
    kind_->addItem(txt("ui.feature.counterbore"), "counterbore");
    kind_->addItem(txt("ui.feature.countersink"), "countersink");
    kind_->addItem(txt("ui.feature.tapped"), "tapped");
    form_->addRow(txt("ui.common.type"), kind_);
    size_ = new QComboBox;
    size_->setObjectName("holeSize");
    for (const auto& row : api::Session::holeTables()["sizes"]) size_->addItem(QString::fromStdString(row["size"].get<std::string>()));
    // The hole diameter comes from the size list; its tolerance is the tolerance of the hole wall (pmi.md §2.2,
    // FX12-PMI-007) — stored always.
    wrapDimension("diameter", size_)->setProperty("construeStoredAlways", true);
    form_->addRow(txt("ui.common.dimension"), dimRow(size_));
    pitch_ = new QComboBox;
    pitch_->setObjectName("holePitch");
    form_->addRow(txt("ui.common.pitch"), pitch_);
    pitchLabel_ = form_->labelForField(pitch_);
    fit_ = new QComboBox;
    fit_->setObjectName("holeFit");
    fit_->addItem(txt("ui.feature.close"), "fine");
    fit_->addItem(txt("ui.feature.normal"), "normal");
    fit_->addItem(txt("ui.feature.loose"), "loose");
    form_->addRow(txt("ui.feature.fit"), fit_);
    fitLabel_ = form_->labelForField(fit_);
    end_ = new QComboBox;
    end_->setObjectName("holeEnd");
    end_->addItem(txt("ui.feature.blind"), "blind");
    end_->addItem(txt("ui.feature.throughAll2"), "throughAll");
    reverse_ = new FlipButton("holeReverse", txt("ui.feature.flipDirection"));
    auto* endRow = new QHBoxLayout;
    endRow->addWidget(end_, 1);
    endRow->addWidget(reverse_);
    form_->addRow(txt("ui.feature.end"), endRow);
    auto* depthRow = new QHBoxLayout;
    depth_ = newDimensionField("end.depth");
    depth_->setObjectName("holeDepth");
    depthRow->addWidget(dimRow(depth_), 1);
    form_->addRow(txt("ui.feature.depth"), depthRow);
    depthLabel_ = form_->labelForField(depthRow);
    info_ = new QLabel;
    info_->setObjectName("holeInfo");
    info_->setWordWrap(true);
    info_->setStyleSheet("color: gray;");
    form_->addRow(info_);
    buildTargetsRow();  // assembly hole (§8а)
    cut_ = new QComboBox;  // where an assembly hole cuts (FX8-ASM-007): like the connection wizard
    cut_->setObjectName("holeCut");
    cut_->addItem(text("ui.solution.cutAssembly"), "assembly");
    cut_->addItem(text("ui.solution.cutParts"), "parts");
    form_->addRow(text("ui.solution.cut"), cut_);
    form_->setRowVisible(cut_, false);
    connect(cut_, &QComboBox::currentIndexChanged, this, &HolePanel::changed);
    for (QComboBox* b : {kind_, size_, pitch_, fit_, end_})
        connect(b, &QComboBox::currentIndexChanged, this, [this, b] {
            if (b == size_) fillPitches();
            changed();
        });
    connect(depth_, &QLineEdit::textEdited, this, &HolePanel::changed);
    connect(depth_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    connect(reverse_, &QToolButton::toggled, this, &HolePanel::changed);
    fillPitches();
}

void HolePanel::fillPitches() {
    const QSignalBlocker b(pitch_);
    pitch_->clear();
    for (const auto& row : api::Session::holeTables()["sizes"])
        if (QString::fromStdString(row["size"].get<std::string>()) == size_->currentText())
            for (const auto& p : row["pitches"]) {
                const double v = p["pitch"];
                pitch_->addItem(QString::number(v) + (pitch_->count() == 0 ? txt("ui.feature.coarse") : ""), v);
            }
}

void HolePanel::startNew(const std::optional<std::string>& sketch) {
    editing_.reset();
    title_->setText(assemblyMode() ? QString(txt("ui.feature.assemblyHole")) : baseTitle_);  // §8а
    banner_->hide();
    sketch_ = sketch.value_or(std::string());
    regions_.reset();
    {
        const QSignalBlocker b1(kind_), b2(size_), b3(fit_), b4(end_);
        kind_->setCurrentIndex(0);
        size_->setCurrentIndex(std::max(0, size_->findText("M6")));
        fit_->setCurrentIndex(1);
        end_->setCurrentIndex(0);
    }
    fillPitches();
    depth_->setText(formatNumber(10 / expr::lengthFactor(model_.session().units().length)));  // HOLE_DEFAULT_DEPTH = 10 mm
    reverse_->setChecked(false);
    loadTargets({});
    {
        const QSignalBlocker b(cut_);
        cut_->setCurrentIndex(std::max(0, cut_->findData(QString::fromStdString(defaultCut_))));
    }
    form_->setRowVisible(cut_, assemblyMode());
    changed();
}

void HolePanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Hole) return;
    const doc::Json& def = f->pattern()->def;
    startNew(def["sketch"].get<std::string>());
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    {
        const QSignalBlocker b1(kind_), b2(size_), b3(fit_), b4(end_);
        kind_->setCurrentIndex(std::max(0, kind_->findData(QString::fromStdString(def["kind"]))));
        size_->setCurrentIndex(std::max(0, size_->findText(QString::fromStdString(def["size"]))));
        fit_->setCurrentIndex(std::max(0, fit_->findData(QString::fromStdString(def.value("fit", std::string("normal"))))));
        end_->setCurrentIndex(def["end"]["type"] == "throughAll" ? 1 : 0);
    }
    fillPitches();
    if (def.contains("pitch"))
        for (int i = 0; i < pitch_->count(); ++i)
            if (std::abs(pitch_->itemData(i).toDouble() - def["pitch"].get<double>()) < 1e-9) pitch_->setCurrentIndex(i);
    if (def["end"].contains("depth")) depth_->setText(displayExpr(def["end"]["depth"].get<std::string>(), d->lengthUnit));
    reverse_->setChecked(def.value("reverse", false));
    loadTargets(def.value("targets", std::vector<std::string>{}));
    {
        const QSignalBlocker b(cut_);
        cut_->setCurrentIndex(def.value("cut", std::string()) == "parts" ? 1 : 0);
    }
    changed();
}

void HolePanel::updateLabels() {
    FeaturePanel::updateLabels();
    const std::string kind = kind_->currentData().toString().toStdString();
    const bool tapped = kind == "tapped", blind = end_->currentData().toString() == "blind";
    pitch_->setVisible(tapped);
    if (pitchLabel_) pitchLabel_->setVisible(tapped);
    fit_->setVisible(!tapped);
    if (fitLabel_) fitLabel_->setVisible(!tapped);
    dimRow(depth_)->setVisible(blind);
    if (depthLabel_) depthLabel_->setVisible(blind);
    if (sketch_.empty()) profiles_->setPrompt(assemblyMode() ? text("ui.hole.placePoint") : QString(txt("ui.feature.selectASketchWithPoints")));
    // Reference line of sizes from the table.
    QString info;
    for (const auto& row : api::Session::holeTables()["sizes"]) {
        if (QString::fromStdString(row["size"].get<std::string>()) != size_->currentText()) continue;
        const double dh = tapped ? pitch_->currentIndex() >= 0 ? row["pitches"][size_t(std::max(0, pitch_->currentIndex()))]["drill"].get<double>() : 0.0
                                 : row["clearance"][fit_->currentData().toString().toStdString()].get<double>();
        // What the diameter means (FX10-HOLE-001): a clearance hole is larger than the screw, a tapped one — the tap drill.
        const QString dText = formatNumber(dh).replace('.', ',');
        if (tapped) {
            QString thread = size_->currentText();
            if (pitch_->currentIndex() > 0) thread += "x" + formatNumber(pitch_->currentData().toDouble());
            info = text("ui.hole.infoTapped", {{"d", dText}, {"thread", thread}});
        } else {
            info = text("ui.hole.infoClearance", {{"d", dText}, {"size", size_->currentText()}});
        }
        if (kind == "counterbore")
            info += QString::fromUtf8(txt("ui.feature.counterboreNN")).arg(row["counterbore"]["d"].get<double>()).arg(row["counterbore"]["t"].get<double>());
        if (kind == "countersink") info += QString::fromUtf8(txt("ui.feature.countersinkN90")).arg(row["countersink"]["d"].get<double>());
    }
    info_->setText(info);
    if (blind) showExprState(depth_, expr::LENGTH);
    ok_->setEnabled(canAccept());
}

doc::Json HolePanel::definition() const {
    const auto d = model_.session().document().document;
    const std::string kind = kind_->currentData().toString().toStdString();
    doc::Json def = {{"type", "hole"}, {"sketch", sketch_}, {"kind", kind}, {"size", size_->currentText().toStdString()}};
    if (kind == "tapped") {
        if (pitch_->currentIndex() > 0) def["pitch"] = pitch_->currentData().toDouble();
    } else if (fit_->currentData().toString() != "normal") {
        def["fit"] = fit_->currentData().toString().toStdString();
    }
    def["end"] = end_->currentData().toString() == "blind" ? doc::Json{{"type", "blind"}, {"depth", depth_->expression()}}
                                                           : doc::Json{{"type", "throughAll"}};
    if (reverse_->isChecked()) def["reverse"] = true;
    if (!targets().empty()) def["targets"] = targets();  // assembly hole (assembly.md §7г.1)
    if (assemblyMode() && cut_->currentData().toString() == "parts") def["cut"] = "parts";  // FX8-ASM-007
    return def;
}

}  // namespace construe::app
