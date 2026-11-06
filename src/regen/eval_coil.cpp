// Coil — spring / custom thread (spec/regen.md §4.20, FX7-PT-004): a profile swept along a helix about an axis.
#include "regen/eval_common.h"

#include <cmath>

namespace construe::regen {

using namespace detail;

EvalPtr Regenerator::evalCoil(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    const doc::Feature& f = c.f;
    const doc::Json& d = f.pattern()->def;
    // 1. Profile — as an extrusion (§4.2 items 1–2).
    const std::string skId = d["sketch"];
    const SketchEntry* se = detail::sketchOf(c, *input, skId);
    if (!se) return finish(c, input);
    doc::Profiles prof;
    if (d["profiles"].is_array()) {
        prof.all = false;
        for (const auto& r : d["profiles"]) prof.regions.push_back(r);
    }
    const auto regions = detail::selectRegions(c, *se, prof, skId);
    if (regions.empty()) return finish(c, input);
    // 2. Axis — as the revolve axis (§4.3): a line of the profile sketch, a datum axis or a reference axis.
    kernel::Axis axis;
    const doc::Json& ax = d["axis"];
    if (ax.contains("feature")) {
        auto it = input->axes.find(ax["feature"].get<std::string>());
        if (it == input->axes.end()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", ax["feature"].get<std::string>()), "/axis/feature");
            return finish(c, input);
        }
        axis.origin = it->second.point, axis.direction = it->second.dir;
    } else if (ax.contains("datum")) {
        const std::string a = ax["datum"];
        axis.direction = sketch::datumAxis(a).value_or(Vec3::UnitZ());
    } else {
        const std::string lineId = ax["sketchLine"];
        const auto skf = c.d.active().find(skId);
        const sketch::Entity* line = skf && skf->sketchData() ? skf->sketchData()->findEntity(lineId) : nullptr;
        const auto& pts = se->result.state.points;
        if (!line || !pts.count(line->p1) || !pts.count(line->p2)) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", lineId), "/axis/sketchLine");
            return finish(c, input);
        }
        const Point3 a = se->frame.toWorld(pts.at(line->p1)), b = se->frame.toWorld(pts.at(line->p2));
        axis.origin = a;
        axis.direction = (b - a).norm() > 0 ? Vec3((b - a).normalized()) : Vec3::Zero();
    }
    // 3. Values: pitch > 0; turns > 0 or height > 0 (turns = height / pitch); start angle.
    const auto pitch = c.field(d["pitch"].get<std::string>(), expr::LENGTH, "/pitch");
    if (!pitch) return finish(c, input);
    if (*pitch <= 0) {
        c.error(Error("REGEN_BAD_VALUE").with("name", std::string("pitch")).with("actual", *pitch), "/pitch");
        return finish(c, input);
    }
    double turns = 0;
    if (d.contains("turns")) {
        const auto t = c.field(d["turns"].get<std::string>(), expr::DIMLESS, "/turns");
        if (!t) return finish(c, input);
        if (*t <= 0) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("turns")).with("actual", *t), "/turns");
            return finish(c, input);
        }
        turns = *t;
    } else {
        const auto h = c.field(d["height"].get<std::string>(), expr::LENGTH, "/height");
        if (!h) return finish(c, input);
        if (*h <= 0) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("height")).with("actual", *h), "/height");
            return finish(c, input);
        }
        turns = *h / *pitch;
    }
    double start = 0;
    if (d.contains("startAngle")) {
        const auto a = c.field(d["startAngle"].get<std::string>(), expr::ANGLE, "/startAngle");
        if (!a) return finish(c, input);
        start = *a;
    }
    const bool left = d.value("hand", std::string("right")) == "left";
    // 4. Tool: the right-hand helical sweep of each region; left hand — reflected in the profile plane (the start profile
    // stays, the turns go the other way about the axis); then turned about the axis by the start angle.
    Eigen::Matrix3d M = Eigen::Matrix3d::Identity();
    if (left) {
        const Vec3 n = se->frame.normal.normalized();
        M = Eigen::Matrix3d::Identity() - 2 * n * n.transpose();
    }
    if (start != 0 && axis.direction.norm() > 0) M = Eigen::AngleAxisd(start, axis.direction.normalized()).toRotationMatrix() * M;
    const bool moved = left || start != 0;
    kernel::Motion motion;
    for (int r = 0; r < 3; ++r)
        for (int q = 0; q < 3; ++q) motion.r[size_t(r * 3 + q)] = M(r, q);
    motion.t = axis.origin - M * axis.origin;
    ToolBuild tool = buildTool(c, regions, [&](const sketch::Region& r) -> Result<kernel::OpResult> {
        auto h = k_.helicalSweep(toKernelProfile(r, se->frame), axis, *pitch, turns, c.cancel);
        if (!h || !moved) return h;
        auto t = k_.transform(h->shape, motion);
        if (!t) return t.error();
        // Face origins of the moved body — those of the helical sweep (transform keeps the faces).
        kernel::OpResult out = *t;
        out.warnings = h->warnings;
        for (auto& o : out.history.faceOrigins) {
            std::vector<kernel::Origin> mapped;
            for (const auto& x : o)
                if (x.kind == kernel::Origin::Kind::FromFace && x.input.face < int(h->history.faceOrigins.size()))
                    for (const auto& y : h->history.faceOrigins[size_t(x.input.face)]) mapped.push_back(y);
            o = mapped;
        }
        return out;
    });
    if (c.cancelled) {
        cancelledOut = true;
        return nullptr;
    }
    const std::string op = d["operation"];
    if (!tool.ok || !applyOperation(c, *st, op, tool)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, tool.shape);
    }
    if (!c.hasError()) st->tools[f.id] = {tool.shape, tool.bases, op == "new" ? "add" : op};
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st, tool.shape);
}

}  // namespace construe::regen
