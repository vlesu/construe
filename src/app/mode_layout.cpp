// Main window modes (app/mode_layout.h).
#include "app/mode_layout.h"

#include <algorithm>

namespace construe::app {

ModeConfirm modeConfirm(Mode m) {
    using A = ConfirmAction;
    switch (m) {
        case Mode::Part: return {};
        case Mode::PickPlane: return {A::None, A::None, A::ToPart};
        case Mode::Sketch: return {A::ExitSketch, A::CancelSketch, A::SketchEscape};  // ✗ — undo the session (S7F-SK-004)
        case Mode::FeatureEdit: return {A::AcceptFeature, A::CancelFeature, A::CancelFeature};
        case Mode::ExportStl: return {A::AcceptExport, A::ToPart, A::ToPart};
        case Mode::Import: return {A::AcceptImport, A::ToPart, A::ToPart};
        case Mode::Settings: return {A::AcceptSettings, A::ToPart, A::ToPart};
        case Mode::Properties: return {A::ToPart, A::None, A::ToPart};  // properties apply immediately — only ✓
        case Mode::Section: return {A::AcceptSection, A::ToPartRebuild, A::ToPartRebuild};
        case Mode::Mate: return {A::AcceptMate, A::ToPart, A::ToPart};
        case Mode::Weld: return {A::AcceptWeld, A::ToPart, A::ToPart};
        case Mode::Interference: return {A::ToPartRebuild, A::None, A::ToPartRebuild};
        case Mode::Annotation: return {A::AcceptAnnotation, A::ToPart, A::ToPart};
        case Mode::Library: return {A::AcceptLibrary, A::ToPart, A::ToPart};
        case Mode::LibrarySave: return {A::AcceptLibrarySave, A::ToPart, A::ToPart};
        case Mode::Solution: return {A::AcceptSolution, A::ToPart, A::ToPart};
        case Mode::SolutionRecord: return {A::AcceptSolutionRecord, A::ToPart, A::ToPart};
        case Mode::InstancePattern: return {A::AcceptInstancePattern, A::ToPart, A::ToPart};
        case Mode::Repair: return {A::RepairDone, A::RepairRevert, A::RepairDone};  // §5.5в: Esc — as ✓
    }
    return {};
}

ModeLayout modeLayout(Mode m, bool revolvePanel) {
    ModeLayout l;
    const bool sketch = m == Mode::Sketch;
    l.ribbonTab = sketch ? 1 : 0;
    l.sketchCommands = sketch;
    l.partCommands = m == Mode::Part;
    l.propsVisible = m == Mode::FeatureEdit || m == Mode::ExportStl || m == Mode::Import || m == Mode::Settings || m == Mode::Properties || m == Mode::Section || m == Mode::Mate || m == Mode::Weld || m == Mode::Interference || m == Mode::Annotation || m == Mode::LibrarySave || m == Mode::SolutionRecord || m == Mode::InstancePattern;
    l.propsVisible = l.propsVisible || m == Mode::Repair;  // repair wizard of lost references (§5.5в, S7F-FEAT-003)
    l.rightPanel = m == Mode::Solution || m == Mode::Library;  // FX8-BLT-004, S7F-UI-008
    l.treeCommands = m == Mode::Part || m == Mode::Sketch;
    l.sketchTool = sketch;
    l.planesVisible = !sketch;
    l.axesVisible = m == Mode::FeatureEdit && revolvePanel;
    l.keepSelection = m == Mode::Part || m == Mode::Properties;
    const ModeConfirm c = modeConfirm(m);
    l.confirmOk = c.accept != ConfirmAction::None;
    l.confirmCancel = c.cancel != ConfirmAction::None;
    l.pickPlaneHint = m == Mode::PickPlane;
    return l;
}

std::vector<int> openPanelSizes(std::vector<int> sizes, size_t index, int minWidth, size_t from) {
    if (index >= sizes.size() || from >= sizes.size() || index == from || sizes[index] >= minWidth) return sizes;
    const int need = minWidth - sizes[index];
    sizes[index] = minWidth;
    // The width comes from the view; if the view is narrower than that, the splitter shrinks the others itself.
    sizes[from] = std::max(0, sizes[from] - need);
    return sizes;
}

}  // namespace construe::app
