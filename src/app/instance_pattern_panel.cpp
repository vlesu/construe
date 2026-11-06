#include "app/instance_pattern_panel.h"
#include "app/panel_header.h"

#include "app/app_model.h"
#include "app/feature_panel.h"
#include "app/flip_button.h"
#include "app/messages.h"

#include <QFormLayout>
#include <QVBoxLayout>

namespace construe::app {

namespace {
const std::pair<const char*, const char*> MODES[] = {{"linear", "ui.common.linear"}, {"circular", "ui.common.circular"}, {"feature", "ui.instancePattern.byPartPattern"}};
}

InstancePatternPanel::InstancePatternPanel(AppModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName("instancePatternPanel");
    auto* col = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(txt("ui.instancePattern.instancePattern"), PanelHeader::CommandButtons{"ipOk", "ipCancel"});
    col->addWidget(head);
    auto* form = new QFormLayout;
    col->addLayout(form);
    mode_ = new QComboBox;
    mode_->setObjectName("ipMode");
    for (const auto& [id, label] : MODES) mode_->addItem(QString::fromUtf8(txt(label)), QString(id));
    form->addRow(txt("ui.common.type"), mode_);
    // Pick fields: the active one receives the next click in the view (§8а).
    sourceBtn_ = new SelectionFieldWidget("ipSource", txt("ui.instancePattern.clickAPartInThe"), this);
    sourceBtn_->addTo(form, txt("ui.instancePattern.instance"));
    refBtn_ = new SelectionFieldWidget("ipRef", txt("ui.instancePattern.clickAnEdgeAxisOr"), this);
    refLabel_ = new QLabel(txt("ui.common.direction"));
    refBtn_->addTo(form, refLabel_);
    for (auto* f : {sourceBtn_, refBtn_}) f->setRequired(true), fields_.add(f);
    connect(sourceBtn_, &SelectionFieldWidget::removed, this, [this](int) {  // ✕ empties the field (§5.5)
        source_.clear();
        setActiveField(Field::Source);
        emit changed();
    });
    connect(refBtn_, &SelectionFieldWidget::removed, this, [this](int) {
        ref_.clear();
        feature_->clear();
        setActiveField(Field::Ref);
        emit changed();
    });
    feature_ = new QComboBox;
    feature_->setObjectName("ipFeature");
    featureLabel_ = new QLabel(txt("ui.instancePattern.partPattern"));
    form->addRow(featureLabel_, feature_);
    count_ = new ExprField;
    count_->setChecker(&model_.session(), expr::DIMLESS);
    count_->setText("3");
    count_->setObjectName("ipCount");
    countLabel_ = new QLabel(txt("ui.common.count"));
    form->addRow(countLabel_, count_);
    spacing_ = new ExprField;
    spacing_->setChecker(&model_.session(), expr::LENGTH);
    spacing_->setText("50");
    spacing_->setObjectName("ipSpacing");
    spacingLabel_ = new QLabel(txt("ui.common.spacing"));
    form->addRow(spacingLabel_, spacing_);
    angle_ = new ExprField;
    angle_->setChecker(&model_.session(), expr::ANGLE);
    angle_->setText("360");
    angle_->setObjectName("ipAngle");
    angleLabel_ = new QLabel(txt("ui.common.angle"));
    form->addRow(angleLabel_, angle_);
    reverse_ = new FlipButton("ipReverse", txt("ui.instancePattern.flipThePatternDirection"));
    reverseLabel_ = new QLabel(txt("ui.common.flip"));
    form->addRow(reverseLabel_, reverse_);
    col->addStretch(1);
    connect(mode_, &QComboBox::activated, this, [this] {
        ref_.clear();  // a direction is not an axis and not a part instance: the reference is picked again
        feature_->clear();
        setActiveField(source_.empty() ? Field::Source : Field::Ref);
        emit changed();
    });
    connect(&fields_, &SelectionFieldGroup::activeChanged, this, [this](SelectionFieldWidget* f) {
        setActiveField(f == sourceBtn_ ? Field::Source : Field::Ref);
    });
    for (ExprField* e : {count_, spacing_, angle_}) connect(e, &QLineEdit::textEdited, this, [this] { emit changed(); });
    connect(reverse_, &QToolButton::toggled, this, [this] { emit changed(); });
    connect(feature_, &QComboBox::activated, this, [this] { emit changed(); });
    connect(head, &PanelHeader::accepted, this, &InstancePatternPanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &InstancePatternPanel::cancelled);
    head->acceptOnEnter(this);  // Enter in a field — ✓ (§2.1д)
}

void InstancePatternPanel::start(const std::optional<std::string>& editing, const doc::Json& def) {
    editing_ = editing;
    source_ = def.value("source", std::string());
    const std::string m = def.value("mode", std::string("linear"));
    {
        const QSignalBlocker b1(mode_), b2(reverse_);
        mode_->setCurrentIndex(std::max(0, mode_->findData(QString::fromStdString(m))));
        ref_.clear();
        count_->setText(m == "circular" ? "4" : "3");
        spacing_->setText("50");
        angle_->setText("360");
        reverse_->setChecked(false);
        if (m == "linear" && def.contains("directions") && !def["directions"].empty()) {
            const auto& d0 = def["directions"][0];
            ref_ = d0.value("ref", std::string());
            count_->setText(QString::fromStdString(d0.value("count", std::string("3"))));
            spacing_->setText(QString::fromStdString(d0.value("spacing", std::string("50"))));
            reverse_->setChecked(d0.value("reverse", false));
        } else if (m == "circular") {
            ref_ = def.value("axis", std::string());
            count_->setText(QString::fromStdString(def.value("count", std::string("4"))));
            angle_->setText(QString::fromStdString(def.value("angle", std::string("360"))));
            reverse_->setChecked(def.value("reverse", false));
        } else if (m == "feature" && def.contains("feature")) {
            ref_ = def["feature"].value("instance", std::string());
        }
    }
    fillFeatures(m == "feature" && def.contains("feature") ? def["feature"].value("feature", std::string()) : std::string());
    field_ = source_.empty() ? Field::Source : Field::Ref;
    sync();
}

void InstancePatternPanel::setActiveField(Field f) {
    field_ = f;
    sync();
}

void InstancePatternPanel::setSource(const std::string& instance) {
    source_ = instance;
    if (field_ == Field::Source) field_ = Field::Ref;  // next click — the direction (§8а)
    sync();
    emit changed();
}

void InstancePatternPanel::setRef(const std::string& ref) {
    ref_ = ref;
    if (mode() == "feature") fillFeatures({});
    sync();
    emit changed();
}

std::string InstancePatternPanel::mode() const { return mode_->currentData().toString().toStdString(); }

void InstancePatternPanel::fillFeatures(const std::string& selected) {
    const QSignalBlocker b(feature_);
    feature_->clear();
    if (mode() != "feature" || ref_.empty() || !features_) return;
    for (const auto& [id, name] : features_(ref_)) feature_->addItem(name, QString::fromStdString(id));
    if (!selected.empty()) feature_->setCurrentIndex(std::max(0, feature_->findData(QString::fromStdString(selected))));
}

void InstancePatternPanel::sync() {
    const std::string m = mode();
    // One object per field: its caption (the mate captions, app-ux §5.5г) and the reference the view resolves.
    auto items = [&](const std::string& r) {
        return r.empty() ? std::vector<SelectionItem>{} : std::vector<SelectionItem>{{labeler_ ? labeler_(r) : QString::fromStdString(r), {}, r}};
    };
    refBtn_->setPrompt(m == "feature" ? txt("ui.instancePattern.clickAPartWithA") : txt("ui.instancePattern.clickAnEdgeAxisOr"));
    sourceBtn_->setSelection(items(source_));
    sourceBtn_->setToolTip(QString::fromStdString(source_));
    refBtn_->setSelection(items(ref_));
    refBtn_->setToolTip(QString::fromStdString(ref_));
    fields_.setActive(field_ == Field::Source ? sourceBtn_ : refBtn_);
    refLabel_->setText(m == "linear" ? txt("ui.common.direction") : m == "circular" ? txt("ui.common.axis") : txt("ui.instancePattern.partWithPattern"));
    const bool feat = m == "feature";
    feature_->setVisible(feat);
    featureLabel_->setVisible(feat);
    count_->setVisible(!feat);
    countLabel_->setVisible(!feat);
    spacing_->setVisible(m == "linear");
    spacingLabel_->setVisible(m == "linear");
    angle_->setVisible(m == "circular");
    angleLabel_->setVisible(m == "circular");
    reverse_->setVisible(!feat);
    reverseLabel_->setVisible(!feat);
}

bool InstancePatternPanel::ready() const {
    if (source_.empty() || ref_.empty()) return false;
    return mode() != "feature" || feature_->currentIndex() >= 0;
}

doc::Json InstancePatternPanel::definition() const {
    const auto d = model_.session().document().document;
    const std::string m = mode();
    doc::Json def = {{"source", source_}, {"mode", m}};
    auto count = [&] {
        const std::string t = count_->expression();
        return t.empty() ? std::string("1") : t;
    };
    if (m == "linear") {
        doc::Json dir = {{"ref", ref_}, {"count", count()}, {"spacing", spacing_->expression()}};
        if (reverse_->isChecked()) dir["reverse"] = true;
        def["directions"] = doc::Json::array({dir});
    } else if (m == "circular") {
        def["axis"] = ref_;
        def["count"] = count();
        def["angle"] = angle_->expression();
        if (reverse_->isChecked()) def["reverse"] = true;
    } else {
        def["feature"] = {{"instance", ref_}, {"feature", feature_->currentData().toString().toStdString()}};
    }
    return def;
}

}  // namespace construe::app
