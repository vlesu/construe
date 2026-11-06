// Writing .FCStd containers (spec/fcstd.md §12а).
#include "fcstd/writer.h"

#include "io/mesh_export.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <set>

namespace construe::fcstd::write {

std::string num(double v) {
    if (!std::isfinite(v)) v = 0;
    char buf[400];
    auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, 16);
    std::string s(buf, r.ptr);
    // −0 and negative values that round to zero are written as zero.
    if (s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos) s.erase(0, 1);
    return s;
}

std::string attr(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\n': o += "&#10;"; break;
            case '\r': o += "&#13;"; break;
            case '\t': o += "&#9;"; break;
            default:
                if (c < 0x20) continue;  // other control characters are not allowed in XML 1.0
                o += char(c);
        }
    }
    return o;
}

namespace {

const std::string IND = "                    ";  // children of <Property> (5 levels)

}  // namespace

Prop dynamic(Prop p, const std::string& group) {
    p.group = group;
    return p;
}

Prop string(const std::string& name, const std::string& value, const std::string& type) {
    return {name, type, IND + "<String value=\"" + attr(value) + "\"/>\n"};
}

Prop boolean(const std::string& name, bool value) {
    return {name, "App::PropertyBool", IND + "<Bool value=\"" + (value ? "true" : "false") + "\"/>\n"};
}

Prop integer(const std::string& name, long long value, const std::string& type) {
    return {name, type, IND + "<Integer value=\"" + std::to_string(value) + "\"/>\n"};
}

Prop real(const std::string& name, double value, const std::string& type) {
    return {name, type, IND + "<Float value=\"" + num(value) + "\"/>\n"};
}

Prop enumeration(const std::string& name, int index, const std::vector<std::string>& items) {
    std::string x = IND + "<Integer value=\"" + std::to_string(index) + "\"" + (items.empty() ? "" : " CustomEnum=\"true\"") + "/>\n";
    if (!items.empty()) {
        x += IND + "<CustomEnumList count=\"" + std::to_string(items.size()) + "\">\n";
        for (const auto& i : items) x += IND + "    <Enum value=\"" + attr(i) + "\"/>\n";
        x += IND + "</CustomEnumList>\n";
    }
    return {name, "App::PropertyEnumeration", x};
}

Prop link(const std::string& name, const std::string& object, const std::string& type) {
    return {name, type, IND + "<Link value=\"" + attr(object) + "\"/>\n"};
}

Prop linkList(const std::string& name, const std::vector<std::string>& objects, const std::string& type) {
    std::string x = IND + "<LinkList count=\"" + std::to_string(objects.size()) + "\">\n";
    for (const auto& o : objects) x += IND + "    <Link value=\"" + attr(o) + "\"/>\n";
    x += IND + "</LinkList>\n";
    return {name, type, x};
}

Prop linkSub(const std::string& name, const std::string& object, const std::vector<std::string>& subs) {
    std::string x = IND + "<LinkSub value=\"" + attr(object) + "\" count=\"" + std::to_string(subs.size()) + "\">\n";
    for (const auto& s : subs) x += IND + "    <Sub value=\"" + attr(s) + "\"/>\n";
    x += IND + "</LinkSub>\n";
    return {name, "App::PropertyLinkSub", x};
}

Prop linkSubList(const std::string& name, const std::vector<std::pair<std::string, std::string>>& items) {
    std::string x = IND + "<LinkSubList count=\"" + std::to_string(items.size()) + "\">\n";
    for (const auto& [o, s] : items) x += IND + "    <Link obj=\"" + attr(o) + "\" sub=\"" + attr(s) + "\"/>\n";
    x += IND + "</LinkSubList>\n";
    return {name, "App::PropertyLinkSubList", x};
}

Placement frame(const Point3& origin, const Vec3& x, const Vec3& y) {
    Eigen::Matrix3d R;
    R.col(0) = x.normalized();
    R.col(1) = y.normalized();
    R.col(2) = R.col(0).cross(R.col(1));
    Placement p;
    p.base = origin;
    p.rotation = Eigen::Quaterniond(R).normalized();
    // Canonical sign (w ≥ 0, the first nonzero component positive when w = 0): the same rotation gives the same text.
    const auto& q = p.rotation.coeffs();  // x, y, z, w
    bool flip = q.w() < 0;
    if (std::abs(q.w()) < 1e-15) {
        for (int i = 0; i < 3; ++i)
            if (std::abs(q[i]) > 1e-15) {
                flip = q[i] < 0;
                break;
            }
    }
    if (flip) p.rotation.coeffs() = -p.rotation.coeffs();
    return p;
}

Prop placement(const std::string& name, const Placement& p, const std::string& type) {
    const auto& q = p.rotation;
    // Axis and angle (A, Ox, Oy, Oz) — informative attributes FreeCAD also writes.
    double angle = 2 * std::acos(std::clamp(q.w(), -1.0, 1.0));
    Vec3 axis(q.x(), q.y(), q.z());
    if (axis.norm() < 1e-15) axis = Vec3::UnitZ(), angle = 0;
    else axis.normalize();
    std::string x = IND + "<PropertyPlacement Px=\"" + num(p.base.x()) + "\" Py=\"" + num(p.base.y()) + "\" Pz=\"" + num(p.base.z()) +
                    "\" Q0=\"" + num(q.x()) + "\" Q1=\"" + num(q.y()) + "\" Q2=\"" + num(q.z()) + "\" Q3=\"" + num(q.w()) +
                    "\" A=\"" + num(angle) + "\" Ox=\"" + num(axis.x()) + "\" Oy=\"" + num(axis.y()) + "\" Oz=\"" + num(axis.z()) + "\"/>\n";
    return {name, type, x};
}

Prop vector(const std::string& name, const Vec3& v) {
    return {name, "App::PropertyVector",
            IND + "<PropertyVector valueX=\"" + num(v.x()) + "\" valueY=\"" + num(v.y()) + "\" valueZ=\"" + num(v.z()) + "\"/>\n"};
}

Prop shape(const std::string& name, const std::string& file) {
    return {name, "Part::PropertyPartShape", IND + "<Part file=\"" + attr(file) + "\"/>\n"};
}

Prop expressions(const std::vector<std::pair<std::string, std::string>>& items) {
    std::string x = IND + "<ExpressionEngine count=\"" + std::to_string(items.size()) + "\">\n";
    for (const auto& [p, e] : items) x += IND + "    <Expression path=\"" + attr(p) + "\" expression=\"" + attr(e) + "\"/>\n";
    x += IND + "</ExpressionEngine>\n";
    return {"ExpressionEngine", "App::PropertyExpressionEngine", x};
}

Prop cells(const std::vector<Cell>& list) {
    std::string x = IND + "<Cells Count=\"" + std::to_string(list.size()) + "\" xlink=\"1\">\n";
    x += IND + "    <XLinks count=\"0\">\n" + IND + "    </XLinks>\n";
    for (const auto& c : list) {
        x += IND + "    <Cell address=\"" + attr(c.address) + "\" content=\"" + attr(c.content) + "\"";
        if (!c.alias.empty()) x += " alias=\"" + attr(c.alias) + "\"";
        x += " />\n";
    }
    x += IND + "</Cells>\n";
    return {"cells", "Spreadsheet::PropertySheet", x};
}

Prop raw(const std::string& name, const std::string& type, const std::string& xml) { return {name, type, xml}; }

Prop floatList(const std::string& name, const std::string& file) {
    return {name, "App::PropertyFloatList", IND + "<FloatList file=\"" + attr(file) + "\"/>\n"};
}

std::string floatListBytes(const std::vector<double>& values) {
    // uint32 count, then IEEE 754 doubles, little-endian (as FreeCAD writes them).
    std::string out;
    auto put = [&](uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out += char((v >> (8 * i)) & 0xFF);
    };
    put(values.size(), 4);
    for (double d : values) {
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof bits);
        put(bits, 8);
    }
    return out;
}

std::string Names::take(const std::string& base) {
    if (!used_.count(base)) {
        used_[base] = 1;
        return base;
    }
    for (int n = 1;; ++n) {
        std::string s = std::to_string(n);
        while (s.size() < 3) s = "0" + s;
        if (!used_.count(base + s)) {
            used_[base + s] = 1;
            return base + s;
        }
    }
}

namespace {

// ProgramVersion of written files: FreeCAD 1.1 format (spec/fcstd.md §12а.2); the suffix names the writer.
const char* PROGRAM_VERSION = "1.1R0 (construe)";

}  // namespace

std::string documentXml(const std::string& label, const std::vector<Object>& objects) {
    std::string x = "<?xml version='1.0' encoding='utf-8'?>\n";
    x += "<!--\n FreeCAD Document, see https://www.freecad.org for more information...\n-->\n";
    x += std::string("<Document SchemaVersion=\"4\" ProgramVersion=\"") + PROGRAM_VERSION + "\" FileVersion=\"1\">\n";
    x += "    <Properties Count=\"2\" TransientCount=\"0\">\n";
    x += "        <Property name=\"CreatedBy\" type=\"App::PropertyString\">\n            <String value=\"construe\"/>\n        </Property>\n";
    x += "        <Property name=\"Label\" type=\"App::PropertyString\">\n            <String value=\"" + attr(label) + "\"/>\n        </Property>\n";
    x += "    </Properties>\n";
    x += "    <Objects Count=\"" + std::to_string(objects.size()) + "\">\n";
    for (size_t i = 0; i < objects.size(); ++i)
        x += "        <Object type=\"" + attr(objects[i].type) + "\" name=\"" + attr(objects[i].name) + "\" id=\"" +
             std::to_string(i + 1) + "\" />\n";
    x += "    </Objects>\n";
    x += "    <ObjectData Count=\"" + std::to_string(objects.size()) + "\">\n";
    for (const auto& o : objects) {
        x += "        <Object name=\"" + attr(o.name) + "\"" + (o.extensions.empty() ? "" : " Extensions=\"True\"") + ">\n";
        if (!o.extensions.empty()) {
            x += "            <Extensions Count=\"" + std::to_string(o.extensions.size()) + "\">\n";
            for (const auto& e : o.extensions) {
                const size_t c = e.rfind(':');
                const std::string en = c == std::string::npos ? e : e.substr(c + 1);
                x += "                <Extension type=\"" + attr(e) + "\" name=\"" + attr(en) + "\">\n                </Extension>\n";
            }
            x += "            </Extensions>\n";
        }
        std::vector<Prop> props = o.props;
        props.push_back(boolean("Visibility", o.visible));
        x += "            <Properties Count=\"" + std::to_string(props.size()) + "\" TransientCount=\"0\">\n";
        for (const auto& p : props) {
            x += "                <Property name=\"" + attr(p.name) + "\" type=\"" + attr(p.type) + "\"" +
                 (p.group.empty() ? std::string() : " group=\"" + attr(p.group) + "\" doc=\"\" attr=\"0\" ro=\"0\" hide=\"0\"") + ">\n";
            x += p.xml;
            x += "                </Property>\n";
        }
        x += "            </Properties>\n";
        x += "        </Object>\n";
    }
    x += "    </ObjectData>\n";
    x += "</Document>\n";
    return x;
}

std::string guiDocumentXml(const std::vector<Object>& objects) {
    std::string x = "<?xml version='1.0' encoding='utf-8'?>\n";
    x += "<Document SchemaVersion=\"1\">\n";
    x += "    <ViewProviderData Count=\"" + std::to_string(objects.size()) + "\">\n";
    for (const auto& o : objects) {
        x += "        <ViewProvider name=\"" + attr(o.name) + "\">\n";
        x += "            <Properties Count=\"1\" TransientCount=\"0\">\n";
        x += "                <Property name=\"Visibility\" type=\"App::PropertyBool\">\n";
        x += std::string("                    <Bool value=\"") + (o.visible ? "true" : "false") + "\"/>\n";
        x += "                </Property>\n";
        x += "            </Properties>\n";
        x += "        </ViewProvider>\n";
    }
    x += "    </ViewProviderData>\n";
    x += "    <Camera settings=\"\"/>\n";
    x += "</Document>\n";
    return x;
}

std::string container(const std::vector<Object>& objects, const std::string& label, const std::map<std::string, std::string>& parts) {
    std::vector<std::pair<std::string, std::string>> list;
    list.push_back({"Document.xml", documentXml(label, objects)});
    list.push_back({"GuiDocument.xml", guiDocumentXml(objects)});
    // FreeCAD reads the shape files in one pass over the zip in the order Document.xml refers to them: a part placed
    // before an earlier reference is never read (observed with FreeCAD 1.1, spec/fcstd.md §12а.1).
    std::set<std::string> done;
    for (const auto& o : objects)
        for (const auto& p : o.props)
            for (size_t a = p.xml.find("file=\""); a != std::string::npos; a = p.xml.find("file=\"", a + 6)) {
                const size_t b = p.xml.find('"', a + 6);
                const std::string file = p.xml.substr(a + 6, b - a - 6);
                auto it = parts.find(file);
                if (it == parts.end() || !done.insert(file).second) continue;
                list.push_back({file, it->second});
            }
    return io::zipStored(list);
}

}  // namespace construe::fcstd::write
