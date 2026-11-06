// Sketch labels (app-ux §6.6): dimension label text, position and rectangle, constraint icons,
// dimension and extension lines. Built by the sketcher for the duration of a computation on top of SketchGeometry.
#pragma once

#include "app/sketch_geometry.h"

#include <QColor>
#include <QLineF>
#include <QPainter>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

// On-screen dimension primitives (app-ux §6.6): extension lines (from the geometry), dimension lines — the line between
// the arrows extended to the label, the radius/diameter line, the angle arc polyline (they grab the dimension,
// FX1-SKI-001) — and arrows (tip, direction).
struct DimPrims {
    std::vector<QLineF> extension, dimension;
    std::vector<std::pair<QPointF, QPointF>> arrows;
};

class SketchAnnotations {
public:
    // isSelected / hover — selection and the object under the cursor: coincidence and "on object" icons are shown
    // only for the points related to them (§6.6). glyphsShown — icon visibility (Shift+C).
    // labelOverride — the dimension whose label is being dragged (§6.6) and its current position in sketch coordinates.
    SketchAnnotations(SketchGeometry g, api::Session& session, std::function<bool(const std::string&)> isSelected, Hit hover,
                      bool glyphsShown, std::optional<std::pair<std::string, Point2>> labelOverride = std::nullopt)
        : g_(std::move(g)), session_(session), isSelected_(std::move(isSelected)), hover_(std::move(hover)), glyphsShown_(glyphsShown),
          labelOverride_(std::move(labelOverride)) {}

    // Dimension text: value in document units without the length unit symbol (angle — with ° or rad), prefixes
    // R / ⌀, "ƒ " for an expression, parentheses for a reference one; value error — "?".
    QString dimText(const sketch::Constraint& c) const;
    // Text without tolerance and the dimension value in mm / rad (none — value error).
    QString dimBaseText(const sketch::Constraint& c) const;
    std::optional<double> dimValue(const sketch::Constraint& c) const;
    // Dimension fit designation (pmi.md §2.2) from the document: record or empty; tolerance suffix for the text (§6.6).
    std::optional<doc::Json> dimTolerance(const sketch::Constraint& c) const;
    QString toleranceSuffix(const sketch::Constraint& c) const;
    // Center of the dimension label: anchor + labelOffset (FX8-SK-004) or near the middle of the geometry.
    QPointF labelPos(const sketch::Constraint& c) const;
    // Dimension anchor in the active state (sketch.md §2.2 «labelOffset»); nothing — no references.
    std::optional<Point2> anchor(const sketch::Constraint& c) const;
    std::optional<QRectF> labelRect(const std::string& constraintId) const;
    std::vector<Glyph> glyphs() const;
    std::optional<QRectF> glyphRect(const std::string& constraintId) const;
    // Dimension primitives with the label at label; references to projections — by their geometry; no geometry — empty.
    std::optional<DimPrims> dimensionPrims(const sketch::Constraint& c, const QPointF& label) const;
    // Extension lines, dimension line and arrows of a dimension with the label at label; false — geometry undefined.
    bool drawDimensionGeometry(QPainter& p, const sketch::Constraint& c, const QPointF& label, const QColor& col) const;
    // Screen distance from s to the dimension's dimension lines (not extension ones); no geometry — 1e18 (FX1-SKI-001).
    double dimensionLineDistance(const sketch::Constraint& c, const QPointF& s) const;

private:
    SketchGeometry g_;  // by value: a value object made of pointers
    api::Session& session_;
    std::function<bool(const std::string&)> isSelected_;
    Hit hover_;
    bool glyphsShown_;
    std::optional<std::pair<std::string, Point2>> labelOverride_;
};

}  // namespace construe::app
