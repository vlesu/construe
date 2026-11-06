// Semantic command availability (app/command_availability.h, spec/app-ux.md §7.5).
#include "app/command_availability.h"

namespace construe::app {

namespace {

using S = AvailabilityState;

bool part(const S& s) { return s.mode == Mode::Part; }
bool isPart(const S& s) { return s.kind == ElementKind::Part; }
bool isAsm(const S& s) { return s.kind == ElementKind::Assembly; }
bool isDrawing(const S& s) { return s.kind == ElementKind::Drawing; }
bool partOrBody(const S& s) { return s.kind == ElementKind::Part || s.kind == ElementKind::Body; }
bool sketch(const S& s) { return s.mode == Mode::Sketch; }
// Modes where document undo and redo apply (§7.4): not in the feature, export, settings, section, annotation,
// mate and interference panels.
bool undoMode(const S& s) {
    switch (s.mode) {
        case Mode::FeatureEdit:
        case Mode::ExportStl:
        case Mode::Import:
        case Mode::Settings:
        case Mode::Section:
        case Mode::Annotation:
        case Mode::Mate:
        case Mode::Weld:
        case Mode::InstancePattern:
        case Mode::Repair:
        case Mode::Interference:
        case Mode::Library:
        case Mode::LibrarySave:
        case Mode::Solution:
        case Mode::SolutionRecord: return false;
        default: return true;
    }
}

// Conditions in words — a single place shared with app-ux §7.5 (texts match the «Condition» table column).
// Not interface texts: the specification text is compared by the tests (app-ux §9.5). i18n-skip-begin
constexpr std::string_view ALWAYS = "всегда";
constexpr std::string_view PART_MODE = "режим `Part`";
constexpr std::string_view PART_EDIT = "`Part`, активна деталь";
constexpr std::string_view PART_BODY = "`Part`, активна деталь, у неё есть тело";
constexpr std::string_view SHEET_WALL = "`Part`, активна деталь, есть параметры листа, нет базовой стенки";
constexpr std::string_view SHEET_BODY = "`Part`, активна деталь, есть тело листа, деталь не преобразована в обычную (S5-SHEET-006)";
constexpr std::string_view PATTERNABLE = "`Part`, активна деталь, есть неподавленная фича с инструментом";
constexpr std::string_view PLAIN_EDIT = "`Part`, активна обычная (не листовая) деталь";
constexpr std::string_view PLAIN_BODY = "`Part`, активна обычная (не листовая) деталь, есть тело";
constexpr std::string_view PART_OR_BODY = "`Part`, активна деталь или тело";
constexpr std::string_view BODY = "`Part`, активно импортированное тело";
constexpr std::string_view SECTION = "`Part`, активна деталь или тело, есть тело";
constexpr std::string_view PMI = "`Part`, активна деталь или тело, есть тело (грани)";
constexpr std::string_view SKETCH = "`Sketch`";
constexpr std::string_view SKETCH_ENT = "`Sketch`, в эскизе есть сущности (не проекции)";
constexpr std::string_view SKETCH_GROUP_SEL = "`Sketch`, в выделении есть элемент связанной группы (массива)";
constexpr std::string_view SKETCH_WEAK = "`Sketch`, в эскизе есть слабые размеры";
constexpr std::string_view SKETCH_PROJECT = "`Sketch`, во входном состоянии эскиза есть тело (рёбра, вершины)";
constexpr std::string_view NORMAL_TO = "3D-вид показан; `Sketch` или `Part` с выбранной плоскостью";
constexpr std::string_view VIEW3D = "3D-вид показан";
constexpr std::string_view ASM = "`Part`, активна сборка";
constexpr std::string_view ASM_FILE = "`Part`, активна сборка, у документа есть путь";
constexpr std::string_view ASM_MATE = "`Part`, активна сборка, экземпляров не меньше двух";
constexpr std::string_view ASM_INTERF = "`Part`, активна сборка, показанных неподавленных экземпляров не меньше двух";
constexpr std::string_view ASM_SECTION = "`Part`, активна сборка, есть экземпляры";
constexpr std::string_view DRW = "`Part`, активен чертёж";
constexpr std::string_view DRW_FILE = "`Part`, активен чертёж, у документа есть путь";
constexpr std::string_view DRW_VIEW = "`Part`, активен чертёж, на листе есть вид";
constexpr std::string_view UNDO = "режим, где действует отмена (§7.4), история отмены не пуста";
constexpr std::string_view REDO = "режим, где действует отмена (§7.4), история повтора не пуста";
constexpr std::string_view EXPORT_MESH = "`Part`, есть тело или экземпляр, экспорт не идёт";
constexpr std::string_view EXPORT_SHEET = "`Part`, активен чертёж или спецификация, экспорт не идёт";
constexpr std::string_view EXPORT_DXF = "`Part`, активен чертёж, экспорт не идёт";
constexpr std::string_view EXPORT_FLAT = "`Part`, активна деталь с телом листа или сборка с экземплярами, экспорт не идёт";
constexpr std::string_view EXPORT_FCSTD = "`Part`, экспорт не идёт";
constexpr std::string_view EXPORT_FCSTD_ACTIVE = "`Part`, активна деталь или тело, экспорт не идёт";
constexpr std::string_view LINKS = "в документе есть ссылки на другие файлы";
constexpr std::string_view LIBRARY_PARTS = "`Part`, в документе есть элементы библиотеки";
constexpr std::string_view CONFIRM = "принять панель можно (как OK панели)";
constexpr std::string_view NEVER = "никогда (заглушка «нет файлов»)";
constexpr std::string_view TREE_SKETCH = "`Part` или `PickPlane`, активна деталь или сборка";
constexpr std::string_view TREE_PROPS = "`Part`, элемент — деталь, тело или сборка";
// i18n-skip-end

bool always(const S&) { return true; }
bool never(const S&) { return false; }
bool partMode(const S& s) { return part(s); }
bool partEdit(const S& s) { return part(s) && isPart(s); }
bool partBody(const S& s) { return partEdit(s) && s.hasBody; }
bool patternable(const S& s) { return partEdit(s) && s.hasPatternable; }
// Tools without meaning for a sheet metal part (§7.5, FX7-SH-002, FX8-SH-004): revolve, sweep, loft, coil, shell, draft.
bool plainEdit(const S& s) { return partEdit(s) && !s.sheetPart; }
bool plainBody(const S& s) { return partBody(s) && !s.sheetPart; }
bool sheetWall(const S& s) { return partEdit(s) && s.sheetParams && !s.sheetBody; }
bool sheetBody(const S& s) { return partEdit(s) && s.sheetBody && s.sheetPart; }
bool partOrBodyEdit(const S& s) { return part(s) && partOrBody(s); }
bool bodyCmd(const S& s) { return part(s) && s.kind == ElementKind::Body; }
bool section(const S& s) { return partOrBodyEdit(s) && s.hasBody; }
bool sketchMode(const S& s) { return sketch(s); }
bool sketchEnt(const S& s) { return sketch(s) && s.sketchHasEntities; }
bool sketchGroupSel(const S& s) { return sketch(s) && s.sketchGroupSelected; }
bool sketchWeak(const S& s) { return sketch(s) && s.sketchWeak > 0; }
bool sketchProject(const S& s) { return sketch(s) && s.bodyTopology; }
bool normalTo(const S& s) { return s.view3d && (sketch(s) || (part(s) && s.planeSelected)); }
bool view3d(const S& s) { return s.view3d; }
bool asmCmd(const S& s) { return part(s) && isAsm(s); }
bool asmFile(const S& s) { return asmCmd(s) && s.hasPath; }
bool asmMate(const S& s) { return asmCmd(s) && s.instances >= 2; }
bool asmInterf(const S& s) { return asmCmd(s) && s.shownInstances >= 2; }
bool asmSection(const S& s) { return asmCmd(s) && s.instances >= 1; }
bool drw(const S& s) { return part(s) && isDrawing(s); }
bool drwFile(const S& s) { return drw(s) && s.hasPath; }
bool drwView(const S& s) { return drw(s) && s.hasSheetView; }
bool undo(const S& s) { return undoMode(s) && s.canUndo; }
bool redo(const S& s) { return undoMode(s) && s.canRedo; }
bool exportMesh(const S& s) { return part(s) && !s.exportPending && (s.hasBody || s.instances > 0); }
bool exportSheet(const S& s) { return part(s) && !s.exportPending && (isDrawing(s) || s.kind == ElementKind::Specification); }
bool exportDxf(const S& s) { return part(s) && !s.exportPending && isDrawing(s); }
bool exportFlat(const S& s) { return part(s) && !s.exportPending && ((isPart(s) && s.sheetBody) || (isAsm(s) && s.instances > 0)); }
bool exportFcstd(const S& s) { return part(s) && !s.exportPending; }
bool exportFcstdActive(const S& s) { return exportFcstd(s) && (s.kind == ElementKind::Part || s.kind == ElementKind::Body); }
bool links(const S& s) { return s.hasLinks; }
bool libraryParts(const S& s) { return part(s) && s.hasLibraryParts; }
bool confirm(const S& s) { return s.confirmAcceptable; }
bool treeSketch(const S& s) { return (part(s) || s.mode == Mode::PickPlane) && (isPart(s) || isAsm(s)); }
bool treeProps(const S& s) { return part(s) && s.subjectKind != ElementKind::Drawing && s.subjectKind != ElementKind::Specification; }

}  // namespace

const std::vector<CommandRule>& commandRules() {
    static const std::vector<CommandRule> rules = {
        // Above the ribbon and the «File» and «Edit» menus.
        {"ribbon_new", PART_MODE, partMode},
        {"actionNew", PART_MODE, partMode},
        {"ribbon_open", PART_MODE, partMode},
        {"actionOpen", PART_MODE, partMode},
        {"recent_*", PART_MODE, partMode},
        {"recentEmpty", NEVER, never},
        {"actionImportStep", PART_MODE, partMode},
        {"actionImportMesh", PART_MODE, partMode},  // S4-MESH-001
        {"actionImportFcstd", PART_MODE, partMode},  // S4A-UI-001
        {"actionDocumentSettings", PART_MODE, partMode},
        {"statusUnits", PART_MODE, partMode},
        {"ribbon_save", ALWAYS, always},
        {"actionSave", ALWAYS, always},
        {"actionSaveAs", ALWAYS, always},
        {"actionSaveAsQuick", ALWAYS, always},
        {"ribbon_undo", UNDO, undo},
        {"actionUndo", UNDO, undo},
        {"ribbon_redo", REDO, redo},
        {"actionRedo", REDO, redo},
        {"ribbon_rebuild", ALWAYS, always},
        {"actionRebuild", ALWAYS, always},
        {"ribbon_newPart", PART_MODE, partMode},
        {"actionNewPlainPart", PART_MODE, partMode},  // FX7-SH-002
        {"actionNewSheetPart", PART_MODE, partMode},
        {"ribbon_newAssembly", PART_MODE, partMode},
        {"ribbon_newDrawing", PART_MODE, partMode},
        {"actionRefreshLinks", LINKS, links},
        {"actionLibraryUpdate", LIBRARY_PARTS, libraryParts},  // S6-LIB-004
        {"actionSaveAsLibrary", PART_EDIT, partEdit},           // S6-LIB-006
        {"actionRecordSolution", ASM, asmCmd},                  // S6-SOL-009
        {"actionExportStl", EXPORT_MESH, exportMesh},
        {"actionExport3mf", EXPORT_MESH, exportMesh},
        {"actionExportStep", EXPORT_MESH, exportMesh},
        {"actionExportPdf", EXPORT_SHEET, exportSheet},
        {"actionExportSvg", EXPORT_SHEET, exportSheet},
        {"actionPrint", EXPORT_SHEET, exportSheet},  // FX7-DRW-004
        {"actionExportDxf", EXPORT_DXF, exportDxf},
        {"actionExportFlatDxf", EXPORT_FLAT, exportFlat},  // S5-DXF-001
        {"actionExportFcstd", EXPORT_FCSTD, exportFcstd},              // S4A-UI-001
        {"actionExportFcstdActive", EXPORT_FCSTD_ACTIVE, exportFcstdActive},
        {"actionAppSettings", ALWAYS, always},
        {"actionExit", ALWAYS, always},
        // View: above the ribbon on the right and the window actions of the hotkeys (FX8-UI-005).
        {"ribbon_isoView", VIEW3D, view3d},
        {"isoView*", VIEW3D, view3d},
        {"ribbon_fit", VIEW3D, view3d},
        {"ribbon_displayStyle", VIEW3D, view3d},
        {"actionStyle*", VIEW3D, view3d},
        {"actionMenuView*", VIEW3D, view3d},
        {"ribbon_normalTo", NORMAL_TO, normalTo},
        {"actionNormalTo", NORMAL_TO, normalTo},
        {"actionAbout", ALWAYS, always},
        {"actionHelp", ALWAYS, always},  // FX7-UI-007
        {"actionHotkeys", ALWAYS, always},
        {"actionReportProblem", ALWAYS, always},  // S4-BUG-001
        {"actionCheckUpdates", ALWAYS, always},   // REL-CT-002
        // Confirmation corner.
        {"confirmOk", CONFIRM, confirm},
        {"confirmCancel", ALWAYS, always},
        // «Part» tab.
        {"ribbon_sketch", PART_EDIT, partEdit},
        {"ribbon_refPlaneGroup", PART_EDIT, partEdit},
        {"tool_planeOffset", PART_EDIT, partEdit},
        {"tool_planeAngle", PART_EDIT, partEdit},
        {"tool_planeThreePoints", PART_EDIT, partEdit},
        {"tool_planeMid", PART_EDIT, partEdit},
        {"ribbon_refAxisGroup", PART_EDIT, partEdit},
        {"tool_axisTwoPoints", PART_EDIT, partEdit},
        {"tool_axisPlanes", PART_EDIT, partEdit},
        {"tool_axisEdge", PART_BODY, partBody},
        {"tool_axisFace", PART_BODY, partBody},
        {"ribbon_extrude", PART_EDIT, partEdit},
        {"ribbon_revolve", PLAIN_EDIT, plainEdit},
        {"ribbon_sweepGroup", PLAIN_EDIT, plainEdit},  // «Sweep ▾» (FX8-RB-001)
        {"tool_sweep", PLAIN_EDIT, plainEdit},
        {"tool_coil", PLAIN_EDIT, plainEdit},  // FX7-PT-004
        {"ribbon_loftGroup", PLAIN_EDIT, plainEdit},  // «Loft ▾» (FX8-RB-001)
        {"tool_loft", PLAIN_EDIT, plainEdit},
        {"tool_draft", PLAIN_BODY, plainBody},
        {"ribbon_threadGroup", PART_BODY, partBody},  // «Thread ▾» (FX8-RB-001)
        {"tool_thread", PART_BODY, partBody},
        {"tool_hole", PART_BODY, partBody},
        {"ribbon_gear", PART_BODY, partBody},  // S6-GEAR-002
        {"ribbon_fillet", PART_BODY, partBody},   // in a sheet part — corners through the thickness (FX8-SH-004)
        {"ribbon_chamfer", PART_BODY, partBody},
        {"ribbon_shell", PLAIN_BODY, plainBody},
        {"ribbon_patternGroup", PATTERNABLE, patternable},
        {"tool_patternLinear", PATTERNABLE, patternable},
        {"tool_patternCircular", PATTERNABLE, patternable},
        {"ribbon_mirrorFeature", PATTERNABLE, patternable},
        {"ribbon_sheetMetal", PART_EDIT, partEdit},  // S5-SHEET-*, S5-FLAT-001
        {"ribbon_sheetWall", SHEET_WALL, sheetWall},  // separate buttons of the sheet ribbon (FX8-SH-002)
        {"ribbon_sheetFlange", SHEET_BODY, sheetBody},
        {"ribbon_sheetBend", SHEET_BODY, sheetBody},
        {"ribbon_sheetCut", SHEET_BODY, sheetBody},
        {"ribbon_sheetUnfold", SHEET_BODY, sheetBody},
        {"actionRecognize", BODY, bodyCmd},     // S4-REC-017
        {"tree_recognize", BODY, bodyCmd},      // body context menu (FX8-RB-003)
        {"ribbon_section", SECTION, section},
        {"ribbon_measure", SECTION, section},  // FX10-MEAS-001
        {"ribbon_roughnessGroup", PART_OR_BODY, partOrBodyEdit},  // «Roughness ▾» (FX8-RB-001)
        {"tool_pmiRoughness", PMI, section},
        {"tool_pmiRest", PART_OR_BODY, partOrBodyEdit},  // FX5-PMI-001
        {"ribbon_geometricGroup", PMI, section},  // «Geometric tolerance ▾» (FX12-PMI-005)
        {"tool_pmiGeometric", PMI, section},
        {"tool_pmiDatum", PMI, section},
        {"ribbon_variables", ALWAYS, always},
        // «Sketch» tab.
        {"ribbon_select", SKETCH, sketchMode},
        {"tool_selectAll", SKETCH_ENT, sketchEnt},
        {"ribbon_construction", SKETCH, sketchMode},  // also a drawing mode (FX7-SK-001)
        {"ribbon_trim", SKETCH_ENT, sketchEnt},
        {"ribbon_point", SKETCH, sketchMode},
        {"ribbon_line", SKETCH, sketchMode},
        {"ribbon_centerline", SKETCH, sketchMode},
        {"ribbon_rectangleGroup", SKETCH, sketchMode},
        {"tool_rectangle", SKETCH, sketchMode},
        {"tool_centerRectangle", SKETCH, sketchMode},
        {"ribbon_polygonGroup", SKETCH, sketchMode},
        {"tool_polygonInscribed", SKETCH, sketchMode},
        {"tool_polygonCircumscribed", SKETCH, sketchMode},
        {"tool_star", SKETCH, sketchMode},  // S4-SKT-003
        {"ribbon_circleGroup", SKETCH, sketchMode},
        {"tool_circle", SKETCH, sketchMode},
        {"tool_circle3", SKETCH, sketchMode},
        {"tool_ellipse", SKETCH, sketchMode},  // S4-SKT-004
        {"ribbon_splineGroup", SKETCH, sketchMode},  // S4A-SPL-001
        {"tool_splineControl", SKETCH, sketchMode},
        {"tool_splineThrough", SKETCH, sketchMode},
        {"ribbon_arcGroup", SKETCH, sketchMode},
        {"tool_arc3", SKETCH, sketchMode},
        {"tool_arcCenter", SKETCH, sketchMode},
        {"tool_tangentArc", SKETCH, sketchMode},
        {"ribbon_text", SKETCH, sketchMode},
        {"ribbon_mirror", SKETCH_ENT, sketchEnt},
        {"ribbon_sketchFilletGroup", SKETCH_ENT, sketchEnt},
        {"tool_sketchFillet", SKETCH_ENT, sketchEnt},
        {"tool_sketchChamfer", SKETCH_ENT, sketchEnt},
        {"ribbon_offset", SKETCH, sketchMode},  // S7F-SK-005: also with projections only or empty (body edge)
        {"ribbon_sketchPatternGroup", SKETCH_ENT, sketchEnt},
        {"tool_sketchPatternLinear", SKETCH_ENT, sketchEnt},
        {"tool_sketchPatternCircular", SKETCH_ENT, sketchEnt},
        {"tool_sketchUnlink", SKETCH_GROUP_SEL, sketchGroupSel},
        {"ribbon_project", SKETCH_PROJECT, sketchProject},
        {"ribbon_dimension", SKETCH, sketchMode},
        {"tool_fullyDefine", SKETCH_WEAK, sketchWeak},     // «Dimension ▾» menu (FX7-SK-013)
        {"tool_fullyDefineAll", SKETCH_WEAK, sketchWeak},
        {"ribbon_constraints", SKETCH, sketchMode},
        {"actionConstraint_*", SKETCH, sketchMode},
        {"tool_showConstraints", SKETCH, sketchMode},  // item of «Constraints ▾» (FX7-SK-014)
        {"ribbon_exitSketch", SKETCH, sketchMode},
        // «Assembly» tab.
        {"ribbon_insert", ASM, asmCmd},
        {"insert_*", ASM, asmCmd},
        {"insertInContext", ASM, asmCmd},
        {"insertFromFile", ASM_FILE, asmFile},
        {"insertFromLibrary", ASM, asmCmd},  // S6-LIB-003, S6-LIB-004
        {"ribbon_asmSketch", ASM, asmCmd},
        {"ribbon_asmPlane", ASM, asmCmd},
        {"ribbon_asmAxis", ASM, asmCmd},
        {"ribbon_asmCut", ASM, asmCmd},   // S5-AFEAT-001
        {"ribbon_asmHole", ASM, asmCmd},  // S5-AFEAT-002
        {"ribbon_asmSolution", ASM, asmCmd},  // S6-SOL-002…008
        {"ribbon_mate", ASM_MATE, asmMate},
        {"ribbon_weld", ASM_MATE, asmMate},  // S5-WELD-001
        {"ribbon_interference", ASM_INTERF, asmInterf},
        {"ribbon_sectionAsm", ASM_SECTION, asmSection},
        {"ribbon_asmPattern", ASM_SECTION, asmSection},    // FX7-ASM-007
        {"ribbon_asmRoughness", ASM_SECTION, asmSection},  // annotations on instance faces (FX12-PMI-003)
        {"ribbon_asmGeometricGroup", ASM_SECTION, asmSection},
        {"tool_asmPmiGeometric", ASM_SECTION, asmSection},
        {"tool_asmPmiDatum", ASM_SECTION, asmSection},
        {"ribbon_exploded", ASM_SECTION, asmSection},      // FX7-ASM-005
        {"tool_explodeReset", ASM_SECTION, asmSection},    // FX8-ASM-006: an item of the «Exploded view ▾» menu
        {"ribbon_asmProperties", ASM, asmCmd},
        {"ribbon_newSpecification", ASM, asmCmd},
        // «Drawing» tab.
        {"tool_addSheet", DRW, drw},  // FX12-DRW-006: an item of the «Edit title block data ▾» menu
        {"ribbon_titleBlock", DRW, drw},
        {"ribbon_baseView", DRW, drw},
        {"baseView_*", DRW, drw},
        {"baseViewFromFile", DRW_FILE, drwFile},
        {"ribbon_threeViews", DRW, drw},
        {"threeViews_*", DRW, drw},
        {"threeViewsFromFile", DRW_FILE, drwFile},
        {"ribbon_projectedView", DRW_VIEW, drwView},
        {"ribbon_sectionView", DRW_VIEW, drwView},
        {"ribbon_detailView", DRW_VIEW, drwView},
        {"ribbon_drawingDimension", DRW_VIEW, drwView},
        {"ribbon_modelDimensions", DRW_VIEW, drwView},
        {"ribbon_modelMarks", DRW_VIEW, drwView},  // FX9-DRW-008
        {"ribbon_drawingRoughness", DRW_VIEW, drwView},  // FX11-DRW-001
        {"ribbon_drawingGeometric", DRW_VIEW, drwView},
        {"tool_drawingDatum", DRW_VIEW, drwView},
        {"tool_drawingRestRoughness", DRW_VIEW, drwView},  // FX12-DRW-007: an item of the «Roughness ▾» menu  // FX12-DRW-008: an item of the «Geometric tolerance ▾» menu
        {"ribbon_showUndimensioned", DRW_VIEW, drwView},  // FX7-DRW-003
        {"ribbon_drawingBalloon", DRW_VIEW, drwView},     // FX14-DRW-002
        {"ribbon_partsList", DRW_VIEW, drwView},          // FX14-DRW-003
        {"ribbon_sheetText", DRW, drw},                   // FX7-DRW-006
        {"ribbon_drawingConsGroup", DRW_VIEW, drwView},   // FX14-DRW-006: constructions on a view
        {"tool_consLine", DRW_VIEW, drwView},
        {"tool_consCircle", DRW_VIEW, drwView},
        {"tool_consArc", DRW_VIEW, drwView},
        {"tool_consPitch", DRW_VIEW, drwView},
        {"tool_consAxis", DRW_VIEW, drwView},
        // Tree context menu (§4.5).
        {"tree_newPart", PART_MODE, partMode},
        {"tree_newSheetPart", PART_MODE, partMode},  // FX7-SH-002
        {"tree_convertToSheet", PART_MODE, partMode},  // S5-SHEET-006
        {"tree_convertToPart", PART_MODE, partMode},
        {"tree_newAssembly", PART_MODE, partMode},
        {"tree_sketch", TREE_SKETCH, treeSketch},
        {"tree_properties", TREE_PROPS, treeProps},
        {"tree_newSpecification", ASM, asmCmd},  // FX8-UI-007
        {"tree_edit", PART_MODE, partMode},
    };
    return rules;
}

const CommandRule* commandRule(std::string_view name) {
    const CommandRule* prefix = nullptr;
    for (const auto& r : commandRules()) {
        if (r.name == name) return &r;
        if (!r.name.empty() && r.name.back() == '*') {
            const std::string_view p = r.name.substr(0, r.name.size() - 1);
            if (name.substr(0, p.size()) == p && name.size() > p.size() && (!prefix || p.size() > prefix->name.size() - 1)) prefix = &r;
        }
    }
    return prefix;
}

bool commandAvailable(std::string_view name, const AvailabilityState& s) {
    const CommandRule* r = commandRule(name);
    return !r || r->available(s);
}

// «Part» tab: two ribbons by the part kind (app-ux §2.3, FX8-SH-001, FX8-SH-002, FX8-RB-001, FX8-RB-002).
const std::vector<std::string_view>& partRibbonLayout(bool sheet) {
    static const std::vector<std::string_view> plain = {
        "sketch", "refPlaneGroup", "refAxisGroup", "|",
        "extrude", "revolve", "sweepGroup", "loftGroup", "threadGroup", "gear", "|",
        "fillet", "chamfer", "shell", "|",
        "patternGroup", "mirrorFeature", "|",
        "sheetMetal", "|",
        "section", "measure", "|",
        "roughnessGroup", "geometricGroup"};
    static const std::vector<std::string_view> sheetPart = {
        "sketch", "refPlaneGroup", "refAxisGroup", "|",
        "sheetMetal", "sheetWall", "sheetFlange", "sheetBend", "sheetCut", "sheetUnfold", "|",
        "extrude", "threadGroup", "gear", "|",
        "fillet", "chamfer", "|",
        "patternGroup", "mirrorFeature", "|",
        "section", "measure", "|",
        "roughnessGroup", "geometricGroup"};
    return sheet ? sheetPart : plain;
}

bool sheetRibbon(const AvailabilityState& s) { return s.kind == ElementKind::Part && s.sheetPart; }

}  // namespace construe::app
