// GOST R 2.104-2023 frame and title block (spec/drawing.md §5): frame, forms 1 and 2а, additional fields, field values.
#include "drawing/title_block.h"

#include "base/number_format.h"

#include "drawing/render.h"
#include "drawing/standard.h"
#include "base/constants.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <span>

namespace construe::drawing {

namespace {

constexpr double FIELD_LEFT = 20, FIELD = 5;  // frame margins, mm (§5.2)
constexpr double BLOCK_W = 185;               // title block width (§5.3)
constexpr double LABEL_H = 2.5;               // field captions and dates (§5.4)

struct LineRow {
    double u0, v0, u1, v1;
    Weight w;
};
constexpr Weight K = Weight::Thick, T = Weight::Thin;

// Form 1 (§5.3, table): local title block coordinates.
constexpr LineRow FORM1_LINES[] = {
    {0, 0, 0, 55, K},      {0, 55, 185, 55, K},   {7, 30, 7, 55, K},     {17, 0, 17, 55, K},    {40, 0, 40, 55, K},
    {55, 0, 55, 55, K},    {65, 0, 65, 55, K},    {0, 30, 65, 30, K},    {0, 35, 65, 35, K},    {0, 40, 65, 40, T},
    {0, 45, 65, 45, T},    {0, 50, 65, 50, T},    {0, 5, 65, 5, T},      {0, 10, 65, 10, T},    {0, 15, 65, 15, T},
    {0, 20, 65, 20, T},    {0, 25, 65, 25, T},    {65, 40, 185, 40, K},  {65, 15, 185, 15, K},  {135, 0, 135, 40, K},
    {135, 35, 185, 35, K}, {135, 20, 185, 20, K}, {150, 20, 150, 40, K}, {167, 20, 167, 40, K}, {140, 20, 140, 35, K},
    {145, 20, 145, 35, K}, {155, 15, 155, 20, K},
};
// Form 2а (§5.3, table).
constexpr LineRow FORM2A_LINES[] = {
    {0, 0, 0, 15, K},   {0, 15, 185, 15, K}, {7, 0, 7, 15, K},   {17, 0, 17, 15, K},   {40, 0, 40, 15, K}, {55, 0, 55, 15, K},
    {65, 0, 65, 15, K}, {0, 5, 65, 5, K},    {0, 10, 65, 10, T}, {175, 0, 175, 15, K}, {175, 10, 185, 10, K},
};

// Title block text: fixed caption (label) or field value (key); fit — width fitting (§5.7), 0 — none.
struct TextRow {
    const char* label;
    const char* key;
    double u, v, h;
    const char* align;
    double fit;
};
constexpr const char* CM = "center middle";
constexpr const char* LM = "left middle";
constexpr const char* RM = "right middle";

// Form 1 texts (§5.4, table).
constexpr TextRow FORM1_TEXTS[] = {
    {"Изм.", nullptr, 3.5, 32.5, LABEL_H, CM, 0},
    {"Лист", nullptr, 12, 32.5, LABEL_H, CM, 0},
    {"№ докум.", nullptr, 28.5, 32.5, LABEL_H, CM, 0},
    {"Подп.", nullptr, 47.5, 32.5, LABEL_H, CM, 0},
    {"Дата", nullptr, 60, 32.5, LABEL_H, CM, 0},
    {"Разраб.", nullptr, 1, 27.5, LABEL_H, LM, 0},
    {"Пров.", nullptr, 1, 22.5, LABEL_H, LM, 0},
    {"Т. контр.", nullptr, 1, 17.5, LABEL_H, LM, 0},
    {nullptr, "extraRole", 1, 12.5, LABEL_H, LM, 15},
    {"Н. контр.", nullptr, 1, 7.5, LABEL_H, LM, 0},
    {"Утв.", nullptr, 1, 2.5, LABEL_H, LM, 0},
    {nullptr, "developer", 18, 27.5, 3.5, LM, 21},
    {nullptr, "checker", 18, 22.5, 3.5, LM, 21},
    {nullptr, "techControl", 18, 17.5, 3.5, LM, 21},
    {nullptr, "extraName", 18, 12.5, 3.5, LM, 21},
    {nullptr, "normControl", 18, 7.5, 3.5, LM, 21},
    {nullptr, "approver", 18, 2.5, 3.5, LM, 21},
    {nullptr, "developerDate", 60, 27.5, LABEL_H, CM, 9},
    {nullptr, "checkerDate", 60, 22.5, LABEL_H, CM, 9},
    {nullptr, "techControlDate", 60, 17.5, LABEL_H, CM, 9},
    {nullptr, "extraDate", 60, 12.5, LABEL_H, CM, 9},
    {nullptr, "normControlDate", 60, 7.5, LABEL_H, CM, 9},
    {nullptr, "approverDate", 60, 2.5, LABEL_H, CM, 9},
    {"Лит.", nullptr, 142.5, 37.5, LABEL_H, CM, 0},
    {"Масса", nullptr, 158.5, 37.5, LABEL_H, CM, 0},
    {"Масштаб", nullptr, 176, 37.5, LABEL_H, CM, 0},
    {nullptr, "letter1", 137.5, 27.5, 3.5, CM, 4},
    {nullptr, "letter2", 142.5, 27.5, 3.5, CM, 4},
    {nullptr, "letter3", 147.5, 27.5, 3.5, CM, 4},
    {nullptr, "mass", 158.5, 27.5, 5, CM, 15},
    {nullptr, "scale", 176, 27.5, 5, CM, 16},
    {"Лист", nullptr, 136, 17.5, LABEL_H, LM, 0},
    {nullptr, "sheet", 154, 17.5, 3.5, RM, 8},
    {"Листов", nullptr, 156, 17.5, LABEL_H, LM, 0},
    {nullptr, "sheets", 184, 17.5, 3.5, RM, 12},
    {nullptr, "designation", 125, 47.5, 7, CM, 118},
    {nullptr, "name", 100, 27.5, 7, CM, 68},
    {nullptr, "material", 100, 7.5, 5, CM, 68},
    {nullptr, "organization", 160, 7.5, 5, CM, 48},
};
// Form 2 (§5.3а, table).
constexpr LineRow FORM2_LINES[] = {
    {0, 0, 0, 40, K},      {0, 40, 185, 40, K},   {7, 25, 7, 40, K},     {17, 0, 17, 40, K},    {40, 0, 40, 40, K},
    {55, 0, 55, 40, K},    {65, 0, 65, 40, K},    {0, 25, 65, 25, K},    {0, 30, 65, 30, K},    {0, 35, 65, 35, T},
    {0, 5, 65, 5, T},      {0, 10, 65, 10, T},    {0, 15, 65, 15, T},    {0, 20, 65, 20, T},    {65, 25, 185, 25, K},
    {135, 0, 135, 25, K},  {135, 20, 185, 20, K}, {135, 15, 185, 15, K}, {150, 15, 150, 25, K}, {165, 15, 165, 25, K},
    {140, 15, 140, 20, K}, {145, 15, 145, 20, K},
};
// Form 2 texts (§5.3а).
constexpr TextRow FORM2_TEXTS[] = {
    {"Изм.", nullptr, 3.5, 27.5, LABEL_H, CM, 0},
    {"Лист", nullptr, 12, 27.5, LABEL_H, CM, 0},
    {"№ докум.", nullptr, 28.5, 27.5, LABEL_H, CM, 0},
    {"Подп.", nullptr, 47.5, 27.5, LABEL_H, CM, 0},
    {"Дата", nullptr, 60, 27.5, LABEL_H, CM, 0},
    {"Разраб.", nullptr, 1, 22.5, LABEL_H, LM, 0},
    {"Пров.", nullptr, 1, 17.5, LABEL_H, LM, 0},
    {nullptr, "extraRole", 1, 12.5, LABEL_H, LM, 15},
    {"Н. контр.", nullptr, 1, 7.5, LABEL_H, LM, 0},
    {"Утв.", nullptr, 1, 2.5, LABEL_H, LM, 0},
    {nullptr, "developer", 18, 22.5, 3.5, LM, 21},
    {nullptr, "checker", 18, 17.5, 3.5, LM, 21},
    {nullptr, "extraName", 18, 12.5, 3.5, LM, 21},
    {nullptr, "normControl", 18, 7.5, 3.5, LM, 21},
    {nullptr, "approver", 18, 2.5, 3.5, LM, 21},
    {nullptr, "developerDate", 60, 22.5, LABEL_H, CM, 9},
    {nullptr, "checkerDate", 60, 17.5, LABEL_H, CM, 9},
    {nullptr, "extraDate", 60, 12.5, LABEL_H, CM, 9},
    {nullptr, "normControlDate", 60, 7.5, LABEL_H, CM, 9},
    {nullptr, "approverDate", 60, 2.5, LABEL_H, CM, 9},
    {"Лит.", nullptr, 142.5, 22.5, LABEL_H, CM, 0},
    {"Лист", nullptr, 157.5, 22.5, LABEL_H, CM, 0},
    {"Листов", nullptr, 175, 22.5, LABEL_H, CM, 0},
    {nullptr, "letter1", 137.5, 17.5, 3.5, CM, 4},
    {nullptr, "letter2", 142.5, 17.5, 3.5, CM, 4},
    {nullptr, "letter3", 147.5, 17.5, 3.5, CM, 4},
    {nullptr, "sheet", 157.5, 17.5, 3.5, CM, 13},
    {nullptr, "sheets", 175, 17.5, 3.5, CM, 18},
    {nullptr, "designation", 125, 32.5, 7, CM, 118},
    {nullptr, "name", 100, 12.5, 7, CM, 68},
    {nullptr, "organization", 160, 7.5, 5, CM, 48},
};
// Form 2а texts (§5.4).
constexpr TextRow FORM2A_TEXTS[] = {
    {"Изм.", nullptr, 3.5, 2.5, LABEL_H, CM, 0},
    {"Лист", nullptr, 12, 2.5, LABEL_H, CM, 0},
    {"№ докум.", nullptr, 28.5, 2.5, LABEL_H, CM, 0},
    {"Подп.", nullptr, 47.5, 2.5, LABEL_H, CM, 0},
    {"Дата", nullptr, 60, 2.5, LABEL_H, CM, 0},
    {nullptr, "designation", 120, 7.5, 7, CM, 108},
    {"Лист", nullptr, 180, 12.5, LABEL_H, CM, 0},
    {nullptr, "sheet", 180, 5, 5, CM, 8},
};

// Field of the binding margin strip (§5.5): caption, value key (nullptr — handwritten signature), row bottom and top.
struct StripRow {
    const char* label;
    const char* key;
    double y0, y1;
};

// Keys (§5.4): table order and inheritance from the first sheet.
struct KeyRow {
    const char* key;
    bool inherit;
};
constexpr KeyRow KEYS[] = {
    {"designation", true},       {"name", true},           {"material", true},           {"letter1", true},
    {"letter2", true},           {"letter3", true},        {"mass", true},               {"scale", true},
    {"sheet", false},            {"sheets", true},         {"organization", true},       {"developer", true},
    {"checker", true},           {"techControl", true},    {"extraName", true},          {"normControl", true},
    {"approver", true},          {"extraRole", true},      {"developerDate", true},      {"checkerDate", true},
    {"techControlDate", true},   {"extraDate", true},      {"normControlDate", true},    {"approverDate", true},
    {"inventoryOriginal", true}, {"replacedInventory", true}, {"duplicateInventory", true}, {"firstUse", true},
    {"referenceNumber", true},   {"format", false},
    // ISO only (§5а.4).
    {"department", true},        {"technicalReference", true}, {"documentType", true}, {"documentStatus", true},
    {"revision", true},          {"issueDate", true},      {"language", true},
};

void addLine(SheetGraphics& g, const Point2& a, const Point2& b, Weight w) {
    Primitive p;
    p.kind = PrimitiveKind::Line;
    p.points = {a, b};
    p.weight = w;
    p.role = "titleBlock";
    g.primitives.push_back(std::move(p));
}

// Field text (§5.4, §5.7): an empty one is not added; with fitting the height is reduced to the fit width.
void addText(SheetGraphics& g, const Point2& pos, const std::string& text, double h, const char* align, double angle, double fit) {
    if (text.empty()) return;
    if (fit > 0)
        if (auto w = textWidth(text, h); w && *w > fit) h = h * fit / *w;
    Primitive p;
    p.kind = PrimitiveKind::Text;
    p.position = pos;
    p.text = text;
    p.height = h;
    p.angle = angle;
    p.align = align;
    p.role = "titleBlock";
    g.primitives.push_back(std::move(p));
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    return s;
}

// Drawing model (§5.6): object of the first main view of the first sheet with a non-empty reference; nullptr — none.
const doc::Element* drawingModel(const doc::Document& d, const doc::Element& drawing) {
    if (drawing.sheets.empty()) return nullptr;
    for (const auto& v : drawing.sheets.front().views) {
        if (v.kind != "base" || v.refElement.empty()) continue;
        if (!v.refFile.empty()) return nullptr;
        const int i = d.elementIndex(v.refElement);
        return i >= 0 ? &d.elements[size_t(i)] : nullptr;
    }
    return nullptr;
}

std::string massOf(const doc::Element& m, const ModelProvider& models) {
    if ((m.kind != "part" && m.kind != "body") || !m.material || !m.material->contains("density")) return {};
    const auto& dj = (*m.material)["density"];
    if (!dj.is_number()) return {};
    auto st = models.model("", m.id);
    if (!st || !*st) return {};
    double v = 0;
    for (const auto& b : (*st)->bodies) v += models.kernel().massProperties(b.shape).volume;
    if (!(v > 0)) return {};
    return formatMass(dj.get<double>() * v * 1e-9);
}

}  // namespace

const std::vector<std::string>& titleBlockKeys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> v;
        for (const auto& k : KEYS) v.emplace_back(k.key);
        return v;
    }();
    return keys;
}

std::string formatMass(double kg) {
    if (!(kg > 0) || !std::isfinite(kg)) return {};
    const int d = std::clamp(2 - int(std::floor(std::log10(kg))), 0, 6);
    const std::string s = fixedTrimmed(kg, d);
    return s == "0" ? std::string() : decimalComma(s);
}

std::map<std::string, std::string> titleBlockDefaults(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                                      const ModelProvider& models) {
    std::map<std::string, std::string> out;
    for (const auto& k : KEYS) out[k.key] = {};
    int index = 0;
    for (size_t i = 0; i < drawing.sheets.size(); ++i)
        if (drawing.sheets[i].id == sheet.id) index = int(i);
    const size_t count = drawing.sheets.size();
    const doc::Element* m = drawingModel(d, drawing);
    if (drawing.designation) out["designation"] = *drawing.designation;
    else if (m && m->designation) out["designation"] = *m->designation;
    if (m) {
        out["name"] = m->name;
        if (m->material && m->material->contains("name") && (*m->material)["name"].is_string())
            out["material"] = (*m->material)["name"].get<std::string>();
        out["mass"] = massOf(*m, models);
    }
    out["scale"] = replaceAll(sheet.scale, ".", ",");
    out["sheet"] = count > 1 ? std::to_string(index + 1) : std::string();
    if (drawingStandard(drawing) == font::DrawingStandard::Iso) out["sheet"] = std::to_string(index + 1) + "/" + std::to_string(count);  // §5а.4
    out["sheets"] = std::to_string(count);
    out["format"] = formatLabel(sheet.format);
    if (index > 0) {
        const auto& first = drawing.sheets.front().titleBlock;
        for (const auto& k : KEYS)
            if (k.inherit)
                if (const auto it = first.find(k.key); it != first.end()) out[k.key] = it->second;
    }
    return out;
}

CalendarDate today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

namespace {
constexpr const char* DATE_TEMPLATE_OPEN = "{now:";
std::string twoDigits(int v) {
    v = ((v % 100) + 100) % 100;
    return std::string(1, char('0' + v / 10)) + char('0' + v % 10);
}
}  // namespace

bool hasDateTemplate(const std::string& value) {
    const size_t at = value.find(DATE_TEMPLATE_OPEN);
    return at != std::string::npos && value.find('}', at) != std::string::npos;
}

std::string expandDateTemplates(const std::string& value, const CalendarDate& date) {
    std::string out;
    size_t i = 0;
    const size_t open = std::string(DATE_TEMPLATE_OPEN).size();
    while (i < value.size()) {
        const size_t at = value.find(DATE_TEMPLATE_OPEN, i);
        const size_t close = at == std::string::npos ? std::string::npos : value.find('}', at + open);
        if (close == std::string::npos) {
            out += value.substr(i);
            break;
        }
        out += value.substr(i, at - i);
        for (size_t k = at + open; k < close; ++k) {
            const char c = value[k];
            if (c == 'd') out += twoDigits(date.day);
            else if (c == 'm') out += twoDigits(date.month);
            else if (c == 'Y') out += std::to_string(date.year);
            else if (c == 'y') out += twoDigits(date.year);
            else out += c;
        }
        i = close + 1;
    }
    return out;
}

std::string formatLabel(const std::string& format) {
    return replaceAll(replaceAll(format, "A", "\xD0\x90"), "x", "\xC3\x97");  // «А» (Cyrillic A), «×»
}

void appendFrame(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                 SheetGraphics& g) {
    const auto size = sheetSize(drawing, sheet);
    if (!size) return;
    const bool first = drawing.sheets.empty() || drawing.sheets.front().id == sheet.id;
    auto values = titleBlockDefaults(d, drawing, sheet, models);
    for (const auto& [k, v] : sheet.titleBlock)
        if (values.count(k)) values[k] = v;
    if (drawingStandard(drawing) == font::DrawingStandard::Iso) {  // §5а
        appendIsoTitleFrame(size->width, size->height, projectionMethod(drawing), values, g);
        return;
    }
    appendTitleFrame(size->width, size->height, first ? TitleForm::Form1 : TitleForm::Form2a, values, true, g);
}

void appendTitleFrame(double W, double H, TitleForm form, const std::map<std::string, std::string>& valuesIn, bool graph26,
                      SheetGraphics& g) {
    const bool first = form != TitleForm::Form2a;
    std::map<std::string, std::string> values = valuesIn;
    // Date templates — today's date on the sheet (§5.4а, FX14-DRW-008); the document keeps the template.
    {
        const CalendarDate now = today();
        for (auto& [k, v] : values)
            if (hasDateTemplate(v)) v = expandDateTemplates(v, now);
    }
    // 1. Frame (§5.2).
    {
        Primitive f;
        f.kind = PrimitiveKind::Polyline;
        f.points = {Point2(FIELD_LEFT, FIELD), Point2(W - FIELD, FIELD), Point2(W - FIELD, H - FIELD), Point2(FIELD_LEFT, H - FIELD)};
        f.closed = true;
        f.weight = Weight::Thick;
        f.role = "frame";
        g.primitives.push_back(std::move(f));
    }
    // 2. Form lines (§5.3).
    const Point2 o(W - FIELD - BLOCK_W, FIELD);
    const auto lines = form == TitleForm::Form1   ? std::span<const LineRow>(FORM1_LINES)
                       : form == TitleForm::Form2 ? std::span<const LineRow>(FORM2_LINES)
                                                  : std::span<const LineRow>(FORM2A_LINES);
    for (const auto& l : lines) addLine(g, o + Point2(l.u0, l.v0), o + Point2(l.u1, l.v1), l.w);
    // 3. Additional field lines (§5.5).
    const StripRow stripLow[] = {{"Инв. № подл.", "inventoryOriginal", 5, 30},
                                 {"Подп. и дата", nullptr, 30, 65},
                                 {"Взам. инв. №", "replacedInventory", 65, 90},
                                 {"Инв. № дубл.", "duplicateInventory", 90, 115},
                                 {"Подп. и дата", nullptr, 115, 150}};
    const StripRow stripHigh[] = {{"Справ. №", "referenceNumber", H - 125, H - 65}, {"Перв. примен.", "firstUse", H - 65, H - 5}};
    const bool high = first && H - 125 >= 150;
    addLine(g, Point2(8, 5), Point2(8, 150), K);
    addLine(g, Point2(13, 5), Point2(13, 150), K);
    for (double y : {5.0, 30.0, 65.0, 90.0, 115.0, 150.0}) addLine(g, Point2(8, y), Point2(20, y), K);
    if (high) {
        addLine(g, Point2(8, H - 125), Point2(8, H - 5), K);
        addLine(g, Point2(13, H - 125), Point2(13, H - 5), K);
        for (double y : {H - 125, H - 65, H - 5}) addLine(g, Point2(8, y), Point2(20, y), K);
    }
    // Field 26 (§5.5): sheets larger than A4 in portrait — top right along the right frame side, turned by 90°;
    // otherwise — top left, turned by 180°.
    const bool graph26Side = W < H && !(W == 210 && H == 297);
    if (graph26 && graph26Side) {
        addLine(g, Point2(W - 19, H - 75), Point2(W - 19, H - 5), K);
        addLine(g, Point2(W - 19, H - 75), Point2(W - 5, H - 75), K);
    } else if (graph26) {
        addLine(g, Point2(20, H - 19), Point2(90, H - 19), K);
        addLine(g, Point2(90, H - 19), Point2(90, H - 5), K);
    }
    // 4. Form texts, then additional fields (§5.4, §5.5).
    const auto rows = form == TitleForm::Form1   ? std::span<const TextRow>(FORM1_TEXTS)
                      : form == TitleForm::Form2 ? std::span<const TextRow>(FORM2_TEXTS)
                                                 : std::span<const TextRow>(FORM2A_TEXTS);
    for (const auto& t : rows) addText(g, o + Point2(t.u, t.v), t.label ? std::string(t.label) : values[t.key], t.h, t.align, 0, t.fit);
    auto strip = [&](std::span<const StripRow> rs) {
        for (const auto& r : rs) {
            const double ym = (r.y0 + r.y1) / 2;
            addText(g, Point2(10.5, ym), r.label, LABEL_H, CM, PI / 2, 0);
            if (r.key) addText(g, Point2(16.5, ym), values[r.key], 3.5, CM, PI / 2, r.y1 - r.y0 - 2);
        }
    };
    strip(stripLow);
    if (high) strip(stripHigh);
    if (graph26 && graph26Side) addText(g, Point2(W - 12, H - 40), values["designation"], 5, CM, PI / 2, 68);
    else if (graph26) addText(g, Point2(55, H - 12), values["designation"], 5, CM, PI, 68);
    addText(g, o + Point2(100, -2.5), "Копировал", LABEL_H, CM, 0, 0);
    addText(g, o + Point2(160, -2.5), "Формат " + values["format"], LABEL_H, CM, 0, 0);
}

}  // namespace construe::drawing
