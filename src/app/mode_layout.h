// Main window modes (app-ux §2.2) and what is visible and available in each: ribbon tab, button groups, properties
// panel, tree commands, view tool, planes and axes, confirmation corner. A pure function without Qt widgets.
#pragma once

#include <cstddef>
#include <vector>

namespace construe::app {

enum class Mode { Part, PickPlane, FeatureEdit, Sketch, ExportStl, Import, Settings, Properties, Section, Mate, Weld, Interference, Annotation, Library, LibrarySave, Solution, SolutionRecord, InstancePattern, Repair };
inline constexpr Mode ALL_MODES[] = {Mode::Part, Mode::PickPlane, Mode::FeatureEdit, Mode::Sketch, Mode::ExportStl, Mode::Import, Mode::Settings, Mode::Properties, Mode::Section, Mode::Mate, Mode::Weld, Mode::Interference, Mode::Annotation, Mode::Library, Mode::LibrarySave, Mode::Solution, Mode::SolutionRecord, Mode::InstancePattern, Mode::Repair};

// What accepting, cancelling and Esc do in a mode (app-ux §2.4 "mode → action", S7F-RF-008): ✓ of the corner and of
// the command panel header, Ctrl+Enter and Enter in a panel field run `accept`; ✗ of the corner and of the header —
// `cancel`; Esc after the Esc ladder steps — `escape`. The main window runs the action; None — nothing to run.
enum class ConfirmAction {
    None,
    ExitSketch, CancelSketch, SketchEscape,
    AcceptFeature, CancelFeature,
    AcceptExport, AcceptImport, AcceptSettings, AcceptSection, AcceptAnnotation, AcceptMate, AcceptWeld,
    AcceptInstancePattern, AcceptLibrary, AcceptLibrarySave, AcceptSolution, AcceptSolutionRecord,
    RepairDone, RepairRevert,
    ToPart,         // close the panel without changing the document
    ToPartRebuild,  // the same, and the view is rebuilt (without the panel's section or interference highlight)
};
struct ModeConfirm {
    ConfirmAction accept = ConfirmAction::None;
    ConfirmAction cancel = ConfirmAction::None;
    ConfirmAction escape = ConfirmAction::None;  // Part: None — the main window's own Esc in the part (drawing, selection)
};
ModeConfirm modeConfirm(Mode m);

struct ModeLayout {
    int ribbonTab = 0;            // 0 — "Part", 1 — "Sketch"
    bool sketchCommands = false;  // sketch tool buttons and "Exit sketch"
    bool partCommands = false;    // feature buttons ("Sketch", "Extrude", …)
    bool propsVisible = false;    // properties panel (feature, export, settings)
    bool treeCommands = false;    // tree commands (double click, context menu)
    bool sketchTool = false;      // view tool is the sketcher (otherwise — selection in the part)
    bool planesVisible = true;    // origin planes
    bool axesVisible = false;     // origin axes (a revolve panel with an "Axis" field is open, §3.4)
    bool keepSelection = false;   // selection in the view is kept when entering the mode
    bool confirmOk = false;       // confirmation corner: ✓ (the mode has an accept action, modeConfirm)
    bool confirmCancel = false;   // confirmation corner: ✗ (the mode has a cancel action)
    bool pickPlaneHint = false;   // "Select a plane" hint in the status bar
    bool rightPanel = false;      // the right panel: connection wizard, library (app-ux §8д, §8г; FX8-BLT-004, S7F-UI-008); the left one stays the tree
};

// revolvePanel — in FeatureEdit mode the revolve panel is open.
ModeLayout modeLayout(Mode m, bool revolvePanel);

// Splitter sizes when the program opens the panel `index` (app-ux §2.2, S7F-UI-007): a panel narrower than minWidth
// (the user has narrowed it down to zero earlier, or it never had a size) gets minWidth; the width is taken from the
// panel `from` (the view). sizes — QSplitter::sizes() of all children (hidden ones — 0).
std::vector<int> openPanelSizes(std::vector<int> sizes, size_t index, int minWidth, size_t from);

}  // namespace construe::app
