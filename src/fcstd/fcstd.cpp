// Reading .FCStd: container, Document.xml, objects and property values (spec/fcstd.md §2–§3, §6.1).
#include "fcstd/fcstd.h"

#include "base/charconv.h"
#include "io/mesh_import.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace construe::fcstd {

namespace {

std::optional<double> toNumber(const std::string* s) {
    if (!s || s->empty()) return std::nullopt;
    // parseDouble (from_chars rules, base.md §2б) does not depend on the process locale: strtod would stop at "." once
    // a Russian locale is set (Qt sets the user locale on Linux), turning every FreeCAD coordinate into "no value".
    const char* b = s->data();
    const char* end = b + s->size();
    double v = 0;
    const auto r = parseDouble(b, end, v);  // also on Apple's libc++ without floating-point from_chars
    if (r.ec != std::errc() || r.ptr != end) return std::nullopt;
    return v;
}

double numAttr(const xml::Element& e, const char* key, double fallback = 0) {
    return toNumber(e.attr(key)).value_or(fallback);
}

int intAttr(const xml::Element& e, const char* key, int fallback) {
    const auto v = toNumber(e.attr(key));
    return v ? int(*v) : fallback;
}

Error badObject(const std::string& name, const std::string& detail) {
    return Error("FCSTD_BAD_OBJECT").with("name", name).with("detail", detail);
}

}  // namespace

// ---------------------------------------------------------------- properties

std::optional<double> Property::number() const {
    if (!node) return std::nullopt;
    for (const char* tag : {"Float", "Integer"})
        if (const auto* c = node->child(tag)) return toNumber(c->attr("value"));
    if (auto b = boolean()) return *b ? 1.0 : 0.0;
    return std::nullopt;
}

std::optional<bool> Property::boolean() const {
    if (!node) return std::nullopt;
    const auto* c = node->child("Bool");
    if (!c) return std::nullopt;
    const std::string v = c->attrOr("value");
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return std::nullopt;
}

std::optional<std::string> Property::string() const {
    if (!node) return std::nullopt;
    const auto* c = node->child("String");
    if (!c || !c->attr("value")) return std::nullopt;
    return *c->attr("value");
}

std::string Property::link() const {
    if (!node) return {};
    const auto* c = node->child("Link");
    return c ? c->attrOr("value") : std::string();
}

std::vector<std::string> Property::linkList() const {
    std::vector<std::string> out;
    if (!node) return out;
    if (const auto* l = node->child("LinkList"))
        for (const auto& c : l->children)
            if (c.name == "Link") out.push_back(c.attrOr("value"));
    return out;
}

LinkSub Property::linkSub() const {
    LinkSub out;
    if (!node) return out;
    if (const auto* l = node->child("LinkSub")) {
        out.object = l->attrOr("value");
        for (const auto& c : l->children)
            // "shadowed" — the old-style name FreeCAD keeps next to a new-style one (sketch axes "Axis" → "Axis0").
            if (c.name == "Sub") out.subs.push_back(c.attr("shadowed") ? *c.attr("shadowed") : c.attrOr("value"));
    }
    return out;
}

std::vector<LinkSub> Property::linkSubList() const {
    std::vector<LinkSub> out;
    if (!node) return out;
    if (const auto* l = node->child("LinkSubList"))
        for (const auto& c : l->children) {
            if (c.name != "Link") continue;
            const std::string obj = c.attrOr("obj");
            if (out.empty() || out.back().object != obj) out.push_back({obj, {}});
            const std::string sub = c.attrOr("sub");
            if (!sub.empty()) out.back().subs.push_back(sub);
        }
    return out;
}

XLinkSub Property::xlinkSub() const {
    XLinkSub out;
    if (!node) return out;
    if (const auto* l = node->child("XLink")) {
        out.file = l->attrOr("file");
        out.object = l->attrOr("name");
        for (const auto& c : l->children)
            if (c.name == "Sub") out.subs.push_back(c.attrOr("value"));
    }
    return out;
}

std::optional<Vec3> Property::vector() const {
    if (!node) return std::nullopt;
    const auto* c = node->child("PropertyVector");
    if (!c) return std::nullopt;
    return Vec3(numAttr(*c, "valueX"), numAttr(*c, "valueY"), numAttr(*c, "valueZ"));
}

std::optional<Placement> Property::placement() const {
    if (!node) return std::nullopt;
    const auto* c = node->child("PropertyPlacement");
    if (!c) return std::nullopt;
    Placement p;
    p.base = Vec3(numAttr(*c, "Px"), numAttr(*c, "Py"), numAttr(*c, "Pz"));
    // Q0..Q2 — vector part, Q3 — scalar part (spec/fcstd.md §2.3).
    Eigen::Quaterniond q(numAttr(*c, "Q3", 1), numAttr(*c, "Q0"), numAttr(*c, "Q1"), numAttr(*c, "Q2"));
    if (q.norm() > 0) q.normalize();
    else q = Eigen::Quaterniond::Identity();
    p.rotation = q;
    return p;
}

std::string Property::shapeFile() const {
    if (!node) return {};
    const auto* c = node->child("Part");
    return c ? c->attrOr("file") : std::string();
}

std::optional<std::string> Property::enumName(const std::vector<std::string>& known) const {
    if (!node) return std::nullopt;
    const auto* i = node->child("Integer");
    if (!i) return std::nullopt;
    const auto idx = toNumber(i->attr("value"));
    if (!idx || *idx < 0) return std::nullopt;
    const size_t n = size_t(*idx);
    if (const auto* list = node->child("CustomEnumList")) {
        size_t k = 0;
        for (const auto& e : list->children)
            if (e.name == "Enum" && k++ == n) return e.attrOr("value");
        return std::nullopt;
    }
    if (n < known.size()) return known[n];
    return std::nullopt;
}

const Property* Object::prop(const std::string& n) const {
    for (const auto& p : properties)
        if (p.name == n) return &p;
    return nullptr;
}

std::string Object::label() const {
    if (const auto* p = prop("Label"))
        if (auto s = p->string(); s && !s->empty()) return *s;
    return name;
}

const Object* FcDocument::find(const std::string& n) const {
    for (const auto& o : objects)
        if (o.name == n) return &o;
    return nullptr;
}

std::optional<std::string> FcDocument::shape(const Object& o, const std::string& property) const {
    const Property* p = o.prop(property);
    if (!p) return std::nullopt;
    const std::string f = p->shapeFile();
    if (f.empty()) return std::nullopt;
    auto it = parts.find(f);
    if (it == parts.end() || it->second.empty()) return std::nullopt;
    return it->second;
}

// ---------------------------------------------------------------- container

std::optional<std::pair<int, int>> programVersion(const std::string& text) {
    // "<major>.<minor>" at the start, then anything ("R<build>", " (Git)").
    size_t i = 0;
    auto digits = [&](int& out) {
        const size_t b = i;
        out = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - b < 6) out = out * 10 + (text[i++] - '0');
        return i > b;
    };
    int major = 0, minor = 0;
    if (!digits(major)) return std::nullopt;
    if (i < text.size() && text[i] == '.') {
        ++i;
        if (!digits(minor)) return std::nullopt;
    }
    return std::make_pair(major, minor);
}

Result<FcDocument> readFcstd(const std::string& bytes) {
    auto zip = io::unzip(bytes);
    if (!zip) {
        const std::string* d = zip.error().stringParam("detail");
        return Error("FCSTD_NOT_FCSTD").with("detail", d ? *d : std::string("zip"));
    }
    FcDocument doc;
    const std::string* xmlText = nullptr;
    for (auto& [name, data] : *zip) {
        doc.parts[name] = std::move(data);
    }
    if (auto it = doc.parts.find("Document.xml"); it != doc.parts.end()) xmlText = &it->second;
    if (!xmlText) return Error("FCSTD_NOT_FCSTD").with("detail", std::string("no Document.xml"));
    auto root = xml::parse(*xmlText);
    if (!root) return root.error();
    if (root->name != "Document") return Error("FCSTD_NOT_FCSTD").with("detail", std::string("root ") + root->name);
    doc.programVersion = root->attrOr("ProgramVersion");
    doc.schemaVersion = intAttr(*root, "SchemaVersion", 0);
    doc.fileVersion = intAttr(*root, "FileVersion", 0);
    const auto ver = programVersion(doc.programVersion);
    if (!ver || ver->first < FCSTD_MIN_MAJOR)
        return Error("FCSTD_UNSUPPORTED_VERSION").with("version", doc.programVersion);

    // <Objects>: names and types in document order; <ObjectData>: properties by name.
    const xml::Element* objs = root->child("Objects");
    const xml::Element* data = root->child("ObjectData");
    if (!objs || !data) return Error("FCSTD_NOT_FCSTD").with("detail", std::string("no Objects"));
    std::map<std::string, size_t> index;
    for (const auto& o : objs->children) {
        if (o.name != "Object") continue;
        Object ob;
        ob.name = o.attrOr("name");
        ob.type = o.attrOr("type");
        if (ob.name.empty() || ob.type.empty()) return badObject(ob.name, "name or type");
        if (index.count(ob.name)) return badObject(ob.name, "duplicate name");
        index[ob.name] = doc.objects.size();
        doc.objects.push_back(std::move(ob));
    }
    // Property elements are shared with the parsed tree (kept alive by the shared root).
    auto shared = std::make_shared<const xml::Element>(std::move(*root));
    const xml::Element* sdata = shared->child("ObjectData");
    for (const auto& o : sdata->children) {
        if (o.name != "Object") continue;
        auto it = index.find(o.attrOr("name"));
        if (it == index.end()) return badObject(o.attrOr("name"), "data without object");
        Object& ob = doc.objects[it->second];
        if (const auto* ex = o.child("Extensions"))
            for (const auto& e : ex->children)
                if (e.name == "Extension") ob.extensions.push_back(e.attrOr("type"));
        const auto* props = o.child("Properties");
        if (!props) continue;
        for (const auto& p : props->children) {
            if (p.name != "Property") continue;  // <_Property> — transient, no value
            Property pr;
            pr.name = p.attrOr("name");
            pr.type = p.attrOr("type");
            pr.node = std::shared_ptr<const xml::Element>(shared, &p);
            ob.properties.push_back(std::move(pr));
        }
    }
    return doc;
}

Result<FcDocument> readFcstdFile(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return Error("IO_READ").with("file", path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return readFcstd(ss.str());
}

// ---------------------------------------------------------------- Sketcher (spec/fcstd.md §6.1)

namespace {

SkGeometry::Kind geometryKind(const std::string& t) {
    using K = SkGeometry::Kind;
    if (t == "Part::GeomLineSegment") return K::Line;
    if (t == "Part::GeomCircle") return K::Circle;
    if (t == "Part::GeomArcOfCircle") return K::Arc;
    if (t == "Part::GeomPoint") return K::Point;
    if (t == "Part::GeomEllipse") return K::Ellipse;
    if (t == "Part::GeomArcOfEllipse") return K::EllipseArc;
    if (t == "Part::GeomBSplineCurve") return K::BSpline;
    return K::Other;
}

bool readGeometry(const xml::Element& g, SkGeometry& out) {
    using K = SkGeometry::Kind;
    out.typeName = g.attrOr("type");
    out.kind = geometryKind(out.typeName);
    if (const auto* c = g.child("Construction")) out.construction = c->attrOr("value") == "1" || c->attrOr("value") == "true";
    if (const auto* ex = g.child("GeoExtensions"))
        for (const auto& e : ex->children)
            if (e.attrOr("type") == "Sketcher::SketchGeometryExtension") {
                out.internalType = intAttr(e, "internalGeometryType", 0);
                // Construction is also bit 1 of geometryModeFlags (the last character of the bit string).
                const std::string flags = e.attrOr("geometryModeFlags");
                if (!g.child("Construction") && !flags.empty()) out.construction = flags.back() == '1';
            }
    auto circleFrame = [&](const xml::Element& c) {
        out.center = Point2(numAttr(c, "CenterX"), numAttr(c, "CenterY"));
        out.normalDown = numAttr(c, "NormalZ", 1) < 0;
        out.angleXU = numAttr(c, "AngleXU");
    };
    switch (out.kind) {
        case K::Line: {
            const auto* l = g.child("LineSegment");
            if (!l) return false;
            out.p1 = Point2(numAttr(*l, "StartX"), numAttr(*l, "StartY"));
            out.p2 = Point2(numAttr(*l, "EndX"), numAttr(*l, "EndY"));
            return true;
        }
        case K::Point: {
            const auto* p = g.child("GeomPoint");
            if (!p) return false;
            out.center = Point2(numAttr(*p, "X"), numAttr(*p, "Y"));
            return true;
        }
        case K::Circle: {
            const auto* c = g.child("Circle");
            if (!c) return false;
            circleFrame(*c);
            out.radius = numAttr(*c, "Radius");
            return true;
        }
        case K::Arc: {
            const auto* c = g.child("ArcOfCircle");
            if (!c) return false;
            circleFrame(*c);
            out.radius = numAttr(*c, "Radius");
            out.startAngle = numAttr(*c, "StartAngle");
            out.endAngle = numAttr(*c, "EndAngle");
            return true;
        }
        case K::Ellipse: {
            const auto* c = g.child("Ellipse");
            if (!c) return false;
            circleFrame(*c);
            out.radius = numAttr(*c, "MajorRadius");
            out.minorRadius = numAttr(*c, "MinorRadius");
            return true;
        }
        case K::EllipseArc: {
            const auto* c = g.child("ArcOfEllipse");
            if (!c) return false;
            circleFrame(*c);
            out.radius = numAttr(*c, "MajorRadius");
            out.minorRadius = numAttr(*c, "MinorRadius");
            out.startAngle = numAttr(*c, "StartAngle");
            out.endAngle = numAttr(*c, "EndAngle");
            return true;
        }
        case K::BSpline: {
            const auto* c = g.child("BSplineCurve");
            if (!c) return false;
            out.degree = intAttr(*c, "Degree", 0);
            out.periodic = c->attrOr("IsPeriodic") == "1" || c->attrOr("IsPeriodic") == "true";
            for (const auto& e : c->children) {
                if (e.name == "Pole") {
                    out.poles.emplace_back(numAttr(e, "X"), numAttr(e, "Y"));
                    out.weights.push_back(numAttr(e, "Weight", 1));
                } else if (e.name == "Knot") {
                    out.knots.push_back(numAttr(e, "Value"));
                    out.mults.push_back(intAttr(e, "Mult", 1));
                }
            }
            return true;
        }
        case K::Other:
            return true;
    }
    return true;
}

}  // namespace

Result<SketchData> sketchData(const Object& sketch) {
    SketchData out;
    if (const Property* p = sketch.prop("Geometry"); p && p->node) {
        const auto* list = p->node->child("GeometryList");
        if (!list) return badObject(sketch.name, "Geometry");
        for (const auto& g : list->children) {
            if (g.name != "Geometry") continue;
            SkGeometry sg;
            if (!readGeometry(g, sg)) return badObject(sketch.name, "Geometry " + sg.typeName);
            out.geometry.push_back(std::move(sg));
        }
    }
    if (const Property* p = sketch.prop("Constraints"); p && p->node) {
        const auto* list = p->node->child("ConstraintList");
        if (!list) return badObject(sketch.name, "Constraints");
        for (const auto& c : list->children) {
            if (c.name != "Constrain") continue;
            SkConstraint sc;
            sc.type = intAttr(c, "Type", 0);
            sc.name = c.attrOr("Name");
            sc.value = numAttr(c, "Value");
            sc.driving = c.attrOr("IsDriving", "1") != "0";
            sc.active = c.attrOr("IsActive", "1") != "0";
            sc.first = intAttr(c, "First", GEO_UNDEF);
            sc.firstPos = intAttr(c, "FirstPos", 0);
            sc.second = intAttr(c, "Second", GEO_UNDEF);
            sc.secondPos = intAttr(c, "SecondPos", 0);
            sc.third = intAttr(c, "Third", GEO_UNDEF);
            sc.thirdPos = intAttr(c, "ThirdPos", 0);
            sc.alignmentType = intAttr(c, "InternalAlignmentType", 0);
            sc.alignmentIndex = intAttr(c, "InternalAlignmentIndex", -1);
            out.constraints.push_back(sc);
        }
    }
    if (const Property* p = sketch.prop("ExternalGeometry")) out.external = p->linkSubList();
    if (const Property* p = sketch.prop("ExternalGeo"); p && p->node)
        if (const auto* list = p->node->child("GeometryList"))
            for (const auto& g : list->children) {
                if (g.name != "Geometry") continue;
                SkGeometry sg;
                if (!readGeometry(g, sg)) return badObject(sketch.name, "ExternalGeo " + sg.typeName);
                out.externalGeometry.push_back(std::move(sg));
            }
    return out;
}

std::vector<std::pair<std::string, std::string>> expressions(const Object& o) {
    std::vector<std::pair<std::string, std::string>> out;
    const Property* p = o.prop("ExpressionEngine");
    if (!p || !p->node) return out;
    if (const auto* e = p->node->child("ExpressionEngine"))
        for (const auto& x : e->children)
            if (x.name == "Expression") out.emplace_back(x.attrOr("path"), x.attrOr("expression"));
    return out;
}

std::vector<Cell> cells(const Object& sheet) {
    std::vector<Cell> out;
    const Property* p = sheet.prop("cells");
    if (!p || !p->node) return out;
    if (const auto* c = p->node->child("Cells"))
        for (const auto& x : c->children)
            if (x.name == "Cell") out.push_back({x.attrOr("address"), x.attrOr("content"), x.attrOr("alias")});
    return out;
}

}  // namespace construe::fcstd
