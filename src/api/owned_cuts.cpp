// Owned cuts of connections "in parts" (api/owned_cuts.h, spec/solutions.md §7.10 item 4).
#include "api/owned_cuts.h"

#include "solutions/solutions.h"

#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace construe::api {

bool jsonNear(const doc::Json& a, const doc::Json& b, double tol) {
    if (a.is_number() && b.is_number()) return std::abs(a.get<double>() - b.get<double>()) <= tol;
    if (a.type() != b.type()) return false;
    if (a.is_array()) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!jsonNear(a[i], b[i], tol)) return false;
        return true;
    }
    if (a.is_object()) {
        if (a.size() != b.size()) return false;
        auto ia = a.begin();
        auto ib = b.begin();
        for (; ia != a.end(); ++ia, ++ib)
            if (ia.key() != ib.key() || !jsonNear(ia.value(), ib.value(), tol)) return false;
        return true;
    }
    return a == b;
}

namespace {

using Key = std::tuple<std::string, std::string, std::string>;  // (connection, output, source)

struct Want {
    Key key;
    std::string element;
    doc::Json feature;  // full feature JSON without "id"
    bool used = false;
};

// Feature JSON of an owned cut (document.md §2.10о) by a report cut and its owned entry; without "id".
doc::Json ownedFeature(const OwnedScope& scope, const std::string& connection, const std::string& connectionName,
                       const doc::Json& cut, const doc::Json& entry) {
    doc::Json f = doc::Json::object();
    f["type"] = "connectionCut";
    const std::string t = cut["title"].get<std::string>();
    f["name"] = (scope.title ? scope.title(t) : t) + " (" + connectionName + ")";
    f["suppressed"] = false;
    doc::Json owner = doc::Json::object();
    owner["assembly"] = scope.assembly;
    owner["connection"] = connection;
    if (!scope.ownerFile.empty()) owner["file"] = scope.ownerFile;
    owner["name"] = connectionName;
    f["owner"] = owner;
    f["source"] = entry["source"];
    f["output"] = cut["output"];
    f["title"] = cut["title"];
    f["frame"] = entry["frame"];
    if (cut["tool"].contains("revolve")) f["revolve"] = cut["tool"]["revolve"];
    else f["box"] = cut["tool"]["box"];
    if (cut.contains("thread")) f["thread"] = cut["thread"];
    if (cut.contains("fit")) f["fit"] = cut["fit"];
    if (cut.contains("diameter") && cut["diameter"].get<double>() > 0) f["diameter"] = cut["diameter"];
    return f;
}

}  // namespace

bool syncOwnedCuts(doc::Document& pd, const OwnedScope& scope) {
    // Desired features (§7.10 item 4): by connections in order, outputs, owned entries of this document.
    std::vector<Want> wants;
    std::map<std::string, const OwnedConnection*> conns;
    for (const auto& [id, oc] : scope.connections) {
        conns[id] = &oc;
        if (oc.kind != OwnedConnection::Kind::Report) continue;
        for (const auto& cut : oc.report["cuts"]) {
            if (!cut.contains("owned")) continue;
            for (const auto& e : cut["owned"])
                if (e["file"].get<std::string>() == scope.partKey)
                    wants.push_back({{id, cut["output"].get<std::string>(), e["source"].get<std::string>()}, e["element"].get<std::string>(),
                                     ownedFeature(scope, id, oc.name, cut, e)});
        }
    }
    // Existing owned features: kept, updated, suppressed or removed (by element, in feature order).
    bool changed = false;
    for (auto& el : pd.elements) {
        if (el.isAssembly()) continue;
        std::vector<doc::FeaturePtr> out;
        bool elChanged = false;
        for (const auto& f : el.features) {
            if (!scope.owned(*f)) {
                out.push_back(f);
                continue;
            }
            const doc::Json& def = f->pattern()->def;
            const std::string cid = def["owner"]["connection"].get<std::string>();
            auto ci = conns.find(cid);
            if (ci == conns.end() && !scope.all) {
                out.push_back(f);
                continue;
            }
            const OwnedConnection::Kind kind = ci == conns.end() ? OwnedConnection::Kind::Remove : ci->second->kind;
            if (kind == OwnedConnection::Kind::Keep) {
                out.push_back(f);
                continue;
            }
            if (kind == OwnedConnection::Kind::Suppress) {
                if (!f->suppressed) {
                    auto nf = std::make_shared<doc::Feature>(*f);
                    nf->suppressed = true;
                    out.push_back(nf);
                    elChanged = true;
                } else {
                    out.push_back(f);
                }
                continue;
            }
            Want* w = nullptr;
            if (kind == OwnedConnection::Kind::Report) {
                const Key key{cid, def["output"].get<std::string>(), def["source"].get<std::string>()};
                for (auto& x : wants)
                    if (!x.used && x.key == key && x.element == el.id) w = &x;
            }
            if (!w) {  // no desired feature: removed (a rollback marker on it moves up, as deleteFeature)
                if (el.rollback.kind == doc::Rollback::Kind::After && el.rollback.feature == f->id)
                    el.rollback = out.empty() ? doc::Rollback{doc::Rollback::Kind::Start, {}} : doc::Rollback{doc::Rollback::Kind::After, out.back()->id};
                elChanged = true;
                continue;
            }
            w->used = true;
            doc::Json want = w->feature;
            // The title part of the name stays as created (library.md §17.4): a language switch does not rename saved
            // cuts; the connection part follows a renamed connection.
            if (def["owner"].contains("name") && def["owner"]["name"].is_string()) {
                const std::string suffix = " (" + def["owner"]["name"].get<std::string>() + ")";
                const std::string& n = f->name;
                if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
                    want["name"] = n.substr(0, n.size() - suffix.size()) + " (" + want["owner"]["name"].get<std::string>() + ")";
            }
            want["id"] = f->id;
            doc::Json have = doc::featureToJsonValue(*f);
            doc::Json wantOrdered = doc::Json::object();
            wantOrdered["id"] = f->id;
            for (auto it = want.begin(); it != want.end(); ++it)
                if (it.key() != "id") wantOrdered[it.key()] = it.value();
            if (jsonNear(have, wantOrdered, solutions::SOL_SYNC_TOL)) {
                out.push_back(f);
                continue;
            }
            // Replaced in place: the same ID and position (§7.10 item 4).
            auto nf = std::make_shared<doc::Feature>(*f);
            nf->name = wantOrdered["name"].get<std::string>();
            nf->suppressed = false;
            doc::PatternData p;
            p.def = doc::Json::object();
            for (auto it = wantOrdered.begin(); it != wantOrdered.end(); ++it)
                if (it.key() != "id" && it.key() != "type" && it.key() != "name" && it.key() != "suppressed") p.def[it.key()] = it.value();
            nf->data = std::move(p);
            out.push_back(nf);
            elChanged = true;
        }
        if (elChanged) {
            el.features = std::move(out);
            changed = true;
        }
    }
    // New features at the end of their elements.
    for (auto& w : wants) {
        if (w.used) continue;
        const int ei = pd.elementIndex(w.element);
        if (ei < 0 || pd.elements[size_t(ei)].isAssembly()) continue;
        auto nf = std::make_shared<doc::Feature>();
        nf->id = doc::allocate(pd, doc::IdKind::Feature);
        nf->type = doc::FeatureType::ConnectionCut;
        nf->name = w.feature["name"].get<std::string>();
        nf->suppressed = false;
        doc::PatternData p;
        p.def = doc::Json::object();
        for (auto it = w.feature.begin(); it != w.feature.end(); ++it)
            if (it.key() != "type" && it.key() != "name" && it.key() != "suppressed") p.def[it.key()] = it.value();
        nf->data = std::move(p);
        pd.elements[size_t(ei)].features.push_back(nf);
        changed = true;
    }
    return changed;
}

}  // namespace construe::api
