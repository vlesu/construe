// Writing FreeCAD .FCStd containers (spec/fcstd.md §12а): objects with properties → Document.xml, GuiDocument.xml and
// the zip with BRep parts. Deterministic: fixed number notation, no time stamps, parts in a fixed order.
#pragma once

#include "base/math.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace construe::fcstd::write {

// Number in the notation of Document.xml: fixed point, 16 decimals (as FreeCAD writes), −0 → 0; locale-independent.
std::string num(double v);
// XML attribute value with entities (& < > " and control characters).
std::string attr(const std::string& s);

// Property of an object: name, FreeCAD type and the XML of its value (children of <Property>).
struct Prop {
    std::string name, type, xml;
    std::string group;  // non-empty — a dynamic property (Python features): written with group/doc/attr/ro/hide
};
// The property as a dynamic one of group `group` (spec/fcstd.md §7а.1).
Prop dynamic(Prop p, const std::string& group);

Prop string(const std::string& name, const std::string& value, const std::string& type = "App::PropertyString");
Prop boolean(const std::string& name, bool value);
Prop integer(const std::string& name, long long value, const std::string& type = "App::PropertyInteger");
Prop real(const std::string& name, double value, const std::string& type = "App::PropertyFloat");
// Enumeration: index; `items` — the item list written as CustomEnumList (empty — the list of the object type is used).
Prop enumeration(const std::string& name, int index, const std::vector<std::string>& items = {});
Prop link(const std::string& name, const std::string& object, const std::string& type = "App::PropertyLink");
Prop linkList(const std::string& name, const std::vector<std::string>& objects, const std::string& type = "App::PropertyLinkList");
Prop linkSub(const std::string& name, const std::string& object, const std::vector<std::string>& subs);
// LinkSubList: (object, sub) pairs in order; an empty sub writes sub="".
Prop linkSubList(const std::string& name, const std::vector<std::pair<std::string, std::string>>& items);

// Placement: position and rotation (quaternion x, y, z, w — Q0..Q3).
struct Placement {
    Vec3 base = Vec3::Zero();
    Eigen::Quaterniond rotation = Eigen::Quaterniond::Identity();
};
// Placement of a frame: origin, axes x, y (z = x × y); the axes must be orthonormal.
Placement frame(const Point3& origin, const Vec3& x, const Vec3& y);
Prop placement(const std::string& name, const Placement& p, const std::string& type = "App::PropertyPlacement");
Prop vector(const std::string& name, const Vec3& v);
// Shape stored in zip part `file` (empty — no shape).
Prop shape(const std::string& name, const std::string& file);
// Expression engine: (path, FreeCAD expression) in order.
Prop expressions(const std::vector<std::pair<std::string, std::string>>& items);
// Spreadsheet cells: (address, content, alias) in order.
struct Cell {
    std::string address, content, alias;
};
Prop cells(const std::vector<Cell>& list);
// Raw property (geometry and constraint lists of sketches).
Prop raw(const std::string& name, const std::string& type, const std::string& xml);
// Float list stored in zip part `file` (App::PropertyFloatList) and the bytes of that part.
Prop floatList(const std::string& name, const std::string& file);
std::string floatListBytes(const std::vector<double>& values);

struct Object {
    std::string name, type;
    std::vector<std::string> extensions;  // extension types (<Extensions>)
    std::vector<Prop> props;
    bool visible = false;  // Visibility (Document.xml and GuiDocument.xml)
};

// Unique FreeCAD object names: base, base001, base002, … (the scheme FreeCAD uses).
class Names {
public:
    std::string take(const std::string& base);
    bool taken(const std::string& name) const { return used_.count(name) > 0; }

private:
    std::map<std::string, int> used_;
};

// Document.xml (spec/fcstd.md §12а.2): document label, objects in order.
std::string documentXml(const std::string& label, const std::vector<Object>& objects);
// GuiDocument.xml: visibility of every object.
std::string guiDocumentXml(const std::vector<Object>& objects);
// The .FCStd container: Document.xml, GuiDocument.xml, then the parts (shapes, float lists) in the order objects refer
// to them (stored zip, fixed dates).
std::string container(const std::vector<Object>& objects, const std::string& label, const std::map<std::string, std::string>& parts);

}  // namespace construe::fcstd::write
