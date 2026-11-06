// Retranslation of the open windows after an interface language switch (spec/app-ux.md §9.5, S8-I18N-001).
#pragma once

#include <QString>

namespace construe::app {

// Text of s in language `to` if s (or each of its parts) is a text of language `from`: the whole string; a string with
// numbers in place of %1…%N; parts between the separators «\n», « — », «: », « (», «)», « ▾», «, »; a part with a
// leading number («0.05 мм»). Untranslatable parts stay as they are; nothing translatable — s unchanged.
QString retranslatedText(const QString& s, const QString& from, const QString& to);

// Labels of all widgets and actions of the application (texts, tooltips, titles, placeholders, combo box items,
// headers, spin box prefixes and suffixes) from `from` to `to`. Editable content (field text, cells, list items) is
// not touched; the views built from the document are rebuilt by their owners.
void retranslateWidgets(const QString& from, const QString& to);

}  // namespace construe::app
