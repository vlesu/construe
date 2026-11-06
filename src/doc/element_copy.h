// Copying objects between documents with ID renumbering (spec/assembly.md §5б.1, FX7-ASM-008…010).
#pragma once

#include "doc/document.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::doc {

struct CopyOptions {
    std::vector<std::string> roots;   // source element IDs
    bool copyDependencies = true;     // deps = copy: same-document dependencies are copied too; otherwise linked
    bool sameDocument = false;        // the source is the target's own file
    std::string sourceLink;           // source file path relative to the target directory ("" — none)
    // Source-relative file reference → target-relative; "" — the reference points to the target file itself;
    // nullopt — the target has no path (API_NO_PATH).
    std::function<std::optional<std::string>(const std::string&)> rebase;
    bool dedupeRoots = false;         // a root may coincide with an existing object (§5б.1 item 3)
    std::vector<std::string> usedIds; // source variable/table IDs used outside the copied objects (instance overrides)
};

struct CopyResult {
    Document document;                          // the new target
    std::vector<std::string> roots;             // root IDs in the target
    std::map<std::string, std::string> ids;     // source ID → target ID
    std::map<std::string, std::string> names;   // renamed variables: old name → new name
};

Result<CopyResult> copyElements(const Document& target, const Document& source, const CopyOptions& options);

// Rewrite a reference string of the source owned by element owner with the copy's map (§5б.1 item 7).
std::string rewriteReference(const std::string& s, const Document& source, const std::string& owner, const CopyResult& r);

// Rewrite strings "<prefix>/<rest>" of a description (not free text, §5б.1 item 7) as "<prefix>/<fn(rest)>".
void rewritePrefixed(Json& j, const std::string& prefix, const std::function<std::string(const std::string&)>& fn);

// Direct same-document dependencies of an element: assembly instances and base views without file/library (§5б.1).
std::vector<std::string> elementDependencies(const Document& d, const std::string& elementId);

// All same-document dependencies (transitively), without the element itself.
std::vector<std::string> allDependencies(const Document& d, const std::string& elementId);

}  // namespace construe::doc
