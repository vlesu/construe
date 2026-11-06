// Sub-shape numbering of a BRep text (spec/fcstd.md §4.3): only the Locations and TShapes sections are read —
// location matrices, record types, the degenerated flag of edges and the sub-shape references; geometry is not
// parsed (the kernel reads the shape).
#include "fcstd/brep_index.h"

#include "base/strings.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <utility>

namespace construe::fcstd {

namespace {

struct Ref {
    int index = 0;     // TShape index (1 = last record)
    int location = 0;  // location index, 0 — identity
};

struct Record {
    std::string type;  // Ve, Ed, Wi, Fa, Sh, So, CS, Co
    bool degenerated = false;
    std::vector<Ref> children;
};

bool isFlags(const std::string& l) {
    if (l.size() != 7) return false;
    for (char c : l)
        if (c != '0' && c != '1') return false;
    return true;
}

// "+12" / "-3" → index; false — not a reference.
bool parseRef(const std::string& tok, int& index) {
    if (tok.size() < 2 || (tok[0] != '+' && tok[0] != '-' && tok[0] != 'i' && tok[0] != 'e')) return false;
    char* end = nullptr;
    const long v = std::strtol(tok.c_str() + 1, &end, 10);
    if (*end != '\0' || v <= 0) return false;
    index = int(v);
    return true;
}

}  // namespace

namespace {

using Mat = std::array<double, 12>;  // 3×4 row-major: rotation | translation

Mat identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }

Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a[size_t(i * 4 + 3)] : 0.0;
            for (int k = 0; k < 3; ++k) v += a[size_t(i * 4 + k)] * b[size_t(k * 4 + j)];
            r[size_t(i * 4 + j)] = v;
        }
    }
    return r;
}

// Inverse of a rigid motion (orthonormal rotation; locations in BRep files are rigid).
Mat inverse(const Mat& a) {
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[size_t(i * 4 + j)] = a[size_t(j * 4 + i)];
    for (int i = 0; i < 3; ++i) {
        double v = 0;
        for (int k = 0; k < 3; ++k) v -= r[size_t(i * 4 + k)] * a[size_t(k * 4 + 3)];
        r[size_t(i * 4 + 3)] = v;
    }
    return r;
}

Mat power(const Mat& a, int p) {
    Mat base = p < 0 ? inverse(a) : a, r = identity();
    for (int k = 0; k < std::abs(p); ++k) r = mul(r, base);
    return r;
}

// Identity key of a location: the matrix rounded to 1e-9 (sub-shapes equal by TShape and location).
std::vector<long long> key(const Mat& m) {
    std::vector<long long> out;
    for (double v : m) out.push_back(static_cast<long long>(std::llround(v * 1e9)));
    return out;
}

}  // namespace

std::optional<BrepIndex> brepIndex(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    // Locations: "1" + 3 rows of 4 numbers (elementary) or "2 i p j p … 0": L = Lj^q · Li^p (later factors on the left).
    std::vector<Mat> locations;
    int count = -1;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("Locations", 0) == 0) {
            const int n = std::atoi(line.c_str() + 9);
            for (int i = 0; i < n; ++i) {
                int type = 0;
                if (!(in >> type)) return std::nullopt;
                if (type == 1) {
                    Mat m{};
                    for (auto& v : m)
                        if (!(in >> v)) return std::nullopt;
                    locations.push_back(m);
                } else if (type == 2) {
                    Mat m = identity();
                    for (;;) {
                        int idx = 0, pw = 0;
                        if (!(in >> idx)) return std::nullopt;
                        if (idx == 0) break;
                        if (!(in >> pw) || idx < 1 || idx > static_cast<int>(locations.size())) return std::nullopt;
                        m = mul(power(locations[size_t(idx - 1)], pw), m);  // each factor multiplies from the left
                    }
                    locations.push_back(m);
                } else {
                    return std::nullopt;
                }
            }
            continue;
        }
        if (line.rfind("TShapes", 0) == 0) {
            count = std::atoi(line.c_str() + 7);
            break;
        }
    }
    if (count <= 0) return std::nullopt;
    auto locationOf = [&](int index) -> std::optional<Mat> {
        if (index == 0) return identity();
        if (index < 0 || index > static_cast<int>(locations.size())) return std::nullopt;
        return locations[size_t(index - 1)];
    };
    static const std::set<std::string> TYPES = {"Ve", "Ed", "Wi", "Fa", "Sh", "So", "CS", "Co"};
    std::vector<Record> written;  // in file order: record k has index count − k
    while (static_cast<int>(written.size()) < count && std::getline(in, line)) {
        line = trim(line);
        if (!TYPES.count(line)) return std::nullopt;
        Record r;
        r.type = line;
        bool first = true;
        // Geometry lines up to the flags line.
        for (;;) {
            if (!std::getline(in, line)) return std::nullopt;
            line = trim(line);
            if (isFlags(line)) break;
            if (first && r.type == "Ed" && !line.empty()) {
                std::istringstream ls(line);
                double tol = 0;
                int sameParam = 0, sameRange = 0, degenerated = 0;
                if (ls >> tol >> sameParam >> sameRange >> degenerated) r.degenerated = degenerated != 0;
            }
            if (!line.empty()) first = false;
        }
        // Sub-shapes: "<orientation><index> <location>" pairs up to "*" (may span lines).
        bool done = false;
        while (!done) {
            if (!std::getline(in, line)) return std::nullopt;
            std::istringstream ls(line);
            std::string tok;
            while (ls >> tok) {
                if (tok == "*") {
                    done = true;
                    break;
                }
                Ref ref;
                std::string loc;
                if (!parseRef(tok, ref.index) || !(ls >> loc)) return std::nullopt;
                ref.location = std::atoi(loc.c_str());
                r.children.push_back(ref);
            }
        }
        written.push_back(std::move(r));
    }
    if (static_cast<int>(written.size()) != count) return std::nullopt;
    // Root reference follows the records.
    int root = 0, rootLoc = 0;
    {
        std::string tok, loc;
        while (in >> tok)
            if (parseRef(tok, root)) break;
        if (root <= 0 || root > count || !(in >> loc)) return std::nullopt;
        rootLoc = std::atoi(loc.c_str());
    }
    auto rec = [&](int index) -> const Record& { return written[static_cast<size_t>(count - index)]; };
    for (const auto& r : written)
        for (const auto& c : r.children)
            if (c.index <= 0 || c.index > count || !locationOf(c.location)) return std::nullopt;
    if (!locationOf(rootLoc)) return std::nullopt;

    // Depth-first traversal from the root; a sub-shape is identified by its TShape and its composed location
    // (orientation does not matter), as an indexed map of shapes does (spec/fcstd.md §4.3).
    BrepIndex out;
    std::set<std::pair<int, std::vector<long long>>> faces, edges;
    int kernelEdge = 0;
    std::function<void(int, const Mat&)> visit = [&](int index, const Mat& loc) {
        const Record& r = rec(index);
        if (r.type == "Fa" && faces.insert({index, key(loc)}).second) ++out.faces;
        if (r.type == "Ed") {
            if (edges.insert({index, key(loc)}).second) out.edgeKernel.push_back(r.degenerated ? -1 : kernelEdge++);
            return;  // vertices of an edge are of no interest
        }
        if (r.type == "Ve") return;
        for (const auto& c : r.children) visit(c.index, mul(loc, *locationOf(c.location)));
    };
    visit(root, *locationOf(rootLoc));
    return out;
}

}  // namespace construe::fcstd
