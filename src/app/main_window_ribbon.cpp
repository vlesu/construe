// Main window: command ribbon (spec/app-ux.md §3).
#include "app/main_window.h"

#include "app/document_hub.h"

#include "app/part_tool.h"
#include "api/sketch_ops.h"
#include "app/scene_builder.h"

#include "app/constraint_commands.h"
#include "app/hotkeys_table.h"
#include "app/icons.h"
#include "app/keys.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include "app/app_data.h"
#include <QActionGroup>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QTableWidget>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <set>
#include <tuple>

namespace construe::app {

// Lays out the «Part» tab as the plain or the sheet ribbon (app-ux §2.3 «Two ribbons», FX8-SH-001): buttons in
// the order of partRibbonLayout, separators between groups, the rest hidden.
void MainWindow::arrangePartRibbon(bool sheet) {
    if (!partRibbon_) return;
    partRibbonSheet_ = sheet;
    QWidget* page = partRibbon_->parentWidget();
    while (QLayoutItem* it = partRibbon_->takeAt(0)) {
        if (QWidget* w = it->widget(); w && w->objectName() == "ribbonSeparator") delete w;
        delete it;
    }
    std::set<QToolButton*> shown;
    for (std::string_view n : partRibbonLayout(sheet)) {
        if (n == "|") {
            auto* f = new QFrame(page);
            f->setObjectName("ribbonSeparator");
            f->setFrameShape(QFrame::VLine);
            f->setFrameShadow(QFrame::Sunken);
            partRibbon_->addSpacing(2);
            partRibbon_->addWidget(f);
            partRibbon_->addSpacing(2);
            continue;
        }
        QToolButton* b = buttons_.at(QString::fromLatin1(n.data(), qsizetype(n.size())));
        partRibbon_->addWidget(b);
        b->show();
        shown.insert(b);
    }
    partRibbon_->addStretch(1);
    for (QToolButton* b : page->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        if (!shown.count(b)) b->hide();
}

void MainWindow::buildRibbon() {
    // Ribbon buttons show an icon; the name and the hotkey are in the tooltip (app-ux §2.1).
    static const std::map<QString, QString> keys = {
        {"sketch", "Shift+S"}, {"extrude", "Shift+E"}, {"revolve", "Shift+W"}, {"fillet", "Shift+F"}, {"chamfer", "Shift+C"},
        {"point", "P"}, {"line", "L"}, {"rectangle", "R"}, {"centerRectangle", "Shift+R"}, {"circle", "C"},
        {"arc3", "A"}, {"arcCenter", "Shift+A"}, {"tangentArc", "T"}, {"trim", "M"}, {"project", "U"},
        {"construction", "Q"}, {"dimension", "D"}, {"exitSketch", "Ctrl+Enter"}, {"showConstraints", "Shift+C"}, {"normalTo", "Ctrl+8"},
        {"new", "Ctrl+N"}, {"open", "Ctrl+O"}, {"save", "Ctrl+S"}, {"undo", "Ctrl+Z"}, {"redo", "Ctrl+Y"}, {"rebuild", "Ctrl+Q"},
        {"fit", "F"}, {"select", "Esc"}, {"selectAll", "Ctrl+A"}};
    auto mk = [this](QWidget* page, QHBoxLayout* l, const QString& name, const QString& label, auto fn) {
        auto* b = new QToolButton(page);
        b->setObjectName("ribbon_" + name);
        b->setText(label);
        b->setIcon(commandIcon(name));
        b->setIconSize(QSize(RIBBON_ICON_PX, RIBBON_ICON_PX));
        b->setToolButtonStyle(b->icon().isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
        const auto k = keys.find(name);
        b->setToolTip(k == keys.end() ? label : label + " (" + k->second + ")");
        b->setAutoRaise(true);
        b->setMinimumSize(RIBBON_ICON_PX + 12, RIBBON_ICON_PX + 12);
        connect(b, &QToolButton::clicked, this, fn);
        l->addWidget(b);
        buttons_[name] = b;
        return b;
    };
    // An item of a ribbon button's arrow menu (FX12-DRW-006, FX12-DRW-008): the menu is created on the first item, the
    // button gets the arrow (MenuButtonPopup: the main part keeps its own command).
    auto addMenuItem = [this](QToolButton* b, const QString& menuName, const QString& name, const QString& label, auto fn) {
        QMenu* m = b->menu();
        if (!m) {
            m = new QMenu(b);
            m->setObjectName(menuName);
            b->setMenu(m);
            b->setPopupMode(QToolButton::MenuButtonPopup);
            b->setMinimumSize(RIBBON_ICON_PX + 24, RIBBON_ICON_PX + 12);
        }
        QAction* a = m->addAction(commandIcon(name), label);
        a->setObjectName("tool_" + name);
        connect(a, &QAction::triggered, this, fn);
        toolVariants_[name] = a;
        return a;
    };
    // Separator between button groups (app-ux §2.3, S1-UX-007).
    auto sep = [](QWidget* page, QHBoxLayout* l) {
        auto* f = new QFrame(page);
        f->setObjectName("ribbonSeparator");
        f->setFrameShape(QFrame::VLine);
        f->setFrameShadow(QFrame::Sunken);
        l->addSpacing(2);
        l->addWidget(f);
        l->addSpacing(2);
    };
    // Group buttons with variants (app-ux §2.3, S1-SK-020): the main part is the last chosen variant, the arrow is a menu.
    struct Variant {
        QString name, label;
        std::function<void()> fn;
    };
    auto group = [&](QWidget* page, QHBoxLayout* lay, const QString& gname, const QString& glabel, const std::vector<Variant>& vs) {
        auto* b = new QToolButton(page);
        b->setObjectName("ribbon_" + gname);
        b->setText(glabel);  // not shown (icon only); the command name for the help (§2.1б)
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setIconSize(QSize(RIBBON_ICON_PX, RIBBON_ICON_PX));
        b->setAutoRaise(true);
        b->setMinimumSize(RIBBON_ICON_PX + 24, RIBBON_ICON_PX + 12);
        b->setPopupMode(QToolButton::MenuButtonPopup);
        auto* menu = new QMenu(b);
        menu->setObjectName(gname + "Menu");
        auto show = [this, b, glabel](const Variant& v) {
            b->setIcon(commandIcon(v.name));
            const auto k = keys.find(v.name);
            b->setToolTip(glabel + ": " + v.label + (k == keys.end() ? QString() : " (" + k->second + ")"));
            b->setProperty("variant", v.name);
        };
        for (const Variant& v : vs) {
            const auto k = keys.find(v.name);
            QAction* a = menu->addAction(commandIcon(v.name), k == keys.end() ? v.label : v.label + " (" + k->second + ")");
            a->setObjectName("tool_" + v.name);
            connect(a, &QAction::triggered, this, [v, show] {
                show(v);
                v.fn();
            });
            toolVariants_[v.name] = a;
        }
        b->setMenu(menu);
        show(vs.front());
        groupDefaults_.push_back([show, first = vs.front()] { show(first); });
        connect(b, &QToolButton::clicked, this, [this, b] {
            if (QAction* a = toolVariant(b->property("variant").toString())) a->trigger();
        });
        lay->addWidget(b);
        buttons_[gname] = b;
    };
    // «Part» tab: all buttons are created once; arrangePartRibbon() lays out the plain or the sheet ribbon
    // (app-ux §2.3 «Two ribbons», FX8-SH-001) — the buttons of the other ribbon are hidden.
    auto* part = new QWidget;
    part->setObjectName("partRibbonPage");
    auto* pl = new QHBoxLayout(part);
    pl->setContentsMargins(4, 2, 4, 2);
    partRibbon_ = pl;
    mk(part, pl, "sketch", txt("ui.common.sketch"), [this] { cmdSketch(); });
    // Reference geometry (§5.3а): group buttons «Plane ▾» and «Axis ▾».
    group(part, pl, "refPlaneGroup", txt("ui.common.plane"),
             {{"planeOffset", txt("ui.common.offset2"), [this] { cmdRefGeom(true, "offset"); }},
              {"planeAngle", txt("ui.common.atAngle"), [this] { cmdRefGeom(true, "angle"); }},
              {"planeThreePoints", txt("ui.common.throughThreePoints"), [this] { cmdRefGeom(true, "threePoints"); }},
              {"planeMid", txt("ui.common.midplane"), [this] { cmdRefGeom(true, "mid"); }}});
    group(part, pl, "refAxisGroup", txt("ui.common.axis"),
             {{"axisTwoPoints", txt("ui.common.throughTwoPoints"), [this] { cmdRefGeom(false, "twoPoints"); }},
              {"axisEdge", txt("ui.common.alongEdge"), [this] { cmdRefGeom(false, "edge"); }},
              {"axisFace", txt("ui.common.faceAxis"), [this] { cmdRefGeom(false, "face"); }},
              {"axisPlanes", txt("ui.common.intersectionOfPlanes"), [this] { cmdRefGeom(false, "planes"); }}});
    mk(part, pl, "extrude", txt("ui.common.extrude"), [this] { cmdExtrude(); });
    mk(part, pl, "revolve", txt("ui.common.revolve"), [this] { cmdRevolve(); });
    // Rarer tools — in the menus of the group buttons (FX8-RB-001).
    group(part, pl, "sweepGroup", txt("ui.common.sweep"),
          {{"sweep", txt("ui.common.sweep"), [this] { startFeature(sweep_); }},
           {"coil", txt("ui.ribbon.coilCustomThread"), [this] { cmdCoil(); }}});  // FX7-PT-004
    group(part, pl, "loftGroup", txt("ui.common.loft"),
          {{"loft", txt("ui.common.loft"), [this] { cmdLoft(); }}, {"draft", txt("ui.common.draft"), [this] { cmdDraft(); }}});
    group(part, pl, "threadGroup", txt("ui.common.thread"),
          {{"thread", txt("ui.common.thread"), [this] { cmdThread(); }}, {"hole", txt("ui.common.hole"), [this] { cmdHole(); }}});
    mk(part, pl, "gear", txt("ui.common.gearRim"), [this] { cmdGear(); })  // S6-GEAR-002
        ->setToolTip(txt("ui.ribbon.gearRimSpurInvoluteRim"));
    mk(part, pl, "fillet", txt("ui.common.fillet"), [this] { cmdEdgeFeature(true); });
    mk(part, pl, "chamfer", txt("ui.common.chamfer"), [this] { cmdEdgeFeature(false); });
    mk(part, pl, "shell", txt("ui.common.shell"), [this] { cmdShell(); });
    group(part, pl, "patternGroup", txt("ui.common.pattern"),
          {{"patternLinear", txt("ui.common.linear"), [this] { cmdPattern(pattern_, "linear"); }},
           {"patternCircular", txt("ui.common.circular"), [this] { cmdPattern(pattern_, "circular"); }}});
    mk(part, pl, "mirrorFeature", txt("ui.common.mirror"), [this] { cmdPattern(mirror_, "linear"); });
    // Sheet metal (§2.3, §5.3к, S5-SHEET-*, S5-FLAT-001): on the sheet ribbon — separate buttons (FX8-SH-002).
    mk(part, pl, "sheetMetal", txt("ui.common.sheetMetalPart"), [this] { cmdSheetMetal(); })
        ->setToolTip(txt("ui.ribbon.sheetMetalPartSheetParameters"));
    mk(part, pl, "sheetWall", txt("ui.common.baseWall"), [this] { startFeature(sheetWall_); });
    mk(part, pl, "sheetFlange", txt("ui.common.flange"), [this] { cmdSheetFlange(); });
    mk(part, pl, "sheetBend", txt("ui.common.bendOnLine"), [this] { startFeature(sheetBend_); });
    mk(part, pl, "sheetCut", txt("ui.common.sheetCut"), [this] { startFeature(sheetCut_); });
    mk(part, pl, "sheetUnfold", txt("ui.common.unfold"), [this] { cmdSheetUnfold(); })->setCheckable(true);
    mk(part, pl, "section", txt("ui.common.section"), [this] { cmdSection(); });
    mk(part, pl, "measure", txt("ui.ribbon.measure"), [this] { cmdMeasure(); })->setToolTip(txt("ui.ribbon.measureTip"));  // FX10-MEAS-001
    // Technical requirements / PMI (§2.3, §5.12, S3-PMI-001…003); «Roughness of other surfaces» — in «Roughness ▾»
    // (FX8-RB-001, FX5-PMI-001).
    group(part, pl, "roughnessGroup", txt("ui.common.roughness"),
          {{"pmiRoughness", txt("ui.common.roughness"), [this] { cmdAnnotation("roughness"); }},
           {"pmiRest", txt("ui.ribbon.roughnessOfOtherSurfaces"), [this] { cmdRestRoughness(); }}});
    // «Size tolerance» is gone — a tolerance is part of the dimension (FX12-PMI-004); «Datum» is a variant of «Geometric
    // tolerance ▾» (FX12-PMI-005).
    group(part, pl, "geometricGroup", txt("ui.ribbon.geometricTolerance"),
          {{"pmiGeometric", txt("ui.ribbon.geometricTolerance"), [this] { cmdAnnotation("geometric"); }},
           {"pmiDatum", txt("ui.common.datum"), [this] { cmdAnnotation("datum"); }}});
    arrangePartRibbon(false);
    auto* sk = new QWidget;
    auto* sl = new QHBoxLayout(sk);
    sl->setContentsMargins(4, 2, 4, 2);
    // «Sketch» tab (§2.3): there is no «Sketch» button (FX1-UI-002); the first is «Select ▾», the
    // default mode, with «Select all» in its menu), next to «Construction
    // geometry» and «Trim» (FX1-UI-005).
    auto* sel = mk(sk, sl, "select", txt("ui.ribbon.select"), [this] { sketch_->setTool(SketchEditor::Tool::Select); });
    {
        auto* selMenu = new QMenu(sel);
        selMenu->setObjectName("selectMenu");
        QAction* all = selMenu->addAction(commandIcon("selectAll"), txt("ui.ribbon.selectAllCtrlA"));
        all->setObjectName("tool_selectAll");
        connect(all, &QAction::triggered, this, [this] { sketch_->selectAll(); });
        toolVariants_["selectAll"] = all;
        sel->setMenu(selMenu);
        sel->setPopupMode(QToolButton::MenuButtonPopup);
        sel->setMinimumSize(RIBBON_ICON_PX + 24, RIBBON_ICON_PX + 12);
    }
    // «Construction geometry» (§2.3, §6.3, FX7-SK-001) — the single construction toggle:
    // new elements are construction ones, a click toggles an element, duplicates merge.
    mk(sk, sl, "construction", txt("ui.ribbon.constructionGeometry"), [this] { sketch_->toggleConstruction(); });
    mk(sk, sl, "trim", txt("ui.ribbon.trim"), [this] { sketch_->setTool(SketchEditor::Tool::Trim); });
    sep(sk, sl);
    mk(sk, sl, "point", txt("ui.common.point2"), [this] { sketch_->setTool(SketchEditor::Tool::Point); });
    mk(sk, sl, "line", txt("ui.common.line2"), [this] { sketch_->setTool(SketchEditor::Tool::Line); });
    mk(sk, sl, "centerline", txt("ui.ribbon.centerline"), [this] { sketch_->setCenterlineTool(); });
    group(sk, sl, "rectangleGroup", txt("ui.ribbon.rectangle"),
          {{"rectangle", txt("ui.ribbon.byCorners"), [this] { sketch_->setTool(SketchEditor::Tool::Rectangle); }},
           {"centerRectangle", txt("ui.ribbon.fromCenter"), [this] { sketch_->setTool(SketchEditor::Tool::CenterRectangle); }}});
    group(sk, sl, "polygonGroup", txt("ui.ribbon.primitives"),  // S4-SKT-003: linked groups polygon, star
          {{"polygonInscribed", txt("ui.ribbon.inscribedNGonByVertex"), [this] { sketch_->setPolygonTool(true); }},
           {"polygonCircumscribed", txt("ui.ribbon.circumscribedNGonBySide"), [this] { sketch_->setPolygonTool(false); }},
           {"star", txt("ui.common.star"), [this] { sketch_->setTool(SketchEditor::Tool::Star); }}});
    group(sk, sl, "circleGroup", txt("ui.common.circle2"),
          {{"circle", txt("ui.ribbon.byCenter"), [this] { sketch_->setTool(SketchEditor::Tool::Circle); }},
           {"circle3", txt("ui.ribbon.by3Points"), [this] { sketch_->setTool(SketchEditor::Tool::Circle3); }},
           {"ellipse", txt("ui.common.ellipse"), [this] { sketch_->setTool(SketchEditor::Tool::Ellipse); }}});  // S4-SKT-004
    // «Arc ▾»: the tangent arc is the first variant and the default (FX10-SK-006).
    group(sk, sl, "arcGroup", txt("ui.common.arc"),
          {{"tangentArc", txt("ui.ribbon.tangent"), [this] { sketch_->startTangentArc(); }},
           {"arc3", txt("ui.ribbon.by3Points"), [this] { sketch_->setTool(SketchEditor::Tool::Arc3); }},
           {"arcCenter", txt("ui.ribbon.fromCenter"), [this] { sketch_->setTool(SketchEditor::Tool::ArcCenter); }}});
    group(sk, sl, "splineGroup", txt("ui.common.spline"),  // S4A-SPL-001
          {{"splineControl", txt("ui.ribbon.byControlPoints"), [this] { sketch_->setTool(SketchEditor::Tool::SplineControl); }},
           {"splineThrough", txt("ui.ribbon.throughPoints"), [this] { sketch_->setTool(SketchEditor::Tool::SplineThrough); }}});
    mk(sk, sl, "text", txt("ui.ribbon.text"), [this] { sketch_->setTool(SketchEditor::Tool::Text); });  // S2-SK-013, §6.11в
    group(sk, sl, "sketchFilletGroup", txt("ui.common.fillet"),
          {{"sketchFillet", txt("ui.common.fillet"), [this] { sketch_->setTool(SketchEditor::Tool::SketchFillet); }},
           {"sketchChamfer", txt("ui.common.chamfer"), [this] { sketch_->setTool(SketchEditor::Tool::SketchChamfer); }}});
    mk(sk, sl, "offset", txt("ui.common.offset"), [this] { sketch_->startOffset(); });
    // Sketch pattern and «Unlink» (§2.3, §6.3; S4-SKT-001, -002, -005).
    group(sk, sl, "sketchPatternGroup", txt("ui.common.pattern"),
          {{"sketchPatternLinear", txt("ui.common.linearPattern"), [this] { sketch_->startPattern(false); }},
           {"sketchPatternCircular", txt("ui.common.circularPattern"), [this] { sketch_->startPattern(true); }}});
    // «Unlink pattern/primitive» (FX7-SK-015, FX8-SK-008): an item of the «Pattern ▾» menu
    // after a separator (not a variant: the main part of the button stays), with an explaining tooltip; available only
    // with a pattern / primitive element selected (§7.5).
    if (QToolButton* pg = buttons_["sketchPatternGroup"]) {
        pg->menu()->addSeparator();
        QAction* unlink = pg->menu()->addAction(commandIcon("sketchUnlink"), txt("ui.ribbon.unlinkPatternPrimitive"));
        unlink->setObjectName("tool_sketchUnlink");
        unlink->setToolTip(text("ui.sketchUnlink.tooltip"));
        pg->menu()->setToolTipsVisible(true);
        connect(unlink, &QAction::triggered, this, [this] { sketch_->unlinkSelection(); });
        toolVariants_["sketchUnlink"] = unlink;
    }
    // «Mirror» — next to «Pattern ▾» (§2.3, FX8-SK-009).
    mk(sk, sl, "mirror", txt("ui.common.mirror"), [this] { sketch_->startMirror(); });
    mk(sk, sl, "project", txt("ui.common.project"), [this] { sketch_->setTool(SketchEditor::Tool::Project); });
    // No «Pierce» button (FX9-SK-008): the point tool snaps to the piercing point itself.
    sep(sk, sl);
    // «Dimension ▾» (§2.3, FX7-SK-013): the main part is the tool, the menu — «Dimension main geometry» and
    // «Dimension all, including splines» (S2-SK-011, S4-WEAK-004, §6.9а).
    {
        auto* dim = mk(sk, sl, "dimension", txt("ui.common.dimension"), [this] { sketch_->setTool(SketchEditor::Tool::Dimension); });
        auto* dimMenu = new QMenu(dim);
        dimMenu->setObjectName("dimensionToolMenu");
        auto fullyDefine = [this](bool splines) {
            auto r = model_->session().sketchFullyDefine(sketch_->dimensionLabelLayout(), splines);  // label layout — FX4-SK-001
            if (!r) showToast(errorText(r.error()), true);
            else if (r->empty()) showToast(text("ui.fullyDefine.none"));
            else showToast(text("ui.fullyDefine.done", {{"count", QString::number(r->size())}}));
            sketch_->refresh();
            sketch_->setTool(SketchEditor::Tool::Select);  // after the command — «Select» (FX1-SKI-007)
        };
        for (const auto& [name, label, splines] : {std::tuple<const char*, const char*, bool>{"fullyDefine", txt("ui.ribbon.dimensionMainGeometry"), false},
                                                   {"fullyDefineAll", txt("ui.ribbon.dimensionAllIncludingSplines"), true}}) {
            QAction* a = dimMenu->addAction(commandIcon("fullyDefine"), label);
            a->setObjectName(QString("tool_") + name);
            connect(a, &QAction::triggered, this, [fullyDefine, splines = splines] { fullyDefine(splines); });
            toolVariants_[name] = a;
        }
        dim->setMenu(dimMenu);
        dim->setPopupMode(QToolButton::MenuButtonPopup);
        dim->setMinimumSize(RIBBON_ICON_PX + 24, RIBBON_ICON_PX + 12);
    }
    // Constraints ▾ (§2.3): all geometric constraints of §6.8 — applied to the selection or "command → pick".
    auto* consBtn = mk(sk, sl, "constraints", txt("ui.ribbon.constraints"), [this] {
        if (auto* b = buttons_["constraints"]; b && b->menu()) b->showMenu();
    });
    auto* consMenu = new QMenu(consBtn);
    consMenu->setObjectName("constraintsMenu");
    for (const auto& c : sketchConstraintCommands()) {
        QAction* a = consMenu->addAction(commandIcon("constraint_" + c.type), c.menuText());
        a->setObjectName("actionConstraint_" + c.type);
        connect(a, &QAction::triggered, this, [this, t = c.type.toStdString()] { sketch_->applyConstraint(t); });
    }
    // «Show constraints» (Shift+C) — the last item, after a separator (FX7-SK-014).
    consMenu->addSeparator();
    QAction* show = consMenu->addAction(commandIcon("showConstraints"), txt("ui.ribbon.showConstraintsShiftC"));
    show->setObjectName("tool_showConstraints");
    show->setCheckable(true);
    show->setChecked(true);
    connect(show, &QAction::triggered, this, [this] { sketch_->setConstraintsShown(!sketch_->constraintsShown()); });
    toolVariants_["showConstraints"] = show;
    consBtn->setMenu(consMenu);
    consBtn->setPopupMode(QToolButton::InstantPopup);
    // Tool buttons have a pressed state (§2.3, FX1-UI-003): a click would toggle it by itself, so after the
    // command the state is synced with the sketch editor.
    for (const QString& n : sketchToolButtonNames()) {
        buttons_[n]->setCheckable(true);
        connect(buttons_[n], &QToolButton::clicked, this, [this] { syncSketchToolButtons(); });
    }
    sl->addStretch(1);
    auto* exit = mk(sk, sl, "exitSketch", txt("ui.ribbon.exitSketch"), [this] { cmdExitSketch(); });
    QFont f = exit->font();
    f.setBold(true);
    exit->setFont(f);
    ribbon_->addTab(part, txt("ui.common.part"));
    ribbon_->addTab(sk, txt("ui.common.sketch"));
    // «Assembly» tab (§8а): «Insert ▾» lists document objects that can be inserted into the
    // active assembly.
    auto* asmTab = new QWidget;
    auto* al = new QHBoxLayout(asmTab);
    al->setContentsMargins(4, 2, 4, 2);
    auto* insertBtn = mk(asmTab, al, "insert", txt("ui.common.insert"), [this] {
        if (auto* b = buttons_["insert"]; b && b->menu()) b->showMenu();
    });
    insertMenu_ = new QMenu(insertBtn);
    insertMenu_->setObjectName("insertMenu");
    insertBtn->setMenu(insertMenu_);
    insertBtn->setPopupMode(QToolButton::InstantPopup);
    mk(asmTab, al, "mate", txt("ui.common.mate"), [this] { cmdMate(); });  // M16; right after «Insert» (FX9-ASM-004)
    sep(asmTab, al);
    // Assembly reference geometry and sketches (S2-ASM-012): the plane and axis mode is chosen in the panel.
    mk(asmTab, al, "asmSketch", txt("ui.common.sketch"), [this] { cmdSketch(); });
    mk(asmTab, al, "asmPlane", txt("ui.common.plane"), [this] { cmdRefGeom(true, "offset"); });
    mk(asmTab, al, "asmAxis", txt("ui.common.axis"), [this] { cmdRefGeom(false, "twoPoints"); });
    // Assembly cut and hole (S5-AFEAT-001, S5-AFEAT-002): the extrude and hole panels in the assembly mode.
    mk(asmTab, al, "asmCut", txt("ui.common.cut"), [this] { cmdExtrude(); });
    mk(asmTab, al, "asmHole", txt("ui.common.hole"), [this] { cmdHole(); });
    mk(asmTab, al, "asmSolution", txt("ui.ribbon.connection"), [this] { cmdSolution(); });  // the connection wizard (§8д, S6-SOL-002)
    sep(asmTab, al);
    mk(asmTab, al, "weld", txt("ui.common.weld"), [this] { cmdWeld(); });  // S5-WELD-001
    mk(asmTab, al, "interference", txt("ui.common.interference"), [this] { cmdInterference(); });  // M17
    mk(asmTab, al, "sectionAsm", txt("ui.common.section"), [this] { cmdSection(); });  // S2-VIEW-003
    mk(asmTab, al, "asmPattern", txt("ui.common.pattern"), [this] { cmdInstancePattern(); });  // FX7-ASM-007
    // Technical requirements on instance faces (FX12-PMI-003): «Roughness» and «Geometric tolerance ▾» with «Datum»
    // (FX12-PMI-005) — the same commands as on the part ribbon.
    mk(asmTab, al, "asmRoughness", txt("ui.common.roughness"), [this] { cmdAnnotation("roughness"); });
    group(asmTab, al, "asmGeometricGroup", txt("ui.ribbon.geometricTolerance"),
          {{"asmPmiGeometric", txt("ui.ribbon.geometricTolerance"), [this] { cmdAnnotation("geometric"); }},
           {"asmPmiDatum", txt("ui.common.datum"), [this] { cmdAnnotation("datum"); }}});
    // Exploded view (FX7-ASM-005): a toggle; «Explode again» — the automatic explosion anew, an item
    // of the button's arrow menu (FX8-ASM-006: no separate ribbon button).
    {
        auto* ex = mk(asmTab, al, "exploded", txt("ui.ribbon.explodedView"), [this] { cmdExploded(); });
        ex->setCheckable(true);
        auto* exMenu = new QMenu(ex);
        exMenu->setObjectName("explodedMenu");
        QAction* again = exMenu->addAction(commandIcon("explodeReset"), txt("ui.ribbon.explodeAgain"));
        again->setObjectName("tool_explodeReset");
        connect(again, &QAction::triggered, this, [this] { cmdExplodeReset(); });
        toolVariants_["explodeReset"] = again;
        ex->setMenu(exMenu);
        ex->setPopupMode(QToolButton::MenuButtonPopup);
        ex->setMinimumSize(RIBBON_ICON_PX + 24, RIBBON_ICON_PX + 12);
    }
    mk(asmTab, al, "asmProperties", txt("ui.common.properties"), [this] {  // properties of the assembly or of the selected instance (§8а, S2-UX-001)
        if (mode_ != Mode::Part || !assemblyActive()) return;
        propertiesPanel_->setInstance(tree_->selectedInstance().value_or(std::string()));
        setMode(Mode::Properties);
    });
    sep(asmTab, al);
    mk(asmTab, al, "newSpecification", txt("ui.ribbon.newBillOfMaterials"), [this] {  // §8в, S3-SPEC-001
        if (auto r = model_->session().addElement("specification"); !r) showToast(errorText(r.error()), true);
    })->setToolTip(txt("ui.ribbon.newBillOfMaterialsBom"));
    al->addStretch(1);
    ribbon_->addTab(asmTab, txt("ui.common.assembly"));
    // «Drawing» tab (§8б): sheets; views, annotations and export are added as stage 3 progresses.
    auto* drwTab = new QWidget;
    auto* dl = new QHBoxLayout(drwTab);
    dl->setContentsMargins(4, 2, 4, 2);
    // Title block (§8б, S3-DRW-002; first on the tab — S7F-DRW-003): show or hide the panel of the current sheet's title block fields.
    auto* tb = mk(drwTab, dl, "titleBlock", txt("ui.ribbon.editTitleBlockData"), [this] {
        const bool on = buttons_["titleBlock"]->isChecked();
        showDrawingPanel(on, on);
    });
    tb->setCheckable(true);
    tb->setChecked(true);  // the drawing and title block properties panel is visible by default (FX1-UI-007)
    // «New sheet» — an item of the button's arrow menu (FX12-DRW-006: no separate ribbon button).
    addMenuItem(tb, "titleBlockMenu", "addSheet", txt("ui.ribbon.newSheet"), [this] {
        auto r = model_->session().addSheet();
        if (!r) showToast(errorText(r.error()), true);
        else selectSheet(*r);
    });
    sep(drwTab, dl);
    // Views (§8б, S3-DRW-003): «View ▾» lists the document's models (the menu is rebuilt with the sheet),
    // «Projected view».
    auto* baseViewBtn = mk(drwTab, dl, "baseView", txt("ui.common.view"), [this] {
        if (auto* b = buttons_["baseView"]; b && b->menu()) b->showMenu();
    });
    baseViewMenu_ = new QMenu(baseViewBtn);
    baseViewMenu_->setObjectName("baseViewMenu");
    baseViewBtn->setMenu(baseViewMenu_);
    baseViewBtn->setPopupMode(QToolButton::InstantPopup);
    // Three views (FX1-DRW-001): a menu of models, then a click places the base view.
    auto* threeBtn = mk(drwTab, dl, "threeViews", txt("ui.ribbon.t3Views"), [this] {
        if (auto* b = buttons_["threeViews"]; b && b->menu()) b->showMenu();
    });
    threeViewsMenu_ = new QMenu(threeBtn);
    threeViewsMenu_->setObjectName("threeViewsMenu");
    threeBtn->setMenu(threeViewsMenu_);
    threeBtn->setPopupMode(QToolButton::InstantPopup);
    mk(drwTab, dl, "projectedView", txt("ui.ribbon.projectedView"), [this] { cmdProjectedView(); });
    // Section views and detail views (§8б, S3-DRW-005).
    mk(drwTab, dl, "sectionView", txt("ui.common.section"), [this] { cmdSectionView(false); });
    mk(drwTab, dl, "detailView", txt("ui.ribbon.detailView"), [this] { cmdSectionView(true); });
    sep(drwTab, dl);
    // Dimensions (§8б, S3-DRW-006): mode for placing dimensions on view edges and vertices.
    auto* dimBtn = mk(drwTab, dl, "drawingDimension", txt("ui.common.dimension"), [this] {
        if (!drawingActive()) return;
        const bool on = buttons_["drawingDimension"]->isChecked();
        if (on) setDrawingTool(DrawingTool::None);
        dims_->setActive(on);
    });
    dimBtn->setCheckable(true);
    mk(drwTab, dl, "modelDimensions", txt("ui.ribbon.insertAllModelDimensions"), [this] { cmdModelDimensions(); });  // FX1-DRW-003
    mk(drwTab, dl, "modelMarks", txt("ui.ribbon.placeAllAdditionalInformation"), [this] { cmdModelMarks(); });  // FX9-DRW-008
    // Undimensioned model dimensions in blue (FX7-DRW-003): a toggle; double click on a blue one keeps it.
    auto* undimBtn = mk(drwTab, dl, "showUndimensioned", txt("ui.ribbon.showUndimensioned"), [this] {
        if (dims_) dims_->setShowUndimensioned(buttons_["showUndimensioned"]->isChecked());
    });
    undimBtn->setCheckable(true);
    // Marks set in the drawing itself (FX9-DRW-009, FX11-DRW-001): a click on the line of a face on a view.
    auto* rough = mk(drwTab, dl, "drawingRoughness", txt("ui.common.roughness"), [this] { cmdDrawingMark("roughness"); });
    // «Rest roughness» set in the drawing only (FX12-DRW-007): an item of the «Roughness ▾» arrow menu.
    addMenuItem(rough, "drawingRoughnessMenu", "drawingRestRoughness", txt("ui.ribbon.roughnessOfOtherSurfaces"),
                [this] { cmdDrawingRestRoughness(); });
    auto* geo = mk(drwTab, dl, "drawingGeometric", txt("ui.pmi.geometricTolerance"), [this] { cmdDrawingMark("geometric"); });
    // «Datum» — an item of the «Geometric tolerance ▾» arrow menu (FX12-DRW-008: no separate ribbon button).
    addMenuItem(geo, "drawingGeometricMenu", "drawingDatum", txt("ui.common.datum"), [this] { cmdDrawingMark("datum"); });
    // Item numbers and the parts list of an assembly drawing (FX14-DRW-002, FX14-DRW-003; drawing.md §6.27, §6.28).
    mk(drwTab, dl, "drawingBalloon", txt("ui.ribbon.itemNumber"), [this] { cmdDrawingBalloon(); });
    mk(drwTab, dl, "partsList", txt("ui.ribbon.partsList"), [this] { cmdPartsList(); });
    sep(drwTab, dl);
    // Text on the sheet (FX7-DRW-006): placement mode — a click opens the text field there.
    auto* textBtn = mk(drwTab, dl, "sheetText", txt("ui.common.text"), [this] {
        if (texts_) texts_->setPlacing(buttons_["sheetText"]->isChecked());
    });
    textBtn->setCheckable(true);
    // Constructions on a view (FX14-DRW-006): thin lines bound to the model — segment, circle, arc, pitch circle, axis line.
    group(drwTab, dl, "drawingConsGroup", txt("ui.ribbon.construction"),
          {{"consLine", txt("ui.common.line2"), [this] { if (drawingActive()) cons_->setPlacing("line"); }},
           {"consCircle", txt("ui.common.circle2"), [this] { if (drawingActive()) cons_->setPlacing("circle"); }},
           {"consArc", txt("ui.common.arc"), [this] { if (drawingActive()) cons_->setPlacing("arc"); }},
           {"consPitch", txt("ui.ribbon.pitchCircle"), [this] { if (drawingActive()) cons_->setPlacing("pitchCircle"); }},
           {"consAxis", txt("ui.ribbon.centerline"), [this] { if (drawingActive()) cons_->setPlacing("axis"); }}});
    dl->addStretch(1);
    ribbon_->addTab(drwTab, txt("ui.ribbon.drawing"));
    // Above the ribbon (§2.3, S1-UX-006): quick access on the left, view on the right.
    auto* quick = new QWidget;
    auto* ql = new QHBoxLayout(quick);
    ql->setContentsMargins(2, 0, 8, 0);
    ql->setSpacing(0);
    mk(quick, ql, "new", txt("ui.common.new"), [this] { cmdNew(); });
    mk(quick, ql, "open", txt("ui.common.open"), [this] { cmdOpen(); });
    auto* save = mk(quick, ql, "save", txt("ui.common.save"), [this] { cmdSave(); });
    // «Save ▾»: the button menu holds «Save as…» (Ctrl+Shift+S) so that the command
    // has an icon (S1-UX-006).
    auto* saveMenu = new QMenu(save);
    QAction* saveAs = saveMenu->addAction(commandIcon("save"), txt("ui.ribbon.saveAsCtrlShiftS"), this, [this] { cmdSaveAs(); });
    saveAs->setObjectName("actionSaveAsQuick");
    save->setMenu(saveMenu);
    save->setPopupMode(QToolButton::MenuButtonPopup);
    mk(quick, ql, "undo", txt("ui.common.undo"), [this] { cmdUndo(); });
    mk(quick, ql, "redo", txt("ui.common.redo"), [this] { cmdRedo(); });
    // A recomputation of the same revision (FX14-QA-007): pending until its result is applied — the long work stripe runs
    // during it and tests wait for it (it was not pending: under load the scene was rebuilt after the step had ended).
    mk(quick, ql, "rebuild", txt("ui.common.rebuild"), [this] {
        if (model_->session().rebuild()) model_->noteRecomputeRequested();
    });
    // New document objects go after a separator following «Rebuild» (§2.3, FX1-UI-001).
    sep(quick, ql);
    auto* newPart = mk(quick, ql, "newPart", txt("ui.common.newPart"), [this] { tree_->addPart(); });
    // «New part ▾» (FX7-SH-002): the kind of the part — ordinary or sheet metal.
    {
        auto* m = new QMenu(newPart);
        m->setObjectName("newPartMenu");
        QAction* plain = m->addAction(commandIcon("newPart"), txt("ui.ribbon.regularPart"), this, [this] { tree_->addPart(); });
        plain->setObjectName("actionNewPlainPart");
        QAction* sheet = m->addAction(commandIcon("sheetMetal"), txt("ui.common.sheetMetalPart"), this, [this] { tree_->addPart(true); });
        sheet->setObjectName("actionNewSheetPart");
        newPart->setMenu(m);
        newPart->setPopupMode(QToolButton::MenuButtonPopup);
    }
    mk(quick, ql, "newAssembly", txt("ui.common.newAssembly"), [this] {  // §8а
        if (auto r = model_->session().addElement("assembly"); !r) showToast(errorText(r.error()), true);
    });
    mk(quick, ql, "newDrawing", txt("ui.ribbon.newDrawing"), [this] {  // §8б; standard and projection method come from the application settings (§10.6)
        auto& s = model_->session();
        api::Json def = api::Json::object();
        if (appSettings_->value("drawing/standard").toString() == "iso") def["standard"] = "iso";
        if (appSettings_->value("drawing/projection").toString() == "third") def["projection"] = "third";
        s.beginGroup("undo.addElement");
        auto r = s.addElement("drawing");
        if (r && !def.empty())
            if (auto u = s.updateDrawing(def); !u) showToast(errorText(u.error()), true);
        // Title block values by default (FX14-SET-001, §10.6): into the first sheet; the next sheets inherit them.
        if (const auto tb = appSettings_->titleBlockDefaults(); r && !tb.empty()) {
            const auto held = s.document().document;
            if (held && held->active().isDrawing() && !held->active().sheets.empty()) {
                const std::string sheetId = held->active().sheets.front().id;
                api::Json j = api::Json::object();
                for (const auto& [k, v] : tb) j[k] = v;
                if (auto u = s.updateSheet(sheetId, api::Json{{"titleBlock", j}}); !u) showToast(errorText(u.error()), true);
            }
        }
        s.endGroup();
        if (!r) showToast(errorText(r.error()), true);
    });
    ribbon_->setCornerWidget(quick, Qt::TopLeftCorner);
    auto* viewBox = new QWidget;
    auto* vl = new QHBoxLayout(viewBox);
    vl->setContentsMargins(8, 0, 2, 0);
    vl->setSpacing(0);
    // «Isometric ▾» — standard view plus fit (§2.3, FX1-UI-008, FX4-UI-001): the main part is isometric,
    // the arrow is a menu of standard views; placed next to «Fit» and «Style ▾».
    auto* iso = mk(viewBox, vl, "isoView", txt("ui.common.isometric"), [this] { standardViewFit(StandardView::Iso); });
    iso->setIcon(commandIcon("views"));
    iso->setToolButtonStyle(Qt::ToolButtonIconOnly);
    iso->setToolTip(txt("ui.ribbon.isometricFitTheModelArrow"));
    {
        auto* isoMenu = new QMenu(iso);
        isoMenu->setObjectName("isoViewMenu");
        const std::pair<const char*, StandardView> isoViews[] = {{txt("ui.common.front"), StandardView::Front}, {txt("ui.common.back"), StandardView::Back},
                                                                {txt("ui.common.left"), StandardView::Left},    {txt("ui.common.right"), StandardView::Right},
                                                                {txt("ui.common.top"), StandardView::Top},    {txt("ui.common.bottom"), StandardView::Bottom},
                                                                {txt("ui.common.isometric"), StandardView::Iso}};
        int isoK = 1;
        for (const auto& [name, sv] : isoViews) {
            QAction* a = isoMenu->addAction(QString::fromUtf8(name));
            a->setObjectName(QString("isoView%1").arg(isoK++));
            connect(a, &QAction::triggered, this, [this, sv = sv] { standardViewFit(sv); });
        }
        iso->setMenu(isoMenu);
        iso->setPopupMode(QToolButton::MenuButtonPopup);
    }
    mk(viewBox, vl, "fit", txt("ui.ribbon.fit"), [this] { view_->fitAll(true); });
    // Display style (§3.4а): a group button whose icon is the current style; the choice is remembered between runs.
    auto* styleBtn = mk(viewBox, vl, "displayStyle", txt("ui.ribbon.style"), [this] {
        if (auto* b = buttons_["displayStyle"]; b && b->menu()) b->showMenu();
    });
    auto* styleBtnMenu = new QMenu(styleBtn);
    styleBtnMenu->setObjectName("displayStyleMenu");
    styleActions_ = new QActionGroup(this);
    const std::pair<DisplayStyle, const char*> styles[] = {{DisplayStyle::ShadedEdges, txt("ui.common.shadedWithEdges")},
                                                           {DisplayStyle::Shaded, txt("ui.common.shaded")},
                                                           {DisplayStyle::HiddenRemoved, txt("ui.common.hiddenLinesRemoved")},
                                                           {DisplayStyle::HiddenDashed, txt("ui.common.hiddenLinesDashed")},
                                                           {DisplayStyle::Wireframe, txt("ui.common.wireframe")}};
    for (const auto& [st, label] : styles) {
        const QString n = styleName(st);
        QAction* a = styleBtnMenu->addAction(commandIcon("style_" + n), QString::fromUtf8(label));
        a->setObjectName("actionStyle" + n.left(1).toUpper() + n.mid(1));
        a->setCheckable(true);
        styleActions_->addAction(a);
        connect(a, &QAction::triggered, this, [this, st = st] { setDisplayStyle(st); });
    }
    styleBtn->setMenu(styleBtnMenu);
    styleBtn->setPopupMode(QToolButton::InstantPopup);
    {
        DisplayStyle initial = DisplayStyle::ShadedEdges;
        if (auto s = styleFromName(appSettings_->displayStyle().toStdString())) initial = *s;  // §10.6
        setDisplayStyle(initial);
    }
    // «Variables» — in the upper right corner next to the view buttons, in every mode (FX7-UI-005).
    mk(viewBox, vl, "variables", txt("ui.common.variables"), [this] {
        variables_->setVisible(!variables_->isVisible());
        if (variables_->isVisible()) openSidePanel(variables_, RIGHT_PANEL_W);  // S7F-UI-007
    });
    ribbon_->setCornerWidget(viewBox, Qt::TopRightCorner);
    // Buttons above the ribbon match the tab bar height: otherwise the ribbon page overlaps their lower part (§2.3).
    const int tabH = ribbon_->tabBar()->sizeHint().height();
    for (const char* n : {"new", "open", "save", "undo", "redo", "rebuild", "newPart", "newAssembly", "newDrawing", "isoView", "fit", "displayStyle", "variables"}) {
        QToolButton* b = buttons_[n];
        b->setIconSize(QSize(QUICK_ICON_PX, QUICK_ICON_PX));
        b->setMinimumSize(0, 0);
        b->setFixedSize(std::max(tabH, QUICK_ICON_PX + 6) + (b->menu() ? 10 : 0), std::max(tabH, QUICK_ICON_PX + 6));
    }
}

// Tool buttons of the «Sketch» tab (§2.3, FX1-UI-003).
const std::vector<QString>& MainWindow::sketchToolButtonNames() {
    static const std::vector<QString> names = {"select", "construction", "trim", "point", "line", "centerline", "rectangleGroup",
                                               "polygonGroup", "circleGroup", "arcGroup", "text", "mirror", "sketchFilletGroup", "sketchPatternGroup",
                                               "offset", "project", "dimension", "constraints"};
    return names;
}

// Exactly the button of the sketch editor's current tool is pressed (§2.3, FX1-UI-003).
void MainWindow::syncSketchToolButtons() {
    using Tool = SketchEditor::Tool;
    QString active;
    if (mode_ == Mode::Sketch) {
        switch (sketch_->tool()) {
            case Tool::Select: active = "select"; break;
            case Tool::Construction: active = "construction"; break;
            case Tool::Trim: active = "trim"; break;
            case Tool::Point: active = "point"; break;
            case Tool::Line: active = sketch_->centerline() ? "centerline" : "line"; break;
            case Tool::Rectangle:
            case Tool::CenterRectangle: active = "rectangleGroup"; break;
            case Tool::Polygon:
            case Tool::Star: active = "polygonGroup"; break;
            case Tool::Circle:
            case Tool::Circle3:
            case Tool::Ellipse: active = "circleGroup"; break;
            case Tool::Arc3:
            case Tool::ArcCenter:
            case Tool::TangentArc: active = "arcGroup"; break;
            case Tool::SplineControl:
            case Tool::SplineThrough: active = "splineGroup"; break;
            case Tool::Text: active = "text"; break;
            case Tool::Mirror: active = "mirror"; break;
            case Tool::PatternLinear:
            case Tool::PatternCircular: active = "sketchPatternGroup"; break;
            case Tool::SketchFillet:
            case Tool::SketchChamfer: active = "sketchFilletGroup"; break;
            case Tool::Offset: active = "offset"; break;
            case Tool::Project: active = "project"; break;
            case Tool::Pierce: active = "pierce"; break;
            case Tool::Dimension: active = "dimension"; break;
            case Tool::Constraint: active = "constraints"; break;
        }
    }
    // «Construction geometry» is a mode toggle (FX7-SK-001): pressed while the mode is on,
    // together with the drawing tool's button.
    const bool constructionOn = mode_ == Mode::Sketch && sketch_->constructionMode();
    for (const QString& n : sketchToolButtonNames())
        if (QToolButton* b = ribbonButton(n)) b->setChecked(n == active || (n == "construction" && constructionOn));
}

}  // namespace construe::app
