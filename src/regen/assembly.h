// Assembly evaluation (spec/assembly.md §2): instances with world placements and part states.
#pragma once

#include "base/error.h"
#include "base/math.h"

#include <Eigen/Geometry>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace construe::regen {

struct PartState;

// Rigid motion p → R·p + t (assembly.md §1).
struct Motion3 {
    Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
    Vec3 t = Vec3::Zero();
    Point3 apply(const Point3& p) const { return R * p + t; }
    Vec3 applyDir(const Vec3& v) const { return R * v; }
    Motion3 operator*(const Motion3& o) const { return {R * o.R, R * o.t + t}; }
};
Motion3 motionOf(const std::array<double, 3>& position, const std::array<double, 4>& quaternion);

// Linked documents of the session (assembly.md §5а): absolute path (generic, '/') → document or read error.
struct LinkedDoc {
    std::shared_ptr<const void> keep;  // ownership of the document (doc::Document) without the header depending on doc
    const void* doc = nullptr;
    std::optional<Error> error;
};
using LinkMap = std::map<std::string, LinkedDoc>;
// Normalized absolute path of the file of reference ref relative to directory dir; a library part reference
// ("library:<id>", document.md §2.13) — as is.
std::string linkPath(const std::string& dir, const std::string& ref);
std::string parentDir(const std::string& path);

// Geometric primitive of a mate reference (assembly.md §7.2): plane (point — centroid, w — outward normal), axis, line
// (point and direction) or point; r — cylinder radius (0 — none).
struct MatePrimitive {
    enum class Kind { Plane, Axis, Line, Point } kind = Kind::Point;
    Vec3 p = Vec3::Zero();
    Vec3 w = Vec3::UnitZ();
    double r = 0;
    int teeth = 0;  // pitch cylinder of a gear rim (FX10-GEAR-004): number of teeth z; 0 — not a rim
};

// Mate of the system of an underdetermined assembly (assembly.md §2б): solved again by every enclosing assembly.
struct SystemMate {
    std::string id;    // own mate ID, a nested one — "<path of the node>/<its ID>"
    std::string type;
    std::array<std::string, 2> leaf;     // entry carrying each primitive (path in the assembly); empty — the assembly frame
    std::array<MatePrimitive, 2> prim;   // in the frame of that entry
    double value = 0;                    // distance / angle, evaluated
};

struct AssemblyState;

struct InstanceEntry {
    std::string path;      // "i3", nested — "i3/i1"
    std::string instance;  // instance ID within its own assembly
    std::string name;
    std::string element;   // inserted element
    std::string file;      // file of the inserted element (absolute path) or empty — this document
    std::string kind;      // part / assembly (empty — element not found)
    int depth = 0;         // 0 — instance of the active assembly
    Motion3 world;
    bool fixed = false, hidden = false, transparent = false, suppressed = false;
    std::optional<Error> error;  // ASM_REF_NOT_FOUND, ASM_CYCLE
    std::shared_ptr<const PartState> state;  // part
    uint64_t stateKey = 0;                   // key of the part's final state (regen.md §5.1)
    int freeDof = -1;  // free placement parameters (mates, §7.5); −1 — instance is not in the system
    std::string connection;  // derived instance of a connection (solutions.md §7.6): the connection feature ID
    std::string pattern;     // child instance of an instance pattern (§7е.3): the pattern ID
    std::string copyOf;      // child instance of a pattern: path of the copied entry of the source
    // Node of an underdetermined nested assembly (§2б, FX9-ASM-010): its evaluated state with the free units and the mate
    // system; null — a rigid whole (fully determined, or a part).
    std::shared_ptr<const AssemblyState> flex;
    // Free unit of a nested underdetermined assembly moved by the solve of this assembly relative to its parent entry
    // (§2б: then its placement goes into the instance's form).
    bool formChanged = false;
    // Sketch entities of the part for mate references to its construction (assembly.md §7.2, FX9-ASM-001): sketch ID →
    // entity ID → kind and point IDs: {"line", p1, p2}, {"circle", center}, {"arc", center, start}.
    std::shared_ptr<const std::map<std::string, std::map<std::string, std::vector<std::string>>>> sketchEntities;
};

// Instance pattern status (assembly.md §7е.3 item 5).
struct PatternStatus {
    std::string id;
    std::optional<Error> error;  // ASM_PATTERN_SOURCE, ASM_PATTERN_BAD_REF, ASM_PATTERN_BAD_VALUE
    int children = 0;
};

// Mate status (assembly.md §7.5).
struct MateStatus {
    std::string id;
    std::optional<Error> error;    // ASM_MATE_REF_LOST, ASM_MATE_BAD_REF, ASM_MATE_CONFLICT
    std::optional<Error> warning;  // ASM_MATE_REDUNDANT
};

// Assembly sketch not depending on instances (assembly.md §7б, FX7-ASM-006): world frame and solved entities.
struct DatumSketch {
    Plane frame;
    std::map<std::string, Point3> points;                         // point entity ID → world point
    std::map<std::string, std::pair<Point3, Point3>> lines;       // line ID → world start and end
    std::map<std::string, std::pair<Point3, double>> circles;     // circle / arc ID → world center, radius
};

// Reference geometry and visible sketch of an assembly for showing it where the assembly is nested (assembly.md §2
// «Assembly objects», FX9-ASM-008): in the frame of entry `node` ("" — the assembly itself), world = node.world · local.
struct AssemblyDatum {
    enum class Kind { Plane, Axis, Sketch } kind = Kind::Plane;
    std::string node;  // path of the entry carrying it in this assembly; empty — its own
    std::string id;    // feature ID in the assembly that owns it
    std::string name;  // feature name
    Plane plane;                                // Plane
    Point3 point = Point3::Zero();              // Axis: point and unit direction
    Vec3 dir = Vec3::UnitZ();
    std::vector<std::vector<Point3>> polylines;  // Sketch: non-construction lines, arcs, circles, ellipses and splines
    // Sketch: its points, lines, circles and arcs in the carrying frame — supports of the enclosing assemblies' mates
    // (assembly.md §7.2, FX9-ASM-014).
    DatumSketch entities;
};

struct AssemblyState {
    std::vector<InstanceEntry> instances;  // parent before children
    std::vector<MateStatus> mates;         // in document order
    int dof = 0;                           // DOF of the mate system
    // Assembly reference geometry not depending on instances (§7б): feature id → world plane / axis (point, direction).
    std::map<std::string, Plane> datumPlanes;
    std::map<std::string, std::pair<Point3, Vec3>> datumAxes;
    std::map<std::string, DatumSketch> datumSketches;  // FX7-ASM-006
    std::vector<PatternStatus> patterns;                // instance patterns in document order (§7е)
    // Exploded view (§7д.2 item 5): evaluated exploded; automatic explosion — the mates suppressed in the mode.
    bool exploded = false, explodedAuto = false;
    // Instances new to the saved exploded view were exploded (§7д.2 item 2, FX12-ASM-002): explodedSuppressed is this
    // evaluation's S, to be written into the document like that of the automatic explosion.
    bool explodedNew = false;
    std::vector<std::string> explodedSuppressed;
    // Exploded view (§7д.5, FX10-ASM-004): fasteners placed by the rule — entry path → the top-level instance whose
    // displacement they follow (empty — none).
    std::map<std::string, std::string> explodedFollow;
    // Exploded view (§7д.2 item 4, FX14-PMI-001): entry path → its world displacement by the explosion (exploded world ·
    // assembled world⁻¹) — what carries objects given in assembled coordinates (annotations on instance faces) with
    // the part; empty outside the exploded view.
    std::map<std::string, Motion3> explodedMove;
    // Underdetermined assembly (§2б, FX9-ASM-010): free bodies of the mate system with freeDof > 0 (paths) and the
    // system's mates (own and of the nested underdetermined assemblies); empty — fully determined.
    std::vector<std::string> flexUnits;
    std::vector<SystemMate> systemMates;
    // Reference planes, axes and visible sketches of this assembly and of its nested ones (FX9-ASM-008), own first.
    std::vector<AssemblyDatum> datums;
    const PatternStatus* patternStatus(const std::string& id) const {
        for (const auto& p : patterns)
            if (p.id == id) return &p;
        return nullptr;
    }
    const MateStatus* mate(const std::string& id) const {
        for (const auto& m : mates)
            if (m.id == id) return &m;
        return nullptr;
    }
    const InstanceEntry* find(const std::string& path) const {
        for (const auto& e : instances)
            if (e.path == path) return &e;
        return nullptr;
    }
    // Displacement by the explosion of the deepest entry containing `path` (a body or entry path, «/»-separated;
    // FX14-PMI-001); identity outside the exploded view or for no entry.
    Motion3 explodedMoveOf(const std::string& path) const {
        for (std::string p = path; !explodedMove.empty();) {
            if (const auto it = explodedMove.find(p); it != explodedMove.end()) return it->second;
            const size_t sl = p.rfind('/');
            if (sl == std::string::npos) break;
            p.resize(sl);
        }
        return {};
    }
};
using AssemblyPtr = std::shared_ptr<const AssemblyState>;

}  // namespace construe::regen
