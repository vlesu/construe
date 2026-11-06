#include "app/mate_panel.h"
#include "app/panel_header.h"

#include "app/app_model.h"
#include "app/feature_panel.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QFormLayout>
#include <QVBoxLayout>

namespace construe::app {

namespace {
const std::pair<const char*, const char*> TYPES[] = {{"coincident", "ui.common.coincident"}, {"concentric", "ui.common.concentric"}, {"distance", "ui.common.distance"},
                                                     {"angle", "ui.common.angle"}, {"parallel", "ui.common.parallel"}, {"perpendicular", "ui.common.perpendicular"},
                                                     {"tangent", "ui.mate.tangent"}, {"gear", "ui.mate.gear"}, {"lock", "ui.mate.lock"}};
}

MatePanel::MatePanel(AppModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName("matePanel");
    auto* col = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(txt("ui.common.mate"), PanelHeader::CommandButtons{"mateOk", "mateCancel"});
    col->addWidget(head);
    seriesHint_ = new QLabel;
    seriesHint_->setObjectName("mateSeriesHint");
    seriesHint_->setWordWrap(true);
    seriesHint_->hide();
    col->addWidget(seriesHint_);
    auto* form = new QFormLayout;
    col->addLayout(form);
    refsLabel_ = new SelectionFieldWidget("mateRefs", txt("ui.mate.clickTwoObjectsInThe"), this);  // §5.5, S7F-RF-009
    refsLabel_->setRequired(true);
    refsLabel_->addTo(form, txt("ui.mate.objects"));
    refItems_ = refsLabel_->items();  // ✕, hover, centering (S7F-UI-001)
    connect(refsLabel_, &SelectionFieldWidget::removed, this, [this](int i) {
        if (i < int(refs_.size())) toggleRef(refs_[size_t(i)]);
    });
    type_ = new QComboBox;
    type_->setObjectName("mateType");
    for (const auto& [id, label] : TYPES) type_->addItem(QString::fromUtf8(txt(label)), QString(id));
    form->addRow(txt("ui.common.type"), type_);
    value_ = new ExprField;
    value_->setChecker(&model_.session(), expr::LENGTH);
    value_->setObjectName("mateValue");
    valueLabel_ = new QLabel(txt("ui.common.value"));
    form->addRow(valueLabel_, value_);
    construction_ = new QCheckBox(txt("ui.mate.partConstructionGeometry"));  // FX9-ASM-001: planes, axes, sketches of the parts
    construction_->setObjectName("mateConstruction");
    construction_->setToolTip(txt("ui.mate.showPartPlanesAxesAnd"));
    col->addWidget(construction_);
    connect(construction_, &QCheckBox::toggled, this, [this](bool on) { emit constructionToggled(on); });
    turn_ = new QPushButton(txt("ui.mate.flip"));
    turn_->setObjectName("mateTurn");
    turn_->setToolTip(txt("ui.mate.flipThePartToThe"));
    turn_->hide();
    col->addWidget(turn_);
    status_ = new QLabel;
    status_->setObjectName("mateStatus");
    status_->setWordWrap(true);
    status_->hide();
    col->addWidget(status_);
    typeHint_ = new QLabel;  // the type adjusted to the pair (FX9-ASM-002)
    typeHint_->setObjectName("mateTypeHint");
    typeHint_->setWordWrap(true);
    typeHint_->setStyleSheet(QString("color: %1").arg(COLOR_WARNING.color.name()));
    typeHint_->hide();
    col->addWidget(typeHint_);
    next_ = new QPushButton(txt("ui.mate.okNext"));
    next_->setObjectName("mateNext");
    next_->setToolTip(txt("ui.mate.saveTheMateAndStart"));
    next_->hide();
    col->addWidget(next_);
    col->addStretch(1);
    connect(next_, &QPushButton::clicked, this, [this] { emit nextRequested(); });
    connect(type_, &QComboBox::activated, this, [this] {
        typeChosen_ = true;  // the user's type is not adjusted any more (FX9-ASM-002)
        setTypeHint({});
        sync();
        emit changed();
    });
    connect(value_, &QLineEdit::textEdited, this, [this] { emit changed(); });
    connect(turn_, &QPushButton::clicked, this, [this] {
        turned_ = !turned_;
        emit changed();
    });
    connect(head, &PanelHeader::accepted, this, &MatePanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &MatePanel::cancelled);
    head->acceptOnEnter(this);  // Enter in a field — ✓ (§2.1д)
}

void MatePanel::start(const std::optional<std::string>& editing, const std::vector<std::string>& refs, const std::string& type,
                      const std::string& value) {
    editing_ = editing;
    turned_ = false;
    refs_ = refs;
    typeChosen_ = editing.has_value() || !type.empty();
    setTypeHint({});
    if (refs_.size() > 2) refs_.resize(2);
    {
        const QSignalBlocker b1(type_);
        type_->setCurrentIndex(std::max(0, type_->findData(QString::fromStdString(type.empty() ? "coincident" : type))));
        value_->setText(QString::fromStdString(value));
    }
    sync();
}

void MatePanel::setStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(error ? QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()) : QString());
    status_->setVisible(!text.isEmpty());
}

void MatePanel::setNextVisible(bool v) { next_->setVisible(v); }

void MatePanel::setTurnAvailable(bool v) {
    turn_->setVisible(v);
    if (!v) turned_ = false;
}

void MatePanel::setSeriesHint(const QString& text) {
    seriesHint_->setText(text);
    seriesHint_->setVisible(!text.isEmpty());
}

void MatePanel::toggleRef(const std::string& ref) {
    auto it = std::find(refs_.begin(), refs_.end(), ref);
    if (it != refs_.end()) refs_.erase(it);
    else if (refs_.size() < 2) refs_.push_back(ref);
    setTypeHint({});
    sync();
    emit changed();
}

void MatePanel::suggestType(const std::string& type) {
    const QSignalBlocker b(type_);
    type_->setCurrentIndex(std::max(0, type_->findData(QString::fromStdString(type))));
    sync();
}

std::string MatePanel::type() const { return type_->currentData().toString().toStdString(); }

QString MatePanel::typeLabel(const std::string& type) {
    for (const auto& [id, label] : TYPES)
        if (type == id) return QString::fromUtf8(txt(label));
    return QString::fromStdString(type);
}

void MatePanel::adjustType(const std::string& type, const QString& hint, const std::optional<QString>& value) {
    if (value) value_->setText(*value);
    suggestType(type);
    setTypeHint(hint);
}

void MatePanel::setTypeHint(const QString& text) {
    typeHint_->setText(text);
    typeHint_->setVisible(!text.isEmpty());
}

void MatePanel::sync() {
    QStringList full;
    std::vector<SelectionItem> items;
    for (const auto& r : refs_) {
        full << QString::fromStdString(r);
        items.push_back({labeler_ ? labeler_(r) : QString::fromStdString(r), {}, r});
    }
    refsLabel_->setSelection(std::move(items));
    refsLabel_->setToolTip(full.join("\n"));  // the references themselves (diagnostics of an assembly reference)
    const QString t = type_->currentData().toString();
    const bool needsValue = t == "distance" || t == "angle";
    value_->setDim(t == "angle" ? expr::ANGLE : expr::LENGTH);
    value_->setVisible(needsValue);
    valueLabel_->setVisible(needsValue);
    if (needsValue && value_->text().isEmpty()) value_->setText("0");
}

doc::Json MatePanel::definition() const {
    doc::Json def = doc::Json::object();
    def["type"] = type_->currentData().toString().toStdString();
    def["refs"] = doc::Json::array();
    for (const auto& r : refs_) def["refs"].push_back(r);
    const QString t = type_->currentData().toString();
    if (t == "distance" || t == "angle") {
        const auto d = model_.session().document().document;
        def["value"] = value_->expression();
    }
    if (turned_) def["flip"] = true;
    return def;
}

}  // namespace construe::app
