// GOST 2.106 assembly specification (spec/specification.md).
#include "drawing/specification.h"

#include "drawing/render.h"
#include "drawing/title_block.h"
#include "regen/instance_patterns.h"
#include "regen/regen.h"
#include "base/constants.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace construe::drawing {

namespace {

constexpr double TABLE_LEFT = 20, TABLE_TOP = 292;     // left edge and top of the table — frame (§4.3)
constexpr double SHEET_W = 210, SHEET_H = 297;         // A4 portrait
constexpr double BOTTOM_FIRST = 45, BOTTOM_NEXT = 20;  // top of form 2 / 2а
constexpr const char* ROLE = "specification";
constexpr const char* CM = "center middle";
constexpr const char* LM = "left middle";

// GOST 2.106 form 1 columns (§4.3, table SPEC_COLUMNS).
struct Column {
    double width;
    const char* header;
    bool vertical;  // header rotated by π/2
    double headerFit;
};
constexpr Column COLUMNS[] = {
    {6, "Формат", true, 13},     {6, "Зона", true, 13},           {8, "Поз.", true, 13},  {70, "Обозначение", false, 68},
    {63, "Наименование", false, 61}, {10, "Кол.", true, 13}, {22, "Примечание", false, 20},
};
enum Col { C_FORMAT, C_ZONE, C_POS, C_DESIGNATION, C_NAME, C_QTY, C_NOTE, C_COUNT };

double colLeft(int c) {
    double x = TABLE_LEFT;
    for (int i = 0; i < c; ++i) x += COLUMNS[i].width;
    return x;
}
double colCenter(int c) { return colLeft(c) + COLUMNS[c].width / 2; }

struct SectionRow {
    const char* id;
    const char* title;
    bool numbered;
    bool byDesignation;
};
constexpr SectionRow SECTIONS[] = {
    {"documentation", "Документация", false, true},  {"complexes", "Комплексы", true, true},
    {"assemblies", "Сборочные единицы", true, true}, {"parts", "Детали", true, true},
    {"software", "Программные изделия и базы данных", true, true},
    {"standard", "Стандартные изделия", true, false}, {"other", "Прочие изделия", true, false},
    {"materials", "Материалы", true, false},          {"kits", "Комплекты", true, true},
};
int sectionRank(const std::string& s) {
    for (int i = 0; i < int(std::size(SECTIONS)); ++i)
        if (s == SECTIONS[i].id) return i;
    return int(std::size(SECTIONS));
}

// Keys of the form 2 title block and additional fields (§5).
const char* const TITLE_KEYS[] = {
    "designation",   "name",          "letter1",         "letter2",           "letter3",        "sheet",
    "sheets",        "organization",  "developer",       "checker",           "extraName",      "normControl",
    "approver",      "extraRole",     "developerDate",   "checkerDate",       "extraDate",      "normControlDate",
    "approverDate",  "inventoryOriginal", "replacedInventory", "duplicateInventory", "firstUse", "referenceNumber",
    "format",
};

// Drawing model (drawing.md §5.6): element of the first main view of the first sheet referencing this file; empty — none.
std::string drawingModelId(const doc::Element& drawing) {
    if (drawing.sheets.empty()) return {};
    for (const auto& v : drawing.sheets.front().views) {
        if (v.kind != "base" || v.refElement.empty()) continue;
        return v.refFile.empty() ? v.refElement : std::string();
    }
    return {};
}

std::string str(const doc::Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
bool has(const doc::Json& j, const char* key) { return j.find(key) != j.end(); }
std::string positionText(const doc::Json& j) {
    const auto it = j.find("position");
    if (it == j.end() || !it->is_number()) return {};
    return std::to_string(static_cast<long long>(it->get<double>()));
}

// Sort key "empty — after non-empty" (§3.4).
bool emptyLastLess(const std::string& a, const std::string& b) {
    if (a.empty() != b.empty()) return b.empty();
    return naturalLess(a, b);
}

// Word wrapping of text to width avail (§4.1).
std::vector<std::string> wrap(const std::string& text, double avail) {
    if (text.empty()) return {std::string()};
    if (!textWidth(text, SPEC_TEXT_H)) return {text};  // font not loaded — no wrapping
    std::vector<std::string> words;
    for (size_t i = 0; i < text.size();) {
        const size_t j = text.find(' ', i);
        const size_t e = j == std::string::npos ? text.size() : j;
        if (e > i) words.push_back(text.substr(i, e - i));
        i = e + 1;
    }
    if (words.empty()) return {text};
    std::vector<std::string> lines;
    std::string cur;
    for (const auto& w : words) {
        const std::string cand = cur.empty() ? w : cur + " " + w;
        const auto cw = textWidth(cand, SPEC_TEXT_H);
        if (cur.empty() || (cw && *cw <= avail)) {
            cur = cand;
        } else {
            lines.push_back(cur);
            cur = w;
        }
    }
    lines.push_back(cur);
    return lines;
}

void addLine(SheetGraphics& g, const Point2& a, const Point2& b, Weight w) {
    Primitive p;
    p.kind = PrimitiveKind::Line;
    p.points = {a, b};
    p.weight = w;
    p.role = ROLE;
    g.primitives.push_back(std::move(p));
}

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
    p.role = ROLE;
    g.primitives.push_back(std::move(p));
}

}  // namespace

// «Format» and «Note» cells of a document row by the drawing's sheets (§3.1, GOST R 2.106-2019 item 4.2.17.1): one
// basic format — the format; one derived format or sheets of different formats — "*)" with the formats in the note
// ("*) А4, А3"), ascending by sheet area (ties — §2.1 table order).
static std::pair<std::string, std::string> drawingFormatCells(const doc::Element& D) {
    std::vector<std::string> formats;
    for (const auto& sh : D.sheets)
        if (std::find(formats.begin(), formats.end(), sh.format) == formats.end()) formats.push_back(sh.format);
    if (formats.empty()) return {};
    const auto& table = sheetFormats();
    const auto rank = [&](const std::string& f) {
        const auto sz = sheetSize(f, "portrait").value_or(SheetSize{});
        return std::make_pair(sz.width * sz.height, int(std::find(table.begin(), table.end(), f) - table.begin()));
    };
    std::sort(formats.begin(), formats.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    if (formats.size() == 1 && formats.front().find('x') == std::string::npos) return {formatLabel(formats.front()), {}};
    std::string note = "*) ";
    for (size_t i = 0; i < formats.size(); ++i) note += (i ? ", " : "") + formatLabel(formats[i]);
    return {"*)", note};
}

const std::vector<std::string>& specSections() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const auto& s : SECTIONS) out.emplace_back(s.id);
        return out;
    }();
    return v;
}

std::string specSectionTitle(const std::string& section) {
    for (const auto& s : SECTIONS)
        if (section == s.id) return s.title;
    return {};
}

bool naturalLess(const std::string& a, const std::string& b) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = digit(a[i]), db = digit(b[j]);
        size_t ie = i, je = j;
        while (ie < a.size() && digit(a[ie]) == da) ++ie;
        while (je < b.size() && digit(b[je]) == db) ++je;
        const std::string ca = a.substr(i, ie - i), cb = b.substr(j, je - j);
        if (da && db) {
            const auto na = ca.find_first_not_of('0'), nb = cb.find_first_not_of('0');
            const std::string va = na == std::string::npos ? std::string() : ca.substr(na);
            const std::string vb = nb == std::string::npos ? std::string() : cb.substr(nb);
            if (va.size() != vb.size()) return va.size() < vb.size();
            if (va != vb) return va < vb;
            if (ca.size() != cb.size()) return ca.size() < cb.size();
        } else if (ca != cb) {
            return ca < cb;  // bytewise UTF-8: char_traits<char> compares as unsigned char
        }
        i = ie;
        j = je;
    }
    return (a.size() - i) < (b.size() - j);
}

SpecContent specificationContent(const doc::Document& d, const doc::Element& spec, const LinkedDocs& linked, const SpecConnections* connections) {
    SpecContent out;
    std::vector<SpecItem> autos;
    const int ai = d.elementIndex(spec.specAssembly);
    if (ai < 0) {
        out.errors.push_back(Error("SPEC_ASSEMBLY_NOT_FOUND").with("element", spec.specAssembly));
    } else if (!d.elements[size_t(ai)].isAssembly()) {
        out.errors.push_back(Error("SPEC_NOT_ASSEMBLY").with("element", spec.specAssembly));
    } else {
        const doc::Element& A = d.elements[size_t(ai)];
        // 1. Documentation — assembly drawings of this assembly.
        for (const auto& D : d.elements) {
            if (!D.isDrawing() || drawingModelId(D) != A.id) continue;
            SpecItem it;
            it.section = "documentation";
            it.key = D.id;
            std::tie(it.format, it.note) = drawingFormatCells(D);
            if (D.designation) it.designation = *D.designation;
            else if (A.designation) it.designation = *A.designation + " СБ";
            it.name = "Сборочный чертёж";
            autos.push_back(std::move(it));
        }
        // 2. Composition — first-level instances grouped by the inserted object.
        std::map<std::string, size_t> group;  // key → index in autos
        std::map<std::string, int> count;
        std::set<std::string> failed;
        // Library part copies of this document (library.md §7): instance variables with the copies as linked documents.
        regen::LinkMap libLinks;
        for (const auto& lp : d.libraryParts) libLinks[doc::libraryRef(lp.id)] = {lp.document, lp.document.get(), std::nullopt};
        std::optional<expr::VariableTable> docVars;
        std::map<std::string, std::string> quals;
        // Child instances of instance patterns (FX7-ASM-007, assembly.md §7е.3): counted with their source.
        std::map<std::string, int> children;
        if (!A.patterns.empty()) {
            const expr::VariableTable vars = regen::documentVariables(d, &libLinks, {});
            auto lookup = [&](const std::string& path) -> std::pair<const doc::Document*, const doc::Element*> {
                const doc::Document* D = &d;
                const doc::Element* E = &A;
                size_t from = 0;
                while (true) {
                    const size_t slash = path.find('/', from);
                    const std::string seg = path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
                    const auto it = std::find_if(E->instances.begin(), E->instances.end(), [&](const doc::Instance& x) { return x.id == seg; });
                    if (it == E->instances.end()) return {nullptr, nullptr};
                    if (doc::isLibraryRef(it->refFile)) {
                        const doc::LibraryPart* lp = d.libraryPart(it->refFile.substr(std::char_traits<char>::length(doc::LIBRARY_REF_PREFIX)));
                        D = lp ? lp->document.get() : nullptr;
                    } else if (!it->refFile.empty()) {
                        D = D == &d && linked ? linked(it->refFile) : nullptr;  // deeper links — not resolved here
                    }
                    if (!D) return {nullptr, nullptr};
                    const int ei = D->elementIndex(it->refElement);
                    if (ei < 0) return {nullptr, nullptr};
                    E = &D->elements[size_t(ei)];
                    if (slash == std::string::npos) return {D, E};
                    from = slash + 1;
                }
            };
            for (const auto& p : A.patterns) children[p.source()] += regen::patternChildCount(A, p, vars, regen::documentUnits(d), lookup);
        }
        for (const auto& in : A.instances) {
            if (in.suppressed) continue;
            const int weight = 1 + (children.count(in.id) ? children.at(in.id) : 0);
            if (doc::isLibraryRef(in.refFile)) {  // standard item: grouped by (copy, instance designation)
                const doc::LibraryPart* lp = d.libraryPart(in.refFile.substr(std::char_traits<char>::length(doc::LIBRARY_REF_PREFIX)));
                if (!lp) continue;  // impossible in a valid document (document.md §5)
                if (!docVars) docVars = regen::documentVariables(d, &libLinks, {}), quals = regen::instanceQualifiers(d);
                const auto table = regen::instanceVariableTable(*docVars, quals.at(in.id), *lp->document, false);
                const std::string name = expr::substitute(lp->designation, table, regen::documentUnits(*lp->document));
                const std::string key = in.refFile + "#" + name;
                if (group.count(key)) {
                    count[key] += weight;
                    autos[group[key]].instances.push_back(in.id);
                    continue;
                }
                SpecItem it;
                it.key = key;
                it.section = "standard";
                it.name = name;
                it.instances.push_back(in.id);
                group[key] = autos.size();
                count[key] = weight;
                autos.push_back(std::move(it));
                continue;
            }
            const std::string key = in.refFile.empty() ? in.refElement : in.refFile + "#" + in.refElement;
            if (failed.count(key)) continue;
            if (group.count(key)) {
                count[key] += weight;
                autos[group[key]].instances.push_back(in.id);
                continue;
            }
            const doc::Document* X = in.refFile.empty() ? &d : (linked ? linked(in.refFile) : nullptr);
            const int ei = X ? X->elementIndex(in.refElement) : -1;
            if (ei < 0) {
                Error e("SPEC_ITEM_NOT_FOUND");
                e.with("instance", in.id).with("element", in.refElement);
                if (!in.refFile.empty()) e.with("file", in.refFile);
                out.errors.push_back(std::move(e));
                failed.insert(key);
                continue;
            }
            const doc::Element& E = X->elements[size_t(ei)];
            SpecItem it;
            it.key = key;
            it.section = E.isAssembly() ? "assemblies" : "parts";
            // Designation with the variables of the object's document substituted (expr.md §5а.1, S6-PARAM-001).
            if (E.designation)
                it.designation = E.designation->find('{') == std::string::npos
                                     ? *E.designation
                                     : expr::substitute(*E.designation, regen::documentVariables(*X), regen::documentUnits(*X));
            it.name = E.name;
            if (E.isAssembly()) {
                for (const auto& S : X->elements)
                    if (S.isSpecification() && S.specAssembly == E.id) {
                        it.format = formatLabel("A4");
                        break;
                    }
            } else {
                it.format = "БЧ";
                for (const auto& D : X->elements)
                    if (D.isDrawing() && drawingModelId(D) == E.id) {
                        std::tie(it.format, it.note) = drawingFormatCells(D);
                        break;
                    }
            }
            it.instances.push_back(in.id);
            group[key] = autos.size();
            count[key] = weight;
            autos.push_back(std::move(it));
        }
        // 2а. Connection fasteners (FX14-SPEC-001): derived instances of the connection features — standard items
        // grouped by (copy, designation) like library part copies, one per record of the report's parts.
        if (connections)
            for (const auto& f : A.features) {
                const auto rep = connections->find(f->id);
                if (rep == connections->end() || !rep->second.is_object()) continue;
                const auto parts = rep->second.find("parts");
                if (parts == rep->second.end() || !parts->is_array()) continue;
                for (const auto& p : *parts) {
                    const std::string lib = str(p, "library"), name = str(p, "designation");
                    if (lib.empty()) continue;
                    const std::string key = doc::libraryRef(lib) + "#" + name;
                    const std::string path = f->id + "/" + str(p, "output");
                    if (group.count(key)) {
                        count[key] += 1;
                        autos[group[key]].instances.push_back(path);
                        continue;
                    }
                    SpecItem it;
                    it.key = key;
                    it.section = "standard";
                    it.name = name;
                    it.instances.push_back(path);
                    group[key] = autos.size();
                    count[key] = 1;
                    autos.push_back(std::move(it));
                }
            }
        for (const auto& [key, idx] : group) autos[idx].quantity = std::to_string(count[key]);
    }
    // 3. Overrides (§3.2): the first record with the row key.
    for (auto& it : autos) {
        const auto r = std::find_if(spec.specRows.begin(), spec.specRows.end(), [&](const doc::Json& j) { return str(j, "key") == it.key; });
        if (r == spec.specRows.end()) continue;
        if (has(*r, "section")) it.section = str(*r, "section");
        if (has(*r, "position")) {
            it.position = positionText(*r);
            it.positionSet = true;
        }
        if (has(*r, "note")) it.note = str(*r, "note");
        if (has(*r, "format")) it.format = str(*r, "format");
        if (has(*r, "zone")) it.zone = str(*r, "zone");
    }
    // Manual rows (§3.3).
    std::vector<SpecItem> all = std::move(autos);
    for (size_t i = 0; i < spec.specExtraRows.size(); ++i) {
        const doc::Json& j = spec.specExtraRows[i];
        SpecItem it;
        it.section = str(j, "section");
        it.extra = int(i);
        it.format = str(j, "format");
        it.zone = str(j, "zone");
        if (has(j, "position")) {
            it.position = positionText(j);
            it.positionSet = true;
        }
        it.designation = str(j, "designation");
        it.name = str(j, "name");
        it.quantity = str(j, "quantity");
        it.note = str(j, "note");
        all.push_back(std::move(it));
    }
    // 4. Order (§3.4).
    std::stable_sort(all.begin(), all.end(), [](const SpecItem& a, const SpecItem& b) {
        const int ra = sectionRank(a.section), rb = sectionRank(b.section);
        if (ra != rb) return ra < rb;
        const bool byDes = ra < int(std::size(SECTIONS)) ? SECTIONS[ra].byDesignation : true;
        if (byDes) {
            if (a.designation != b.designation) return emptyLastLess(a.designation, b.designation);
            return naturalLess(a.name, b.name);
        }
        if (a.name != b.name) return naturalLess(a.name, b.name);
        return emptyLastLess(a.designation, b.designation);
    });
    // 5. Positions (§3.5).
    long long next = 1;
    for (auto& it : all) {
        const int r = sectionRank(it.section);
        if (r >= int(std::size(SECTIONS)) || !SECTIONS[r].numbered) continue;
        if (it.positionSet) {
            next = std::stoll(it.position) + 1;
        } else {
            it.position = std::to_string(next++);
        }
    }
    out.items = std::move(all);
    return out;
}

namespace {
// Line blocks that are not split (§4.1).
struct Block {
    std::vector<SpecLine> lines;
    bool heading = false;
};
std::vector<Block> specBlocks(const SpecContent& c) {
    std::vector<Block> blocks;
    auto blank = [&] { blocks.push_back({{SpecLine{}}, false}); };
    blank();
    for (const auto& s : SECTIONS) {
        std::vector<int> idx;
        for (int i = 0; i < int(c.items.size()); ++i)
            if (c.items[size_t(i)].section == s.id) idx.push_back(i);
        if (idx.empty()) continue;
        SpecLine h;
        h.kind = SpecLine::Kind::Heading;
        h.section = s.id;
        blocks.push_back({{h}, true});
        blank();
        for (int i : idx) {
            const SpecItem& it = c.items[size_t(i)];
            const auto names = wrap(it.name, COLUMNS[C_NAME].width - 2);
            const auto notes = wrap(it.note, COLUMNS[C_NOTE].width - 2);
            Block b;
            const size_t n = std::max(names.size(), notes.size());
            for (size_t k = 0; k < n; ++k) {
                SpecLine l;
                l.kind = SpecLine::Kind::Item;
                l.item = i;
                l.first = k == 0;
                if (k < names.size()) l.name = names[k];
                if (k < notes.size()) l.note = notes[k];
                b.lines.push_back(std::move(l));
            }
            blocks.push_back(std::move(b));
        }
        for (int k = 0; k < SPEC_RESERVE_ROWS; ++k) blank();
    }
    return blocks;
}
}  // namespace

SpecPages specificationPages(const SpecContent& c) {
    const std::vector<Block> blocks = specBlocks(c);
    // Layout (§4.2).
    SpecPages pages(1);
    auto cap = [&](size_t p) { return p == 0 ? SPEC_ROWS_FIRST : SPEC_ROWS_NEXT; };
    for (const auto& b : blocks) {
        auto rem = [&] { return cap(pages.size() - 1) - int(pages.back().size()); };
        const bool isBlank = b.lines.size() == 1 && b.lines[0].kind == SpecLine::Kind::Blank;
        if (isBlank && pages.size() > 1 && pages.back().empty()) continue;
        if (b.heading && rem() < 3) pages.emplace_back();
        const int n = int(b.lines.size());
        if (!isBlank && n > rem() && !pages.back().empty() && n <= SPEC_ROWS_NEXT) pages.emplace_back();
        if (isBlank && rem() <= 0) {
            pages.emplace_back();
            continue;  // an empty line at the start of a sheet is not needed
        }
        for (const auto& l : b.lines) {
            if (rem() <= 0) pages.emplace_back();
            pages.back().push_back(l);
        }
    }
    while (pages.size() > 1 && pages.back().empty()) pages.pop_back();
    return pages;
}

const std::vector<std::string>& specificationTitleKeys() {
    static const std::vector<std::string> v(std::begin(TITLE_KEYS), std::end(TITLE_KEYS));
    return v;
}

std::map<std::string, std::string> specificationTitleDefaults(const doc::Document& d, const doc::Element& spec, int page, int pages) {
    std::map<std::string, std::string> out;
    for (const char* k : TITLE_KEYS) out[k] = {};
    const int ai = d.elementIndex(spec.specAssembly);
    const doc::Element* A = ai >= 0 ? &d.elements[size_t(ai)] : nullptr;
    if (spec.designation) out["designation"] = *spec.designation;
    else if (A && A->designation) out["designation"] = *A->designation;
    if (A) out["name"] = A->name;
    out["sheet"] = pages > 1 ? std::to_string(page + 1) : std::string();
    out["sheets"] = std::to_string(pages);
    out["format"] = formatLabel("A4");
    return out;
}

SheetGraphics composeSpecificationPage(const doc::Document& d, const doc::Element& spec, const SpecContent& c, const SpecPages& pages,
                                       int page) {
    SheetGraphics g;
    g.width = SHEET_W;
    g.height = SHEET_H;
    for (const auto& e : c.errors) g.errors.push_back({std::string(), e});
    // 1. Sheet trim.
    Primitive trim;
    trim.kind = PrimitiveKind::Polyline;
    trim.points = {Point2(0, 0), Point2(SHEET_W, 0), Point2(SHEET_W, SHEET_H), Point2(0, SHEET_H)};
    trim.closed = true;
    trim.role = "trim";
    g.primitives.push_back(std::move(trim));
    // 2. Frame, title block and additional fields (§5).
    auto values = specificationTitleDefaults(d, spec, page, int(pages.size()));
    for (const auto& [k, v] : spec.specTitleBlock)
        if (values.count(k)) values[k] = v;
    appendTitleFrame(SHEET_W, SHEET_H, page == 0 ? TitleForm::Form2 : TitleForm::Form2a, values, false, g);
    // 3. Table lines.
    const double bottom = page == 0 ? BOTTOM_FIRST : BOTTOM_NEXT;
    const double headerBottom = TABLE_TOP - SPEC_HEADER_H;
    for (int col = 1; col < C_COUNT; ++col) addLine(g, Point2(colLeft(col), TABLE_TOP), Point2(colLeft(col), bottom), Weight::Thick);
    const double right = colLeft(C_COUNT);
    addLine(g, Point2(TABLE_LEFT, headerBottom), Point2(right, headerBottom), Weight::Thick);
    for (int k = 1;; ++k) {
        const double y = headerBottom - SPEC_ROW_H * k;
        if (!(y > bottom + 0.5)) break;
        addLine(g, Point2(TABLE_LEFT, y), Point2(right, y), Weight::Thin);
    }
    // 4. Table header.
    const double hy = TABLE_TOP - SPEC_HEADER_H / 2;
    for (int col = 0; col < C_COUNT; ++col) {
        const Column& cc = COLUMNS[col];
        addText(g, Point2(colCenter(col), hy), cc.header, cc.vertical ? 2.5 : 3.5, CM, cc.vertical ? PI / 2 : 0, cc.headerFit);
    }
    // 5. Lines.
    if (page < 0 || page >= int(pages.size())) return g;
    const auto& lines = pages[size_t(page)];
    for (size_t li = 0; li < lines.size(); ++li) {
        const SpecLine& l = lines[li];
        const double yc = headerBottom - SPEC_ROW_H * double(li) - SPEC_ROW_H / 2;
        if (l.kind == SpecLine::Kind::Heading) {
            const std::string title = specSectionTitle(l.section);
            const double cx = colCenter(C_NAME);
            addText(g, Point2(cx, yc), title, SPEC_TEXT_H, CM, 0, COLUMNS[C_NAME].width - 2);
            if (auto w = textWidth(title, SPEC_TEXT_H)) {
                const double ww = std::min(*w, COLUMNS[C_NAME].width - 2);
                const double y = yc - SPEC_TEXT_H / 2 - SPEC_UNDERLINE_GAP;
                addLine(g, Point2(cx - ww / 2, y), Point2(cx + ww / 2, y), Weight::Thin);
            }
            continue;
        }
        if (l.kind != SpecLine::Kind::Item) continue;
        const SpecItem& it = c.items[size_t(l.item)];
        auto left = [&](int col, const std::string& s) { addText(g, Point2(colLeft(col) + 1, yc), s, SPEC_TEXT_H, LM, 0, COLUMNS[col].width - 2); };
        auto center = [&](int col, const std::string& s) { addText(g, Point2(colCenter(col), yc), s, SPEC_TEXT_H, CM, 0, COLUMNS[col].width - 2); };
        if (l.first) {
            center(C_FORMAT, it.format);
            center(C_ZONE, it.zone);
            center(C_POS, it.position);
            left(C_DESIGNATION, it.designation);
        }
        left(C_NAME, l.name);
        if (l.first) center(C_QTY, it.quantity);
        left(C_NOTE, l.note);
    }
    return g;
}

// Parts list on a drawing sheet (drawing.md §6.28, FX14-DRW-003): the columns of form 1 without «Format» and «Zone».
namespace {
constexpr int PARTS_COLUMNS[] = {C_POS, C_DESIGNATION, C_NAME, C_QTY, C_NOTE};
}  // namespace

std::vector<SpecLine> partsListLines(const SpecContent& c) {
    std::vector<SpecLine> out;
    for (const auto& b : specBlocks(c))
        for (const auto& l : b.lines) out.push_back(l);
    if (!out.empty() && out.front().kind == SpecLine::Kind::Blank) out.erase(out.begin());
    while (!out.empty() && out.back().kind == SpecLine::Kind::Blank) out.pop_back();
    return out;
}

SheetSize partsListSize(const SpecContent& c) {
    double w = 0;
    for (int col : PARTS_COLUMNS) w += COLUMNS[col].width;
    return {w, SPEC_HEADER_H + SPEC_ROW_H * double(partsListLines(c).size())};
}

void appendPartsListTable(const SpecContent& c, const Point2& topLeft, const std::string& role, SheetGraphics& g) {
    const size_t first = g.primitives.size();
    const auto lines = partsListLines(c);
    const SheetSize sz = partsListSize(c);
    const double x0 = topLeft.x(), top = topLeft.y(), bottom = top - sz.height, headerBottom = top - SPEC_HEADER_H;
    // Column left edges in the table.
    std::vector<double> lefts;
    double x = x0;
    for (int col : PARTS_COLUMNS) {
        lefts.push_back(x);
        x += COLUMNS[col].width;
    }
    const double right = x;
    // Frame, column separators and the header bottom — main lines; row separators — thin.
    addLine(g, Point2(x0, top), Point2(right, top), Weight::Thick);
    addLine(g, Point2(x0, bottom), Point2(right, bottom), Weight::Thick);
    addLine(g, Point2(x0, top), Point2(x0, bottom), Weight::Thick);
    addLine(g, Point2(right, top), Point2(right, bottom), Weight::Thick);
    for (size_t k = 1; k < lefts.size(); ++k) addLine(g, Point2(lefts[k], top), Point2(lefts[k], bottom), Weight::Thick);
    addLine(g, Point2(x0, headerBottom), Point2(right, headerBottom), Weight::Thick);
    for (size_t k = 1; k < lines.size(); ++k) {
        const double y = headerBottom - SPEC_ROW_H * double(k);
        addLine(g, Point2(x0, y), Point2(right, y), Weight::Thin);
    }
    auto cx = [&](size_t k) { return lefts[k] + COLUMNS[PARTS_COLUMNS[k]].width / 2; };
    const double hy = top - SPEC_HEADER_H / 2;
    for (size_t k = 0; k < lefts.size(); ++k) {
        const Column& cc = COLUMNS[PARTS_COLUMNS[k]];
        addText(g, Point2(cx(k), hy), cc.header, cc.vertical ? 2.5 : 3.5, CM, cc.vertical ? PI / 2 : 0, cc.headerFit);
    }
    for (size_t li = 0; li < lines.size(); ++li) {
        const SpecLine& l = lines[li];
        const double yc = headerBottom - SPEC_ROW_H * double(li) - SPEC_ROW_H / 2;
        if (l.kind == SpecLine::Kind::Heading) {
            const std::string title = specSectionTitle(l.section);
            const double w = COLUMNS[C_NAME].width - 2;
            addText(g, Point2(cx(2), yc), title, SPEC_TEXT_H, CM, 0, w);
            if (auto tw = textWidth(title, SPEC_TEXT_H)) {
                const double ww = std::min(*tw, w);
                const double y = yc - SPEC_TEXT_H / 2 - SPEC_UNDERLINE_GAP;
                addLine(g, Point2(cx(2) - ww / 2, y), Point2(cx(2) + ww / 2, y), Weight::Thin);
            }
            continue;
        }
        if (l.kind != SpecLine::Kind::Item) continue;
        const SpecItem& it = c.items[size_t(l.item)];
        auto left = [&](size_t k, const std::string& t) {
            addText(g, Point2(lefts[k] + 1, yc), t, SPEC_TEXT_H, LM, 0, COLUMNS[PARTS_COLUMNS[k]].width - 2);
        };
        auto center = [&](size_t k, const std::string& t) { addText(g, Point2(cx(k), yc), t, SPEC_TEXT_H, CM, 0, COLUMNS[PARTS_COLUMNS[k]].width - 2); };
        if (l.first) {
            center(0, it.position);
            left(1, it.designation);
        }
        left(2, l.name);
        if (l.first) center(3, it.quantity);
        left(4, l.note);
    }
    for (size_t i = first; i < g.primitives.size(); ++i) g.primitives[i].role = role;
}

std::string specPageId(int page) { return "page" + std::to_string(page + 1); }

int specPageIndex(const std::string& id) {
    if (id.rfind("page", 0) != 0 || id.size() < 5 || id.size() > 12 || id[4] == '0') return -1;
    int n = 0;
    for (size_t i = 4; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return -1;
        n = n * 10 + (id[i] - '0');
    }
    return n - 1;
}

}  // namespace construe::drawing
