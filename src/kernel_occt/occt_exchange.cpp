// Data exchange on OCCT (spec/kernel-interface.md §3.16, §9.9а).
#include "kernel_occt/occt_exchange.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace construe::kernel::occt {

namespace {

Quantity_Color colorOf(const std::string& hex) {
    unsigned r = 0, g = 0, b = 0;
    std::sscanf(hex.c_str(), "#%02x%02x%02x", &r, &g, &b);
    return Quantity_Color(r / 255.0, g / 255.0, b / 255.0, Quantity_TOC_sRGB);
}

std::string hexOf(const Quantity_Color& c) {
    Standard_Real r = 0, g = 0, b = 0;
    c.Values(r, g, b, Quantity_TOC_sRGB);
    auto q = [](double v) { return unsigned(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", q(r), q(g), q(b));
    return buf;
}

TCollection_ExtendedString ext(const std::string& utf8) { return TCollection_ExtendedString(utf8.c_str(), Standard_True); }

std::string utf8Of(const TCollection_ExtendedString& s) {
    std::string out(size_t(s.LengthOfCString()) + 1, '\0');
    Standard_PCharacter p = out.data();
    s.ToUTF8CString(p);
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string nameOf(const TDF_Label& l) {
    Handle(TDataStd_Name) n;
    return l.FindAttribute(TDataStd_Name::GetID(), n) ? utf8Of(n->Get()) : std::string();
}

TopoDS_Shape solidsCompound(const TopoDS_Shape& s) {
    TopoDS_Compound c;
    BRep_Builder b;
    b.MakeCompound(c);
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) b.Add(c, ex.Current());
    return c;
}

bool hasSolid(const TopoDS_Shape& s) { return !s.IsNull() && TopExp_Explorer(s, TopAbs_SOLID).More(); }

Handle(TDocStd_Document) newDoc() {
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    return doc;
}

}  // namespace

Result<std::string> brepWrite(const TopoDS_Shape& s) {
    try {
        TopoDS_Shape copy = s;
        BRepTools::Clean(copy);
        std::ostringstream os;
        BRepTools::Write(copy, os);
        return os.str();
    } catch (const Standard_Failure& e) {
        return Error("KERNEL_INTERNAL").with("detail", std::string(e.GetMessageString()));
    }
}

Result<TopoDS_Shape> brepRead(const std::string& text) {
    try {
        std::istringstream is(text);
        TopoDS_Shape s;
        BRep_Builder b;
        BRepTools::Read(s, is, b);
        if (!hasSolid(s)) return Error("KERNEL_IMPORT_FAILED").with("detail", std::string(s.IsNull() ? "brep parse" : "no solids"));
        return solidsCompound(s);
    } catch (const Standard_Failure& e) {
        return Error("KERNEL_IMPORT_FAILED").with("detail", std::string(e.GetMessageString()));
    }
}

Status stepWrite(const std::string& path, const std::vector<OccStepProduct>& products, int root) {
    try {
        Handle(TDocStd_Document) doc = newDoc();
        Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
        // Products in dependency order: inserted ones first (components refer to already created labels).
        std::vector<TDF_Label> labels(products.size());
        std::vector<int> state(products.size(), 0);
        std::function<void(int)> make = [&](int i) {
            if (state[size_t(i)] == 2) return;
            state[size_t(i)] = 1;
            const auto& p = products[size_t(i)];
            for (const auto& c : p.components)
                if (state[size_t(std::get<0>(c))] == 0) make(std::get<0>(c));
            TDF_Label l;
            if (!p.shape.IsNull()) {
                l = st->AddShape(p.shape, Standard_False);
                if (p.color) ct->SetColor(l, colorOf(*p.color), XCAFDoc_ColorSurf);
                for (const auto& [face, color] : p.faceColors) {
                    TDF_Label fl = st->AddSubShape(l, face);
                    if (!fl.IsNull()) ct->SetColor(fl, colorOf(color), XCAFDoc_ColorSurf);
                }
            } else {
                l = st->NewShape();
                for (const auto& [prod, trsf, name] : p.components) {
                    TDF_Label cl = st->AddComponent(l, labels[size_t(prod)], TopLoc_Location(trsf));
                    if (!cl.IsNull() && !name.empty()) TDataStd_Name::Set(cl, ext(name));
                }
            }
            if (!p.name.empty()) TDataStd_Name::Set(l, ext(p.name));
            labels[size_t(i)] = l;
            state[size_t(i)] = 2;
        };
        make(root);
        st->UpdateAssemblies();
        Interface_Static::SetCVal("write.step.unit", "MM");
        Interface_Static::SetCVal("write.step.schema", "AP214IS");
        STEPCAFControl_Writer w;
        w.SetColorMode(Standard_True);
        w.SetNameMode(Standard_True);
        if (!w.Transfer(doc, STEPControl_AsIs)) return Error("IO_WRITE").with("detail", std::string("step transfer")).with("file", path);
        // Write to a temporary file and rename (like the other exports).
        const auto target = std::filesystem::u8path(path);
        auto tmp = target;
        tmp += ".tmp";
        const auto tmpU8 = tmp.u8string();
        if (w.Write(std::string(tmpU8.begin(), tmpU8.end()).c_str()) != IFSelect_RetDone)
            return Error("IO_WRITE").with("detail", std::string("step write")).with("file", path);
        std::error_code ec;
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return Error("IO_WRITE").with("detail", std::string("rename")).with("file", path);
        }
        return okStatus();
    } catch (const Standard_Failure& e) {
        return Error("IO_WRITE").with("detail", std::string(e.GetMessageString())).with("file", path);
    }
}

Result<std::pair<std::vector<OccStepProduct>, int>> stepRead(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec))
        return Error("IO_READ").with("detail", std::string("cannot open")).with("file", path);
    try {
        Handle(TDocStd_Document) doc = newDoc();
        STEPCAFControl_Reader r;
        r.SetColorMode(Standard_True);
        r.SetNameMode(Standard_True);
        const auto u8 = std::filesystem::u8path(path).u8string();
        if (r.ReadFile(std::string(u8.begin(), u8.end()).c_str()) != IFSelect_RetDone)
            return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("not a STEP file"));
        if (!r.Transfer(doc)) return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("transfer"));
        Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
        std::vector<OccStepProduct> out;
        std::map<std::string, int> index;  // label entry (TDF_Tool::Entry) → product
        std::function<int(const TDF_Label&)> visit = [&](const TDF_Label& l) -> int {
            TCollection_AsciiString entry;
            TDF_Tool::Entry(l, entry);
            const std::string key = entry.ToCString();
            if (auto it = index.find(key); it != index.end()) return it->second;
            OccStepProduct p;
            p.name = nameOf(l);
            Quantity_Color c;
            if (ct->GetColor(l, XCAFDoc_ColorSurf, c) || ct->GetColor(l, XCAFDoc_ColorGen, c)) p.color = hexOf(c);
            if (st->IsAssembly(l)) {
                TDF_LabelSequence comps;
                st->GetComponents(l, comps);
                std::vector<std::tuple<int, gp_Trsf, std::string>> cs;
                for (int i = 1; i <= comps.Length(); ++i) {
                    TDF_Label ref;
                    if (!st->GetReferredShape(comps.Value(i), ref)) continue;
                    const int pi = visit(ref);
                    cs.push_back({pi, st->GetLocation(comps.Value(i)).Transformation(), nameOf(comps.Value(i))});
                }
                p.components = std::move(cs);
            } else {
                const TopoDS_Shape s = st->GetShape(l);
                if (hasSolid(s)) {
                    p.shape = solidsCompound(s);
                    TDF_LabelSequence subs;
                    st->GetSubShapes(l, subs);
                    for (int i = 1; i <= subs.Length(); ++i) {
                        const TopoDS_Shape fs = st->GetShape(subs.Value(i));
                        Quantity_Color fc;
                        if (fs.ShapeType() == TopAbs_FACE && (ct->GetColor(subs.Value(i), XCAFDoc_ColorSurf, fc) || ct->GetColor(subs.Value(i), XCAFDoc_ColorGen, fc)))
                            p.faceColors.push_back({fs, hexOf(fc)});
                    }
                }
            }
            const int id = int(out.size());
            out.push_back(std::move(p));
            index[key] = id;
            return id;
        };
        TDF_LabelSequence free;
        st->GetFreeShapes(free);
        if (free.Length() == 0) return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("empty"));
        int root = 0;
        if (free.Length() == 1) {
            root = visit(free.Value(1));
        } else {  // several free products: a synthetic unnamed assembly (§3.16)
            OccStepProduct top;
            for (int i = 1; i <= free.Length(); ++i) top.components.push_back({visit(free.Value(i)), gp_Trsf(), std::string()});
            root = int(out.size());
            out.push_back(std::move(top));
        }
        return std::make_pair(std::move(out), root);
    } catch (const Standard_Failure& e) {
        return Error("KERNEL_IMPORT_FAILED").with("detail", std::string(e.GetMessageString()));
    }
}

}  // namespace construe::kernel::occt
