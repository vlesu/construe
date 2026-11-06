// CoreApi command errors (spec/core-api.md §7): shared constructors for the session and pure command functions.
#pragma once

#include "base/error.h"
#include "doc/json_text.h"

#include <string>

namespace construe::api {

inline Error apiError(const char* code) { return Error(code); }
inline Error notFound(const std::string& id) { return Error("API_NOT_FOUND").with("id", id); }
inline Error badArg(const std::string& name) { return Error("API_INVALID_ARGUMENT").with("name", name); }
inline Error invalidItems(const std::string& id, const std::string& detail) {
    return Error("API_INVALID_ITEMS").with("id", id).with("detail", detail);
}
inline Error invalidDefinition(const std::string& detail, const std::string& path = {}) {
    Error e("API_INVALID_DEFINITION");
    e.with("detail", detail);
    if (!path.empty()) e.at(path);
    return e;
}

// Merge-patch of a definition into an object (core-api.md §7а): a key with null removes the field, otherwise the value
// replaces it whole (no recursion into nested objects).
inline void mergePatch(doc::Json& target, const doc::Json& patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().is_null()) target.erase(it.key());
        else target[it.key()] = it.value();
    }
}

// Path of a document error relative to the object at prefix (core-api.md §7а): the remainder after prefix if the error
// path starts with it, otherwise empty (the error is outside the object).
inline std::string schemaPathIn(const Error& e, const std::string& prefix) {
    const std::string p = e.path.value_or("");
    return p.rfind(prefix, 0) == 0 ? p.substr(prefix.size()) : std::string();
}

// Document schema error of an object at prefix → API_INVALID_DEFINITION (core-api.md §7а): detail — the schema detail
// or "schema", path — schemaPathIn (none if empty). Other errors are returned unchanged.
inline Error schemaErrorAt(const Error& e, const std::string& prefix) {
    if (e.code != "DOC_SCHEMA") return e;
    const std::string* detail = e.stringParam("detail");
    return invalidDefinition(detail ? *detail : std::string("schema"), schemaPathIn(e, prefix));
}

}  // namespace construe::api
