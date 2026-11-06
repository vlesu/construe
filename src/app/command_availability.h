// Semantic command availability (spec/app-ux.md §7.5, FX4-UI-002): the single place where the
// mode, the kind of the active object and the document contents decide whether a ribbon button, menu item or tree
// command is available. Pure logic without widgets: the main window gathers the state and applies it to all controls.
#pragma once

#include "app/mode_layout.h"

#include <string>
#include <string_view>
#include <vector>

namespace construe::app {

// Kind of the document's active object (document.md §2.4): part, body (import), assembly, drawing, bill of materials.
enum class ElementKind { Part, Body, Assembly, Drawing, Specification };

// State that availability depends on (app-ux §7.5, «condition» column).
struct AvailabilityState {
    Mode mode = Mode::Part;
    ElementKind kind = ElementKind::Part;
    bool view3d = true;            // 3D view is shown (not a drawing and not a bill of materials)
    bool hasBody = false;          // the active part or body has a body (final state)
    bool hasPatternable = false;   // there is an unsuppressed feature with a tool (extrude, revolve, sweep, loft, hole)
    bool sheetParams = false;      // the active part has sheet parameters (sheetMetal, S5-SHEET-001)
    bool sheetBody = false;        // the active part has a sheet body (base wall, S5-SHEET-002)
    bool sheetPart = false;        // the active part is a sheet metal part (document.md §2.4 isSheetPart, FX7-SH-002)
    int instances = 0;             // instances in the active assembly
    int shownInstances = 0;        // shown and unsuppressed instances
    bool hasSheetView = false;     // the current drawing sheet has a view (all views are model views)
    bool hasPath = false;          // the document has a path (saved to a file)
    bool hasLinks = false;         // the document has links to other files
    bool hasLibraryParts = false;  // the document has library part copies (document.md §2.13)
    bool canUndo = false, canRedo = false;
    bool planeSelected = false;    // a plane is selected (in the view or the tree)
    bool sketchHasEntities = false;  // the open sketch has its own entities (not projections)
    int sketchWeak = 0;            // weak dimensions and relations in the open sketch (S4-WEAK-004)
    bool sketchGroupSelected = false; // the sketch selection contains a member of a linked group (S4-SKT-005)
    bool otherSketch = false;      // there is another sketch above the open sketch in the history
    bool bodyTopology = false;     // the input state of the open sketch has body edges or vertices
    bool confirmAcceptable = true; // the panel can be accepted now (like the panel's OK button)
    bool exportPending = false;    // an export is in progress
    // Subject of a tree context-menu command (the item the menu was opened on): its kind.
    ElementKind subjectKind = ElementKind::Part;
};

struct CommandRule {
    std::string_view name;       // element objectName; a trailing «*» means a prefix (items created per document)
    std::string_view condition;  // condition in words — as in the app-ux §7.5 table
    bool (*available)(const AvailabilityState&);
};

// Rule table (order as in app-ux §7.5).
const std::vector<CommandRule>& commandRules();
// Rule for an objectName (an exact match takes precedence over a prefix); nullptr — no rule.
const CommandRule* commandRule(std::string_view objectName);
// Command availability; a command without a rule is available (the coverage test requires a rule for each, FX4-UI-002).
bool commandAvailable(std::string_view objectName, const AvailabilityState& s);

// «Part» tab buttons in order (app-ux §2.3 «Two ribbons», FX8-SH-001): names without «ribbon_», «|» — a
// separator. A button not in the list of the current ribbon is hidden (it cannot become available in this part kind).
const std::vector<std::string_view>& partRibbonLayout(bool sheet);
// The sheet ribbon is shown: the active object is a sheet metal part.
bool sheetRibbon(const AvailabilityState& s);

}  // namespace construe::app
