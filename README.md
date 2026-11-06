# construe

**construe** is a free and open-source parametric 3D CAD program for designing mechanical parts, assemblies and their
drawings. A part is built as a history of sketches and features; change any dimension or variable and the whole model is
rebuilt. The interface follows the habits of users of parametric CAD programs: a ribbon, a feature tree, sketches with
geometric constraints, panels instead of modal dialogs.

> **Status: alpha (0.1.0-alpha).** The program is under active development. It is usable for real parts, but file
> formats and behavior may still change, and you should expect bugs. Please report problems — from the program
> (Help → Report a problem) or as an issue in this repository.
>
> The interface and the user guide are available in **English** and **Russian** (the interface language is chosen in the
> program settings). construe runs on Windows (64-bit), Linux (x86_64) and macOS (Apple Silicon).

## Who it is for

- Engineers and designers who build machine parts and assemblies and need production drawings.
- Makers and hobbyists who design parts for 3D printing and CNC machining.
- Anyone who wants a parametric CAD program they can study, build and change themselves.

## Features

**Sketching**
- 2D sketches on planes and on model faces: points, lines, rectangles, polygons and stars, circles, ellipses, arcs,
  splines, text.
- Geometric constraints and dimensions with a live solver; the sketch is always fully defined — dimensions you have not
  set are inferred and shown as weak (pale) ones.
- Snaps and inferred constraints while drawing, trim, mirror, offset (also of model edges and projections), patterns,
  projection and intersection of model geometry, fillets and chamfers of sketch corners.
- Variables and expressions in any dimension (`= L/2`), with units.

**Part modeling**
- Extrude (to a depth, symmetric, through all, up to a face or plane, up to the next face), revolve, sweep, loft, rib,
  hole, shell, draft, thread, fillet, chamfer, mirror and patterns.
- Reference planes, axes and points; sections of the model.
- Sheet metal parts with flanges, bends and flat patterns; gear rims.
- A feature tree with rollback, suppression and editing of any feature; a repair wizard re-attaches features whose
  references were lost after an edit.
- Part properties: material, mass, color of the part and of single faces.

**Assemblies**
- Insert parts and subassemblies, mates (coincident, concentric, distance, angle and others) with degrees-of-freedom
  feedback, dragging within the mates, exploded views, interference check.
- Editing a part in the context of the assembly; welds.
- A library of standard parts (fasteners, bearings and more) and a wizard for standard connections (bolted joints and
  others) that creates holes and fasteners in one step.

**Drawings**
- Drawing sheets with a title block, projected, section and detail views, dimensions, annotations, roughness and fit
  symbols, technical requirements and a bill of materials.
- Dimensions and annotations taken from the model in one click, with tolerances and fits that are part of each model
  dimension; what is not on the sheet yet is shown in blue and placed by a double click.
- Item numbers (balloons) and a parts list on assembly drawings, following the assembly specification; construction
  lines on views (center circles of hole patterns and others) attached to the model.
- Print and export to PDF, SVG and DXF.

**Files and exchange**
- Native `.construe` documents (a part, an assembly and a drawing can live in one file).
- Import and export of STEP; export of STL and 3MF for 3D printing, import of STL and 3MF meshes; exchange of `.FCStd` files with
  FreeCAD.
- Recognition of design features on imported solid bodies.

**Help and updates**
- Built-in help with the user guide, a command reference and the full hotkey table; an update check.

What each command does, with screenshots, is described in the user guide.

## Documentation

- User guide: [English](docs/user/guide.en.md) · [Russian](docs/user/guide.ru.md) — also available in the program
  (Help → Help).
- Installing on Windows, Linux and macOS: [English](docs/user/install.en.md) · [Russian](docs/user/install.ru.md).
- Building from source on Windows, Linux and macOS: [English](docs/user/build.en.md) · [Russian](docs/user/build.ru.md).
- What's new: [English](docs/user/changes.en.md) · [Russian](docs/user/changes.ru.md); packages — on the
  [releases page](https://github.com/vlesu/construe/releases).

## License

construe is licensed under the **GNU Affero General Public License v3.0 only** (`AGPL-3.0-only`); the full text is in
[LICENSE](LICENSE). Everything you create with the program — `.construe` documents, exported STEP, STL, 3MF, PDF, DXF,
SVG and other files — belongs to you and is not covered by the program's license (see section 2 of [LICENSE](LICENSE)).
Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES). Bug reports
are the main way to help; pull requests are not merged but are welcome as suggestions — see
[CONTRIBUTING.md](CONTRIBUTING.md).
