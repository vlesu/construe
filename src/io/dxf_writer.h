// Shared skeleton of a DXF AC1015 file (spec/io.md §2г): writing pairs, numbers and angles, handles in order of
// appearance, the HEADER, TABLES, BLOCKS and OBJECTS sections. Used by the drawing sheet export (§2г) and the flat
// pattern export (§2д); each of them supplies its tables and writes its entities.
#pragma once

#include "base/math.h"

#include <string>
#include <string_view>
#include <vector>

namespace construe::io::dxf {

// Real number: at most `digits` (6 by default) digits after the point, no trailing zeros, "-0" → "0" (§2г "Writing pairs").
std::string num(double x, int digits = 6);
// Angle in degrees reduced to [0, 360) taking num() rounding into account (§2г).
std::string degrees(double rad);
// Counterclockwise sweep from a0 to a1 reduced to (0, 2π].
double sweep(double a0, double a1);

// "Code, value" pairs and handles issued in order (§2г "Writing pairs", "Handles").
class Writer {
public:
    explicit Writer(unsigned& next) : next_(next) {}
    void pair(int code, std::string_view v);
    void number(int code, double x) { pair(code, num(x)); }
    void integer(int code, long long v) { pair(code, std::to_string(v)); }
    void point(int code, const Point2& p, bool z = true);
    std::string handle(int code = 5);
    std::string& str() { return s_; }

private:
    std::string s_;
    unsigned& next_;
};

// Linetype after ByBlock, ByLayer and Continuous: positive lengths alternating dash, gap.
struct Linetype {
    std::string name, description;
    std::vector<double> dash;
};
// Layer after layer "0"; an off layer has a negative color and is not plotted.
struct Layer {
    std::string name;
    int color = 7;
    std::string linetype = "Continuous";
    bool off = false;
};
// Text style after Standard.
struct Style {
    std::string name, file;
};
struct Tables {
    std::vector<Linetype> linetypes;
    std::vector<Layer> layers;
    std::vector<Style> styles;
};

// A file with extents [0, 0] … [width, height]: the constructor writes CLASSES, TABLES and BLOCKS and opens ENTITIES;
// entity() writes the common start of an entity in model space; finish() closes ENTITIES, writes OBJECTS and EOF and
// prepends the HEADER (it needs $HANDSEED).
class File {
public:
    File(double width, double height, const Tables& tables);
    void entity(const char* type, const std::string& layer);
    Writer& w() { return w_; }
    std::string finish();

private:
    unsigned next_ = 1;
    Writer w_{next_};
    std::string modelSpace_;
    double width_ = 0, height_ = 0;
};

}  // namespace construe::io::dxf
