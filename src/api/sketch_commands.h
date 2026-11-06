// CoreApi sketch commands (spec/core-api.md §4.6.2) — pure functions over the document: parsing and validation
// of sketchAdd items, sketchUpdate changes, sketchDelete deletion; the document with new sketch data.
// The session adds the sketch context, history, solving of affected sketches and regeneration.
#pragma once

#include "doc/document.h"
#include "sketch/sketch.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::api {

using Json = doc::Json;

// Projection geometry for sketch data sd of feature f in document d (core-api §4.6, Р-018); nothing — unavailable.
using ProjectionResolver =
    std::function<std::optional<sketch::SketchData>(const doc::Document& d, const doc::Feature& f, sketch::SketchData sd)>;

struct SketchAddResult {
    sketch::SketchData data;                      // sketch data with the added items
    std::map<std::string, std::string> response;  // temporary ID → permanent
};
// sketchAdd items for sketch featureId: temporary IDs → permanent (the ID counters of document nd grow),
// refs, refs normalization, fix target, branch (§4.6.2). Errors — API_INVALID_ITEMS, SKETCH_BAD_REFS,
// inferBranch errors.
Result<SketchAddResult> sketchAddItems(doc::Document& nd, const std::string& featureId, const Json& items,
                                       const ProjectionResolver& projections);
// sketchUpdate changes (§4.6.2) over the sketch data of featureId in document d.
Result<sketch::SketchData> sketchApplyUpdate(const doc::Document& d, const std::string& featureId, const Json& changes);
// Deletion of entities (with their points) and constraints (and constraints on the deleted entities).
Result<sketch::SketchData> sketchApplyDelete(sketch::SketchData sd, const std::vector<std::string>& ids);
// Document with new sketch data of featureId; a document validation failure — API_INVALID_ITEMS.
Result<doc::Document> withSketch(const doc::Document& base, const std::string& featureId, const sketch::SketchData& s);

}  // namespace construe::api
