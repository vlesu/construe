#include "app/properties_panel.h"

#include "app/app_model.h"
#include "app/expr_input.h"
#include "app/messages.h"
#include "app/panel_header.h"
#include "app/theme.h"
#include "base/number_format.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace construe::app {

namespace {

// PART_COLOR_SWATCHES (app-ux §5.10).
const char* const SWATCHES[] = {"#8C8F93", "#C9CED3", "#B87333", "#C9A44C", "#4F8FCC", "#6DB3A0",
                                "#E0E0D8", "#D9534F", "#F0AD4E", "#5CB85C", "#5B5F63", "#FFFFFF"};
constexpr int SWATCH_PX = 18;

// Row of color swatches: one widget (not a set of buttons); clicking a swatch emits colorPicked.
class SwatchBar : public QWidget {
public:
    explicit SwatchBar(std::function<void(const std::string&)> picked, QWidget* parent = nullptr) : QWidget(parent), picked_(std::move(picked)) {
        setFixedHeight(SWATCH_PX + 4);
        setMinimumWidth(int(std::size(SWATCHES)) * (SWATCH_PX + 3));
        setCursor(Qt::PointingHandCursor);
    }
    QRect swatchRect(int i) const { return QRect(2 + i * (SWATCH_PX + 3), 2, SWATCH_PX, SWATCH_PX); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        for (int i = 0; i < int(std::size(SWATCHES)); ++i) {
            p.setPen(QColor("#666666"));
            p.setBrush(QColor(SWATCHES[i]));
            p.drawRect(swatchRect(i));
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        for (int i = 0; i < int(std::size(SWATCHES)); ++i)
            if (swatchRect(i).contains(e->position().toPoint())) picked_(SWATCHES[i]);
    }

private:
    std::function<void(const std::string&)> picked_;
};

QString fmt(double v, int prec = 3) { return QString::number(v, 'g', prec + 4); }

}  // namespace

PropertiesPanel::PropertiesPanel(AppModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName("propertiesPanel");
    auto* col = new QVBoxLayout(this);
    // Header with the close cross (app-ux §2.1в, FX8-UI-008); properties apply at once, closing only hides the panel.
    auto* head = new PanelHeader(txt("ui.common.properties"), "propertiesClose");
    col->addWidget(head);
    const QFont f = head->title()->font();  // bold, for the section titles below
    formBox_ = new QWidget;
    auto* form = new QFormLayout(formBox_);
    form->setContentsMargins(0, 0, 0, 0);
    form_ = form;
    col->addWidget(formBox_);
    // Assembly instance (app-ux §8а, S2-UX-001).
    instBox_ = new QWidget;
    instBox_->setObjectName("instanceProperties");
    {
        auto* f2 = new QFormLayout(instBox_);
        f2->setContentsMargins(0, 0, 0, 0);
        instName_ = new QLineEdit;
        instName_->setObjectName("instanceName");
        f2->addRow(txt("ui.common.name3"), instName_);
        auto lab = [&](const char* name, const QString& text) {
            auto* l = new QLabel;
            l->setObjectName(name);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            f2->addRow(text, l);
            return l;
        };
        instObject_ = lab("instanceObject", txt("ui.common.object"));
        instDesignation_ = lab("instanceDesignation", txt("ui.common.designation"));
        instMaterial_ = lab("instanceMaterial", txt("ui.common.material"));
        instMass_ = lab("instanceMass", txt("ui.props.massKg"));
        auto chk = [&](const char* name, const QString& text, const char* flag) {
            auto* c = new QCheckBox(text);
            c->setObjectName(name);
            f2->addRow("", c);
            connect(c, &QCheckBox::toggled, this, [this, flag](bool v) {
                if (loading_ || instance_.empty()) return;
                auto& s = model_.session();
                const auto d = s.document().document;
                if (auto r = s.setInstanceFlags(instance_, doc::Json{{flag, v}}); !r) emit message(errorText(r.error(), d.get()), true);
            });
            return c;
        };
        instFixed_ = chk("instanceFixed", txt("ui.props.fixed"), "fixed");
        instHidden_ = chk("instanceHidden", txt("ui.props.hidden"), "hidden");
        instSuppressed_ = chk("instanceSuppressed", txt("ui.props.suppressed"), "suppressed");
        instTransparent_ = chk("instanceTransparent", txt("ui.common.transparent"), "transparent");
        // Instance color (FX12-ASM-003): as «Цвет детали» — swatches, «По детали», #RRGGBB and «…»; stored in the instance.
        {
            auto* row = new QWidget;
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            auto* sw = new SwatchBar([this](const std::string& c) { setInstanceColor(c); });
            sw->setObjectName("instanceSwatches");
            rl->addWidget(sw);
            auto* byPart = new QToolButton;
            byPart->setObjectName("instanceColorByPart");
            byPart->setText(txt("ui.props.byPart"));
            rl->addWidget(byPart);
            f2->addRow(txt("ui.props.instanceColor"), row);
            auto* hexRow = new QWidget;
            auto* hl = new QHBoxLayout(hexRow);
            hl->setContentsMargins(0, 0, 0, 0);
            instColorHex_ = new QLineEdit;
            instColorHex_->setObjectName("instanceColorHex");
            instColorHex_->setPlaceholderText("#RRGGBB");
            hl->addWidget(instColorHex_, 1);
            auto* pickC = new QToolButton;
            pickC->setObjectName("instanceColorPick");
            pickC->setText(QString::fromUtf8("…"));
            pickC->setToolTip(txt("ui.props.chooseColor"));
            hl->addWidget(pickC);
            f2->addRow("", hexRow);
            connect(byPart, &QToolButton::clicked, this, [this] { setInstanceColor(std::nullopt); });
            connect(pickC, &QToolButton::clicked, this, [this] { openInstanceColorDialog(); });
            connect(instColorHex_, &QLineEdit::editingFinished, this, [this] {
                if (loading_) return;
                const QString t = instColorHex_->text().trimmed().toUpper();
                if (!t.isEmpty()) setInstanceColor(t.toStdString());
            });
        }
        auto* open = new QPushButton(txt("ui.props.openObject"));
        open->setObjectName("instanceOpen");
        instOpen_ = open;
        f2->addRow("", open);
        // Instance variables (§8а, S6-PARAM-004).
        instVars_ = new QWidget;
        instVars_->setObjectName("instanceVariables");
        instVarsForm_ = new QFormLayout(instVars_);
        instVarsForm_->setContentsMargins(0, 6, 0, 0);
        f2->addRow(instVars_);
        connect(open, &QPushButton::clicked, this, [this] {
            if (!instance_.empty()) emit openInstance(QString::fromStdString(instance_));
        });
        connect(instName_, &QLineEdit::editingFinished, this, [this] {
            if (loading_ || instance_.empty()) return;
            auto& s = model_.session();
            const auto d = s.document().document;
            for (const auto& in : d->active().instances)
                if (in.id == instance_ && in.name == instName_->text().trimmed().toStdString()) return;
            if (auto r = s.renameInstance(instance_, instName_->text().toStdString()); !r) emit message(errorText(r.error(), d.get()), true);
        });
    }
    col->addWidget(instBox_);
    instBox_->hide();
    name_ = new QLineEdit;
    name_->setObjectName("partName");
    form->addRow(txt("ui.common.name2"), name_);
    designation_ = new QLineEdit;
    designation_->setObjectName("partDesignation");
    designation_->setPlaceholderText(QString::fromUtf8(txt("ui.props.abcd123456001")));
    designation_->setToolTip(txt("ui.props.designationPerGost2201"));
    form->addRow(txt("ui.common.designation"), designation_);
    material_ = new QComboBox;
    material_->setObjectName("partMaterial");
    form->addRow(txt("ui.common.material"), material_);
    ownBox_ = new QWidget;
    auto* own = new QFormLayout(ownBox_);
    own->setContentsMargins(0, 0, 0, 0);
    auto field = [&](const char* name, const QString& label) {
        auto* e = new QLineEdit;
        e->setObjectName(name);
        own->addRow(label, e);
        return e;
    };
    ownName_ = field("ownName", txt("ui.common.name2"));
    ownDensity_ = field("ownDensity", QString::fromUtf8(txt("ui.props.densityKgM")));
    ownE_ = field("ownYoung", txt("ui.props.youngSModulusMpa"));
    ownNu_ = field("ownPoisson", txt("ui.props.poissonSRatio"));
    ownYield_ = field("ownYield", txt("ui.props.yieldStrengthMpa"));
    ownUltimate_ = field("ownUltimate", txt("ui.props.ultimateStrengthMpa"));
    auto* assign = new QPushButton(txt("ui.props.assign"));
    assign->setObjectName("ownAssign");
    own->addRow(assign);
    ownBox_->hide();
    form->addRow(ownBox_);
    // Part color.
    auto* colorRow = new QWidget;
    colorRow_ = colorRow;
    auto* cr = new QHBoxLayout(colorRow);
    cr->setContentsMargins(0, 0, 0, 0);
    auto* bar = new SwatchBar([this](const std::string& c) { setPartColor(c); });
    bar->setObjectName("partSwatches");
    cr->addWidget(bar);
    auto* byMaterial = new QToolButton;
    byMaterial->setObjectName("partColorByMaterial");
    byMaterial->setText(txt("ui.props.byMaterial"));
    cr->addWidget(byMaterial);
    form->addRow(txt("ui.props.partColor"), colorRow);
    colorHex_ = new QLineEdit;
    colorHex_->setObjectName("partColorHex");
    colorHex_->setPlaceholderText("#RRGGBB");
    // Beside the field — a color dialog with the standard mixing palette (S7F-UI-002).
    auto* hexRow = new QWidget;
    auto* hr = new QHBoxLayout(hexRow);
    hr->setContentsMargins(0, 0, 0, 0);
    hr->addWidget(colorHex_, 1);
    auto* pick = new QToolButton;
    pick->setObjectName("partColorPick");
    pick->setText(QString::fromUtf8("…"));
    pick->setToolTip(txt("ui.props.chooseColor"));
    hr->addWidget(pick);
    form->addRow("", hexRow);
    connect(pick, &QToolButton::clicked, this, [this] { openColorDialog(); });
    transparent_ = new QCheckBox(txt("ui.props.transparentInAssemblies"));  // S2-VIEW-004
    transparent_->setObjectName("partTransparent");
    form->addRow("", transparent_);
    connect(transparent_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_) return;
        auto& s = model_.session();
        if (auto r = s.setElementProperties(s.activeElement(), doc::Json{{"transparent", on}}); !r) emit message(errorText(r.error()), true);
    });
    // Face color.
    faceRow_ = new QWidget;
    auto* fr = new QHBoxLayout(faceRow_);
    fr->setContentsMargins(0, 0, 0, 0);
    auto* fbar = new SwatchBar([this](const std::string& c) { setFaceColor(c); });
    fbar->setObjectName("faceSwatches");
    fr->addWidget(fbar);
    auto* reset = new QToolButton;
    reset->setObjectName("faceColorReset");
    reset->setText(txt("ui.props.reset"));
    fr->addWidget(reset);
    faceTitle_ = new QLabel(txt("ui.props.faceColor"));
    form->addRow(faceTitle_, faceRow_);
    faceTitle_->hide();
    faceRow_->hide();
    // Mass properties.
    auto* massTitle = new QLabel(txt("ui.props.massProperties"));
    massTitle->setFont(f);
    form->addRow(massTitle);
    auto ro = [&](const char* name, const QString& label) {
        auto* l = new QLabel;
        l->setObjectName(name);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(label, l);
        return l;
    };
    volume_ = ro("massVolume", QString::fromUtf8(txt("ui.props.volumeMm")));
    area_ = ro("massArea", QString::fromUtf8(txt("ui.props.areaMm")));
    mass_ = ro("massMass", txt("ui.props.massKg"));
    centroid_ = ro("massCentroid", txt("ui.props.centerOfMassMm"));
    moments_ = ro("massMoments", QString::fromUtf8(txt("ui.props.principalMomentsKgMm")));
    missing_ = new QLabel;
    missing_->setObjectName("assemblyMissingMaterial");
    form->addRow(missing_);
    missing_->hide();
    col->addStretch(1);

    connect(head, &PanelHeader::closeRequested, this, &PropertiesPanel::accepted);
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        if (loading_) return;
        auto& s = model_.session();
        const auto d = s.document().document;
        if (name_->text().trimmed().toStdString() == d->active().name) return;
        auto r = s.renameElement(d->active().id, name_->text().toStdString());
        if (!r) emit message(errorText(r.error(), d.get()), true);
    });
    connect(designation_, &QLineEdit::editingFinished, this, [this] {
        if (loading_) return;
        auto& s = model_.session();
        const auto d = s.document().document;
        const std::string t = designation_->text().trimmed().toStdString();
        if (t == d->active().designation.value_or(std::string())) return;
        auto r = s.setElementProperties(d->active().id, doc::Json{{"designation", t.empty() ? doc::Json(nullptr) : doc::Json(t)}});
        if (!r) emit message(errorText(r.error(), d.get()), true);
    });
    connect(material_, &QComboBox::activated, this, [this](int i) { applyMaterialIndex(i); });
    connect(assign, &QPushButton::clicked, this, &PropertiesPanel::applyOwnMaterial);
    connect(byMaterial, &QToolButton::clicked, this, [this] { setPartColor(std::nullopt); });
    connect(colorHex_, &QLineEdit::editingFinished, this, [this] {
        if (loading_) return;
        const QString t = colorHex_->text().trimmed().toUpper();
        if (t.isEmpty()) return;
        setPartColor(t.toStdString());
    });
    connect(reset, &QToolButton::clicked, this, [this] { setFaceColor(std::nullopt); });
}

void PropertiesPanel::setInstanceColor(const std::optional<std::string>& c) {
    if (loading_ || instance_.empty()) return;
    auto& s = model_.session();
    const auto d = s.document().document;
    if (auto r = s.setInstanceColor(instance_, c); !r) emit message(errorText(r.error(), d.get()), true);
}

void PropertiesPanel::openInstanceColorDialog() {
    // As the part color dialog (S7F-UI-002): non-native, non-modal, one at a time.
    for (auto* open : findChildren<QColorDialog*>("instanceColorDialog"))
        if (open->isVisible()) {
            open->raise();
            open->activateWindow();
            return;
        }
    auto* dlg = new QColorDialog(this);
    dlg->setObjectName("instanceColorDialog");
    dlg->setOption(QColorDialog::DontUseNativeDialog);
    dlg->setWindowTitle(txt("ui.props.instanceColor"));
    const QColor current(instColorHex_->text().trimmed());
    dlg->setCurrentColor(current.isValid() ? current : COLOR_BODY.color);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QColorDialog::colorSelected, this, [this](const QColor& c) {
        if (!c.isValid()) return;
        instColorHex_->setText(c.name(QColor::HexRgb).toUpper());
        setInstanceColor(instColorHex_->text().toStdString());
    });
    dlg->setModal(false);
    dlg->show();
}

void PropertiesPanel::openColorDialog() {
    // A non-native, non-modal Qt dialog (S1-UX-001; the same on every system, e2e drives it): basic colors, the
    // spectrum, HTML. OK — the color goes to the field and is applied as a typed one; Cancel — nothing. One at a time.
    for (auto* open : findChildren<QColorDialog*>("partColorDialog"))
        if (open->isVisible()) {
            open->raise();
            open->activateWindow();
            return;
        }
    auto* dlg = new QColorDialog(this);
    dlg->setObjectName("partColorDialog");
    dlg->setOption(QColorDialog::DontUseNativeDialog);
    dlg->setWindowTitle(txt("ui.props.partColor"));
    const QColor current(colorHex_->text().trimmed());
    dlg->setCurrentColor(current.isValid() ? current : COLOR_BODY.color);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QColorDialog::colorSelected, this, [this](const QColor& c) {
        if (!c.isValid()) return;
        colorHex_->setText(c.name(QColor::HexRgb).toUpper());
        setPartColor(colorHex_->text().toStdString());
    });
    dlg->setModal(false);
    dlg->show();
}

void PropertiesPanel::refresh() {
    loading_ = true;
    // Assembly instance (S2-UX-001): name, object, composition (read-only), checkboxes.
    {
        auto& s = model_.session();
        const auto d = s.document().document;
        const doc::Instance* in = nullptr;
        if (!instance_.empty() && d->active().isAssembly())
            for (const auto& x : d->active().instances)
                if (x.id == instance_) in = &x;
        if (!in) instance_.clear();
        formBox_->setVisible(!in);
        instBox_->setVisible(in != nullptr);
        if (in) {
            faceTitle_->hide();
            faceRow_->hide();
            instName_->setText(QString::fromStdString(in->name));
            const int ei = d->elementIndex(in->refElement);
            const doc::LibraryPart* lp = s.instanceLibraryPart(in->id);  // library element: its title (§8г)
            instOpen_->setText(lp ? txt("ui.library.replaceDots") : txt("ui.props.openObject"));  // FX10-LIB-004
            instObject_->setText(lp ? QString::fromStdString(s.libraryText(lp->title))  // library.md §17.2
                                    : in->refFile.empty() && ei >= 0 ? QString::fromStdString(d->elements[size_t(ei)].name)
                                                                     : QString::fromStdString(in->refFile));
            const QString dash = QString::fromUtf8("—");
            if (auto inf = s.instanceInfo(in->id)) {
                instDesignation_->setText(inf->designation ? QString::fromStdString(*inf->designation) : dash);
                instMaterial_->setText(inf->material ? QString::fromStdString(*inf->material) : dash);
                instMass_->setText(inf->mass ? formatNumber(*inf->mass) : dash);
            } else {
                instDesignation_->setText(dash), instMaterial_->setText(dash), instMass_->setText(dash);
            }
            instFixed_->setChecked(in->fixed);
            instHidden_->setChecked(in->hidden);
            instSuppressed_->setChecked(in->suppressed);
            instTransparent_->setChecked(in->transparent);
            instColorHex_->setText(in->color ? QString::fromStdString(*in->color) : QString());
            refreshInstanceVariables();
            loading_ = false;
            return;
        }
    }
    auto& s = model_.session();
    const auto d = s.document().document;
    const auto& el = d->active();
    name_->setText(QString::fromStdString(el.name));
    designation_->setText(QString::fromStdString(el.designation.value_or(std::string())));
    transparent_->setChecked(el.transparent);
    material_->clear();
    material_->addItem(QString::fromUtf8(txt("ui.props.notSet")), QString());
    int current = 0;
    for (const auto& m : api::Session::materialLibrary()["materials"]) {
        material_->addItem(QString::fromStdString(m["name"].get<std::string>()), QString::fromStdString(m["id"].get<std::string>()));
        if (el.material && el.material->contains("id") && (*el.material)["id"] == m["id"]) current = material_->count() - 1;
    }
    if (el.material && !el.material->contains("id")) {
        material_->addItem(QString::fromStdString((*el.material)["name"].get<std::string>()), QString("__current__"));
        current = material_->count() - 1;
    }
    material_->addItem(QString::fromUtf8(txt("ui.props.custom")), QString("__own__"));
    material_->setCurrentIndex(current);
    colorHex_->setText(QString::fromStdString(el.color.value_or(std::string())));
    // Assembly (§8а, S2-ASM-009): no material, color or transparency; mass properties are the assembly's.
    const bool asmEl = el.isAssembly();
    for (QWidget* w : {static_cast<QWidget*>(material_), colorRow_, static_cast<QWidget*>(colorHex_), static_cast<QWidget*>(transparent_)})
        form_->setRowVisible(w, !asmEl);
    if (asmEl) ownBox_->hide();
    missing_->setVisible(false);
    if (asmEl) {
        if (auto am = s.assemblyMassProperties()) {
            const auto& mp = am->props;
            volume_->setText(fmt(mp.volume));
            area_->setText(fmt(mp.area));
            mass_->setText(mp.mass ? fmt(*mp.mass) : QString::fromUtf8("—"));
            centroid_->setText(QString("%1; %2; %3").arg(fmt(mp.centroid.x())).arg(fmt(mp.centroid.y())).arg(fmt(mp.centroid.z())));
            moments_->setText(mp.mass ? QString("%1; %2; %3").arg(fmt(mp.principalMoments[0])).arg(fmt(mp.principalMoments[1])).arg(fmt(mp.principalMoments[2]))
                                      : QString::fromUtf8("—"));
            if (am->missingMaterial > 0) {
                missing_->setText(QString(txt("ui.props.noMaterialN")).arg(am->missingMaterial));
                missing_->setVisible(true);
            }
        }
        loading_ = false;
        return;
    }
    if (auto mp = s.massProperties(api::StateRef::final())) {
        volume_->setText(fmt(mp->volume));
        area_->setText(fmt(mp->area));
        mass_->setText(mp->mass ? fmt(*mp->mass) : QString::fromUtf8("—"));
        centroid_->setText(QString("%1; %2; %3").arg(fmt(mp->centroid.x())).arg(fmt(mp->centroid.y())).arg(fmt(mp->centroid.z())));
        if (mp->density)
            moments_->setText(QString("%1; %2; %3")
                                  .arg(fmt(mp->principalMoments[0] * *mp->density))
                                  .arg(fmt(mp->principalMoments[1] * *mp->density))
                                  .arg(fmt(mp->principalMoments[2] * *mp->density)));
        else
            moments_->setText(QString::fromUtf8("—"));
    }
    loading_ = false;
}

void PropertiesPanel::setFace(const std::string& face) {
    face_ = face;
    faceTitle_->setVisible(!face.empty());
    faceRow_->setVisible(!face.empty());
}

void PropertiesPanel::applyMaterialIndex(int index) {
    const QString id = material_->itemData(index).toString();
    if (id == "__own__") {
        ownBox_->show();
        return;
    }
    ownBox_->hide();
    if (id == "__current__") return;
    auto& s = model_.session();
    const auto d = s.document().document;
    doc::Json mat = nullptr;
    for (const auto& m : api::Session::materialLibrary()["materials"])
        if (QString::fromStdString(m["id"].get<std::string>()) == id) {
            mat = m;
            mat.erase("standard");
        }
    auto r = s.setElementProperties(d->active().id, doc::Json{{"material", mat}});
    if (!r) emit message(errorText(r.error(), d.get()), true);
}

void PropertiesPanel::applyOwnMaterial() {
    auto num = [](QLineEdit* e, bool& ok) {
        const auto v = parseDecimal(e->text());
        ok = v.has_value();
        return v.value_or(0);
    };
    bool ok1 = false, ok2 = false, ok3 = false, ok4 = false, ok5 = true;
    const double rho = num(ownDensity_, ok1), E = num(ownE_, ok2), nu = num(ownNu_, ok3), su = num(ownUltimate_, ok4);
    doc::Json yield = nullptr;
    if (!ownYield_->text().trimmed().isEmpty()) yield = num(ownYield_, ok5);
    auto& s = model_.session();
    const auto d = s.document().document;
    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5) {
        emit message(txt("ui.props.materialPropertiesMustBeNumbers"), true);
        return;
    }
    doc::Json mat = {{"name", ownName_->text().trimmed().toStdString()}, {"density", rho}, {"youngModulus", E}, {"poisson", nu},
                     {"yieldStrength", yield}, {"tensileStrength", su}, {"color", "#8C8F93"}};
    auto r = s.setElementProperties(d->active().id, doc::Json{{"material", mat}});
    if (!r) emit message(errorText(r.error(), d.get()), true);
    else ownBox_->hide();
}

void PropertiesPanel::setPartColor(const std::optional<std::string>& c) {
    auto& s = model_.session();
    const auto d = s.document().document;
    auto r = s.setElementProperties(d->active().id, doc::Json{{"color", c ? doc::Json(*c) : doc::Json(nullptr)}});
    if (!r) emit message(errorText(r.error(), d.get()), true);
}

void PropertiesPanel::setFaceColor(const std::optional<std::string>& c) {
    if (face_.empty()) return;
    auto& s = model_.session();
    auto r = s.setFaceColor(face_, c);
    if (!r) emit message(errorText(r.error(), s.document().document.get()), true);
}

}  // namespace construe::app

namespace construe::app {

void PropertiesPanel::refreshInstanceVariables() {
    auto& s = model_.session();
    auto iv = s.instanceVariables(instance_);
    if (!iv || iv->variables.empty()) {
        instVars_->hide();
        instVarsKey_.clear();
        return;
    }
    instVars_->show();
    // Structure: instance, tables with rows, variables (with help and defaults).
    // The library texts (table name, variable help) are shown in the interface language (library.md §17.2): a language
    // switch rebuilds the form.
    const bool lib = s.instanceLibraryPart(instance_) != nullptr;
    auto libText = [&](const std::string& t) { return QString::fromStdString(lib ? s.libraryText(t) : t); };
    std::string key = instance_ + "|" + iv->qualifier + "|" + s.libraryLanguage();
    for (const auto& t : iv->tables) {
        key += "|t" + t.id + ":" + t.active;
        for (const auto& r : t.variants) key += "," + r.name;
    }
    for (const auto& v : iv->variables) key += "|v" + v.id + ":" + v.name + ":" + v.expr + ":" + v.comment.value_or("");
    if (key != instVarsKey_) {
        instVarsKey_ = key;
        while (instVarsForm_->rowCount() > 0) instVarsForm_->removeRow(0);
        variantBoxes_.clear();
        overrideEdits_.clear();
        valueLabels_.clear();
        instQualifier_ = new QLabel(text("ui.variant.qualifier", {{"name", QString::fromStdString(iv->qualifier)}}));
        instQualifier_->setObjectName("instanceQualifier");
        instQualifier_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        instVarsForm_->addRow(instQualifier_);
        for (const auto& t : iv->tables) {
            auto* box = new QComboBox;
            box->setObjectName(QString::fromStdString("instanceVariant_" + t.id));
            box->addItem(text("ui.variant.instanceDefault", {{"name", QString::fromStdString(t.active)}}), QString());
            for (const auto& r : t.variants) box->addItem(QString::fromStdString(r.name), QString::fromStdString(r.name));
            connect(box, &QComboBox::activated, this, [this](int) {
                if (!loading_) applyInstanceVariables();
            });
            instVarsForm_->addRow(lib && t.name == library::LIB_SIZES_TABLE_NAME ? text("ui.library.sizesTable") : QString::fromStdString(t.name), box);
            variantBoxes_[t.id] = box;
        }
        for (const auto& v : iv->variables) {
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* edit = new QLineEdit;
            edit->setObjectName(QString::fromStdString("instanceOverride_" + v.name));
            edit->setPlaceholderText(QString::fromStdString(v.expr));
            // Defined by the standard and the size (FX10-LIB-006): only the size row changes it.
            if (iv->standard.count(v.id)) {
                edit->setReadOnly(true);
                edit->setEnabled(false);
                edit->setPlaceholderText(text("ui.library.byStandard"));
            }
            auto* val = new QLabel;
            val->setObjectName(QString::fromStdString("instanceValue_" + v.name));
            val->setMinimumWidth(60);
            h->addWidget(edit, 1);
            h->addWidget(val);
            auto* label = new QLabel(QString::fromStdString(v.name));
            if (v.comment) {
                label->setToolTip(libText(*v.comment));
                edit->setToolTip(libText(*v.comment));
            }
            connect(edit, &QLineEdit::editingFinished, this, [this] {
                if (!loading_) applyInstanceVariables();
            });
            instVarsForm_->addRow(label, row);
            overrideEdits_[v.id] = edit;
            valueLabels_[v.id] = val;
        }
    }
    // Values: the chosen rows, formulas (an edited field is not overwritten) and the values Q.v.
    for (auto& [tid, box] : variantBoxes_) {
        QString row;
        for (const auto& r : iv->variantRows)
            if (r.table == tid) row = QString::fromStdString(r.row);
        const int i = box->findData(row);
        box->setCurrentIndex(i < 0 ? 0 : i);
    }
    const auto vars = s.variableValues();
    const auto units = s.units();
    for (const auto& v : iv->variables) {
        QLineEdit* edit = overrideEdits_[v.id];
        QString formula;
        for (const auto& o : iv->overrides)
            if (o.variable == v.id) formula = QString::fromStdString(o.expr);
        if (!edit->hasFocus()) edit->setText(formula);
        QLabel* val = valueLabels_[v.id];
        const expr::VariableEntry* e = vars.find(iv->qualifier + "." + v.name);
        val->setToolTip(QString());
        if (e && e->isString()) {
            val->setText(QString::fromStdString(e->text));
        } else if (e && e->value.ok()) {
            val->setText(QString::fromStdString(expr::format(*e->value, units, UI_DECIMALS)));
        } else {
            val->setText(QString::fromUtf8("ⓧ"));
            if (e) val->setToolTip(errorText(e->value.error(), s.document().document.get()));
        }
    }
}

void PropertiesPanel::applyInstanceVariables() {
    if (instance_.empty()) return;
    std::vector<doc::Instance::VariantRow> rows;
    for (const auto& [tid, box] : variantBoxes_)
        if (const QString r = box->currentData().toString(); !r.isEmpty()) rows.push_back({tid, r.toStdString()});
    std::vector<doc::Instance::Override> overrides;
    for (const auto& [vid, edit] : overrideEdits_) {
        const std::string t = normalizeExprInput(edit->text());  // an expression field (§5.6)
        if (!t.empty()) overrides.push_back({vid, t});
    }
    auto& s = model_.session();
    const auto d = s.document().document;
    for (const auto& in : d->active().instances)
        if (in.id == instance_ && in.variantRows == rows && in.overrides == overrides) return;
    if (auto r = s.setInstanceVariables(instance_, rows, overrides); !r) emit message(errorText(r.error(), d.get()), true);
}

}  // namespace construe::app
