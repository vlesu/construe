// Linked sketch groups (spec/sketch.md §2.6).
#include "sketch/groups.h"

#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <set>

namespace construe::sketch {

std::vector<std::string> ownedPoints(const Entity& e) {
    switch (e.type) {
        case EntityType::Line: return {e.p1, e.p2};
        case EntityType::Circle: return {e.center};
        case EntityType::Ellipse: return {e.center};  // S4-SKT-004
        case EntityType::EllipseArc: return {e.center, e.start, e.end};  // S4-SKT-007
        case EntityType::Arc: return {e.center, e.start, e.end};
        case EntityType::Text: return {e.center};
        case EntityType::Bezier:
        case EntityType::BSpline: return e.poles;  // S4A-SPL-001
        default: return {};
    }
}

double CircularPattern::step() const {
    const bool full = std::abs(std::abs(angle) - 2 * std::numbers::pi) <= PATTERN_FULL_TOL;
    return full ? angle / count : angle / (count - 1);
}

namespace {

struct Xform {
    bool rotate = false;
    std::string from;  // translate: the relation starts from the copy of instance `from` instead of the source (FX12-FEAT-001)
    double vx = 0, vy = 0;           // translate
    std::string line;                // translate along an axis segment: vector in its frame (S4-SKT-006)
    double ux = 1, uy = 0;           // unit direction of the axis segment
    double cx = 0, cy = 0, phi = 0;  // rotate
    std::string center;
    void apply(double x, double y, double& ox, double& oy) const {
        if (!rotate) {
            ox = x + vx, oy = y + vy;
            return;
        }
        const double c = std::cos(phi), s = std::sin(phi), dx = x - cx, dy = y - cy;
        ox = cx + dx * c - dy * s;
        oy = cy + dx * s + dy * c;
    }
};

// Copies of the sources for one instance (§2.6.3 "Generated elements").
void copyInstance(const SketchData& s, const std::vector<std::string>& sources, const std::string& key, const Xform& t,
                  std::vector<GroupItem>& out) {
    auto role = [&](const std::string& id) { return key + "/" + id; };
    auto pointCopy = [&](const Entity& p) {
        Entity q = p;
        q.id = "@" + role(p.id);
        t.apply(p.x, p.y, q.x, q.y);
        out.push_back({role(p.id), q, std::nullopt});
    };
    for (const auto& sid : sources) {
        const Entity* e = s.findEntity(sid);
        if (!e) continue;
        if (e->type == EntityType::Point) {
            pointCopy(*e);
        } else {
            for (const auto& pid : ownedPoints(*e))
                if (const Entity* p = s.findEntity(pid)) pointCopy(*p);
            Entity c = *e;
            c.id = "@" + role(e->id);
            for (auto* ref : {&c.p1, &c.p2, &c.center, &c.start, &c.end})
                if (!ref->empty()) *ref = "@" + role(*ref);
            for (auto& q : c.poles) q = "@" + role(q);
            if (t.rotate && c.type == EntityType::Ellipse) c.angle += t.phi;  // the axis turns with the copy (FX7-SK-012)
            out.push_back({role(e->id), c, std::nullopt});
        }
        Constraint rel;
        rel.id = "@" + role(e->id) + "/rel";
        if (t.rotate) {
            rel.type = "rotate";
            rel.refs = {e->id, "@" + role(e->id), t.center};
            rel.rotation = t.phi;
        } else {
            rel.type = "translate";
            rel.refs = {t.from.empty() ? e->id : "@" + t.from + "/" + e->id, "@" + role(e->id)};
            rel.vector = std::array<double, 2>{t.vx, t.vy};
            if (!t.line.empty()) {  // offset in the segment frame: along û and v̂ = rot90(û) (§2.2, S4-SKT-006)
                rel.refs.push_back(t.line);
                rel.vector = std::array<double, 2>{t.vx * t.ux + t.vy * t.uy, -t.vx * t.uy + t.vy * t.ux};
            }
        }
        out.push_back({role(e->id) + "/rel", std::nullopt, rel});
    }
}

}  // namespace

std::vector<GroupItem> linearPatternItems(const SketchData& s, const std::vector<std::string>& sources, const LinearPattern& p) {
    std::vector<GroupItem> out;
    // Axis directions: from the x axis, or from the direction of the axis segment by current coordinates (S4-SKT-006).
    double base = 0, ux = 1, uy = 0;
    if (const Entity* l = p.line.empty() ? nullptr : s.findEntity(p.line)) {
        const Entity* a = s.findEntity(l->p1);
        const Entity* b = s.findEntity(l->p2);
        if (a && b) base = std::atan2(b->y - a->y, b->x - a->x), ux = std::cos(base), uy = std::sin(base);
    }
    // The second axis by its own segment (FX12-FEAT-001): angle2 from its direction.
    double base2 = base, u2dx = ux, u2dy = uy;
    if (const Entity* l = p.line2.empty() ? nullptr : s.findEntity(p.line2)) {
        const Entity* a = s.findEntity(l->p1);
        const Entity* b = s.findEntity(l->p2);
        if (a && b) base2 = std::atan2(b->y - a->y, b->x - a->x), u2dx = std::cos(base2), u2dy = std::sin(base2);
    }
    const double u1x = p.step1 * std::cos(base + p.angle1), u1y = p.step1 * std::sin(base + p.angle1);
    const double u2x = p.step2 * std::cos(base2 + p.angle2), u2y = p.step2 * std::sin(base2 + p.angle2);
    auto key = [](int i, int j) { return std::to_string(i) + "." + std::to_string(j); };
    for (int i = 0; i < p.count1; ++i)
        for (int j = 0; j < p.count2; ++j) {
            if (i == 0 && j == 0) continue;
            Xform t;
            t.vx = i * u1x + j * u2x;
            t.vy = i * u1y + j * u2y;
            if (p.line2.empty() || j == 0) {
                t.line = p.line, t.ux = ux, t.uy = uy;
                copyInstance(s, sources, key(i, j), t, out);
                continue;
            }
            // Along the second segment: the relation from the copy (i, 0) (the source for i = 0) by j·u₂ in the frame of
            // that segment, so the second axis follows its turn; the position — the full offset.
            Xform rel = t;
            rel.line = p.line2, rel.ux = u2dx, rel.uy = u2dy;
            if (i > 0) rel.from = key(i, 0);
            // copyInstance places the copy by (vx, vy) and writes the relation vector from them: the relation must carry
            // j·u₂ only, so the copy is placed by the full offset and the relation vector is set after.
            const size_t first = out.size();
            copyInstance(s, sources, key(i, j), rel, out);
            for (size_t k = first; k < out.size(); ++k)
                if (out[k].constraint && out[k].constraint->type == "translate") {
                    const double jx = j * u2x, jy = j * u2y;
                    out[k].constraint->vector = std::array<double, 2>{jx * u2dx + jy * u2dy, -jx * u2dy + jy * u2dx};
                }
        }
    return out;
}

std::vector<GroupItem> circularPatternItems(const SketchData& s, const std::vector<std::string>& sources, const CircularPattern& p) {
    std::vector<GroupItem> out;
    Xform t;
    t.rotate = true;
    t.center = p.center;
    if (p.center != "origin")
        if (const Entity* c = s.findEntity(p.center)) t.cx = c->x, t.cy = c->y;
    const double step = p.step();
    for (int k = 1; k < p.count; ++k) {
        t.phi = k * step;
        copyInstance(s, sources, std::to_string(k), t, out);
    }
    return out;
}

SketchData applyGroupItems(SketchData s, Group group, const std::vector<GroupItem>& items,
                           const std::function<std::string(bool entity)>& allocate) {
    std::map<std::string, std::string> oldByRole;
    std::set<std::string> oldIds;
    for (const auto& m : group.members) {
        oldByRole[m.role] = m.id;
        oldIds.insert(m.id);
    }
    // IDs by role (item 2): a previous member of the same kind, or a new one.
    std::map<std::string, std::string> idOf;  // "@role" → ID
    std::vector<GroupMember> members;
    for (const auto& it : items) {
        std::string id;
        if (auto o = oldByRole.find(it.role); o != oldByRole.end()) {
            if (it.entity) {
                const Entity* e = s.findEntity(o->second);
                if (e && e->type == it.entity->type) id = o->second;
            } else if (const Constraint* c = s.findConstraint(o->second); c && c->type == it.constraint->type) {
                id = o->second;
            }
        }
        if (id.empty()) id = allocate(it.entity.has_value());
        idOf["@" + it.role] = id;
        members.push_back({it.role, id});
    }
    std::set<std::string> kept;
    for (const auto& m : members) kept.insert(m.id);
    std::set<std::string> vanished;
    for (const auto& id : oldIds)
        if (!kept.count(id) && s.findEntity(id)) vanished.insert(id);
    // item 3: previous members — from the arrays; constraints on vanished entities — too.
    std::erase_if(s.entities, [&](const Entity& e) { return oldIds.count(e.id) > 0; });
    std::erase_if(s.constraints, [&](const Constraint& c) {
        if (oldIds.count(c.id)) return true;
        for (const auto& r : c.refs)
            if (vanished.count(r)) return true;
        return false;
    });
    // item 4: generated ones — at the end.
    auto resolve = [&](const std::string& r) {
        auto it = idOf.find(r);
        return it == idOf.end() ? r : it->second;
    };
    for (const auto& it : items) {
        if (!it.entity) continue;
        Entity e = *it.entity;
        e.id = idOf.at("@" + it.role);
        for (auto* ref : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
            if (!ref->empty()) *ref = resolve(*ref);
        for (auto& q : e.poles) q = resolve(q);
        s.entities.push_back(std::move(e));
    }
    for (const auto& it : items) {
        if (!it.constraint) continue;
        Constraint c = *it.constraint;
        c.id = idOf.at("@" + it.role);
        for (auto& r : c.refs) r = resolve(r);
        s.constraints.push_back(std::move(c));
    }
    group.members = std::move(members);
    bool replaced = false;
    for (auto& g : s.groups)
        if (g.id == group.id) {
            g = group;
            replaced = true;
        }
    if (!replaced) s.groups.push_back(std::move(group));
    return s;
}

SketchData removeGroup(SketchData s, const std::string& groupId, bool keepMembers) {
    const Group* g = s.findGroup(groupId);
    if (!g) return s;
    if (!keepMembers) {
        std::set<std::string> ids;
        for (const auto& m : g->members) ids.insert(m.id);
        std::erase_if(s.entities, [&](const Entity& e) { return ids.count(e.id) > 0; });
        std::erase_if(s.constraints, [&](const Constraint& c) {
            if (ids.count(c.id)) return true;
            for (const auto& r : c.refs)
                if (ids.count(r)) return true;
            return false;
        });
    }
    std::erase_if(s.groups, [&](const Group& x) { return x.id == groupId; });
    return s;
}

}  // namespace construe::sketch
