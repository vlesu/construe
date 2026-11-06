// 3D view scene builder (app-ux §3.4, §5.4, §6.1, §11.6): which bodies, sketch regions and planes to show
// and at what precision — by window mode, feature being edited, preview and view scale. No widgets:
// from api::Session and a request description; MainWindow passes the result to the view.
#pragma once

#include "api/session.h"
#include "app/viewport.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::app {

struct SceneRequest {
    enum class Mode { Part, Sketch, FeatureEdit };  // other window modes show the model as Part
    Mode mode = Mode::Part;
    std::string sketch;                    // Sketch — the open sketch
    std::optional<std::string> editing;    // FeatureEdit — the feature being edited (none — a new one)
    std::string operation;                 // FeatureEdit — the panel operation: new | add | cut | intersect
    std::optional<std::string> preview;    // FeatureEdit — the current preview
    bool profilesPanel = false;            // a panel with a "Profiles" field is open (extrude, revolve)
    std::string panelSketch;               // the panel's sketch
    double mmPerPx = 1;                    // view scale (tessellation precision, §11.6)
    uint64_t revision = 0;                 // result revision (body buffer keys)
    std::vector<api::Json> sections;       // active sections in every mode (§3.4, S2-VIEW-002, S7F-VIEW-001); empty — bodies are whole
    std::map<std::string, regen::Motion3> dragMotions;  // assembly: temporary placements of top-level instances (§8а)
    std::set<std::string> highlight;                    // assembly: instance paths highlighted in COLOR_INTERFERENCE (§8а)
    bool partPlanes = false;  // assembly, mate mode: planes of the parts of instances for the tree eye (FX10-ASM-001)
    std::vector<std::string> shownBases;   // Part: "Import" features with an open eye — base bodies on top (§3.4, S4-REC-009)
    bool unfolded = false;                 // Part: the sheet part is shown as its flat pattern (§7.4, S5-FLAT-001)
    std::optional<std::vector<std::string>> shownSketches;  // visible sketches per tree eye (§3.4); none — visibleSketches()
};

struct SceneContent {
    std::vector<DrawBody> bodies;
    double planeHalf = 0;      // half-size of the origin planes (§3.4)
    std::vector<DrawPlane> refPlanes;  // reference planes of the shown state (§3.4, S2-REF-*)
    std::vector<DrawAxis> refAxes;     // reference axes of the shown state
    size_t failedFaces = 0;    // untriangulated faces of the shown bodies (§9.1)
    std::vector<DrawWeld> weldLines;  // assembly: weld lines of welds on faces (§8а, weld.md §7; FX8-WLD-001)
    DrawNestedReference nested;  // assembly: planes, axes and visible sketches of nested subassemblies (FX9-ASM-008)
};

class SceneBuilder {
public:
    explicit SceneBuilder(api::Session& session) : s_(session) {}

    // Display tessellation chord (§11.6) for a body with bounding-box diagonal diag at scale mmPerPx.
    static double chordFor(double mmPerPx, double diag);
    // Allowed chord of a ready mesh (§11.6 "Ready mesh", FX3-PERF-002): max(chordFor, min(VIEW_CHORD_REUSE · t, hi)).
    static double acceptChordFor(double mmPerPx, double diag);
    // Whether the scene must be rebuilt after a scale change (§11.6): some body with a known mesh chord has it
    // coarser than allowed at mmPerPx.
    static bool needsRetessellation(const std::vector<DrawBody>& bodies, double mmPerPx);
    // Radius of a conventional thread ring on a cylindrical face of radius r (app-ux §3.4, FX10-HOLE-001): the inscribed
    // ring polygon lies off the face mesh on the empty side, so depth testing does not hide it behind the triangles.
    static double threadRingRadius(double r, bool internal);
    // Sections of the view (app-ux §3.4, S7F-VIEW-001) — the same in every window mode: the enabled sections in document
    // order; with the section panel open (panelDef) the edited section (editing) is replaced by the panel's definition,
    // a new one goes last.
    static std::vector<api::Json> viewSections(const std::vector<api::Session::SectionInfo>& all, const std::optional<std::string>& editing,
                                               const api::Json* panelDef);
    // Bodies, planes and the count of untriangulated faces.
    SceneContent build(const SceneRequest& r) const;
    // Sketch regions (§3.4): in a part — of visible sketches; in a panel with profiles — also the panel's sketch;
    // in other modes — none.
    std::vector<DrawRegion> regions(const SceneRequest& r) const;
    // Weld lines of assembly asmEl (app-ux §8а, S5-WELD-002): edges of each weld from the meshes of the shown instance
    // bodies (body "<path>/<body>", edge with the name after "<path>/"), in weld and edge order.
    static std::vector<DrawWeld> weldLines(const doc::Element& asmEl, const std::vector<DrawBody>& bodies);
    // Weld bodies (COLOR_WELD_BODY, not pickable) and the lines of welds on faces (§8а, weld.md §7; FX8-WLD-001,
    // FX8-WLD-002); a dragged host instance moves them with it.
    void addWelds(const SceneRequest& r, SceneContent& out) const;
    // Render color of an assembly entry in the 3D view (app-ux §8а «Цвета экземпляров»; FX12-ASM-003, FX13-JNT-004,
    // FX13-LIB-001): the instance color of the top-level instance on its path; else a fastener of the connection wizard
    // (a derived instance) — COLOR_FASTENER; else the part color, then its material color (a library part — of its copy in
    // the document); else a library part — COLOR_LIBRARY_PART, otherwise COLOR_BODY.
    static QColor instanceColor(const doc::Document& d, const regen::InstanceEntry& e);

private:
    std::vector<DrawBody> assemblyBodies(const SceneRequest& r) const;  // active assembly instances (assembly.md §5)
    std::vector<DrawBody> bodiesOf(const api::StateRef& ref, const QColor& color, float alpha, bool pickable,
                                   const std::string& keyPrefix, double mmPerPx, const std::vector<api::Json>* sections = nullptr) const;
    api::Session& s_;
};

}  // namespace construe::app
