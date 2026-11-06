// Message texts and substitutions (spec/app-ux.md §9.2).
#pragma once

#include "base/error.h"
#include "doc/document.h"

#include <QString>
#include <QStringList>

#include <map>
#include <string>

namespace construe::app {

// Interface language (spec/app-ux.md §9.5): "ru" (source language, the default until set) or "en".
QStringList interfaceLanguages();
QString language();
// Sets the language of text() / txt(); an unknown language — false, nothing changes.
bool setLanguage(const QString& lang);
// Language by the «Interface language» setting: "ru", "en" — as is; "system" (or anything else) — the
// language of the first entry of systemUiLanguages (QLocale::uiLanguages(), "ru-RU", "en_US" …) if it is an interface
// language, otherwise "en".
QString resolveLanguage(const QString& setting, const QStringList& systemUiLanguages);

// Text by key with {name} → value substitutions, in the current language (a key without a translation — the source
// language, an unknown key — the key itself).
QString text(const std::string& key, const std::map<std::string, QString>& subst = {});
// The same in the given language (help in another language, app-ux §2.1б).
QString textIn(const QString& lang, const std::string& key, const std::map<std::string, QString>& subst = {});
// UTF-8 text of a key in the current language, without substitutions; the pointer stays valid for the whole run. For
// widget labels: txt("ui.common.cancel"). An unknown key — the argument itself.
const char* txt(const char* key);
// Text of `s` in language `to` if `s` is exactly a text of some key in language `from` (the first such key); otherwise
// the empty string. Retranslation of the open windows after a language switch (§9.5).
QString translatedText(const QString& s, const QString& from, const QString& to);

// Display name of a sketch entity type (§9.2): «Line» …; a type without a name — «Element».
QString entityDisplayName(sketch::EntityType t);

// Display name of a document object by ID (feature — its name, entity — «Line e12», dimension — D12).
QString objectName(const doc::Document* d, const std::string& id);

// Kernel error text from the error.<CODE> template; file is the file path for {file}.
QString errorText(const Error& e, const doc::Document* d = nullptr, const QString& file = {});

// Sketch fillet / chamfer corner (app-ux §6.3, FX7-SK-006): the reason why a point is not a corner (problem of
// sketchops::findCorner, lines — non-construction lines there) and the text of a sketchFillet / sketchChamfer error.
QString cornerProblemText(const std::string& problem, int lines);
QString cornerErrorText(const Error& e, const doc::Document* d = nullptr);

// Number for display: at most UI_DECIMALS decimal places, no trailing zeros, "-0" → "0" (fixedTrimmed, spec/base.md §7г).
QString formatNumber(double v);
// Display of a length/angle field expression (app-ux §5.6, vision item 10): a number in the document unit is shown
// without the unit ("10 mm" with mm → "10"); anything else is shown as stored.
QString displayExpr(const std::string& stored, const std::string& docUnit);

}  // namespace construe::app
