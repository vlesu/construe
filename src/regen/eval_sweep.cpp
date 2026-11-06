// Sweep (spec/regen.md, sweep evaluator).
#include "regen/eval_common.h"

namespace construe::regen {

using namespace detail;

EvalPtr Regenerator::evalSweep(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    [[maybe_unused]] const Cancel* cancel = c.cancel;
    auto sketchOf = [&](const std::string& id) { return detail::sketchOf(c, *input, id); };
    auto selectRegions = [&](const SketchEntry& se, const doc::Profiles& p, const std::string& sketchId) {
        return detail::selectRegions(c, se, p, sketchId);
    };
    // §4.11
    const doc::Json& d = f.pattern()->def;
    const std::string skId = d["sketch"];
    const SketchEntry* se = sketchOf(skId);
    if (!se) return finish(c, input);
    doc::Profiles prof;
    if (d["profiles"].is_array()) {
        prof.all = false;
        for (const auto& r : d["profiles"]) prof.regions.push_back(r);
    }
    const auto regions = selectRegions(*se, prof, skId);
    if (regions.empty()) return finish(c, input);
    // 2. Path curves: arc — traversed counterclockwise about axis from a to b.
    struct PathSeg {
        std::string id;
        bool arc = false;
        Point3 a, b, center;
        Vec3 axis = Vec3::UnitZ();
    };
    std::vector<PathSeg> segs;
    auto pathError = [&](const char* detail) { c.error(Error("REGEN_SWEEP_PATH").with("detail", std::string(detail)), "/path"); };
    const doc::Json& path = d["path"];
    if (path.contains("sketch")) {
        const std::string pid = path["sketch"];
        auto it = input->sketches.find(pid);
        const auto pf = c.d.active().find(pid);
        if (it == input->sketches.end() || !pf || !pf->sketchData()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", pid), "/path/sketch");
            return finish(c, input);
        }
        const auto& P = it->second.result.state.points;
        const Plane& fr = it->second.frame;
        auto W = [&](const std::string& id) { return fr.toWorld(P.at(id)); };
        // Splines and ellipses are not path curves: an error naming the kind, not a silent drop (FX10-PT-003).
        std::string kind;
        std::vector<std::string> unsupported;
        for (const auto& e : pf->sketchData()->entities)
            if (!e.construction && (sketch::isSplineType(e.type) || sketch::isEllipseType(e.type))) {
                if (kind.empty()) kind = sketch::isSplineType(e.type) ? "spline" : "ellipse";
                unsupported.push_back(e.id);
            }
        if (!unsupported.empty()) {
            c.error(Error("REGEN_SWEEP_PATH_CURVE").with("kind", kind).with("ids", unsupported), "/path");
            return finish(c, input);
        }
        for (const auto& e : pf->sketchData()->entities) {
            if (e.construction) continue;
            if (e.type == sketch::EntityType::Line && P.count(e.p1) && P.count(e.p2)) segs.push_back({e.id, false, W(e.p1), W(e.p2), {}, {}});
            else if (e.type == sketch::EntityType::Arc && P.count(e.start) && P.count(e.end) && P.count(e.center))
                segs.push_back({e.id, true, W(e.start), W(e.end), W(e.center), fr.normal});
            else if (e.type == sketch::EntityType::Circle) {
                pathError("closed curve");
                return finish(c, input);
            }
        }
    } else {
        std::vector<std::string> lost, ambiguous;
        int k = 0;
        for (const auto& j : path["edges"]) {
            ++k;
            const std::string name = j;
            const auto rr = naming::resolve(name, naming::EntityKind::Edge, input->names);
            const auto fate = naming::refFate(rr, false);  // one path segment — one edge
            if (fate == naming::RefFate::Lost) {
                lost.push_back(name);
                continue;
            }
            if (fate == naming::RefFate::Ambiguous) {
                ambiguous.push_back(name);
                continue;
            }
            std::vector<Message> w;
            referenceWarnings(rr, name, w);
            for (auto& m : w) c.messages.push_back(m);
            const auto& ee = input->names.edges.at(rr.entities.front().index);
            const kernel::Shape& sh = input->bodies.at(ee.body).shape;
            const auto cv = k_.edgeCurve(sh, ee.logical);
            const std::string sid = "q" + std::to_string(k);
            if (cv.kind == kernel::CurveInfo::Kind::Line) {
                segs.push_back({sid, false, cv.p1, cv.p2, {}, {}});
            } else if (cv.kind == kernel::CurveInfo::Kind::Circle && !cv.closed) {
                // Ends of the arc — the principal vertices of the logical edge (edgeCurve gives no ends for a circle).
                const auto ct = kernel::canonicalTopology(k_, sh);
                const auto& ends = ct.logicalEdges.at(size_t(ee.logical)).ends;
                if (ends.size() != 2) {
                    pathError("unsupported curve");
                    return finish(c, input);
                }
                PathSeg s{sid, true, k_.vertexPoint(sh, ends[0]), k_.vertexPoint(sh, ends[1]), cv.center, cv.axis.normalized()};
                // Of the two arcs between the ends — the one on whose side of the chord the edge's center of mass lies.
                const Point3 g = k_.edgeCentroid(sh, ee.logical);
                const Vec3 ra = s.a - s.center, rb = s.b - s.center;
                double phi = std::atan2(s.axis.dot(ra.cross(rb)), ra.dot(rb));
                if (phi <= 0) phi += 2 * PI;
                const Vec3 mid = Eigen::AngleAxisd(phi / 2, s.axis) * ra;
                if ((g - s.center).dot(mid) < 0) s.axis = -s.axis;
                segs.push_back(s);
            } else {
                pathError("unsupported curve");
                return finish(c, input);
            }
        }
        if (!lost.empty()) c.error(Error("REGEN_REF_LOST").with("ids", lost), "/path/edges");
        else if (!ambiguous.empty()) c.error(Error("REGEN_REF_AMBIGUOUS").with("ids", ambiguous), "/path/edges");
        if (c.hasError()) return finish(c, input);
    }
    if (segs.empty()) {
        pathError("not a single chain");
        return finish(c, input);
    }
    // 3. Chain: end nodes within tolerance.
    std::vector<Point3> nodes;
    auto nodeOf = [&](const Point3& p) {
        for (size_t i = 0; i < nodes.size(); ++i)
            if ((nodes[i] - p).norm() <= SWEEP_JOIN_TOL) return int(i);
        nodes.push_back(p);
        return int(nodes.size()) - 1;
    };
    std::vector<std::pair<int, int>> ends;
    std::vector<int> degree;
    for (const auto& s : segs) {
        const int na = nodeOf(s.a), nb = nodeOf(s.b);
        ends.push_back({na, nb});
        degree.resize(nodes.size(), 0);
        ++degree[size_t(na)], ++degree[size_t(nb)];
    }
    std::vector<int> terminal;
    for (size_t i = 0; i < degree.size(); ++i) {
        if (degree[i] > 2) {
            pathError("not a single chain");
            return finish(c, input);
        }
        if (degree[i] == 1) terminal.push_back(int(i));
    }
    const bool closed = terminal.empty();
    if (!closed && terminal.size() != 2) {
        pathError("not a single chain");
        return finish(c, input);
    }
    const Plane& pf = se->frame;
    auto onPlane = [&](const Point3& q) { return std::abs(pf.normal.dot(q - pf.origin)) <= SWEEP_JOIN_TOL; };
    // Center of mass of the first profile region's segment points: picks the start among candidates.
    Point3 g = Point3::Zero();
    {
        int cnt = 0;
        for (const auto& sg : regions.front()->outer.segments) {
            g += pf.toWorld(sg.kind == sketch::Segment::Kind::Line ? sg.p1 : sg.kind == sketch::Segment::Kind::Spline ? sg.start : sg.center);
            ++cnt;
        }
        g /= std::max(1, cnt);
    }
    int startNode = -1;
    if (!closed) {
        const bool on0 = onPlane(nodes[size_t(terminal[0])]), on1 = onPlane(nodes[size_t(terminal[1])]);
        if (on0 && on1) startNode = (nodes[size_t(terminal[0])] - g).norm() <= (nodes[size_t(terminal[1])] - g).norm() ? terminal[0] : terminal[1];
        else if (on0) startNode = terminal[0];
        else if (on1) startNode = terminal[1];
    } else {
        // Closed path (FX7-PT-002): the start is where the path pierces the profile plane — a node or a point inside a
        // segment (the segment is split there); of several — the nearest to the profile.
        double best = 1e300;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (onPlane(nodes[i]) && (nodes[i] - g).norm() < best) best = (nodes[i] - g).norm(), startNode = int(i);
        if (startNode < 0) {
            int segAt = -1;
            Point3 at = Point3::Zero();
            for (size_t i = 0; i < segs.size(); ++i) {
                const PathSeg& sg = segs[i];
                std::vector<Point3> hits;
                if (!sg.arc) {
                    const double den = pf.normal.dot(sg.b - sg.a);
                    if (std::abs(den) > 1e-12) {
                        const double t = pf.normal.dot(pf.origin - sg.a) / den;
                        if (t > 0 && t < 1) hits.push_back(sg.a + t * (sg.b - sg.a));
                    }
                } else {
                    const Vec3 u = sg.a - sg.center, v = sg.axis.cross(u);
                    const double A = pf.normal.dot(u), B = pf.normal.dot(v), C = pf.normal.dot(pf.origin - sg.center);
                    const double R = std::hypot(A, B);
                    const Vec3 rb = sg.b - sg.center;
                    double span = std::atan2(sg.axis.dot(u.cross(rb)), u.dot(rb));
                    if (span <= 0) span += 2 * PI;
                    if (R > 1e-12 && std::abs(C) <= R)
                        for (double sgn : {1.0, -1.0}) {
                            double th = std::atan2(B, A) + sgn * std::acos(std::clamp(C / R, -1.0, 1.0));
                            th = std::fmod(th + 4 * PI, 2 * PI);
                            if (th > 1e-9 && th < span - 1e-9) hits.push_back(sg.center + std::cos(th) * u + std::sin(th) * v);
                        }
                }
                for (const auto& h : hits)
                    if ((h - g).norm() < best) best = (h - g).norm(), segAt = int(i), at = h;
            }
            if (segAt >= 0) {
                // Split: the piece from the pierce point to b gets "<id>_1", from a to the pierce point — "<id>_2".
                PathSeg p1 = segs[size_t(segAt)], p2 = segs[size_t(segAt)];
                p1.id += "_1", p1.a = at;
                p2.id += "_2", p2.b = at;
                segs[size_t(segAt)] = p1;
                segs.push_back(p2);
                nodes.push_back(at);
                startNode = int(nodes.size()) - 1;
                const auto old = ends[size_t(segAt)];
                ends[size_t(segAt)] = {startNode, old.second};
                ends.push_back({old.first, startNode});
            }
        }
    }
    if (startNode < 0) {
        pathError("path does not start on profile plane");
        return finish(c, input);
    }
    // Traversal: segments in chain order, oriented from the start.
    struct Step {
        const PathSeg* s;
        Point3 from, to;
        Vec3 axis;  // arc: traversed counterclockwise about axis
    };
    std::vector<Step> chain;
    std::vector<bool> used(segs.size(), false);
    int at = startNode;
    // Closed path: of the two segments at the start, the one leaving the profile plane more steeply goes first (a
    // segment lying in the profile plane cannot carry the profile).
    int first = -1;
    if (closed) {
        double best = -1;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (ends[i].first != at && ends[i].second != at) continue;
            const PathSeg& s = segs[i];
            const bool fwd = ends[i].first == at;
            const Point3 p = fwd ? s.a : s.b;
            const Vec3 t = !s.arc ? Vec3((fwd ? s.b - s.a : s.a - s.b).normalized())
                                  : Vec3((fwd ? s.axis : Vec3(-s.axis)).cross(p - s.center).normalized());
            const double k = std::abs(t.dot(pf.normal));
            if (k > best + 1e-12) best = k, first = int(i);
        }
    }
    for (size_t n = 0; n < segs.size(); ++n) {
        int next = n == 0 ? first : -1;
        for (size_t i = 0; i < segs.size() && next < 0; ++i)
            if (!used[i] && (ends[i].first == at || ends[i].second == at)) next = int(i);
        if (next < 0) {
            pathError("not a single chain");
            return finish(c, input);
        }
        used[size_t(next)] = true;
        const PathSeg& s = segs[size_t(next)];
        const bool fwd = ends[size_t(next)].first == at;
        chain.push_back({&s, fwd ? s.a : s.b, fwd ? s.b : s.a, fwd ? s.axis : Vec3(-s.axis)});
        at = fwd ? ends[size_t(next)].second : ends[size_t(next)].first;
    }
    auto tangentAt = [](const Step& st, const Point3& p) -> Vec3 {
        if (!st.s->arc) return (st.to - st.from).normalized();
        return st.axis.cross(p - st.s->center).normalized();
    };
    const Vec3 t0 = tangentAt(chain.front(), chain.front().from);
    const bool normalStart = t0.cross(pf.normal).norm() <= SWEEP_ANGLE_TOL;
    if (std::abs(t0.dot(pf.normal)) < SWEEP_MIN_PROFILE_SIN) {
        pathError("profile not normal to path");
        return finish(c, input);
    }
    bool smooth = true;
    for (size_t i = 1; i < chain.size(); ++i) {
        const Vec3 t1 = tangentAt(chain[i - 1], chain[i - 1].to), t2 = tangentAt(chain[i], chain[i].from);
        if (t1.cross(t2).norm() > SWEEP_ANGLE_TOL || t1.dot(t2) <= 0) smooth = false;
        if (t1.dot(t2) < -1 + SWEEP_ANGLE_TOL) {
            pathError("path not smooth");  // reversal: no mitre exists
            return finish(c, input);
        }
    }
    // 4а. Closed, cornered or oblique path (FX7-PT-002): one sweep along the whole chain with mitred corners.
    if (closed || !smooth || !normalStart) {
        std::vector<kernel::PathSegment> kp;
        for (const Step& stp : chain) {
            kernel::PathSegment ps;
            ps.id = stp.s->id, ps.arc = stp.s->arc, ps.a = stp.from, ps.b = stp.to, ps.center = stp.s->center, ps.axis = stp.axis;
            kp.push_back(ps);
        }
        if (closed) kp.back().b = kp.front().a;
        ToolBuild total = buildTool(c, regions, [&](const sketch::Region& r) { return k_.pipeSweep(toKernelProfile(r, pf), kp, cancel); });
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        if (!total.ok) return finish(c, input);
        const std::string op = d["operation"];
        if (!applyOperation(c, *st, op, total)) {
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            return finish(c, input, total.shape);
        }
        if (!c.hasError()) st->tools[f.id] = {total.shape, total.bases, op == "new" ? "add" : op};
        rename(k_, *st);
        nameTableMessages(c, *input, *st);
        return finish(c, st, total.shape);
    }
    // 4. Pieces: extrusion along line segments, revolution along arcs; the profile is carried by the frame motion.
    Plane cur = pf;
    ToolBuild total;
    auto withPathSegment = [&](std::vector<std::string> bases, const std::string& sid) {
        const std::string key = f.id + ".side(";
        for (auto& b : bases)
            for (size_t pos = b.find(key); pos != std::string::npos; pos = b.find(key, pos + 1)) {
                const size_t close = b.find(')', pos + key.size());
                if (close != std::string::npos) b.insert(close, "/" + sid);
            }
        return bases;
    };
    for (size_t i = 0; i < chain.size(); ++i) {
        const Step& stp = chain[i];
        ToolBuild piece;
        if (!stp.s->arc) {
            const Vec3 t = stp.to - stp.from;
            kernel::Extent ext;
            ext.kind = kernel::Extent::Kind::Blind;
            ext.depth = t.norm();
            const bool rev = cur.normal.dot(t) < 0;
            piece = buildTool(c, regions, [&](const sketch::Region& r) { return k_.extrude(toKernelProfile(r, cur), ext, rev); });
            cur.origin += t;
        } else {
            const Vec3 ra = stp.from - stp.s->center, rb = stp.to - stp.s->center;
            double ang = std::atan2(stp.axis.dot(ra.cross(rb)), ra.dot(rb));
            if (ang <= 0) ang += 2 * PI;
            kernel::Axis ax{stp.s->center, stp.axis};
            kernel::RevolveExtent ext{kernel::RevolveExtent::Kind::Angle, ang};
            piece = buildTool(c, regions, [&](const sketch::Region& r) { return k_.revolve(toKernelProfile(r, cur), ax, ext); });
            const Eigen::AngleAxisd rot(ang, stp.axis);
            cur.origin = stp.s->center + rot * (cur.origin - stp.s->center);
            cur.x = rot * cur.x, cur.y = rot * cur.y, cur.normal = rot * cur.normal;
        }
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        if (!piece.ok) return finish(c, input);
        piece.bases = withPathSegment(piece.bases, stp.s->id);
        if (total.shape.null()) {
            total = piece;
            continue;
        }
        auto u = k_.boolean(kernel::BoolKind::Union, total.shape, piece.shape, cancel);
        if (!u) {
            c.kernelError(u.error());
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            return finish(c, input);
        }
        c.kernelWarnings(u->warnings);
        std::vector<Error> bw;
        total.bases = naming::assignBases(naming::OpKind::Boolean, f.id, *u, {inputOf(total.bases), inputOf(piece.bases)}, &bw);
        for (auto& w : bw) c.warning(w);
        total.shape = u->shape;
    }
    total.ok = true;
    // 5. Operation, names, tool.
    const std::string op = d["operation"];
    if (!applyOperation(c, *st, op, total)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, total.shape);
    }
    if (!c.hasError()) st->tools[f.id] = {total.shape, total.bases, op == "new" ? "add" : op};
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st, total.shape);
}

}  // namespace construe::regen
