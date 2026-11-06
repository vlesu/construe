// Our features → PartDesign features (spec/fcstd.md §12г): the reverse of the mapping table §7.1; references to
// edges and faces — FreeCAD sub-element names in the shape written for the previous feature (§12г.3).
#include "fcstd/export_internal.h"

#include "base/charconv.h"

#include "regen/hole_tables.h"
#include "sketch/sketch.h"

#include <charconv>
#include <cmath>
#include <numbers>

namespace construe::fcstd::xp {

namespace {

using Result_ = std::variant<std::vector<write::Object>, Unmapped>;

constexpr double RAD2DEG = 180.0 / std::numbers::pi;

Unmapped unmapped(const std::string& reason) { return Unmapped{reason}; }

std::string str(const Json& j, const std::string& key, const std::string& fallback = {}) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}
bool flag(const Json& j, const std::string& key) { return j.contains(key) && j[key].is_boolean() && j[key].get<bool>(); }

// Enumeration item lists written with the object (FreeCAD 1.x; spec/fcstd.md §7.1, §12г.2).
const std::vector<std::string> ISO_COARSE = {
    "M1x0.25", "M1.1x0.25", "M1.2x0.25", "M1.4x0.3", "M1.6x0.35", "M1.8x0.35", "M2x0.4", "M2.2x0.45", "M2.5x0.45", "M3x0.5",
    "M3.5x0.6", "M4x0.7", "M4.5x0.75", "M5x0.8", "M6x1.0", "M7x1.0", "M8x1.25", "M9x1.25", "M10x1.5", "M11x1.5", "M12x1.75",
    "M14x2.0", "M16x2.0", "M18x2.5", "M20x2.5", "M22x2.5", "M24x3.0", "M27x3.0", "M30x3.5", "M33x3.5", "M36x4.0", "M39x4.0",
    "M42x4.5", "M45x4.5", "M48x5.0", "M52x5.0", "M56x5.5", "M60x5.5", "M64x6.0", "M68x6.0"};
const std::vector<std::string> ISO_FINE = {
    "M1x0.2", "M1.1x0.2", "M1.2x0.2", "M1.4x0.2", "M1.6x0.2", "M1.8x0.2", "M2x0.25", "M2.2x0.25", "M2.5x0.35", "M3x0.35",
    "M3.5x0.35", "M4x0.5", "M4.5x0.5", "M5x0.5", "M5.5x0.5", "M6x0.75", "M7x0.75", "M8x0.75", "M8x1.0", "M9x0.75", "M9x1.0",
    "M10x0.75", "M10x1.0", "M10x1.25", "M11x0.75", "M11x1.0", "M12x1.0", "M12x1.25", "M12x1.5", "M14x1.0", "M14x1.25",
    "M14x1.5", "M15x1.0", "M15x1.5", "M16x1.0", "M16x1.5", "M17x1.0", "M17x1.5", "M18x1.0", "M18x1.5", "M18x2.0", "M20x1.0",
    "M20x1.5", "M20x2.0", "M22x1.0", "M22x1.5", "M22x2.0", "M24x1.0", "M24x1.5", "M24x2.0", "M25x1.0", "M25x1.5", "M25x2.0",
    "M27x1.0", "M27x1.5", "M27x2.0", "M28x1.0", "M28x1.5", "M28x2.0", "M30x1.0", "M30x1.5", "M30x2.0", "M30x3.0", "M32x1.5",
    "M32x2.0", "M33x1.5", "M33x2.0", "M33x3.0", "M35x1.5", "M35x2.0", "M36x1.5", "M36x2.0", "M36x3.0", "M39x1.5", "M39x2.0",
    "M39x3.0", "M40x1.5", "M40x2.0", "M40x3.0", "M42x1.5", "M42x2.0", "M42x3.0", "M42x4.0", "M45x1.5", "M45x2.0", "M45x3.0",
    "M45x4.0", "M48x1.5", "M48x2.0", "M48x3.0", "M48x4.0", "M50x1.5", "M50x2.0", "M50x3.0", "M52x1.5", "M52x2.0", "M52x3.0",
    "M52x4.0", "M55x1.5", "M55x2.0", "M55x3.0", "M55x4.0", "M56x1.5", "M56x2.0", "M56x3.0", "M56x4.0", "M58x1.5", "M58x2.0",
    "M58x3.0", "M58x4.0", "M60x1.5", "M60x2.0", "M60x3.0", "M60x4.0", "M62x1.5", "M62x2.0", "M62x3.0", "M62x4.0", "M64x1.5",
    "M64x2.0", "M64x3.0", "M64x4.0", "M65x1.5", "M65x2.0", "M65x3.0", "M65x4.0", "M68x1.5", "M68x2.0", "M68x3.0", "M68x4.0",
    "M70x1.5", "M70x2.0", "M70x3.0", "M70x4.0", "M70x6.0", "M72x1.5", "M72x2.0", "M72x3.0", "M72x4.0", "M72x6.0", "M75x1.5",
    "M75x2.0", "M75x3.0", "M75x4.0", "M75x6.0", "M76x1.5", "M76x2.0", "M76x3.0", "M76x4.0", "M76x6.0", "M80x1.5", "M80x2.0",
    "M80x3.0", "M80x4.0", "M80x6.0", "M85x2.0", "M85x3.0", "M85x4.0", "M85x6.0", "M90x2.0", "M90x3.0", "M90x4.0", "M90x6.0",
    "M95x2.0", "M95x3.0", "M95x4.0", "M95x6.0", "M100x2.0", "M100x3.0", "M100x4.0", "M100x6.0"};

// Pitch of a thread size name "M6x1.0".
double pitchOf(const std::string& s) {
    const size_t x = s.find('x');
    double v = 0;  // from_chars: independent of the process locale
    if (x != std::string::npos) parseDouble(s.data() + x + 1, s.data() + s.size(), v);
    return v;
}

struct Obj {
    write::Object o;
    std::vector<std::pair<std::string, std::string>> exprs;

    Obj(const std::string& name, const std::string& type, const doc::Feature& f) {
        o.name = name;
        o.type = type;
        o.props.push_back(write::string("Label", f.name));
    }
    void add(write::Prop p) { o.props.push_back(std::move(p)); }
    // A length/angle/number field: the value and its binding (§12е.2).
    void field(Ctx& c, const std::string& prop, const std::string& text, std::optional<expr::Dim> dim, const std::string& type,
               double scale = 1.0, const std::string& fallback = {}) {
        double v = c.value(text, dim).value_or(0) * scale;
        if (type.find("Integer") != std::string::npos) {
            add(write::integer(prop, std::llround(v), type));
        } else {
            add(write::real(prop, v, type));
        }
        (void)fallback;
        if (auto b = c.binding(text, dim, prop)) exprs.push_back({prop, *b});
    }
    std::vector<write::Object> done() {
        if (!exprs.empty()) o.props.push_back(write::expressions(exprs));
        return {o};
    }
};

std::optional<std::string> sketchObject(Ctx& c, const Json& j, const std::string& key = "sketch") {
    const std::string id = str(j, key);
    auto it = c.objectOf.find(id);
    if (id.empty() || it == c.objectOf.end()) return std::nullopt;
    return it->second;
}

// A LineRef or a plane normal → (object, sub); positioned — the line must lie on the reference (axis of rotation).
std::optional<std::pair<std::string, std::string>> lineLink(Ctx& c, const Json& ref) {
    if (ref.contains("datum")) {
        auto it = c.originOf.find(ref["datum"].get<std::string>());
        if (it == c.originOf.end()) return std::nullopt;
        return std::make_pair(it->second, std::string());
    }
    if (ref.contains("edge")) {
        auto subs = c.edgeSubs({ref["edge"].get<std::string>()});
        if (!subs || subs->size() != 1) return std::nullopt;
        return std::make_pair(c.prevSolid, (*subs)[0]);
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> planeLink(Ctx& c, const Json& ref) {
    if (ref.contains("datum")) {
        auto it = c.originOf.find(ref["datum"].get<std::string>());
        if (it == c.originOf.end()) return std::nullopt;
        return std::make_pair(it->second, std::string());
    }
    if (ref.contains("face")) {
        auto subs = c.faceSubs({ref["face"].get<std::string>()});
        if (!subs || subs->size() != 1) return std::nullopt;
        return std::make_pair(c.prevSolid, (*subs)[0]);
    }
    if (ref.contains("feature")) {
        auto it = c.objectOf.find(ref["feature"].get<std::string>());
        if (it == c.objectOf.end()) return std::nullopt;
        return std::make_pair(it->second, std::string());
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> originals(Ctx& c, const Json& j, std::string* why) {
    std::vector<std::string> out;
    for (const auto& id : j.value("features", Json::array())) {
        auto it = c.objectOf.find(id.get<std::string>());
        if (it == c.objectOf.end()) return *why = "original:" + id.get<std::string>(), std::nullopt;
        if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
    }
    if (out.empty()) return *why = "noOriginals", std::nullopt;
    return out;
}

Result_ extrude(Ctx& c, const doc::Feature& f, const Json& j, const std::string& name) {
    if (!j["profiles"].is_string()) return unmapped("profiles");
    auto sk = sketchObject(c, j);
    if (!sk) return unmapped("noProfile");
    const std::string op = str(j, "operation", "add");
    if (op != "add" && op != "new" && op != "cut") return unmapped("mode:operation=" + op);
    const bool pocket = op == "cut";
    const std::string end = str(j["end"], "type");
    Obj o(name, pocket ? "PartDesign::Pocket" : "PartDesign::Pad", f);
    o.add(write::linkSub("Profile", *sk, {}));
    if (end == "blind" || end == "symmetric") {
        o.add(write::enumeration("Type", 0));  // Length
        o.field(c, "Length", str(j["end"], "depth"), expr::LENGTH, "App::PropertyLength");
    } else if (pocket && (end == "throughAll" || end == "throughAllBoth")) {
        o.add(write::enumeration("Type", 1));  // ThroughAll
    } else {
        return unmapped("mode:end=" + end);
    }
    o.add(write::enumeration("SideType", end == "symmetric" || end == "throughAllBoth" ? 2 : 0));
    const bool reverse = str(j, "direction") == "reverse";
    // Pad goes along the sketch normal, Pocket against it (§7.1).
    o.add(write::boolean("Reversed", pocket ? !reverse : reverse));
    o.add(write::boolean("Refine", false));
    return o.done();
}

Result_ revolve(Ctx& c, const doc::Feature& f, const Json& j, const std::string& name) {
    if (!j["profiles"].is_string()) return unmapped("profiles");
    auto sk = sketchObject(c, j);
    if (!sk) return unmapped("noProfile");
    const std::string op = str(j, "operation", "add");
    if (op != "add" && op != "new" && op != "cut") return unmapped("mode:operation=" + op);
    const Json& ax = j["axis"];
    std::pair<std::string, std::string> axis;
    bool reversed = false;
    if (ax.contains("datum")) {
        auto l = lineLink(c, ax);
        if (!l) return unmapped("axis");
        axis = *l;
    } else if (ax.contains("sketchLine")) {
        const std::string line = ax["sketchLine"];
        auto ai = c.sketchAxes.find(str(j, "sketch"));
        if (ai != c.sketchAxes.end() && ai->second.count(line)) {
            axis = {*sk, "Axis" + std::to_string(ai->second.at(line))};
        } else {
            // A profile line on a built-in axis: that axis (its direction — ours or opposite).
            auto st = c.stateAfter(f.id);
            const doc::Feature* skf = c.element->find(str(j, "sketch")).get();
            const auto* sd = skf ? skf->sketchData() : nullptr;
            const sketch::Entity* le = sd ? sd->findEntity(line) : nullptr;
            if (!st || !le || !st->sketches.count(skf->id)) return unmapped("axis");
            const Plane& fr = st->sketches.at(skf->id).frame;
            const auto* a = sd->findEntity(le->p1);
            const auto* b = sd->findEntity(le->p2);
            if (!a || !b) return unmapped("axis");
            const Point3 pa = fr.toWorld({a->x, a->y}), pb = fr.toWorld({b->x, b->y});
            bool rev = false;
            const std::string d = detail::datumAxisFor(pa, pb - pa, &rev);
            if (d.empty()) return unmapped("axis");
            axis = {c.originOf.at(d), ""};
            reversed = rev;
        }
    } else {
        return unmapped("axis");
    }
    Obj o(name, op == "cut" ? "PartDesign::Groove" : "PartDesign::Revolution", f);
    o.add(write::linkSub("Profile", *sk, {}));
    o.add(write::linkSub("ReferenceAxis", axis.first, axis.second.empty() ? std::vector<std::string>{""} : std::vector<std::string>{axis.second}));
    const std::string type = str(j["angle"], "type");
    o.add(write::enumeration("Type", 0));  // Angle
    if (type == "full") {
        o.add(write::real("Angle", 360, "App::PropertyAngle"));
    } else {
        o.field(c, "Angle", str(j["angle"], "value"), expr::ANGLE, "App::PropertyAngle", RAD2DEG);
        if (type == "symmetric") o.add(write::boolean("Midplane", true));
    }
    o.add(write::boolean("Reversed", reversed));
    o.add(write::boolean("Refine", false));
    return o.done();
}

Result_ dress(Ctx& c, const doc::Feature& f, const Json& j, const std::string& name) {
    if (c.prevSolid.empty()) return unmapped("noBase");
    if (f.type == doc::FeatureType::Fillet || f.type == doc::FeatureType::Chamfer) {
        // A chamfer by a distance and an angle or two distances (FX7-PT-010): the reference face of FreeCAD is chosen
        // differently — not mapped (the result is carried as a shape).
        if (j.contains("mode")) return unmapped("mode:chamfer=" + str(j, "mode"));
        std::vector<std::string> edges;
        for (const auto& e : j["edges"]) edges.push_back(e.get<std::string>());
        auto subs = c.edgeSubs(edges);
        if (!subs) return unmapped("edges");
        const bool fillet = f.type == doc::FeatureType::Fillet;
        Obj o(name, fillet ? "PartDesign::Fillet" : "PartDesign::Chamfer", f);
        o.add(write::linkSub("Base", c.prevSolid, *subs));
        if (fillet) {
            o.field(c, "Radius", str(j, "radius"), expr::LENGTH, "App::PropertyQuantityConstraint");
        } else {
            o.add(write::enumeration("ChamferType", 0));  // Equal distance
            o.field(c, "Size", str(j, "distance"), expr::LENGTH, "App::PropertyQuantityConstraint");
        }
        o.add(write::boolean("Refine", false));
        return o.done();
    }
    std::vector<std::string> faces;
    for (const auto& e : j["faces"]) faces.push_back(e.get<std::string>());
    auto subs = c.faceSubs(faces);
    if (!subs) return unmapped("faces");
    if (f.type == doc::FeatureType::Shell) {
        Obj o(name, "PartDesign::Thickness", f);
        o.add(write::linkSub("Base", c.prevSolid, *subs));
        o.field(c, "Value", str(j, "thickness"), expr::LENGTH, "App::PropertyLength");
        o.add(write::enumeration("Mode", 0));  // Skin
        o.add(write::enumeration("Join", 0));  // Arc
        o.add(write::boolean("Intersection", false));
        // FreeCAD's thickness goes inward when Reversed is set (§7.1).
        o.add(write::boolean("Reversed", !flag(j, "outward")));
        o.add(write::boolean("Refine", false));
        return o.done();
    }
    // Draft: the pull direction of FreeCAD — against the outward normal of a neutral face, along the normal of a neutral
    // plane (FreeCAD's normal of the origin plane, §7.1); ours — the outward face normal, our plane normal.
    auto neutral = planeLink(c, j["neutral"]);
    if (!neutral) return unmapped("neutralPlane");
    const bool faceNeutral = j["neutral"].contains("face");
    bool reversed = flag(j, "reverse") != faceNeutral;
    if (j["neutral"].contains("datum")) {
        const std::string d = j["neutral"]["datum"];
        const auto ours = sketch::datumPlane(d);
        if (ours && (originPlacement(datumPlaneRole(d)).rotation * Vec3::UnitZ()).dot(ours->normal) < 0) reversed = !reversed;
    }
    Obj o(name, "PartDesign::Draft", f);
    o.add(write::linkSub("Base", c.prevSolid, *subs));
    o.add(write::linkSub("NeutralPlane", neutral->first, {neutral->second}));
    o.field(c, "Angle", str(j, "angle"), expr::ANGLE, "App::PropertyAngle", RAD2DEG);
    o.add(write::boolean("Reversed", reversed));
    o.add(write::boolean("Refine", false));
    return o.done();
}

Result_ hole(Ctx& c, const doc::Feature& f, const Json& j, const std::string& name) {
    auto sk = sketchObject(c, j);
    if (!sk) return unmapped("noProfile");
    const std::string kind = str(j, "kind");
    if (kind != "simple" && kind != "tapped") return unmapped("mode:kind=" + kind);
    const std::string size = str(j, "size");
    const auto row = regen::holeSize(size);
    if (!row) return unmapped("threadSize");
    double pitch = (*row)["pitches"][0]["pitch"].get<double>();
    if (kind == "tapped" && j.contains("pitch")) pitch = j["pitch"].get<double>();
    // The FreeCAD size item: the coarse list first, then the fine one.
    int threadType = 0, index = -1;
    for (int list = 0; list < 2 && index < 0; ++list) {
        const auto& items = list == 0 ? ISO_COARSE : ISO_FINE;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].rfind(size + "x", 0) == 0 && (kind == "simple" || std::abs(pitchOf(items[i]) - pitch) < 1e-9)) {
                threadType = list + 1, index = int(i);
                break;
            }
    }
    if (index < 0) return unmapped("threadSize");
    double diameter = 0;
    if (kind == "tapped") {
        for (const auto& p : (*row)["pitches"])
            if (std::abs(p["pitch"].get<double>() - pitch) < 1e-9) diameter = p["drill"];
        if (diameter <= 0) return unmapped("threadSize");
    } else {
        diameter = (*row)["clearance"][j.value("fit", std::string("normal"))];
    }
    const std::string fit = j.value("fit", std::string("normal"));
    Obj o(name, "PartDesign::Hole", f);
    o.add(write::linkSub("Profile", *sk, {}));
    o.add(write::integer("BaseProfileType", 1));  // sketch points only (§12г.2)
    o.add(write::enumeration("ThreadType", threadType));
    o.add(write::enumeration("ThreadSize", index, threadType == 1 ? ISO_COARSE : ISO_FINE));
    o.add(write::enumeration("ThreadFit", fit == "fine" ? 1 : fit == "loose" ? 2 : 0));
    o.add(write::boolean("Threaded", kind == "tapped"));
    o.add(write::boolean("ModelThread", false));
    o.add(write::enumeration("HoleCutType", 0));  // None
    const std::string end = str(j["end"], "type");
    if (end == "throughAll") {
        o.add(write::enumeration("DepthType", 1));
    } else {
        o.add(write::enumeration("DepthType", 0));  // Dimension
        o.field(c, "Depth", str(j["end"], "depth"), expr::LENGTH, "App::PropertyLength");
        o.add(write::enumeration("DrillPoint", 1));  // Angled
        o.add(write::real("DrillPointAngle", 118, "App::PropertyAngle"));
        o.add(write::boolean("DrillForDepth", false));
    }
    o.add(write::boolean("Reversed", flag(j, "reverse")));
    o.add(write::real("Diameter", diameter, "App::PropertyLength"));
    o.add(write::boolean("Refine", false));
    return o.done();
}

Result_ pattern(Ctx& c, const doc::Feature& f, const Json& j, const std::string& name) {
    std::string why;
    auto orig = originals(c, j, &why);
    if (!orig) return unmapped(why);
    const bool mirror = f.type == doc::FeatureType::Mirror;
    const std::string mode = str(j, "mode");
    Obj o(name, mirror ? "PartDesign::Mirrored" : mode == "linear" ? "PartDesign::LinearPattern" : "PartDesign::PolarPattern", f);
    o.add(write::linkList("Originals", *orig));
    o.add(write::enumeration("TransformMode", 0));  // Features
    if (mirror) {
        auto p = planeLink(c, j["plane"]);
        if (!p) return unmapped("mirrorPlane");
        o.add(write::linkSub("MirrorPlane", p->first, {p->second}));
    } else if (mode == "linear") {
        const Json& dirs = j["directions"];
        for (size_t k = 0; k < dirs.size() && k < 2; ++k) {
            const std::string sfx = k == 0 ? "" : "2";
            auto l = lineLink(c, dirs[k]["ref"]);
            if (!l) return unmapped("direction");
            o.add(write::linkSub("Direction" + sfx, l->first, {l->second}));
            o.add(write::boolean("Reversed" + sfx, flag(dirs[k], "reverse")));
            o.add(write::enumeration("Mode" + sfx, 1));  // Spacing
            const Json& cnt = dirs[k]["count"];
            o.field(c, "Occurrences" + sfx, cnt.is_string() ? cnt.get<std::string>() : doc::Json(cnt).dump(), expr::DIMLESS,
                    "App::PropertyIntegerConstraint");
            o.field(c, "Offset" + sfx, str(dirs[k], "spacing"), expr::LENGTH, "App::PropertyLength");
            // Per-gap spacings: −1 — the common offset. FreeCAD 1.1 fills the second list with zeros when it is missing
            // (all instances of the second direction at the first one), so both are written (§12г.2).
            const long long n = std::llround(c.value(cnt.is_string() ? cnt.get<std::string>() : doc::Json(cnt).dump(), expr::DIMLESS).value_or(1));
            const std::string file = name + ".Spacings" + sfx;
            c.parts[file] = write::floatListBytes(std::vector<double>(size_t(std::max(0LL, n - 1)), -1.0));
            o.add(write::floatList("Spacings" + sfx, file));
        }
    } else {
        const Json& ax = j["axis"];
        if (ax.contains("face")) return unmapped("axis");
        auto l = lineLink(c, ax);
        if (!l) return unmapped("axis");
        o.add(write::linkSub("Axis", l->first, {l->second}));
        o.add(write::boolean("Reversed", flag(j, "reverse")));
        o.add(write::enumeration("Mode", 0));  // Extent
        const Json& cnt = j["count"];
        o.field(c, "Occurrences", cnt.is_string() ? cnt.get<std::string>() : doc::Json(cnt).dump(), expr::DIMLESS,
                "App::PropertyIntegerConstraint");
        o.field(c, "Angle", str(j, "angle"), expr::ANGLE, "App::PropertyAngle", RAD2DEG);
    }
    o.add(write::boolean("Refine", false));
    return o.done();
}

Result_ sweepLoft(Ctx& c, const doc::Feature& f, const Json& j, const std::string& name) {
    const std::string op = str(j, "operation", "add");
    if (op != "add" && op != "new" && op != "cut") return unmapped("mode:operation=" + op);
    const bool cut = op == "cut";
    if (f.type == doc::FeatureType::Sweep) {
        if (!j["profiles"].is_string()) return unmapped("profiles");
        auto sk = sketchObject(c, j);
        if (!sk) return unmapped("noProfile");
        if (!j["path"].contains("sketch")) return unmapped("spine");
        auto path = sketchObject(c, j["path"]);
        if (!path) return unmapped("spine");
        Obj o(name, cut ? "PartDesign::SubtractivePipe" : "PartDesign::AdditivePipe", f);
        o.add(write::linkSub("Profile", *sk, {}));
        o.add(write::linkSub("Spine", *path, {}));
        o.add(write::enumeration("Mode", 0));            // Standard
        o.add(write::enumeration("Transition", 0));      // Transformed
        o.add(write::enumeration("Transformation", 0));  // Constant
        o.add(write::boolean("Refine", false));
        return o.done();
    }
    const Json& secs = j["sections"];
    std::vector<std::string> objs;
    for (const auto& s : secs) {
        if (s.contains("region")) return unmapped("sections");
        auto so = sketchObject(c, s);
        if (!so) return unmapped("sections");
        objs.push_back(*so);
    }
    if (objs.size() < 2) return unmapped("sections");
    Obj o(name, cut ? "PartDesign::SubtractiveLoft" : "PartDesign::AdditiveLoft", f);
    o.add(write::linkSub("Profile", objs[0], {}));
    std::vector<std::pair<std::string, std::string>> rest;
    for (size_t i = 1; i < objs.size(); ++i) rest.push_back({objs[i], ""});
    o.add(write::linkSubList("Sections", rest));
    o.add(write::boolean("Ruled", false));
    o.add(write::boolean("Closed", false));
    o.add(write::boolean("Refine", false));
    return o.done();
}

// Offset datum plane from a built-in plane (§12г.2): attached to the body origin plane with the offset that gives our
// plane; other modes are reference geometry without a counterpart.
Result_ datumPlane(Ctx& c, const doc::Feature& f, const Json& j, const std::string& name) {
    if (str(j, "mode") != "offset" || j.contains("to") || !j.contains("ref") || !j["ref"].contains("datum")) {
        c.note("datumNotMapped");
        return unmapped("skip");
    }
    auto st = c.stateAfter(f.id);
    if (!st || !st->planes.count(f.id)) {
        c.note("datumNotMapped");
        return unmapped("skip");
    }
    const Plane& pl = st->planes.at(f.id);
    const std::string d = j["ref"]["datum"];
    const write::Placement S = originPlacement(datumPlaneRole(d));
    const write::Placement P = write::frame(pl.origin, pl.x, pl.y);
    const Eigen::Quaterniond rel = (S.rotation.conjugate() * P.rotation).normalized();
    const Eigen::Matrix3d R = rel.toRotationMatrix();
    const write::Placement off = write::frame(Point3(S.rotation.conjugate() * (P.base - S.base)), R.col(0), R.col(1));
    Obj o(name, "PartDesign::Plane", f);
    o.o.extensions = {"Part::AttachExtension"};
    o.add(write::linkSubList("AttachmentSupport", {{c.originOf.at(d), ""}}));
    o.add(write::enumeration("MapMode", 5));  // FlatFace
    o.add(write::placement("AttachmentOffset", off));
    o.add(write::placement("Placement", P));
    // The offset distance along the support normal: + or − our distance.
    const double z = off.base.z();
    if (auto b = c.binding(str(j, "distance"), expr::LENGTH, "distance"))
        o.exprs.push_back({"AttachmentOffset.Base.z", z < 0 ? "-(" + *b + ")" : *b});
    return o.done();
}

}  // namespace

std::string objectBase(const doc::Feature& f) {
    const Json j = doc::featureToJsonValue(f);
    const std::string op = str(j, "operation", "add");
    const bool cut = op == "cut";
    switch (f.type) {
        case doc::FeatureType::Sketch: return "Sketch";
        case doc::FeatureType::Extrude: return cut || op == "intersect" ? "Pocket" : "Pad";
        case doc::FeatureType::Revolve: return cut || op == "intersect" ? "Groove" : "Revolution";
        case doc::FeatureType::Fillet: return "Fillet";
        case doc::FeatureType::Chamfer: return "Chamfer";
        case doc::FeatureType::Shell: return "Thickness";
        case doc::FeatureType::Draft: return "Draft";
        case doc::FeatureType::Hole: return "Hole";
        case doc::FeatureType::Pattern: return str(j, "mode") == "linear" ? "LinearPattern" : "PolarPattern";
        case doc::FeatureType::Mirror: return "Mirrored";
        case doc::FeatureType::Sweep: return cut || op == "intersect" ? "SubtractivePipe" : "AdditivePipe";
        case doc::FeatureType::Loft: return cut || op == "intersect" ? "SubtractiveLoft" : "AdditiveLoft";
        case doc::FeatureType::Plane: return "DatumPlane";
        case doc::FeatureType::Axis: return "DatumLine";
        case doc::FeatureType::Thread: return "Thread";
        default: return "";  // import, sheet metal: the base body (§12г.1)
    }
}

std::variant<std::vector<write::Object>, Unmapped> exportFeature(Ctx& c, const doc::Feature& f, const std::string& name) {
    const Json j = doc::featureToJsonValue(f);
    switch (f.type) {
        case doc::FeatureType::Extrude: return extrude(c, f, j, name);
        case doc::FeatureType::Revolve: return revolve(c, f, j, name);
        case doc::FeatureType::Fillet:
        case doc::FeatureType::Chamfer:
        case doc::FeatureType::Shell:
        case doc::FeatureType::Draft: return dress(c, f, j, name);
        case doc::FeatureType::Hole: return hole(c, f, j, name);
        case doc::FeatureType::Pattern:
        case doc::FeatureType::Mirror: return pattern(c, f, j, name);
        case doc::FeatureType::Sweep:
        case doc::FeatureType::Loft: return sweepLoft(c, f, j, name);
        case doc::FeatureType::Plane: return datumPlane(c, f, j, name);
        case doc::FeatureType::Axis:
            c.note("referenceOnly");
            return unmapped("skip");
        case doc::FeatureType::Thread:
            c.note("cosmeticThread");
            return unmapped("skip");
        default: return unmapped("unsupportedType");
    }
}

}  // namespace construe::fcstd::xp
