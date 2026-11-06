// Recording a solution in the program (spec/solutions.md §8а): the description built from what the engineer set on an
// example assembly, and saving the package into a library folder with the load checks of §3.5.
#include "solutions/solutions.h"

#include "base/files.h"
#include "base/strings.h"

#include <filesystem>
#include <fstream>

namespace construe::solutions {

namespace fs = std::filesystem;

namespace {

Error toolError(const std::string& output, int line, const char* reason) {
    return Error("SOL_TOOL_TEXT").with("output", output).with("line", double(line)).with("reason", std::string(reason));
}

// Tool text (§8а.2): non-blank lines of two expressions separated by ';'.
Result<std::vector<std::pair<std::string, std::string>>> toolLines(const std::string& text, const std::string& output) {
    std::vector<std::pair<std::string, std::string>> out;
    int line = 0;
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        ++line;
        const std::string l = trim(text.substr(a, b - a));
        a = b + 1;
        if (l.empty()) continue;
        const auto semi = l.find(';');
        if (semi == std::string::npos || l.find(';', semi + 1) != std::string::npos) return toolError(output, line, "fields");
        std::string x = trim(l.substr(0, semi)), y = trim(l.substr(semi + 1));
        if (x.empty() || y.empty()) return toolError(output, line, "fields");
        out.emplace_back(std::move(x), std::move(y));
    }
    return out;
}

}  // namespace

Result<doc::Json> recordDefinition(const Recording& r) {
    using J = doc::Json;
    J j = J::object();
    j["format"] = "construe-solution";
    j["formatVersion"] = SOL_FORMAT_VERSION;
    j["id"] = r.id;
    j["title"] = r.title;
    if (!r.description.empty()) j["description"] = r.description;
    J section = J::array();
    for (const auto& s : r.section) section.push_back(s);
    if (section.empty()) section.push_back("Мои решения");
    j["section"] = section;
    // Inputs; a point lies on the nearest preceding face input.
    J inputs = J::array();
    std::string lastFace, frameFace, frameAxis, framePoint;
    for (const auto& in : r.inputs) {
        J ij = {{"id", in.id}, {"type", in.type}, {"title", in.title}};
        if (in.type == "face") {
            lastFace = in.id;
            if (frameFace.empty()) frameFace = in.id;
        } else if (in.type == "cylinder") {
            if (frameAxis.empty()) frameAxis = in.id;
        } else if (in.type == "point") {
            if (lastFace.empty()) return Error("SOL_RECORD_FRAME").with("name", in.id);
            ij["face"] = lastFace;
            if (framePoint.empty()) framePoint = in.id;
        }
        inputs.push_back(std::move(ij));
    }
    if (frameFace.empty() || (frameAxis.empty() && framePoint.empty())) return Error("SOL_RECORD_FRAME").with("name", std::string());
    j["inputs"] = inputs;
    J frame = {{"face", frameFace}};
    if (!frameAxis.empty()) frame["axis"] = frameAxis;
    if (!framePoint.empty()) frame["point"] = framePoint;
    j["frame"] = frame;
    if (r.probe) j["pack"] = {{"probe", *r.probe}};
    if (!r.tables.empty()) {
        J tables = J::array();
        for (const auto& t : r.tables) {
            J tj = {{"id", t.id}};
            if (!t.builtin.empty()) tj["builtin"] = t.builtin;
            else tj["library"] = t.library;
            tables.push_back(std::move(tj));
        }
        j["tables"] = tables;
    }
    if (!r.rules.empty()) {
        J rules = J::array();
        for (const auto& x : r.rules) {
            if (x.kind == "let") {
                rules.push_back({{"let", x.name}, {"expr", x.expr}});
                continue;
            }
            J rj = {{"select", x.name}, {"table", x.table}, {"where", J::array({J::array({x.column, x.op, x.expr})})}};
            if (!x.order.empty()) rj["order"] = J::array({x.column, x.order});
            rules.push_back(std::move(rj));
        }
        j["rules"] = rules;
    }
    J outputs = J::array();
    for (size_t i = 0; i < r.outputs.size(); ++i) {
        const auto& o = r.outputs[i];
        const std::string id = "o" + std::to_string(i + 1);
        if (o.kind == "part") {
            outputs.push_back({{"id", id}, {"type", "part"}, {"title", o.title}, {"table", o.table}, {"row", o.row}, {"z", o.z},
                               {"direction", o.direction}});
            continue;
        }
        auto lines = toolLines(o.geometry, id);
        if (!lines) return lines.error();
        J tool = J::object();
        if (o.tool == "box") {
            if (lines->size() != 3) return toolError(id, 0, "count");
            J box = J::object();
            const char* axes[] = {"x", "y", "z"};
            for (size_t k = 0; k < 3; ++k) box[axes[k]] = J::array({(*lines)[k].first, (*lines)[k].second});
            tool["box"] = box;
        } else {
            if (lines->size() < 3) return toolError(id, 0, "count");
            J poly = J::array();
            for (const auto& [rr, z] : *lines) poly.push_back(J::array({rr, z}));
            tool["revolve"] = poly;
        }
        outputs.push_back({{"id", id}, {"type", "cut"}, {"title", o.title}, {"parts", o.parts}, {"tool", tool}});
    }
    j["outputs"] = outputs;
    if (auto s = checkDefinition(j); !s) return s.error();
    return j;
}

Result<std::string> saveSolution(const std::string& parent, const std::string& folder, const doc::Json& definition) {
    if (parent.empty()) return Error("API_INVALID_ARGUMENT").with("name", std::string("parent"));
    if (folder.empty() || folder == "." || folder == ".." || folder.find('/') != std::string::npos ||
        folder.find('\\') != std::string::npos)
        return Error("API_INVALID_ARGUMENT").with("name", std::string("folder"));
    const fs::path dir = fs::u8path(parent) / fs::u8path(folder);
    const std::string dirText = u8str(dir);
    std::error_code ec;
    if (fs::exists(dir, ec)) return Error("LIB_FOLDER_EXISTS").with("dir", dirText);
    fs::create_directories(dir, ec);
    if (ec) return Error("IO_WRITE").with("path", dirText);
    bool written = false;
    {
        std::ofstream f(dir / SOL_DESCRIPTION_FILE, std::ios::binary | std::ios::trunc);
        f << definition.dump(2) << '\n';
        written = bool(f);
    }  // the file is closed before a possible removal of the folder
    if (!written) {
        fs::remove_all(dir, ec);
        return Error("IO_WRITE").with("path", dirText);
    }
    auto s = loadSolution(dirText);
    if (!s) {
        fs::remove_all(dir, ec);
        return s.error();
    }
    return s->dir;
}

}  // namespace construe::solutions
