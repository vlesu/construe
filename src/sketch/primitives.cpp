// Sketch primitives: generators of polygon and star groups (spec/sketch.md §2.6.4, S4-SKT-003).
#include "sketch/primitives.h"

#include "base/constants.h"

#include <cmath>

namespace construe::sketch {

namespace {

struct Gen {
    std::vector<GroupItem> out;
    void point(const std::string& role, double x, double y) {
        Entity e;
        e.id = "@" + role;
        e.type = EntityType::Point;
        e.x = x, e.y = y;
        out.push_back({role, e, std::nullopt});
    }
    void line(const std::string& role, const std::string& a, const std::string& b) {
        Entity e;
        e.id = "@" + role;
        e.type = EntityType::Line;
        e.p1 = "@" + a, e.p2 = "@" + b;
        out.push_back({role, e, std::nullopt});
    }
    void circle(const std::string& role, const std::string& center, double r) {
        Entity e;
        e.id = "@" + role;
        e.type = EntityType::Circle;
        e.center = "@" + center, e.r = r, e.construction = true;
        out.push_back({role, e, std::nullopt});
    }
    Constraint& con(const std::string& role, const std::string& type, std::vector<std::string> refs) {
        Constraint c;
        c.id = "@" + role;
        c.type = type;
        for (auto& r : refs) r = "@" + r;
        c.refs = std::move(refs);
        out.push_back({role, std::nullopt, c});
        return *out.back().constraint;
    }
};

const Entity* memberEntity(const SketchData& s, const Group& g, const std::string& role) {
    const GroupMember* m = g.member(role);
    return m ? s.findEntity(m->id) : nullptr;
}

}  // namespace

std::optional<PrimitivePlacement> polygonPlacement(const SketchData& s, const Group& old) {
    const Entity* c = memberEntity(s, old, "center");
    const Entity* circle = memberEntity(s, old, "circle");
    const Entity* side = memberEntity(s, old, "side0");
    if (!c || !circle || !side) return std::nullopt;
    const Entity* a = s.findEntity(side->p1);
    const Entity* b = s.findEntity(side->p2);
    if (!a || !b) return std::nullopt;
    const bool inscribed = !old.member("outer");
    const double tx = inscribed ? a->x : (a->x + b->x) / 2, ty = inscribed ? a->y : (a->y + b->y) / 2;
    return PrimitivePlacement{c->x, c->y, circle->r, std::atan2(ty - c->y, tx - c->x)};
}

std::optional<PrimitivePlacement> starPlacement(const SketchData& s, const Group& old) {
    const Entity* c = memberEntity(s, old, "center");
    const Entity* edge = memberEntity(s, old, "edge0");
    if (!c || !edge) return std::nullopt;
    const Entity* a = s.findEntity(edge->p1);
    if (!a) return std::nullopt;
    return PrimitivePlacement{c->x, c->y, 0, std::atan2(a->y - c->y, a->x - c->x)};
}

std::vector<GroupItem> polygonItems(const SketchData& s, const Group* old, const PolygonParams& p) {
    PrimitivePlacement at = p.start;
    if (old)
        if (auto cur = polygonPlacement(s, *old)) at = *cur;
    const int n = p.sides;
    const double R = p.inscribed ? at.size : at.size / std::cos(PI / n);
    const double start = p.inscribed ? at.angle : at.angle - PI / n;
    auto vx = [&](int i) { return at.x + R * std::cos(start + 2 * PI * (i % n) / n); };
    auto vy = [&](int i) { return at.y + R * std::sin(start + 2 * PI * (i % n) / n); };
    Gen g;
    g.point("center", at.x, at.y);
    for (int i = 0; i < n; ++i) {
        const std::string k = "side" + std::to_string(i);
        g.point(k + "/p1", vx(i), vy(i));
        g.point(k + "/p2", vx(i + 1), vy(i + 1));
    }
    for (int i = 0; i < n; ++i) {
        const std::string k = "side" + std::to_string(i);
        g.line(k, k + "/p1", k + "/p2");
    }
    const std::string vertexCircle = p.inscribed ? "circle" : "outer";
    g.circle(vertexCircle, "center", R);
    if (!p.inscribed) {
        g.point("circleCenter", at.x, at.y);
        g.circle("circle", "circleCenter", at.size);
        g.con("circleCenterJoin", "coincident", {"circleCenter", "center"});
    }
    for (int i = 0; i < n; ++i)
        g.con("join" + std::to_string(i), "coincident", {"side" + std::to_string(i) + "/p2", "side" + std::to_string((i + 1) % n) + "/p1"});
    for (int i = 0; i < n; ++i) g.con("onCircle" + std::to_string(i), "pointOnObject", {"side" + std::to_string(i) + "/p1", vertexCircle});
    for (int i = 1; i < n; ++i) g.con("equal" + std::to_string(i), "equal", {"side0", "side" + std::to_string(i)});
    if (!p.inscribed) {
        // Vertices counterclockwise — the center is to the left of side 0: branch side = +1 (sketch.md §2.2).
        Constraint& t = g.con("tangent", "tangent", {"side0", "circle"});
        Branch b;
        b.kind = Branch::Kind::Side;
        b.side = 1;
        t.branch = b;
    }
    return g.out;
}

std::vector<GroupItem> starItems(const SketchData& s, const Group* old, const StarParams& p) {
    PrimitivePlacement at = p.start;
    if (old)
        if (auto cur = starPlacement(s, *old)) at = *cur;
    const int m = 2 * p.rays;
    auto rad = [&](int j) { return (j % 2 == 0 ? p.d1 : p.d2) / 2; };
    auto vx = [&](int j) { return at.x + rad(j % m) * std::cos(at.angle + PI * (j % m) / p.rays); };
    auto vy = [&](int j) { return at.y + rad(j % m) * std::sin(at.angle + PI * (j % m) / p.rays); };
    Gen g;
    g.point("center", at.x, at.y);
    for (int j = 0; j < m; ++j) {
        const std::string k = "edge" + std::to_string(j);
        g.point(k + "/p1", vx(j), vy(j));
        g.point(k + "/p2", vx(j + 1), vy(j + 1));
    }
    for (int j = 0; j < m; ++j) {
        const std::string k = "edge" + std::to_string(j);
        g.line(k, k + "/p1", k + "/p2");
    }
    g.circle("outer", "center", p.d1 / 2);
    g.point("innerCenter", at.x, at.y);
    g.circle("inner", "innerCenter", p.d2 / 2);
    g.con("innerCenterJoin", "coincident", {"innerCenter", "center"});
    for (int j = 0; j < m; ++j)
        g.con("join" + std::to_string(j), "coincident", {"edge" + std::to_string(j) + "/p2", "edge" + std::to_string((j + 1) % m) + "/p1"});
    for (int j = 0; j < m; ++j)
        g.con("onCircle" + std::to_string(j), "pointOnObject", {"edge" + std::to_string(j) + "/p1", j % 2 == 0 ? "outer" : "inner"});
    for (int j = 1; j < m; ++j) g.con("equal" + std::to_string(j), "equal", {"edge0", "edge" + std::to_string(j)});
    Constraint& d1 = g.con("d1", "diameter", {"outer"});
    d1.value = p.d1Text, d1.driving = true;
    Constraint& d2 = g.con("d2", "diameter", {"inner"});
    d2.value = p.d2Text, d2.driving = true;
    return g.out;
}

}  // namespace construe::sketch
