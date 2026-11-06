// Geometric sketch constraints as UI commands (app-ux §6.8): type, name, key — for the ribbon
// «Constraints ▾» (§2.3) and the context panel (§7.2).
#pragma once

#include "app/messages.h"
#include <QString>

#include <vector>

namespace construe::app {

struct ConstraintCommand {
    QString type;   // type in data / applyConstraint: coincident (coincidence or "on object"), horizontal, …
    QString label;
    QString key;  // empty — no hotkey
    // Label in the menu and tooltip: "Name (key)" or "Name".
    QString menuText() const { return key.isEmpty() ? label : label + " (" + key + ")"; }
};

// Built on each call: the labels follow the interface language (app-ux §9.5).
inline std::vector<ConstraintCommand> sketchConstraintCommands() {
    const std::vector<ConstraintCommand> c = {
        {"coincident", txt("ui.constraint.coincidentOnObject"), "I"}, {"horizontal", txt("ui.constraint.horizontal"), "H"},
        {"vertical", txt("ui.constraint.vertical"), "V"},               {"parallel", txt("ui.constraint.parallel"), "Shift+L"},
        {"perpendicular", txt("ui.constraint.perpendicular"), "Shift+T"}, {"tangent", txt("ui.constraint.tangent"), "N"},
        {"equal", txt("ui.common.equal"), "E"},                    {"concentric", txt("ui.constraint.concentric"), "O"},
        {"midpoint", txt("ui.common.midpoint"), "Shift+M"},            {"symmetric", txt("ui.common.symmetric"), "Y"},
        {"fix", txt("ui.constraint.fix"), "X"},                {"collinear", txt("ui.constraint.collinear"), ""},
        {"coradial", txt("ui.constraint.coradial"), ""},
        {"intersection", txt("ui.constraint.pointAtIntersection"), ""}, {"pathLength", txt("ui.constraint.pathLengthSumOfLengths"), ""},
        {"curvature", txt("ui.common.equalCurvature"), ""},  // at a spline joint (S4A-SPL-002)
    };
    return c;
}

}  // namespace construe::app
