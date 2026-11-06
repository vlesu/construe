// Document JSON schema as the source of truth: validation (DOC_SCHEMA) and key order
// of the canonical form (spec/document.md §4.1, §5). The schema is embedded at build time
// from docs/dev/spec/formats/document.schema.json.
#pragma once

#include "doc/json_text.h"

namespace construe::doc {

class Schema {
public:
    // Document schema (embedded).
    static const Schema& document();

    explicit Schema(Json root);

    // Schema validation: the first DOC_SCHEMA error in the order of §5 (path — JSON Pointer).
    Status validate(const Json& instance) const;
    // Validation of a value by the subschema $defs/<defName>.
    Status validateDef(const Json& instance, const std::string& defName) const;

    // Canonical multi-line serialization of the document (§4.1).
    std::string writeCanonical(const Json& instance) const;
    // Single-line serialization of a value by the subschema $defs/<defName> (serializeFeature).
    std::string writeCompact(const Json& instance, const std::string& defName) const;

private:
    Json root_;

    const Json& resolve(const Json& s) const;
    const Json* chooseVariant(const Json& s, const Json& inst) const;
    bool matches(const Json& s, const Json& inst) const;
    void check(const Json& s, const Json& inst, const std::string& path, std::optional<Error>& err) const;
    void write(const Json& s, const Json& inst, int indent, bool inlineMode, const std::string& key,
               std::string& out) const;
};

// Schema from an embedded JSON text (bytes of a schema file built into the program); a text that is not valid JSON —
// an empty schema (accepts any value). Used for the document, library package and solution schemas.
Schema embeddedSchema(const unsigned char* data, size_t size);

// JSON Pointer segment (RFC 6901): ~ → ~0, / → ~1.
std::string pointerSegment(const std::string& key);

}  // namespace construe::doc
