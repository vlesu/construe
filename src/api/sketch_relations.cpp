// Design relations of a recovered sketch (spec/reconstruction.md §5.4, FX10-IO-004).
#include "api/sketch_relations.h"

#include <cmath>
#include <functional>
#include <optional>
#include <map>
#include <numeric>
#include <set>

namespace construe::api::sketchops {

namespace {

// Coincidence clusters (two-ref `coincident`) over the points of d: point → representative (first in entity order).
std::map<std::string, std::string> clusters(const sketch::SketchData& d) {
    std::vector<std::string> pts;
    std::map<std::string, size_t> index;
    for (const auto& e : d.entities)
        if (e.type == sketch::EntityType::Point) index[e.id] = pts.size(), pts.push_back(e.id);
    std::vector<size_t> parent(pts.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    for (const auto& c : d.constraints) {
        if (c.type != "coincident" || c.refs.size() != 2) continue;
        const auto a = index.find(c.refs[0]), b = index.find(c.refs[1]);
        if (a == index.end() || b == index.end()) continue;
        const size_t ra = root(a->second), rb = root(b->second);
        if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
    }
    std::map<std::string, std::string> out;
    for (size_t i = 0; i < pts.size(); ++i) out[pts[i]] = pts[root(i)];
    return out;
}

}  // namespace

std::vector<doc::Json> designRelations(const sketch::SketchData& d, const sketch::SolvedState& st, double tol) {
    std::vector<doc::Json> out;
    const auto rep = clusters(d);
    auto pos = [&](const std::string& id) -> std::optional<Point2> {
        const auto it = st.points.find(id);
        if (it == st.points.end()) return std::nullopt;
        return it->second;
    };
    // Main points: points of non-construction curves, one per coincidence cluster, in entity order.
    std::vector<std::string> main;
    std::set<std::string> seen;
    auto addMain = [&](const std::string& id) {
        if (id.empty() || !rep.count(id) || !pos(id)) return;
        if (seen.insert(rep.at(id)).second) main.push_back(rep.at(id));
    };
    for (const auto& e : d.entities) {
        if (e.construction) continue;
        if (e.type == sketch::EntityType::Line) addMain(e.p1), addMain(e.p2);
        else if (e.type == sketch::EntityType::Arc) addMain(e.center), addMain(e.start), addMain(e.end);
        else if (e.type == sketch::EntityType::Circle) addMain(e.center);
        else if (e.type != sketch::EntityType::Point) return {};  // other curves (splines, ellipses, text) — no relations
    }
    if (main.empty()) return out;
    // Symmetry about each sketch axis: every main point has its mirror image among the main points or lies on the axis.
    for (const bool vertical : {true, false}) {  // "sketch:y" (x = 0) mirrors x, "sketch:x" (y = 0) mirrors y
        std::vector<std::pair<std::string, std::string>> pairs;
        bool symmetric = true;
        std::set<std::string> used;
        for (const auto& p : main) {
            const Point2 a = *pos(p);
            const Point2 m = vertical ? Point2(-a.x(), a.y()) : Point2(a.x(), -a.y());
            if ((m - a).norm() <= tol) continue;  // on the axis
            if (used.count(p)) continue;
            std::string q;
            for (const auto& o : main)
                if (o != p && !used.count(o) && (*pos(o) - m).norm() <= tol) {
                    q = o;
                    break;
                }
            if (q.empty()) {
                symmetric = false;
                break;
            }
            used.insert(p), used.insert(q);
            pairs.push_back({p, q});
        }
        if (!symmetric || pairs.empty()) continue;
        for (const auto& [p, q] : pairs)
            out.push_back({{"type", "symmetric"}, {"refs", {p, q, vertical ? "@sketch:y" : "@sketch:x"}}});
    }
    // Equal lengths: parallel lines of equal length that are not opposite sides of a quadrilateral.
    struct Seg {
        std::string id, a, b;
        Point2 p, q;
    };
    std::vector<Seg> lines;
    for (const auto& e : d.entities)
        if (e.type == sketch::EntityType::Line && !e.construction && rep.count(e.p1) && rep.count(e.p2) && pos(e.p1) && pos(e.p2))
            lines.push_back({e.id, rep.at(e.p1), rep.at(e.p2), *pos(e.p1), *pos(e.p2)});
    auto joined = [&](const std::string& u, const std::string& v) {
        for (const auto& l : lines)
            if ((l.a == u && l.b == v) || (l.a == v && l.b == u)) return true;
        return false;
    };
    std::vector<bool> grouped(lines.size(), false);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (grouped[i]) continue;
        const Point2 di = lines[i].q - lines[i].p;
        const double li = di.norm();
        if (li <= tol) continue;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            if (grouped[j]) continue;
            const Point2 dj = lines[j].q - lines[j].p;
            const double lj = dj.norm();
            if (std::abs(li - lj) > tol || std::abs(di.x() * dj.y() - di.y() * dj.x()) > tol * std::max(li, lj)) continue;
            const auto& A = lines[i];
            const auto& B = lines[j];
            if (A.a == B.a || A.a == B.b || A.b == B.a || A.b == B.b) continue;  // a continuation, not a pair
            if ((joined(A.a, B.a) && joined(A.b, B.b)) || (joined(A.a, B.b) && joined(A.b, B.a))) continue;  // quadrilateral
            grouped[j] = true;
            out.push_back({{"type", "equal"}, {"refs", {A.id, B.id}}});
        }
    }
    // Equal radii: circles and arcs.
    struct Curve {
        std::string id;
        double r;
    };
    std::vector<Curve> curves;
    for (const auto& e : d.entities) {
        if (e.construction) continue;
        if (e.type == sketch::EntityType::Circle) {
            if (const auto it = st.radii.find(e.id); it != st.radii.end()) curves.push_back({e.id, it->second});
        } else if (e.type == sketch::EntityType::Arc && pos(e.center) && pos(e.start)) {
            curves.push_back({e.id, (*pos(e.start) - *pos(e.center)).norm()});
        }
    }
    std::vector<bool> cgrouped(curves.size(), false);
    for (size_t i = 0; i < curves.size(); ++i) {
        if (cgrouped[i]) continue;
        for (size_t j = i + 1; j < curves.size(); ++j)
            if (!cgrouped[j] && std::abs(curves[i].r - curves[j].r) <= tol) {
                cgrouped[j] = true;
                out.push_back({{"type", "equal"}, {"refs", {curves[i].id, curves[j].id}}});
            }
    }
    return out;
}

int addDesignRelations(Session& s, double tol) {
    const auto sid = s.editContext().sketch;
    if (!sid) return 0;
    const auto held = s.document().document;  // keep the document alive while its sketch data is read
    const auto f = held->active().find(*sid);
    const auto* data = f ? f->sketchData() : nullptr;
    auto st0 = s.sketchState(*sid);
    if (!data || !st0) return 0;
    const auto cands = designRelations(*data, st0->result.state, tol);
    if (cands.empty()) return 0;
    auto good = [&]() {
        const auto st = s.sketchState(*sid);
        if (!st) return false;
        const auto status = st->result.status;
        return (status == sketch::SketchStatus::UnderConstrained || status == sketch::SketchStatus::FullyConstrained) &&
               st->result.errors.empty() && st->result.redundant.empty() && st->result.unsatisfied.empty();
    };
    if (!good()) return 0;
    // Sketch axes on demand: "@sketch:x" → the ID of its projected entity.
    std::map<std::string, std::string> axis;
    auto resolved = [&](doc::Json item) -> std::optional<doc::Json> {
        for (auto& r : item["refs"]) {
            const std::string ref = r.get<std::string>();
            if (ref.rfind("@sketch:", 0) != 0) continue;
            const std::string src = ref.substr(1);
            if (!axis.count(src)) {
                auto a = s.sketchAdd(doc::Json::array({{{"id", "t1"}, {"type", "projected"}, {"source", src}, {"construction", true}}}));
                if (!a || !a->count("t1")) return std::nullopt;
                axis[src] = a->at("t1");
            }
            r = axis.at(src);
        }
        return item;
    };
    auto add = [&](const std::vector<doc::Json>& items) -> std::vector<std::string> {
        doc::Json batch = doc::Json::array();
        for (size_t i = 0; i < items.size(); ++i) {
            auto it = resolved(items[i]);
            if (!it) return {};
            (*it)["id"] = "t" + std::to_string(i + 1);
            batch.push_back(*it);
        }
        auto r = s.sketchAdd(batch);
        if (!r) return {};
        std::vector<std::string> ids;
        for (const auto& [tmp, id] : *r) ids.push_back(id);
        return ids;
    };
    int added = 0;
    if (auto ids = add(cands); !ids.empty()) {
        if (good()) return int(ids.size());
        (void)s.sketchDelete(ids);
    }
    std::set<std::string> usedAxes;
    for (const auto& c : cands) {
        auto ids = add({c});
        if (ids.empty()) continue;
        if (good()) {
            ++added;
            for (const auto& r : c["refs"])
                if (r.get<std::string>().rfind("@sketch:", 0) == 0) usedAxes.insert(r.get<std::string>().substr(1));
        } else {
            (void)s.sketchDelete(ids);
        }
    }
    std::vector<std::string> unused;
    for (const auto& [src, id] : axis)
        if (!usedAxes.count(src)) unused.push_back(id);
    if (!unused.empty()) (void)s.sketchDelete(unused);
    return added;
}

}  // namespace construe::api::sketchops
