// Assembly mate and manipulator choices of the client (spec/app-ux.md §8а; FX9-ASM-002, -003, -004, -009): pure
// functions over the assembly state, used by the main window and tested on their own.
#pragma once

#include "api/session.h"

#include <QPointF>
#include <QRectF>

#include <map>

#include <functional>
#include <string>
#include <vector>

namespace construe::app {

// Types tried instead of an impossible automatic one, in the preference order (FX9-ASM-002); "distance" and "angle" are
// tried with the current value of the pair (Session::mateCurrentValue).
std::vector<std::string> mateTypeFallbacks(const std::string& type);

// Top-level instance of a path ("i3/i1" → "i3").
std::string topInstanceOf(const std::string& path);

// The instance being mated (FX9-ASM-003): the series instance, otherwise the not fixed top-level instance of the panel's
// references that is the latest in `order` (the one the tree shows the mate under); empty — none.
std::string matedInstanceOf(const std::vector<std::string>& refs, const std::string& series, const std::vector<std::string>& order,
                            const std::function<bool(const std::string&)>& fixed);

// A top-level part or subassembly the manipulator can pull (FX8-ASM-003, FX9-ASM-009): not fixed, not fully defined, not
// a pattern child or a fastener, without error; a subassembly moves as a whole (it has no part state of its own).
bool movableEntry(const regen::InstanceEntry* e);

// Free body of an underdetermined subassembly that carries `path` (a body or nested instance path, FX9-ASM-010,
// assembly.md §2б): the manipulator of that nested part; empty — the part moves only with its subassembly.
std::string nestedManipUnit(const regen::AssemblyState& st, const std::string& path);

// What a press on the free-degree manipulator grabs (S7F-ASM-003, app-ux §8а): `off` — the press from the icon centre
// on the screen, `px` — FREE_MANIP_PX. The centre — a free pull; an arrow — a pull along `axis` (unit screen direction
// of the nearer arrow pair); the ring — a turn about the view axis.
struct ManipGrip {
    enum class Kind { Free, Arrow, Ring } kind = Kind::Free;
    QPointF axis;
};
ManipGrip manipGripAt(const QPointF& off, double px);

// Screen angle (radians, counterclockwise on the screen positive; screen y grows down) swept about `centre` from
// `press` to `cur` (S7F-ASM-003).
double screenTurnAngle(const QPointF& centre, const QPointF& press, const QPointF& cur);

// Turn of the manipulator ring accumulated over a whole drag (FX12-ASM-001, app-ux §8а): the cursor angle about `centre`
// (counterclockwise on the screen positive) is tracked move by move, each step taken the shorter way, so the sum is
// continuous and unbounded — a full circle of the mouse is a full turn (2π), two circles — 4π, without a jump at ±π.
class ScreenTurn {
public:
    void start(const QPointF& centre, const QPointF& press);
    // The accumulated angle after the cursor moved to `cur`; a point nearer than 1 px to the centre keeps the angle.
    double update(const QPointF& cur);
    double angle() const { return acc_; }

private:
    QPointF c_;
    double last_ = 0, acc_ = 0;
    bool valid_ = false;
};

// Instances chosen by the selection box (FX13-ASM-002, app-ux §8а): `projected` — the screen projections of the shown
// mesh vertices of each top-level instance. `window` (box drawn left to right) — all of them inside the box (bounds
// inclusive); otherwise (right to left) — at least one. Instances without points are never chosen; the result is in
// the order of `projected`.
std::vector<std::string> instancesInBox(const QRectF& box, bool window, const std::map<std::string, std::vector<QPointF>>& projected);

// The group of the group manipulator (FX13-ASM-002): the distinct top-level instances of `tops` (in first-seen order) if
// there are two or more and each passes `movable`; otherwise empty.
std::vector<std::string> manipGroupOf(const std::vector<std::string>& tops, const std::function<bool(const std::string&)>& movable);

// The rigid move of a group (FX13-ASM-002): each start placement composed with `delta` (delta ∘ start).
std::map<std::string, regen::Motion3> groupMoved(const std::map<std::string, regen::Motion3>& start, const regen::Motion3& delta);

// The unit a pick, a box or a tree node moves (FX14-ASM-001, app-ux §8а): on the exploded view (`st.exploded`) a derived
// connection fastener (solutions.md §7.6) is a unit of its own — the longest prefix of `path` (a body or entry path,
// split at «/») that is an entry with `connection`; otherwise (and outside the exploded view) — the top-level instance.
std::string moveUnitOf(const regen::AssemblyState& st, const std::string& path);

// A unit of `moveUnitOf` the group manipulator can move (FX14-ASM-001): `movableEntry`, or on the exploded view a derived
// connection fastener with a state, without error, not suppressed.
bool movableUnit(const regen::AssemblyState& st, const std::string& id);

}  // namespace construe::app
