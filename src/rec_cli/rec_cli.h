// Console import and history reconstruction (FX10-REC-001, spec/reconstruction.md §12): a file goes in, the reconstructed
// document and a machine-readable log come out. No Qt and no user interface: it rebuilds with the rec module alone.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace construe::rec_cli {

struct Options {
    std::string input;             // .step/.stp, or .construe: the bodies of the document are reconstructed in place
    std::string out;               // reconstructed document (.construe); empty — not written
    std::string log;               // JSON log; empty — not written
    std::string body;              // .construe: only the body element with this ID; empty — all body elements
    double roundTolerance = 0.01;  // the program's default rounding of dimension values, mm
    double healTolerance = 0.05;   // shape healing tolerance, mm
    double timeLimit = 0;          // s of wall time per file: the reconstruction is cancelled then; 0 — no limit
};

// Parses the command line (without the program name); false and the error text on a wrong call.
bool parseArgs(const std::vector<std::string>& args, Options& o, std::string& error);
// Command line help.
std::string usage();

// Runs the import and the reconstruction. The log (schema — reconstruction.md §12): file, status (ok | error | timeout),
// error, seconds, bodies: [{ element, level, seconds, parts: [{ element, level, instances, base, notes, features, sketches }] }];
// a part's features — types in order (without sketches), sketches — { lines, arcs, circles, splines, points, texts,
// constraints } each.
nlohmann::json run(const Options& o);

// One line per body for the console.
std::string summary(const nlohmann::json& log);

}  // namespace construe::rec_cli
