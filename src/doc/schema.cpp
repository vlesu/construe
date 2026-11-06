#include "doc/schema.h"

#include "base/number_format.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <vector>

namespace construe::doc {

namespace {

const unsigned char kSchemaData[] = {
#include "doc/schema_data.inc"
};

// A compiled "pattern" of the schema (FX13-PERF-001): compiling a std::regex per checked string made every
// serialization of a document (each command compares the serialized documents) slow; patterns are few and fixed.
const std::regex& compiledPattern(const std::string& pattern) {
    static std::mutex mu;
    static std::map<std::string, std::regex> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(pattern);
    if (it == cache.end()) it = cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript)).first;
    return it->second;
}

size_t codePoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool jsonEqual(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) return a.get<double>() == b.get<double>();
    return a == b;
}

Error schemaError(const std::string& path, const std::string& detail, const std::string& name = {}) {
    Error e("DOC_SCHEMA");
    e.at(path.empty() ? "" : path);
    e.with("detail", detail);
    if (!name.empty()) e.with("name", name);
    return e;
}

bool typeOk(const std::string& t, const Json& v) {
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "null") return v.is_null();
    if (t == "number") return v.is_number();
    if (t == "integer") {
        if (!v.is_number()) return false;
        const double d = v.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
    }
    return false;
}

const std::set<std::string> kInlineArrays = {"variables", "entities", "constraints", "groups", "variants", "variantRows", "overrides", "placements", "form"};

}  // namespace

std::string pointerSegment(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out.push_back(c);
    }
    return out;
}

Schema embeddedSchema(const unsigned char* data, size_t size) {
    auto j = parseJsonStrict(std::string(reinterpret_cast<const char*>(data), size));
    return Schema(j.ok() ? *j : Json::object());
}

const Schema& Schema::document() {
    static const Schema s = embeddedSchema(kSchemaData, sizeof(kSchemaData));
    return s;
}

Schema::Schema(Json root) : root_(std::move(root)) {}

const Json& Schema::resolve(const Json& s) const {
    const Json* cur = &s;
    while (cur->is_object() && cur->contains("$ref")) {
        const std::string ref = (*cur)["$ref"];
        if (ref == "#") {  // the root schema (embedded document, document.md §2.13)
            cur = &root_;
            continue;
        }
        const std::string prefix = "#/$defs/";
        cur = &root_["$defs"][ref.substr(prefix.size())];
    }
    return *cur;
}

bool Schema::matches(const Json& s, const Json& inst) const {
    std::optional<Error> err;
    check(s, inst, "", err);
    return !err;
}

const Json* Schema::chooseVariant(const Json& s, const Json& inst) const {
    const Json& variants = s["oneOf"];
    if (inst.is_object()) {
        // Discriminator — const/enum of field key in a variant; a nested oneOf (feature with modes, document.md §2.10а)
        // matches if at least one of its variants matches.
        auto keyMatches = [&](auto&& self, const Json& v, const std::string& key, bool& hasKey) -> bool {
            if (v.contains("oneOf")) {
                bool any = false;
                for (const auto& w0 : v["oneOf"]) any = self(self, resolve(w0), key, hasKey) || any;
                return any;
            }
            if (!v.contains("properties") || !v["properties"].contains(key)) return false;
            const Json& t = v["properties"][key];
            if (!t.contains("const") && !t.contains("enum")) return false;
            hasKey = true;
            if (!inst.contains(key)) return false;
            if (t.contains("const")) return jsonEqual(t["const"], inst[key]);
            for (const auto& e : t["enum"])
                if (jsonEqual(e, inst[key])) return true;
            return false;
        };
        std::vector<const Json*> cands;
        for (const auto& v0 : variants) cands.push_back(&resolve(v0));
        bool filtered = false;
        for (const char* key : {"type", "mode", "kind"}) {
            bool discriminated = false;
            std::vector<const Json*> hits;
            for (const Json* v : cands)
                if (keyMatches(keyMatches, *v, key, discriminated)) hits.push_back(v);
            if (!discriminated) continue;
            if (!inst.contains(key)) return nullptr;
            cands = hits;
            filtered = true;
            if (cands.size() <= 1) break;
        }
        if (filtered) return cands.empty() ? nullptr : cands.front();
        for (const auto& v0 : variants) {
            const Json& v = resolve(v0);
            if (v.contains("required") && v["required"].size() == 1 && inst.contains(v["required"][0].get<std::string>()))
                return &v;
        }
        // Objects without a discriminator (branches): the first matching variant.
        for (const auto& v0 : variants)
            if (matches(v0, inst)) return &resolve(v0);
        return nullptr;
    }
    for (const auto& v0 : variants)
        if (matches(v0, inst)) return &resolve(v0);
    return nullptr;
}

void Schema::check(const Json& s0, const Json& inst, const std::string& path, std::optional<Error>& err) const {
    if (err) return;
    const Json& s = resolve(s0);
    if (s.contains("oneOf")) {
        const Json* v = chooseVariant(s, inst);
        if (!v) {
            if (inst.is_object() && !inst.contains("type")) {
                bool needsType = false;
                for (const auto& v0 : s["oneOf"]) {
                    const Json& vv = resolve(v0);
                    needsType = needsType || (vv.contains("properties") && vv["properties"].contains("type"));
                }
                if (needsType) {
                    err = schemaError(path, "missing discriminator", "type");
                    return;
                }
            }
            if (inst.is_object() && inst.contains("type")) {
                err = schemaError(path + "/type", "unknown variant");
                return;
            }
            // Not an object: the only variant of the same JSON type is checked entirely, so that
            // the error points to the nested location (e.g. /profiles/0) rather than the whole array.
            auto jsonType = [](const Json& j) -> std::string {
                if (j.is_array()) return "array";
                if (j.is_string()) return "string";
                if (j.is_boolean()) return "boolean";
                if (j.is_number()) return "number";
                if (j.is_null()) return "null";
                return "object";
            };
            const Json* sameType = nullptr;
            int count = 0;
            for (const auto& v0 : s["oneOf"]) {
                const Json& vv = resolve(v0);
                std::string t;
                if (vv.contains("type") && vv["type"].is_string()) t = vv["type"].get<std::string>();
                else if (vv.contains("const")) t = jsonType(vv["const"]);
                if (t == "integer") t = "number";
                if (t == jsonType(inst)) {
                    sameType = &vv;
                    ++count;
                }
            }
            if (count == 1 && !inst.is_object()) {
                check(*sameType, inst, path, err);
                if (err) return;
            }
            err = schemaError(path, "no matching variant");
            return;
        }
        check(*v, inst, path, err);
        return;
    }
    if (s.contains("allOf"))
        for (const auto& sub : s["allOf"]) check(sub, inst, path, err);
    if (err) return;
    if (s.contains("const") && !jsonEqual(s["const"], inst)) {
        err = schemaError(path, "value must be " + s["const"].dump());
        return;
    }
    if (s.contains("enum")) {
        bool found = false;
        for (const auto& e : s["enum"]) found = found || jsonEqual(e, inst);
        if (!found) {
            err = schemaError(path, "value not in enum");
            return;
        }
    }
    if (s.contains("type") && !typeOk(s["type"].get<std::string>(), inst)) {
        err = schemaError(path, "expected " + s["type"].get<std::string>());
        return;
    }
    if (inst.is_string()) {
        const std::string str = inst;
        if (s.contains("minLength") && codePoints(str) < s["minLength"].get<size_t>()) {
            err = schemaError(path, "string too short");
            return;
        }
        if (s.contains("maxLength") && codePoints(str) > s["maxLength"].get<size_t>()) {
            err = schemaError(path, "string too long");
            return;
        }
        if (s.contains("pattern")) {
            if (!std::regex_search(str, compiledPattern(s["pattern"].get<std::string>()))) {
                err = schemaError(path, "pattern mismatch");
                return;
            }
        }
    }
    if (inst.is_number()) {
        const double d = inst.get<double>();
        if (s.contains("minimum") && d < s["minimum"].get<double>()) err = schemaError(path, "below minimum");
        else if (s.contains("maximum") && d > s["maximum"].get<double>()) err = schemaError(path, "above maximum");
        else if (s.contains("exclusiveMinimum") && d <= s["exclusiveMinimum"].get<double>())
            err = schemaError(path, "not above exclusive minimum");
        if (err) return;
    }
    if (inst.is_object() && (s.contains("properties") || s.contains("required"))) {
        const Json empty = Json::object();
        const Json& props = s.contains("properties") ? s["properties"] : empty;
        std::set<std::string> required;
        if (s.contains("required"))
            for (const auto& r : s["required"]) required.insert(r.get<std::string>());
        for (auto it = props.begin(); it != props.end(); ++it)
            if (required.count(it.key()) && !inst.contains(it.key())) {
                err = schemaError(path, "missing required key", it.key());
                return;
            }
        if (s.contains("additionalProperties") && s["additionalProperties"] == false) {
            std::vector<std::string> unknown;
            for (auto it = inst.begin(); it != inst.end(); ++it)
                if (!props.contains(it.key())) unknown.push_back(it.key());
            if (!unknown.empty()) {
                std::sort(unknown.begin(), unknown.end());
                err = schemaError(path, "unknown key", unknown.front());
                return;
            }
        }
        for (auto it = props.begin(); it != props.end(); ++it)
            if (inst.contains(it.key())) {
                check(it.value(), inst[it.key()], path + "/" + pointerSegment(it.key()), err);
                if (err) return;
            }
    }
    if (inst.is_array()) {
        if (s.contains("minItems") && inst.size() < s["minItems"].get<size_t>()) {
            err = schemaError(path, "too few items");
            return;
        }
        if (s.contains("maxItems") && inst.size() > s["maxItems"].get<size_t>()) {
            err = schemaError(path, "too many items");
            return;
        }
        if (s.contains("uniqueItems") && s["uniqueItems"] == true)
            for (size_t i = 1; i < inst.size(); ++i)
                for (size_t k = 0; k < i; ++k)
                    if (jsonEqual(inst[k], inst[i])) {
                        err = schemaError(path + "/" + std::to_string(i), "duplicate item");
                        return;
                    }
        if (s.contains("items"))
            for (size_t i = 0; i < inst.size(); ++i) {
                check(s["items"], inst[i], path + "/" + std::to_string(i), err);
                if (err) return;
            }
    }
}

Status Schema::validate(const Json& instance) const {
    std::optional<Error> err;
    check(root_, instance, "", err);
    if (err) return *err;
    return okStatus();
}

Status Schema::validateDef(const Json& instance, const std::string& defName) const {
    std::optional<Error> err;
    check(root_["$defs"][defName], instance, "", err);
    if (err) return *err;
    return okStatus();
}

void Schema::write(const Json& s0, const Json& inst, int indent, bool inlineMode, const std::string& key,
                   std::string& out) const {
    const Json* s = &resolve(s0);
    while (s->contains("oneOf")) {  // nested oneOf (feature with modes) — down to the variant with properties
        const Json* v = chooseVariant(*s, inst);
        if (!v || v == s) break;
        s = v;
    }
    const std::string pad(static_cast<size_t>(indent), ' ');
    const std::string pad2(static_cast<size_t>(indent + 2), ' ');
    if (inst.is_object()) {
        if (inst.empty()) {
            out += "{}";
            return;
        }
        std::vector<std::string> keys;
        if (s->contains("properties"))
            for (auto it = (*s)["properties"].begin(); it != (*s)["properties"].end(); ++it)
                if (inst.contains(it.key())) keys.push_back(it.key());
        for (auto it = inst.begin(); it != inst.end(); ++it)
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) keys.push_back(it.key());
        const Json emptyProps = Json::object();
        const Json& props = s->contains("properties") ? (*s)["properties"] : emptyProps;
        out += inlineMode ? "{" : "{\n";
        for (size_t i = 0; i < keys.size(); ++i) {
            const Json& sub = props.contains(keys[i]) ? props[keys[i]] : emptyProps;
            if (!inlineMode) out += pad2;
            out += jsonString(keys[i]) + ": ";
            write(sub, inst[keys[i]], indent + 2, inlineMode, keys[i], out);
            if (i + 1 < keys.size()) out += inlineMode ? ", " : ",";
            if (!inlineMode) out += "\n";
        }
        if (!inlineMode) out += pad;
        out += "}";
        return;
    }
    if (inst.is_array()) {
        if (inst.empty()) {
            out += "[]";
            return;
        }
        const Json emptyItems = Json::object();
        const Json& items = s->contains("items") ? (*s)["items"] : emptyItems;
        if (inlineMode) {
            out += "[";
            for (size_t i = 0; i < inst.size(); ++i) {
                write(items, inst[i], indent, true, {}, out);
                if (i + 1 < inst.size()) out += ", ";
            }
            out += "]";
            return;
        }
        const bool elementsInline = kInlineArrays.count(key) != 0;
        out += "[\n";
        for (size_t i = 0; i < inst.size(); ++i) {
            out += pad2;
            write(items, inst[i], indent + 2, elementsInline, {}, out);
            if (i + 1 < inst.size()) out += ",";
            out += "\n";
        }
        out += pad + "]";
        return;
    }
    if (inst.is_string()) {
        out += jsonString(inst.get<std::string>());
    } else if (inst.is_boolean()) {
        out += inst.get<bool>() ? "true" : "false";
    } else if (inst.is_null()) {
        out += "null";
    } else if (inst.is_number()) {
        out += canonicalNumber(inst.get<double>());
    }
}

std::string Schema::writeCanonical(const Json& instance) const {
    std::string out;
    write(root_, instance, 0, false, {}, out);
    out += "\n";
    return out;
}

std::string Schema::writeCompact(const Json& instance, const std::string& defName) const {
    std::string out;
    write(root_["$defs"][defName], instance, 0, true, {}, out);
    return out;
}

}  // namespace construe::doc
