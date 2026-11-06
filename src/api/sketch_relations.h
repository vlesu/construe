// Design relations of a recovered sketch (spec/reconstruction.md §5.4, FX10-IO-004): symmetry about the sketch axes and
// equalities of similar elements, offered before the sketch is dimensioned so that fewer dimensions are needed.
#pragma once

#include "api/session.h"
#include "doc/json_text.h"
#include "sketch/data.h"
#include "sketch/sketch.h"

#include <string>
#include <vector>

namespace construe::api::sketchops {

// Candidates in order of preference: every item is a sketchAdd item (a `symmetric` or `equal` constraint); a reference
// "@sketch:x" / "@sketch:y" stands for the projected sketch axis the caller creates. Symmetry about an axis is offered
// only when every main point of the sketch has its mirror image (or lies on the axis); equal — lines of equal length
// that are parallel and are not opposite sides of a quadrilateral, circles and arcs of equal radius (chains to the
// first of each group). tol — coordinate tolerance, mm.
std::vector<doc::Json> designRelations(const sketch::SketchData& d, const sketch::SolvedState& st, double tol);

// Adds the design relations of the open sketch of s (FX10-IO-004): all at once if the sketch stays solvable and not
// over-defined, otherwise one by one keeping those that keep it so; a sketch axis is created only for an accepted
// symmetry. Returns the number of added relations.
int addDesignRelations(Session& s, double tol);

}  // namespace construe::api::sketchops
