// Message texts and substitutions (spec/app-ux.md §9.2).
#include "app/messages.h"

#include "app/messages_table.h"
#include "app/theme.h"
#include "base/log.h"
#include "base/number_format.h"

#include <QFileInfo>
#include <QRegularExpression>

#include <cmath>
#include <map>

namespace construe::app {

QString displayExpr(const std::string& stored, const std::string& docUnit) {
    static const QRegularExpression num("^\\s*([+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?)\\s*([a-z]+)\\s*$");
    const auto m = num.match(QString::fromStdString(stored));
    if (m.hasMatch() && m.captured(4).toStdString() == docUnit) return m.captured(1);
    return QString::fromStdString(stored);
}

QString formatNumber(double v) { return QString::fromStdString(fixedTrimmed(v, UI_DECIMALS)); }

namespace {

std::string g_language = "ru";

// Text of a key in a language: the language table, then the source language; nullptr — unknown key.
const std::string* lookup(const std::string& lang, const std::string& key) {
    if (const auto* t = messageTableOf(lang))
        if (auto it = t->find(key); it != t->end()) return &it->second;
    const auto& ru = messageTable();
    auto it = ru.find(key);
    return it != ru.end() ? &it->second : nullptr;
}

QString substitute(const QString& s, const std::map<std::string, QString>& subst) {
    static const QRegularExpression re("\\{([A-Za-z]+)\\}");
    QString out;
    qsizetype last = 0;
    auto m = re.globalMatch(s);
    while (m.hasNext()) {
        auto mm = m.next();
        out += s.mid(last, mm.capturedStart() - last);
        const std::string k = mm.captured(1).toStdString();
        auto v = subst.find(k);
        out += v != subst.end() ? v->second : QStringLiteral("?");
        last = mm.capturedEnd();
    }
    out += s.mid(last);
    return out;
}

}  // namespace

QStringList interfaceLanguages() {
    QStringList out;
    for (const char* l : MESSAGE_LANGUAGES) out << l;
    return out;
}

QString language() { return QString::fromStdString(g_language); }

bool setLanguage(const QString& lang) {
    if (!messageTableOf(lang.toStdString())) return false;
    g_language = lang.toStdString();
    return true;
}

QString resolveLanguage(const QString& setting, const QStringList& systemUiLanguages) {
    if (setting != "system" && interfaceLanguages().contains(setting)) return setting;
    const QString first = systemUiLanguages.isEmpty() ? QString() : systemUiLanguages.first().toLower();
    const QString code = first.section('-', 0, 0).section('_', 0, 0);
    return interfaceLanguages().contains(code) ? code : QString("en");
}

QString text(const std::string& key, const std::map<std::string, QString>& subst) {
    const std::string* t = lookup(g_language, key);
    return substitute(t ? QString::fromUtf8(t->c_str()) : QString::fromStdString(key), subst);
}

QString textIn(const QString& lang, const std::string& key, const std::map<std::string, QString>& subst) {
    const std::string* t = lookup(lang.toStdString(), key);
    return substitute(t ? QString::fromUtf8(t->c_str()) : QString::fromStdString(key), subst);
}

const char* txt(const char* key) {
    const std::string* t = lookup(g_language, key);
    return t ? t->c_str() : key;
}

QString translatedText(const QString& s, const QString& from, const QString& to) {
    // Reverse index text → key of a language: built once per language (the tables are fixed).
    static std::map<std::string, std::map<QString, std::string>> index;
    const std::string f = from.toStdString();
    auto [it, fresh] = index.try_emplace(f);
    if (fresh) {
        // A text shared by several keys: interface labels (ui.*) first, the hotkey table cells (often language-neutral,
        // «Part») last — so that a label goes to its own translation.
        for (const char* prefix : {"ui.", "action.", "undo.", "error.", ""})
            for (const auto& [k, v] : messageTable())
                if (k.rfind(prefix, 0) == 0) it->second.try_emplace(QString::fromUtf8(lookup(f, k)->c_str()), k);
    }
    auto k = it->second.find(s);
    return k == it->second.end() ? QString() : textIn(to, k->second);
}

QString entityDisplayName(sketch::EntityType t) {
    // A switch, not a table indexed by the enum: a new entity type cannot read past the end (-Wswitch flags it).
    switch (t) {
        case sketch::EntityType::Point: return txt("ui.common.point2");
        case sketch::EntityType::Line: return txt("ui.common.line2");
        case sketch::EntityType::Circle: return txt("ui.common.circle2");
        case sketch::EntityType::Arc: return txt("ui.common.arc");
        case sketch::EntityType::Projected: return txt("ui.common.project");
        case sketch::EntityType::Text: return txt("ui.common.text");
        case sketch::EntityType::Ellipse: return txt("ui.common.ellipse");
        case sketch::EntityType::EllipseArc: return txt("ui.msg.ellipticalArc");
        case sketch::EntityType::Bezier: return txt("ui.msg.bezierCurve");
        case sketch::EntityType::BSpline: return txt("ui.common.spline");
    }
    return txt("ui.msg.element");
}

QString objectName(const doc::Document* d, const std::string& id) {
    if (!d) return QString::fromStdString(id);
    for (const auto& el : d->elements)
        for (const auto& f : el.features) {
            if (f->id == id) return QString::fromStdString(f->name);
            if (const auto* s = f->sketchData()) {
                if (const auto* e = s->findEntity(id)) return entityDisplayName(e->type) + " " + QString::fromStdString(id);
                if (const auto* c = s->findConstraint(id)) {
                    if (sketch::isDimensionType(c->type)) return "D" + QString::fromStdString(id.substr(1));
                    // "<Title> (<entities>)" (§9.2).
                    const std::map<std::string, const char*> titles = {
                        {"coincident", txt("ui.common.coincident")},     {"horizontal", txt("ui.msg.horizontal")},
                        {"vertical", txt("ui.msg.vertical")},   {"parallel", txt("ui.common.parallel")},
                        {"perpendicular", txt("ui.common.perpendicular")}, {"tangent", txt("ui.msg.tangent")},
                        {"equal", txt("ui.common.equal")},           {"concentric", txt("ui.common.concentric")},
                        {"pointOnObject", txt("ui.msg.onObject")},  {"midpoint", txt("ui.common.midpoint")},
                        {"symmetric", txt("ui.msg.symmetric")},       {"fix", txt("ui.msg.fix")},
                        {"collinear", txt("ui.msg.collinear")},  {"curvature", txt("ui.common.equalCurvature")}};
                    auto t = titles.find(c->type);
                    QStringList refs;
                    for (const auto& r : c->refs) refs << (r == "origin" ? QString(txt("ui.common.origin")) : objectName(d, r));
                    return QString::fromUtf8(t != titles.end() ? t->second : c->type.c_str()) + " (" + refs.join(", ") + ")";
                }
            }
        }
    for (const auto& v : d->variables)
        if (v.id == id) return QString::fromStdString(v.name);
    return QString::fromStdString(id);
}

QString cornerProblemText(const std::string& problem, int lines) {
    static const std::map<std::string, std::string> keys = {
        {"not a point", "ui.corner.notPoint"}, {"no lines", "ui.corner.noLines"},   {"one line", "ui.corner.oneLine"},
        {"many lines", "ui.corner.manyLines"}, {"not lines", "ui.corner.notLines"}, {"parallel", "ui.corner.parallel"},
        {"degenerate", "ui.corner.degenerate"}, {"group", "ui.corner.group"}};
    const auto it = keys.find(problem);
    return text(it != keys.end() ? it->second : "ui.corner.notPoint", {{"count", QString::number(lines)}});
}

QString cornerErrorText(const Error& e, const doc::Document* d) {
    const auto* name = e.stringParam("name");
    const auto* detail = e.stringParam("detail");
    if (e.code == "API_INVALID_ITEMS" && detail && *detail == "group") return cornerProblemText("group", 0);
    if (e.code != "API_INVALID_ARGUMENT" || !name) return errorText(e, d);
    if (*name == "point") return cornerProblemText(detail ? *detail : "", int(e.numberParam("count").value_or(0)));
    if (detail && *detail == "too large") return text("ui.corner.tooLarge");
    return errorText(e, d);  // a value that is not a positive length / angle
}

QString errorText(const Error& e, const doc::Document* d, const QString& file) {
    std::map<std::string, QString> s;
    for (const auto& [k, v] : e.params) {
        if (const auto* str = std::get_if<std::string>(&v)) {
            s[k] = (k == "id") ? objectName(d, *str) : QString::fromStdString(*str);
        } else if (const auto* num = std::get_if<double>(&v)) {
            s[k] = formatNumber(*num);
        } else if (const auto* list = std::get_if<std::vector<std::string>>(&v)) {
            QStringList parts;
            for (const auto& x : *list) parts << objectName(d, x);
            s[k] = parts.join(", ");
        }
    }
    s["code"] = QString::fromStdString(e.code);
    if (e.path) s["path"] = QString::fromStdString(*e.path);
    QString f = file;
    if (f.isEmpty())
        if (const auto* p = e.stringParam("file")) f = QString::fromStdString(*p);
    if (!f.isEmpty()) s["file"] = QFileInfo(f).fileName();
    if (const auto* id = e.stringParam("id")) {
        const QString n = objectName(d, *id);
        for (const char* k : {"object", "feature", "sketch", "constraint", "body"}) s[k] = n;
    }
    if (const auto* it = std::get_if<std::vector<std::string>>(e.params.count("ids") ? &e.params.at("ids") : nullptr)) {
        QStringList parts;
        for (const auto& x : *it) parts << objectName(d, x);
        s["entities"] = parts.join(", ");
    }
    // KERNEL_TOLERANCE_GROWTH: the tolerance in micrometres (§9.2, FX7-UI-008) — in millimetres it would round to 0.
    if (e.code == "KERNEL_TOLERANCE_GROWTH")
        if (const auto* v = std::get_if<double>(e.params.count("actual") ? &e.params.at("actual") : nullptr)) s["actual"] = formatNumber(*v * 1000);
    // EXPR_TYPE: type words (§9.2); API_TABLE_TEXT: reason text ui.tableText.<reason>.
    if (e.code == "EXPR_TYPE")
        for (const char* k : {"expected", "actual"})
            if (const auto* v = e.stringParam(k)) {
                const std::map<std::string, QString> words = {
                    {"int", txt("ui.msg.integer")}, {"number", txt("ui.msg.number")}, {"float", txt("ui.msg.decimalNumber")}, {"string", txt("ui.msg.string")}};
                if (auto it = words.find(*v); it != words.end()) s[k] = it->second;
            }
    if (e.code == "SHEET_RECOGNIZE")  // reason text (app-ux §5.3к, S5-SHEET-006)
        if (const auto* r = e.stringParam("detail")) s["detail"] = text("ui.sheetRecognize." + *r);
    if (e.code == "REGEN_SWEEP_PATH_CURVE")  // the kind of the unsupported path curve (regen §4.11 item 2, FX10-PT-003)
        if (const auto* r = e.stringParam("kind")) s["kind"] = text("ui.sweepPathKind." + *r);
    if (e.code == "API_TABLE_TEXT" || e.code == "LIB_SIZES_TEXT")
        if (const auto* r = e.stringParam("reason")) s["reason"] = text("ui.tableText." + *r);
    const std::string key = "error." + e.code;
    if (!messageTable().count(key)) return text("error.UNKNOWN", s);
    // An unknown variable name — the similar existing names as a hint (FX9-SK-004).
    if (e.code == "EXPR_UNKNOWN_NAME")
        if (const auto* sim = std::get_if<std::vector<std::string>>(e.params.count("similar") ? &e.params.at("similar") : nullptr); sim && !sim->empty()) {
            QStringList names;
            for (const auto& n : *sim) names << QString::fromStdString(n);
            return text(key, s) + " " + text("ui.expr.similar", {{"names", names.join(", ")}});
        }
    return text(key, s);
}

}  // namespace construe::app
