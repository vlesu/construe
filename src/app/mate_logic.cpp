// Assembly mate and manipulator choices of the client (app/mate_logic.h).
#include "app/mate_logic.h"

#include "app/theme.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::app {

std::vector<std::string> mateTypeFallbacks(const std::string& type) {
    // Then the valued types with the current value (they keep the parts in place).
    if (type == "coincident" || type == "concentric" || type == "tangent") return {"parallel", "distance", "angle"};
    return {};
}

std::string topInstanceOf(const std::string& path) { return path.substr(0, path.find('/')); }

std::string matedInstanceOf(const std::vector<std::string>& refs, const std::string& series, const std::vector<std::string>& order,
                            const std::function<bool(const std::string&)>& fixed) {
    if (!series.empty()) return series;
    int best = -1;
    for (const auto& ref : refs) {
        const std::string top = api::Session::refTopInstance(ref);
        if (top.empty()) continue;  // source and assembly geometry — no instance
        if (fixed(top)) continue;
        const auto it = std::find(order.begin(), order.end(), top);
        if (it != order.end()) best = std::max(best, int(it - order.begin()));
    }
    return best >= 0 ? order[size_t(best)] : std::string();
}

bool movableEntry(const regen::InstanceEntry* e) {
    return e && e->depth == 0 && !e->fixed && e->freeDof != 0 && e->pattern.empty() && e->connection.empty() && !e->error &&
           (e->state || e->kind == "assembly");
}

std::string nestedManipUnit(const regen::AssemblyState& st, const std::string& path) {
    const std::string u = api::Session::mateUnitOf(st, path);
    if (u == topInstanceOf(path)) return {};
    const auto* e = st.find(u);
    return e && !e->fixed && e->freeDof > 0 && !e->error ? u : std::string();
}

ManipGrip manipGripAt(const QPointF& off, double px) {
    ManipGrip g;
    const double r = std::hypot(off.x(), off.y());
    if (r > FREE_MANIP_RING_FRACTION * px) {
        g.kind = ManipGrip::Kind::Ring;
    } else if (r > FREE_MANIP_CENTER_FRACTION * px) {
        g.kind = ManipGrip::Kind::Arrow;
        g.axis = std::abs(off.x()) >= std::abs(off.y()) ? QPointF(1, 0) : QPointF(0, 1);
    }
    return g;
}

double screenTurnAngle(const QPointF& centre, const QPointF& press, const QPointF& cur) {
    const QPointF a0 = press - centre, a1 = cur - centre;
    double phi = std::atan2(-a1.y(), a1.x()) - std::atan2(-a0.y(), a0.x());
    while (phi > std::numbers::pi) phi -= 2 * std::numbers::pi;  // the shorter way, a drag past the press point
    while (phi < -std::numbers::pi) phi += 2 * std::numbers::pi;
    return phi;
}

void ScreenTurn::start(const QPointF& centre, const QPointF& press) {
    c_ = centre;
    acc_ = 0;
    const QPointF a = press - centre;
    valid_ = std::hypot(a.x(), a.y()) >= 1;
    last_ = valid_ ? std::atan2(-a.y(), a.x()) : 0;
}

double ScreenTurn::update(const QPointF& cur) {
    const QPointF a = cur - c_;
    if (std::hypot(a.x(), a.y()) < 1) return acc_;
    const double raw = std::atan2(-a.y(), a.x());
    if (!valid_) {  // the press was on the centre: the first move away fixes the start direction
        valid_ = true;
        last_ = raw;
        return acc_;
    }
    double d = raw - last_;
    while (d > std::numbers::pi) d -= 2 * std::numbers::pi;
    while (d < -std::numbers::pi) d += 2 * std::numbers::pi;
    last_ = raw;
    acc_ += d;
    return acc_;
}

std::vector<std::string> instancesInBox(const QRectF& box, bool window, const std::map<std::string, std::vector<QPointF>>& projected) {
    const QRectF r = box.normalized();
    auto inside = [&](const QPointF& q) { return q.x() >= r.left() && q.x() <= r.right() && q.y() >= r.top() && q.y() <= r.bottom(); };
    std::vector<std::string> out;
    for (const auto& [id, pts] : projected) {
        if (pts.empty()) continue;
        const bool hit = window ? std::all_of(pts.begin(), pts.end(), inside) : std::any_of(pts.begin(), pts.end(), inside);
        if (hit) out.push_back(id);
    }
    return out;
}

std::vector<std::string> manipGroupOf(const std::vector<std::string>& tops, const std::function<bool(const std::string&)>& movable) {
    std::vector<std::string> g;
    for (const auto& t : tops)
        if (std::find(g.begin(), g.end(), t) == g.end()) g.push_back(t);
    if (g.size() < 2) return {};
    for (const auto& t : g)
        if (!movable(t)) return {};
    return g;
}

std::map<std::string, regen::Motion3> groupMoved(const std::map<std::string, regen::Motion3>& start, const regen::Motion3& delta) {
    std::map<std::string, regen::Motion3> out;
    for (const auto& [id, m] : start) out[id] = delta * m;
    return out;
}

std::string moveUnitOf(const regen::AssemblyState& st, const std::string& path) {
    if (st.exploded)
        for (std::string p = path;;) {
            if (const auto* e = st.find(p); e && !e->connection.empty()) return p;
            const size_t sl = p.rfind('/');
            if (sl == std::string::npos) break;
            p.resize(sl);
        }
    return topInstanceOf(path);
}

bool movableUnit(const regen::AssemblyState& st, const std::string& id) {
    const auto* e = st.find(id);
    if (movableEntry(e)) return true;
    return st.exploded && e && !e->connection.empty() && e->state && !e->error && !e->suppressed;
}

}  // namespace construe::app
