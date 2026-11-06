// CoreApi feature commands (spec/core-api.md §4.4) - pure functions over a document: building a feature JSON from
// a definition, reference checks, insertion/replacement in the document. The session adds history and regeneration.
#pragma once

#include "doc/document.h"

#include <string>

namespace construe::api {

using Json = doc::Json;

// Feature JSON from a definition (addFeature/updateFeature input): an object with type, without id/name/suppressed.
Result<Json> featureJsonFromDefinition(const Json& def, const std::string& id, const std::string& name, bool suppressed);
doc::FeatureType featureTypeFromName(const std::string& type);
// Definition references (profile sketch, axis segment): exist and are above position position.
Status checkDefinitionRefs(const doc::Document& d, const Json& def, const std::string& featureId, int position);
// Insertion position of a new feature - right after the rollback marker.
int insertionIndex(const doc::Element& el);
// Document with feature fj inserted (replace = false) or replacing the feature (true) at position index; schema
// errors - API_INVALID_DEFINITION with a path inside the feature, a dangling reference - API_ORDER_VIOLATION.
Result<doc::Document> applyFeatureJson(const doc::Document& base, const std::string& id, Json fj, int index, bool replace);
// Document with a replaced feature (by ID).
doc::Document replaceFeature(doc::Document d, const doc::Feature& f);

}  // namespace construe::api
