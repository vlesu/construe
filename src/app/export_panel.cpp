// STL, 3MF and flat pattern DXF export panel (app/export_panel.h).
#include "app/export_panel.h"
#include "app/panel_header.h"
#include "app/choice_row.h"

#include "app/feature_panel.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <numbers>

namespace construe::app {

namespace {
// Document length unit label for field captions (§10.3).
QString lengthUnitLabel(const std::string& u) {
    if (u == "mm") return txt("ui.common.mm2");
    if (u == "cm") return txt("ui.common.cm");
    if (u == "m") return txt("ui.common.m");
    return QString::fromStdString(u);
}
}  // namespace

ExportPanel::ExportPanel(api::Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    setObjectName("exportPanel");
    auto* ev = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(txt("ui.common.export"), PanelHeader::CommandButtons{"exportOk", "exportCancel"});
    title_ = head->title();
    ok_ = head->okButton();
    ev->addWidget(head);
    auto* form = new QFormLayout;
    form_ = form;
    // File, object, contents, file per part (§10.3, S2-IO-001…003, S2-IO-005; flat pattern DXF — S5-DXF-001).
    auto* file = new ChoiceRow({{"stl", "STL", "exportFileStl"}, {"3mf", "3MF", "exportFile3mf"}, {"dxf", txt("ui.export.flatPatternDxf"), "exportFileFlatDxf"}});
    fileStl_ = file->button("stl");
    file3mf_ = file->button("3mf");
    fileFlat_ = file->button("dxf");
    form->addRow(txt("ui.common.file"), file);
    object_ = new QComboBox;
    object_->setObjectName("exportObject");
    form->addRow(txt("ui.common.object"), object_);
    items_ = new CheckList("exportItems");
    items_->setMaximumHeight(120);
    form->addRow(txt("ui.export.contents"), items_);
    perPart_ = new QCheckBox(txt("ui.export.filePerPart"));
    perPart_->setObjectName("exportPerPart");
    form->addRow("", perPart_);
    bendLines_ = new QCheckBox(txt("ui.export.bendLines"));
    bendLines_->setObjectName("exportBendLines");
    bendLines_->setChecked(true);
    form->addRow("", bendLines_);
    print_ = new QCheckBox;
    print_->setObjectName("exportPrint");
    setPrinterClearance(clearance_);
    form->addRow("", print_);
    // STL format: a separate row of choices (not shared with "File").
    auto* fmtW = new ChoiceRow({{"binary", txt("ui.export.binary"), "exportBinary"}, {"ascii", "ASCII", "exportAscii"}});
    binary_ = fmtW->button("binary");
    ascii_ = fmtW->button("ascii");
    formatRow_ = fmtW;
    form->addRow(txt("ui.export.format"), fmtW);
    precision_ = new QComboBox;
    precision_->setObjectName("exportPrecision");
    precision_->addItems({txt("ui.export.coarse"), txt("ui.export.normal"), txt("ui.export.fine"), txt("ui.export.custom")});
    precision_->setCurrentIndex(1);
    form->addRow(txt("ui.export.accuracy"), precision_);
    chord_ = new ExprField;  // checked by the panel together with the kernel bounds (update)
    chord_->setObjectName("exportChord");
    chordLabel_ = new QLabel;
    form->addRow(chordLabel_, chord_);
    angle_ = new ExprField;
    angle_->setObjectName("exportAngle");
    form->addRow(QString::fromUtf8(txt("ui.export.angle")), angle_);
    connect(precision_, &QComboBox::currentIndexChanged, this, [this] {
        update();
        runCheck();
    });
    connect(chord_, &QLineEdit::textEdited, this, &ExportPanel::update);
    connect(angle_, &QLineEdit::textEdited, this, &ExportPanel::update);
    connect(fileStl_, &QAbstractButton::toggled, this, &ExportPanel::update);
    connect(print_, &QCheckBox::toggled, this, [this] {
        update();
        runCheck();
    });
    connect(fileFlat_, &QAbstractButton::toggled, this, [this] {
        fillItems();
        update();
        runCheck();
    });
    connect(object_, &QComboBox::currentIndexChanged, this, [this] {
        fillItems();
        update();
        runCheck();
    });
    connect(items_, &CheckList::changed, this, [this] {
        update();
        runCheck();
    });
    ev->addLayout(form);
    check_ = new QLabel;
    check_->setObjectName("exportCheck");
    check_->setWordWrap(true);
    ev->addWidget(check_);
    ev->addStretch(1);
    connect(head, &PanelHeader::accepted, this, &ExportPanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &ExportPanel::cancelled);
}

std::optional<std::pair<double, double>> ExportPanel::params(QString* err, QLineEdit** bad) const {
    const int i = precision_->currentIndex();
    if (i == 3) {
        // «Custom»: field expressions, kernel bounds (kernel-interface §3.7).
        const std::string unit = session_.document().document->lengthUnit;  // copy: document() is a temporary snapshot
        const auto c = session_.checkExpression(chord_->expression(), expr::LENGTH);
        if (bad) *bad = chord_;
        if (!c) {
            if (err) *err = errorText(c.error());
            return std::nullopt;
        }
        if (!(c->value >= kernel::KERNEL_MIN_CHORD && c->value <= kernel::KERNEL_MAX_CHORD)) {
            if (err) {
                auto toDoc = [&](double mm) {
                    auto q = session_.checkExpression(QString::number(mm, 'g', 17).toStdString() + " mm", expr::LENGTH);
                    return q ? formatNumber(q->valueInDocUnits) + " " + lengthUnitLabel(unit) : QString();
                };
                *err = text("ui.export.chordRange", {{"min", toDoc(kernel::KERNEL_MIN_CHORD)}, {"max", toDoc(kernel::KERNEL_MAX_CHORD)}});
            }
            return std::nullopt;
        }
        if (bad) *bad = angle_;
        std::string at = angle_->expression();
        if (QRegularExpression("^\\s*[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?\\s*$").match(QString::fromStdString(at)).hasMatch()) at += " deg";
        const auto a = session_.checkExpression(at, expr::ANGLE);
        if (!a) {
            if (err) *err = errorText(a.error());
            return std::nullopt;
        }
        if (!(a->value >= kernel::KERNEL_MIN_ANGLE_TOL && a->value <= std::numbers::pi / 2 + 1e-12)) {
            if (err) *err = text("ui.export.angleRange", {{"min", formatNumber(qRadiansToDegrees(kernel::KERNEL_MIN_ANGLE_TOL)) + QString::fromUtf8("°")}});
            return std::nullopt;
        }
        if (bad) *bad = nullptr;
        return std::make_pair(c->value, std::min(a->value, std::numbers::pi / 2));
    }
    const auto normal = session_.stlDefaultTolerances();
    if (i == 1) return std::make_pair(normal.chordTol, normal.angleTol);
    // Coarse/Fine are derived from the "Normal" values (§10.3).
    const bool coarse = i == 0;
    return std::make_pair(std::clamp(normal.chordTol * (coarse ? STL_COARSE_FACTOR : STL_FINE_FACTOR), kernel::KERNEL_MIN_CHORD, kernel::KERNEL_MAX_CHORD),
                          qDegreesToRadians(coarse ? STL_COARSE_ANGLE_DEG : STL_FINE_ANGLE_DEG));
}

// Contents (§10.3): top-level assembly instances or bodies of the active part; for an inactive part — "All bodies".
void ExportPanel::fillItems() { items_->setItems(contentRows()); }

std::vector<CheckList::Item> ExportPanel::contentRows() const {
    std::vector<CheckList::Item> rows;
    const auto d = session_.document().document;
    const int ei = d->elementIndex(object_->currentData().toString().toStdString());
    if (ei < 0) return rows;
    const auto& el = d->elements[size_t(ei)];
    auto add = [&](const QString& text, const QString& id, bool enabled) { rows.push_back({id, text, true, enabled}); };
    if (flat()) {
        // Sheet metal parts of the object (§10.3 «Flat pattern DXF», core-api §4.7е); an error is shown by the check.
        api::Session::FlatExportOptions o;
        o.element = el.id;
        if (auto parts = session_.flatParts(o))
            for (const auto& p : *parts) add(QString::fromStdString(p.name), QString::fromStdString(p.key), true);
        return rows;
    }
    if (el.isAssembly()) {
        for (const auto& in : el.instances)
            if (!in.suppressed) add(QString::fromStdString(in.name), QString::fromStdString(in.id), true);
    } else if (el.id == d->active().id) {
        if (auto bs = session_.bodies(api::StateRef::final()))
            for (size_t i = 0; i < bs->size(); ++i) add(QString(txt("ui.export.bodyN")).arg(i + 1), QString::fromStdString((*bs)[i].bodyId), true);
    } else {
        add(txt("ui.export.allBodies"), QString(), false);
    }
    return rows;
}

std::optional<api::Session::MeshExportOptions> ExportPanel::meshOptions() const {
    const auto p = params();
    if (!p) return std::nullopt;
    api::Session::MeshExportOptions o;
    o.fileFormat = file3mf_->isChecked() ? io::MeshFileFormat::ThreeMf : io::MeshFileFormat::Stl;
    o.stlFormat = binary_->isChecked() ? io::StlFormat::Binary : io::StlFormat::Ascii;
    if (precision_->currentIndex() != 1) {
        o.chordalDeviation = p->first;
        o.angularDeviation = p->second;
    }
    o.element = object_->currentData().toString().toStdString();
    std::vector<std::string> ids;
    for (const auto& id : items_->checkedIds())
        if (!id.empty()) ids.push_back(id);
    const int checked = items_->checkedCount();
    if (checked == 0) return std::nullopt;
    if (checked < items_->count()) o.items = ids;  // everything checked — "all"
    o.perPart = !file3mf_->isChecked() && perPart_->isChecked();
    if (print_->isChecked()) {
        printprep::Options po;
        po.clearance = clearance_;
        o.print = po;
    }
    return o;
}

void ExportPanel::setPrinterClearance(double mm) {
    clearance_ = mm;
    print_->setText(text("ui.export.print", {{"clearance", formatNumber(mm)}}));
}

// Mesh check (§10.3, S2-IO-002): report below the fields; does not block export.
void ExportPanel::runCheck() {
    if (flat()) {
        // Contours of the selected sheet metal parts (§10.3 «Flat pattern DXF»): a line per part or the error text.
        flatOk_ = false;
        api::Session::FlatExportOptions o;
        o.element = object_->currentData().toString().toStdString();
        o.items = items_->checkedIds();
        const bool any = !o.items.empty();
        if (items_->count() > 0 && !any) {
            check_->clear();
            ok_->setEnabled(false);
            return;
        }
        auto r = session_.flatParts(o);
        if (!r) {
            check_->setText(errorText(r.error()));
            check_->setStyleSheet(QString("color: %1;").arg(COLOR_WARNING.color.name()));
            ok_->setEnabled(false);
            return;
        }
        QStringList lines;
        for (const auto& p : *r)
            lines << text("ui.export.flatOk", {{"name", QString::fromStdString(p.name)}, {"loops", QString::number(p.loops)}, {"area", formatNumber(p.area)}});
        check_->setText(lines.join("\n"));
        check_->setStyleSheet(QString());
        flatOk_ = true;
        update();
        return;
    }
    const auto o = meshOptions();
    if (!o) {
        check_->clear();
        return;
    }
    auto r = session_.meshReport(*o);
    if (!r) {
        check_->setText(errorText(r.error()));
        check_->setStyleSheet(QString("color: %1;").arg(COLOR_WARNING.color.name()));
        return;
    }
    QStringList bad;
    for (const auto& c : *r)
        if (!c.ok())
            bad << text("ui.export.checkBad", {{"name", QString::fromStdString(c.name)}, {"open", QString::number(c.openEdges)},
                                               {"flipped", QString::number(c.flippedEdges)}, {"nonManifold", QString::number(c.nonManifoldEdges)},
                                               {"degenerate", QString::number(c.degenerate)}});
    check_->setText(bad.isEmpty() ? text("ui.export.checkOk") : bad.join("\n"));
    check_->setStyleSheet(bad.isEmpty() ? QString() : QString("color: %1;").arg(COLOR_WARNING.color.name()));
}

std::optional<api::Session::FlatExportOptions> ExportPanel::flatOptions() const {
    if (!flat() || !flatOk_) return std::nullopt;
    api::Session::FlatExportOptions o;
    o.element = object_->currentData().toString().toStdString();
    const std::vector<std::string> ids = items_->checkedIds();
    if (ids.empty()) return std::nullopt;
    if (int(ids.size()) < items_->count()) o.items = ids;  // everything checked — "all"
    o.bendLines = bendLines_->isChecked();
    o.perPart = perPart_->isChecked();
    return o;
}

std::optional<io::StlOptions> ExportPanel::options() const {
    const auto p = params();
    if (!p) return std::nullopt;
    io::StlOptions o;
    o.format = binary_->isChecked() ? io::StlFormat::Binary : io::StlFormat::Ascii;
    if (precision_->currentIndex() != 1) {
        o.chordalDeviation = p->first;
        o.angularDeviation = p->second;
    }
    return o;
}

void ExportPanel::reset(Kind kind) {
    // Format "Binary", precision "Normal"; "Custom" fields — the "Normal" values in document units.
    const auto d = session_.document().document;  // keep the snapshot
    const std::string unit = d->lengthUnit;
    chordLabel_->setText(txt("ui.export.chord") + lengthUnitLabel(unit));
    {
        QSignalBlocker b(precision_), b2(object_), b3(fileStl_), b4(file3mf_), b5(perPart_), b6(fileFlat_), b7(bendLines_), b8(print_);
        (kind == Kind::ThreeMf ? file3mf_ : kind == Kind::FlatDxf ? fileFlat_ : fileStl_)->setChecked(true);
        perPart_->setChecked(false);
        bendLines_->setChecked(true);
        print_->setChecked(false);
        object_->clear();
        for (const auto& e : d->elements) object_->addItem(QString::fromStdString(e.name), QString::fromStdString(e.id));
        object_->setCurrentIndex(std::max(0, object_->findData(QString::fromStdString(d->active().id))));
        fillItems();
        binary_->setChecked(true);  // §10.3 defaults on every open
        precision_->setCurrentIndex(1);
        if (const auto n = params()) {
            auto q = session_.checkExpression(QString::number(n->first, 'g', 17).toStdString() + " mm", expr::LENGTH);
            chord_->setText(q ? formatNumber(q->valueInDocUnits) : QString());
            angle_->setText(formatNumber(qRadiansToDegrees(n->second)));
        }
    }
    update();
    runCheck();
}

void ExportPanel::update() {
    const bool custom = precision_->currentIndex() == 3;
    chord_->setEnabled(custom);
    angle_->setEnabled(custom);
    QString err;
    QLineEdit* bad = nullptr;
    const bool ok = params(&err, &bad).has_value();
    // An error highlights its field (like an expression field, §5.6).
    for (auto* f : {chord_, angle_}) {
        const bool isBad = !ok && f == bad;
        f->setStyleSheet(isBad ? QString("border: 1px solid %1;").arg(COLOR_ERROR.color.name()) : QString());
        f->setToolTip(isBad ? err : QString());
    }
    const bool stl = fileStl_->isChecked(), flatDxf = flat();
    binary_->setEnabled(stl);
    ascii_->setEnabled(stl);
    perPart_->setEnabled(stl || flatDxf);
    title_->setText(flatDxf ? txt("ui.common.exportFlatPatternDxf") : stl ? txt("ui.common.exportStl") : txt("ui.common.export3mf"));
    // Flat pattern DXF: mesh rows hidden, «Bend lines» shown (§10.3).
    for (QWidget* f : {formatRow_, static_cast<QWidget*>(precision_), static_cast<QWidget*>(chord_), static_cast<QWidget*>(angle_)})
        form_->setRowVisible(f, !flatDxf);
    form_->setRowVisible(bendLines_, flatDxf);
    form_->setRowVisible(print_, !flatDxf);
    const bool any = items_->checkedCount() > 0;
    ok_->setEnabled(flatDxf ? flatOk_ && any : ok && any);
}

}  // namespace construe::app
