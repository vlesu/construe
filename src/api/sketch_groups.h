// Linked sketch groups in CoreApi (spec/sketch.md §2.6, spec/core-api.md §4.6.2 "Linked groups"):
// validation of sources and parameters, evaluation of parameter expressions, type generators, rebuild, reconciliation,
// deletion. Pure functions over sketch data; the session adds history and solving.
#pragma once

#include "doc/document.h"
#include "expr/expr.h"
#include "sketch/groups.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace construe::api {

using Json = doc::Json;
using Allocate = std::function<std::string(bool entity)>;

// Group parameters: JSON (schema structure) ↔ model.
std::map<std::string, sketch::GroupParam> groupParamsFromJson(const Json& params);
Json groupParamsToJson(const std::map<std::string, sketch::GroupParam>& params);

// Generator of a group of type type (sketch.md §2.6.2): validation of sources and parameters, evaluation of expressions
// with the document variables env (S4-SKT-006; empty — no variables); selfId — the group ID (its members cannot be
// sources), old — the previous state or nullptr.
// Errors: API_INVALID_ARGUMENT (name — the field; "type"), API_INVALID_ITEMS (detail "sources").
Result<std::vector<sketch::GroupItem>> generateGroup(const sketch::SketchData& s, const std::string& type,
                                                     const std::vector<std::string>& sources, const Json& params,
                                                     const expr::DocumentUnits& units, const expr::Env& env,
                                                     const std::string& selfId, const sketch::Group* old);
// Positive form of the offset distance text (sketch.md §2.6.5): the text itself for value > 0; otherwise the text
// without the leading minus when the rest evaluates to −value, else "-(text)".
std::string offsetValueText(const std::string& text, double value, const expr::DocumentUnits& units, const expr::Env& env);
// Rebuild of group groupId (absent from the data — creation) with type type, sources and parameters (§2.6.2).
Result<sketch::SketchData> rebuildGroup(const sketch::SketchData& s, const std::string& groupId, const std::string& type,
                                        const std::vector<std::string>& sources, const Json& params,
                                        const expr::DocumentUnits& units, const expr::Env& env, const Allocate& allocate);
// Reconciliation of groups after entity deletion (§2.6.2 "Reconciliation"), until stable.
sketch::SketchData reconcileGroups(sketch::SketchData s, const expr::DocumentUnits& units, const expr::Env& env,
                                   const Allocate& allocate);
// Rebuild by variables (§2.6.2 "Rebuild by variables", S4-SKT-006): every group of every sketch of the document whose
// parameter expressions reference a name is rebuilt with the document variables; a failed rebuild leaves the group
// unchanged. Returns the new document.
doc::Document rebuildVariableGroups(doc::Document d);
// Offset groups of an older program (FX12-SK-001, sketch.md §2.6.5 «Документы прежних версий»): every offset group whose
// member roles differ from those its sources and parameters generate now is rebuilt; one that cannot be generated (no
// projection geometry yet) stays as it is. Returns the new document.
doc::Document migrateOffsetGroups(doc::Document d);
// IDs for sketchDelete (§2.6.2 "Deletion"): IDs of groups and group members are replaced by deleting whole groups;
// returns the data without these groups and the remaining IDs for ordinary deletion.
std::pair<sketch::SketchData, std::vector<std::string>> deleteGroupsOf(sketch::SketchData s, const std::vector<std::string>& ids);

}  // namespace construe::api
