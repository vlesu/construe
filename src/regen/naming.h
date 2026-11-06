// Stable topological naming (spec/naming.md). Submodule of regen.
#pragma once

#include "base/error.h"
#include "kernel/kernel.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <string>
#include <vector>

namespace construe::naming {

inline constexpr double NAMING_ORDER_TOL = 1e-5;
inline constexpr double NAMING_MEASURE_REL_TOL = 1e-9;
inline constexpr int NAMING_VERSION = 1;

// State body with base names of faces (index = FaceIdx).
struct NamedBody {
    std::string id;
    kernel::Shape shape;
    std::vector<std::string> bases;
};

struct FaceEntry {
    int body = 0;      // body index in the list
    int face = 0;      // FaceIdx
    std::string base, full;
    std::set<std::string> nb;  // neighbor bases (§4.2)
    Point3 key = Point3::Zero();  // geometric key (face center of mass)
    double measure = 0;           // area
};

struct EdgeEntry {
    int body = 0;
    int logical = 0;               // LogicalEdgeIdx
    std::vector<int> kernelEdges;
    int f1 = 0, f2 = 0;            // indices into the table's faces
    std::set<std::string> ends;    // bases of the faces at the ends (§4.4)
    std::string full;
    Point3 key = Point3::Zero();
    double measure = 0;  // length
};

struct VertexEntry {
    int body = 0;
    int vertex = 0;
    std::vector<int> faces;  // indices into the table's faces
    std::string full;
    Point3 key = Point3::Zero();
};

enum class EntityKind { Face, Edge, Vertex };

struct EntityRef {
    EntityKind kind = EntityKind::Face;
    int index = 0;  // within the corresponding list of the table
    bool operator<(const EntityRef& o) const { return std::tie(kind, index) < std::tie(o.kind, o.index); }
    bool operator==(const EntityRef&) const = default;
};

struct NameTable {
    std::vector<FaceEntry> faces;
    std::vector<EdgeEntry> edges;
    std::vector<VertexEntry> vertices;
    std::map<std::string, EntityRef> byName;
    std::vector<Error> warnings;  // NAMING_GEOMETRIC_TIE, NAMING_ORPHAN_FACE
    // Assembly (naming.md §5.3а): instance path → the part's table and the offsets of its entries in this table.
    struct Sub {
        std::shared_ptr<const NameTable> table;
        int face0 = 0, edge0 = 0, vertex0 = 0;
    };
    std::map<std::string, Sub> subs;
};

// Origin when composing operations within a feature (§4.1).
enum class OpKind { Generate, Boolean, Combine, Fillet, Chamfer };

struct InputNames {
    std::vector<std::string> bases;        // face bases of the input
    std::vector<std::string> edgeNames;    // full names of the input's logical edges (fillet/chamfer)
    std::map<int, std::string> vertexNames;  // full names of the input's principal vertices (by VertexIdx)
    std::map<int, std::string> innerEdgeOf;  // inner wall edge (LogicalEdgeIdx) → full name of its outer edge (FX5-FEAT-001)
};

// Face bases of a kernel operation result.
std::vector<std::string> assignBases(OpKind op, const std::string& featureId, const kernel::OpResult& result,
                                     const std::vector<InputNames>& inputs, std::vector<Error>* warnings = nullptr);

// Face base of pattern copy featureId with label label (§4.1, S2-FEAT-002): GenBase → F.copy(L,b),
// merge(…) → merge(copies of the members).
std::string copyBase(const std::string& featureId, const std::string& label, const std::string& base);

// Face base of the mirror copy featureId (§4.1, S2-FEAT-003): GenBase → F.mirror(b), merge(…) → merge(copies of the members).
std::string mirrorBase(const std::string& featureId, const std::string& base);

// Base of an inner shell face featureId (§4.1, S2-FEAT-004): GenBase → F.inner(b), merge(…) → merge(…).
std::string innerBase(const std::string& featureId, const std::string& base);

// Outer base of an inner shell face base (§4.1, FX5-FEAT-001): F.inner(b) → b; merge(F.inner(m₁),…) with one F →
// merge(m₁,…); otherwise nullopt.
std::optional<std::string> outerBaseOfInner(const std::string& base);

// Full names of the state.
NameTable computeNames(const kernel::Kernel& k, const std::vector<NamedBody>& bodies);

enum class ResolveStatus { Exact, Remapped, Split, Ambiguous, Lost };

struct ResolveResult {
    ResolveStatus status = ResolveStatus::Lost;
    std::vector<EntityRef> entities;
    bool merged = false, ordinal = false;
    std::optional<Error> error;  // NAMING_REF_PARSE / NAMING_REF_KIND
};

// Assembly name "<path>/<name>" (§5.3а, §5.3б): path — leading segments "i<digits>", each followed by "/" (topological
// names may contain "/": side(s/e)); no path — path is empty, leaf — the whole name.
struct PathName {
    std::string path, leaf;
};
PathName splitPath(const std::string& name);
inline bool hasPath(const std::string& name) { return !splitPath(name).path.empty(); }

ResolveResult resolve(const std::string& name, EntityKind expected, const NameTable& table);

// What a consumer does with one resolved reference (§5.4 "Общие правила"): Lost — parse/kind error or LOST (no entities);
// Ambiguous — AMBIGUOUS, and SPLIT for a single-entity consumer (allowSplit = false); otherwise Resolved (entities).
enum class RefFate { Resolved, Lost, Ambiguous };
inline RefFate refFate(const ResolveResult& rr, bool allowSplit) {
    if (rr.error || rr.status == ResolveStatus::Lost || rr.entities.empty()) return RefFate::Lost;
    if (rr.status == ResolveStatus::Ambiguous || (rr.status == ResolveStatus::Split && !allowSplit)) return RefFate::Ambiguous;
    return RefFate::Resolved;
}

// Parsing a name into parts (§2.3).
struct ParsedName {
    EntityKind kind = EntityKind::Face;
    std::string base;                 // face: base
    std::vector<std::string> disc;    // face/edge: discriminator (may be absent)
    bool hasDisc = false;
    int ordinal = 0;                  // 0 — none
    std::vector<std::string> parts;   // edge: 2 full face names; vertex: face names
};

Result<ParsedName> parse(const std::string& name);
// Canonical form of a parsed name (§3): format(parse(n)) == n.
std::string format(const ParsedName& p);

}  // namespace construe::naming
