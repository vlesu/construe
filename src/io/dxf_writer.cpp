// Shared DXF AC1015 skeleton (io/dxf_writer.h, spec/io.md §2г).
#include "io/dxf_writer.h"

#include "base/geom2.h"
#include "base/number_format.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <utility>

namespace construe::io::dxf {

namespace {
}  // namespace

std::string num(double x, int digits) { return fixedTrimmed(x, digits); }

std::string degrees(double rad) {
    double d = std::fmod(rad * RAD_TO_DEG, 360.0);
    if (d < 0) d += 360.0;
    std::string s = num(d);
    return s == "360" ? "0" : s;
}

double sweep(double a0, double a1) { return geom2::ccwSweepPositive(a0, a1); }

void Writer::pair(int code, std::string_view v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%3d\r\n", code);
    s_ += buf;
    s_ += v;
    s_ += "\r\n";
}

void Writer::point(int code, const Point2& p, bool z) {
    number(code, p.x());
    number(code + 10, p.y());
    if (z) number(code + 20, 0);
}

std::string Writer::handle(int code) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%X", next_++);
    pair(code, buf);
    return buf;
}

File::File(double width, double height, const Tables& tables) : width_(width), height_(height) {
    Writer& w = w_;
    const double W = width, H = height;
    // CLASSES — empty.
    w.pair(0, "SECTION");
    w.pair(2, "CLASSES");
    w.pair(0, "ENDSEC");

    // ---------------------------------------------------------------- TABLES
    w.pair(0, "SECTION");
    w.pair(2, "TABLES");
    auto table = [&](const char* name, int count, bool dimstyle = false) {
        w.pair(0, "TABLE");
        w.pair(2, name);
        const std::string h = w.handle();
        w.pair(330, "0");
        w.pair(100, "AcDbSymbolTable");
        w.integer(70, count);
        if (dimstyle) {
            w.pair(100, "AcDbDimStyleTable");
            w.integer(71, 0);
        }
        return h;
    };
    auto record = [&](const char* type, const std::string& owner, const char* subclass, const std::string& name, bool flags = true,
                      int handleCode = 5) {
        w.pair(0, type);
        w.handle(handleCode);
        w.pair(330, owner);
        w.pair(100, "AcDbSymbolTableRecord");
        w.pair(100, subclass);
        w.pair(2, name);
        if (flags) w.integer(70, 0);
    };
    // VPORT
    {
        const std::string t = table("VPORT", 1);
        record("VPORT", t, "AcDbViewportTableRecord", "*ACTIVE");
        w.point(10, Point2(0, 0), false);
        w.point(11, Point2(1, 1), false);
        w.point(12, Point2(W / 2, H / 2), false);
        w.point(13, Point2(0, 0), false);
        w.point(14, Point2(10, 10), false);
        w.point(15, Point2(10, 10), false);
        w.number(16, 0), w.number(26, 0), w.number(36, 1);
        w.number(17, 0), w.number(27, 0), w.number(37, 0);
        w.number(40, 1.05 * H);
        w.number(41, H > 0 ? W / H : 1);
        w.number(42, 50), w.number(43, 0), w.number(44, 0), w.number(50, 0), w.number(51, 0);
        for (const auto& [code, v] : std::array<std::pair<int, int>, 8>{{{71, 0}, {72, 100}, {73, 1}, {74, 3}, {75, 0}, {76, 0}, {77, 0}, {78, 0}}})
            w.integer(code, v);
        w.pair(0, "ENDTAB");
    }
    // LTYPE
    {
        const std::string t = table("LTYPE", int(3 + tables.linetypes.size()));
        auto ltype = [&](const std::string& name, const std::string& descr, const std::vector<double>& dash) {
            record("LTYPE", t, "AcDbLinetypeTableRecord", name);
            w.pair(3, descr);
            w.integer(72, 65);
            w.integer(73, int(dash.size()));
            double total = 0;
            for (double d : dash) total += d;
            w.number(40, total);
            for (size_t i = 0; i < dash.size(); ++i) {
                w.number(49, i % 2 ? -dash[i] : dash[i]);
                w.integer(74, 0);
            }
        };
        ltype("ByBlock", "", {});
        ltype("ByLayer", "", {});
        ltype("Continuous", "Solid line", {});
        for (const auto& lt : tables.linetypes) ltype(lt.name, lt.description, lt.dash);
        w.pair(0, "ENDTAB");
    }
    // LAYER
    {
        const std::string t = table("LAYER", int(1 + tables.layers.size()));
        auto layer = [&](const Layer& l) {
            record("LAYER", t, "AcDbLayerTableRecord", l.name);
            w.integer(62, l.off ? -l.color : l.color);
            w.pair(6, l.linetype);
            if (l.off) w.integer(290, 0);
            w.integer(370, -3);
        };
        layer(Layer{"0"});
        for (const auto& l : tables.layers) layer(l);
        w.pair(0, "ENDTAB");
    }
    // STYLE
    {
        const std::string t = table("STYLE", int(1 + tables.styles.size()));
        auto style = [&](const std::string& name, const std::string& file) {
            record("STYLE", t, "AcDbTextStyleTableRecord", name);
            w.number(40, 0);
            w.number(41, 1);
            w.number(50, 0);
            w.integer(71, 0);
            w.number(42, 2.5);
            w.pair(3, file);
            w.pair(4, "");
        };
        style("Standard", "txt");
        for (const auto& st : tables.styles) style(st.name, st.file);
        w.pair(0, "ENDTAB");
    }
    // VIEW, UCS — empty; APPID, DIMSTYLE.
    table("VIEW", 0);
    w.pair(0, "ENDTAB");
    table("UCS", 0);
    w.pair(0, "ENDTAB");
    {
        const std::string t = table("APPID", 1);
        record("APPID", t, "AcDbRegAppTableRecord", "ACAD");
        w.pair(0, "ENDTAB");
    }
    {
        const std::string t = table("DIMSTYLE", 1, true);
        record("DIMSTYLE", t, "AcDbDimStyleTableRecord", "Standard", true, 105);
        w.pair(0, "ENDTAB");
    }
    std::string paperSpace;
    {
        const std::string t = table("BLOCK_RECORD", 2);
        auto br = [&](const char* name) {
            w.pair(0, "BLOCK_RECORD");
            const std::string h = w.handle();
            w.pair(330, t);
            w.pair(100, "AcDbSymbolTableRecord");
            w.pair(100, "AcDbBlockTableRecord");
            w.pair(2, name);
            return h;
        };
        modelSpace_ = br("*Model_Space");
        paperSpace = br("*Paper_Space");
        w.pair(0, "ENDTAB");
    }
    w.pair(0, "ENDSEC");

    // ---------------------------------------------------------------- BLOCKS
    w.pair(0, "SECTION");
    w.pair(2, "BLOCKS");
    const std::array<std::pair<const char*, std::string>, 2> blocks = {{{"*Model_Space", modelSpace_}, {"*Paper_Space", paperSpace}}};
    for (const auto& [name, rec] : blocks) {
        w.pair(0, "BLOCK");
        w.handle();
        w.pair(330, rec);
        w.pair(100, "AcDbEntity");
        w.pair(8, "0");
        w.pair(100, "AcDbBlockBegin");
        w.pair(2, name);
        w.integer(70, 0);
        w.point(10, Point2(0, 0));
        w.pair(3, name);
        w.pair(1, "");
        w.pair(0, "ENDBLK");
        w.handle();
        w.pair(330, rec);
        w.pair(100, "AcDbEntity");
        w.pair(8, "0");
        w.pair(100, "AcDbBlockEnd");
    }
    w.pair(0, "ENDSEC");

    // ---------------------------------------------------------------- ENTITIES (filled by the caller)
    w.pair(0, "SECTION");
    w.pair(2, "ENTITIES");
}

void File::entity(const char* type, const std::string& layer) {
    w_.pair(0, type);
    w_.handle();
    w_.pair(330, modelSpace_);
    w_.pair(100, "AcDbEntity");
    w_.pair(8, layer);
}

std::string File::finish() {
    Writer& w = w_;
    w.pair(0, "ENDSEC");

    // ---------------------------------------------------------------- OBJECTS
    w.pair(0, "SECTION");
    w.pair(2, "OBJECTS");
    w.pair(0, "DICTIONARY");
    const std::string root = w.handle();
    w.pair(330, "0");
    w.pair(100, "AcDbDictionary");
    w.integer(281, 1);
    w.pair(3, "ACAD_GROUP");
    char groupHandle[32];
    std::snprintf(groupHandle, sizeof groupHandle, "%X", next_);
    w.pair(350, groupHandle);
    w.pair(0, "DICTIONARY");
    w.handle();
    w.pair(330, root);
    w.pair(100, "AcDbDictionary");
    w.integer(281, 1);
    w.pair(0, "ENDSEC");
    w.pair(0, "EOF");

    // ---------------------------------------------------------------- HEADER (last: needs $HANDSEED)
    const double W = width_, H = height_;
    unsigned unused = 0;
    Writer hd(unused);
    hd.pair(0, "SECTION");
    hd.pair(2, "HEADER");
    auto var = [&](const char* name) { hd.pair(9, name); };
    var("$ACADVER"), hd.pair(1, "AC1015");
    var("$DWGCODEPAGE"), hd.pair(3, "ANSI_1252");
    var("$INSBASE"), hd.point(10, Point2(0, 0));
    var("$EXTMIN"), hd.point(10, Point2(0, 0));
    var("$EXTMAX"), hd.point(10, Point2(W, H));
    var("$LIMMIN"), hd.point(10, Point2(0, 0), false);
    var("$LIMMAX"), hd.point(10, Point2(W, H), false);
    var("$LTSCALE"), hd.number(40, 1);
    var("$INSUNITS"), hd.integer(70, 4);
    var("$MEASUREMENT"), hd.integer(70, 1);
    var("$LWDISPLAY"), hd.integer(290, 1);
    var("$HANDSEED");
    char seed[32];
    std::snprintf(seed, sizeof seed, "%X", next_);
    hd.pair(5, seed);
    hd.pair(0, "ENDSEC");
    return hd.str() + w.str();
}

}  // namespace construe::io::dxf
