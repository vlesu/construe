// Registry of document format migrations (spec/document.md §4.3, §4.3а; S8-COMPAT-001). Migration k turns a JSON tree
// of schema version k into version k + 1; parse() applies the registry in order from the file's version up to
// CURRENT_SCHEMA_VERSION. Rule: migrations are only appended — an entry is never removed, reordered or changed after
// a release (frozen by the file compatibility corpus, S8-COMPAT-001).
#pragma once

#include "doc/json_text.h"

#include <vector>

namespace construe::doc {

struct Migration {
    int from = 0;              // source schema version; the result is from + 1
    const char* name = "";     // stable name "migrate_<from>_to_<from+1>"
    const char* summary = "";  // what changes (§4.4)
    void (*apply)(Json&) = nullptr;  // pure transformation of the document tree; does not touch "schemaVersion"
};

// All migrations in ascending order of `from`: entry i has from == i (version 0 is the test-only version, §4.3).
const std::vector<Migration>& migrations();

// Applies migrations from the tree's schemaVersion (an integer in [0, CURRENT_SCHEMA_VERSION]) up to the current
// version, setting schemaVersion after each step.
void migrateToCurrent(Json& j);

}  // namespace construe::doc
