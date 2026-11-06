// FreeCAD .FCStd files (spec/fcstd.md): reading the container into a model of objects and properties, and import of
// the model into a construe document. Own zip and XML reading — no FreeCAD code or libraries (spec/fcstd.md §1).
#pragma once

#include "base/error.h"
#include "base/math.h"
#include "fcstd/xml.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace construe::fcstd {

inline constexpr int FCSTD_MIN_MAJOR = 1;  // oldest supported FreeCAD major version (spec/fcstd.md §3)

// Placement of FreeCAD: position and rotation quaternion (Q0..Q3 = x, y, z, w; spec/fcstd.md §2.3).
struct Placement {
    Vec3 base = Vec3::Zero();
    Eigen::Quaterniond rotation = Eigen::Quaterniond::Identity();
    Point3 apply(const Point3& p) const { return rotation * p + base; }
    Vec3 applyDir(const Vec3& v) const { return rotation * v; }
};

// Link with sub-element names (App::PropertyLinkSub, one item of App::PropertyLinkSubList).
struct LinkSub {
    std::string object;
    std::vector<std::string> subs;
};

// App::PropertyXLink / App::PropertyXLinkSub (spec/fcstd.md §2.2): document file (empty — this document), object and
// sub-element names in file order (empty names kept).
struct XLinkSub {
    std::string file, object;
    std::vector<std::string> subs;
};

// Property of an object (spec/fcstd.md §2.2): name, type and its XML element; typed readers below.
struct Property {
    std::string name, type;
    std::shared_ptr<const xml::Element> node;  // <Property>; its children hold the value

    std::optional<double> number() const;      // <Float value>, <Integer value> (enumeration index), <Bool>
    std::optional<bool> boolean() const;       // <Bool value="true|false">
    std::optional<std::string> string() const;  // <String value>
    std::string link() const;                  // <Link value> (PropertyLink); empty — no link
    std::vector<std::string> linkList() const;  // <LinkList><Link value>
    LinkSub linkSub() const;                   // <LinkSub value count><Sub value>
    std::vector<LinkSub> linkSubList() const;  // <LinkSubList><Link obj sub> grouped by object in file order
    XLinkSub xlinkSub() const;                 // <XLink file stamp name><Sub value>
    std::optional<Vec3> vector() const;        // <PropertyVector valueX valueY valueZ>
    std::optional<Placement> placement() const;  // <PropertyPlacement Px.. Q0..Q3>
    std::string shapeFile() const;             // <Part file> of Part::PropertyPartShape; empty — none
    // Enumeration (App::PropertyEnumeration): the item name from <CustomEnumList> when the file lists the items,
    // otherwise from `known` (the fixed item list of FreeCAD 1.x for this property, spec/fcstd.md §7); nullopt —
    // index out of range.
    std::optional<std::string> enumName(const std::vector<std::string>& known = {}) const;
};

struct Object {
    std::string name, type;
    std::vector<std::string> extensions;  // extension types (spec/fcstd.md §2.1)
    std::vector<Property> properties;     // file order
    const Property* prop(const std::string& name) const;
    std::string label() const;  // Label or name
};

// Model of a file (spec/fcstd.md §2): document attributes, objects in file order, other parts of the zip.
struct FcDocument {
    std::string programVersion;  // ProgramVersion attribute of <Document>
    int schemaVersion = 0, fileVersion = 0;
    std::vector<Object> objects;
    std::map<std::string, std::string> parts;  // zip part name → bytes (BRep shapes, string tables)
    const Object* find(const std::string& name) const;
    // BRep text of a shape property (empty file or no part — nullopt).
    std::optional<std::string> shape(const Object& o, const std::string& property = "Shape") const;
};

// Major and minor version from ProgramVersion ("1.1R20260928 (Git shallow)" → {1, 1}); nullopt — not a version.
std::optional<std::pair<int, int>> programVersion(const std::string& text);

// Reading the container (spec/fcstd.md §3): zip → Document.xml → objects. Errors: FCSTD_NOT_FCSTD (not a zip or no
// Document.xml), FCSTD_BAD_XML (`detail`, `line`), FCSTD_UNSUPPORTED_VERSION (`version`), FCSTD_BAD_OBJECT
// (`name`, `detail`).
Result<FcDocument> readFcstd(const std::string& bytes);
// Reading a file (UTF-8 path): no file — IO_READ (`file`).
Result<FcDocument> readFcstdFile(const std::string& path);

// ---------------------------------------------------------------- Sketcher data (spec/fcstd.md §6.1)

// Geometry of a sketch in its own coordinates (z = 0); angles in radians.
struct SkGeometry {
    enum class Kind { Line, Circle, Arc, Point, Ellipse, EllipseArc, BSpline, Other };
    Kind kind = Kind::Other;
    std::string typeName;  // Part::Geom... of the file
    bool construction = false;
    int internalType = 0;  // internalGeometryType of the Sketcher extension (spec/fcstd.md §6.1)
    Point2 p1 = Point2::Zero(), p2 = Point2::Zero();  // Line: Start, End
    Point2 center = Point2::Zero();                   // Circle, Arc, Ellipse, EllipseArc; Point: the point
    double radius = 0, minorRadius = 0;               // Ellipse: major, minor
    double angleXU = 0;                               // rotation of the major axis (Ellipse), of the parameter origin (Arc)
    double startAngle = 0, endAngle = 0;              // Arc, EllipseArc: parameter range, counterclockwise
    bool normalDown = false;                          // circle normal −Z (mirrored geometry)
    // BSpline: poles with weights, degree, distinct knots with multiplicities, periodic (spec/fcstd.md §6.1).
    std::vector<Point2> poles;
    std::vector<double> weights, knots;
    std::vector<int> mults;
    int degree = 0;
    bool periodic = false;
};

struct SkConstraint {
    int type = 0;  // FreeCAD constraint type code (spec/fcstd.md §6.2)
    std::string name;
    double value = 0;
    bool driving = true, active = true;
    int first = -2000, firstPos = 0, second = -2000, secondPos = 0, third = -2000, thirdPos = 0;
    int alignmentType = 0;   // InternalAlignmentType
    int alignmentIndex = -1;  // InternalAlignmentIndex (pole of a B-spline)
};

inline constexpr int GEO_UNDEF = -2000;  // GeoId "no element" (spec/fcstd.md §6.2)

struct SketchData {
    std::vector<SkGeometry> geometry;
    std::vector<SkConstraint> constraints;
    std::vector<LinkSub> external;          // ExternalGeometry links (GeoId −3, −4, … in this order)
    std::vector<SkGeometry> externalGeometry;  // ExternalGeo: GeoId −1 − i (axes H, V, then external), sketch coordinates
};

// Sketch geometry and constraints of a Sketcher::SketchObject (FCSTD_BAD_OBJECT on malformed lists).
Result<SketchData> sketchData(const Object& sketch);

// Expressions of an object (App::PropertyExpressionEngine): path → expression, file order.
std::vector<std::pair<std::string, std::string>> expressions(const Object& o);

// Spreadsheet cells (Spreadsheet::PropertySheet): address, content, alias — file order.
struct Cell {
    std::string address, content, alias;
};
std::vector<Cell> cells(const Object& sheet);

}  // namespace construe::fcstd
