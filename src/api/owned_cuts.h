// Owned cuts of connections "in parts" (spec/solutions.md §7.10, S6-SOL-010): bringing the connectionCut features of a
// part document to the reports of the connections of an owner assembly. Internal header of the api module.
#pragma once

#include "doc/document.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace construe::api {

// State of a connection for materialization (solutions.md §7.10 item 4).
struct OwnedConnection {
    enum class Kind {
        Report,    // a report in the "parts" mode: owned features follow its "owned" entries
        Keep,      // the connection has an error: its owned features stay as they are
        Suppress,  // suppressed or rolled back: its owned features stay and are suppressed
        Remove,    // absent or in the "assembly" mode: its owned features are removed
    };
    Kind kind = Kind::Remove;
    std::string name;   // connection name (owner.name, the feature name)
    doc::Json report;   // Kind::Report
};

struct OwnedScope {
    std::string assembly;   // owner assembly element ID
    std::string ownerFile;  // owner.file to write ("" — the part is in the assembly's document)
    std::string partKey;    // the part document as in report "file" entries ("" — the assembly's document)
    // Ownership of a feature of the part document by the assembly (regen::ownedByAssembly with the document paths).
    std::function<bool(const doc::Feature&)> owned;
    // Connections in the assembly's feature order; owned features of unlisted connections — removed (all = true) or
    // untouched (all = false, a command over listed connections only).
    std::vector<std::pair<std::string, OwnedConnection>> connections;
    bool all = true;
    // The name of a new cut feature from the output title (library.md §17.4: in the interface language at creation);
    // empty — the title as is. An existing feature keeps the name it was created with (only the connection part follows).
    std::function<std::string(const std::string&)> title;
};

// Brings the owned features of document pd to the scope (§7.10 item 4); true — pd changed.
bool syncOwnedCuts(doc::Document& pd, const OwnedScope& scope);

// Whether two JSON values are equal with numbers within tol (solutions.md §9 SOL_SYNC_TOL).
bool jsonNear(const doc::Json& a, const doc::Json& b, double tol);

}  // namespace construe::api
