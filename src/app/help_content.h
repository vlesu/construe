// Built-in help (spec/app-ux.md §2.1б, FX7-UI-007, FX8-UI-002): the user guide (docs/user/guide.<lang>.md, a program
// resource) and the command reference — a section per ribbon command with its icon, name, tooltip and the guide
// paragraphs that mention it — split into the topics of the help tree. Pure functions over text, without windows.
#pragma once

#include <QIcon>
#include <QString>
#include <QStringList>

#include <vector>

namespace construe::app {

struct HelpCommand {
    QString object;   // objectName of the button or menu item (ribbon_extrude, tool_line…)
    QString label;    // the command name as on the interface
    QString tooltip;  // the tooltip (name and hotkey, or a longer description)
    QIcon icon;
    QString group;    // the ribbon page: «Above the ribbon, left», «Above the ribbon, right» or the tab name (FX8-UI-002)
};

// A topic of the help tree (§2.1б, FX8-UI-002): a guide section or a group of the command reference.
struct HelpTopic {
    QString id;        // "guide:<n>" (n — section index, the same in every language), "commands:<n>", "hotkeys"
    QString section;   // "guide" or "commands" — the top level of the tree
    QString title;     // the tree item text
    QString body;      // markdown (guide) or HTML (commands, hotkeys)
    bool markdown = false;
    QString anchor;    // guide: slug of the heading — target of the guide's own links «#…»
};

// Languages of the help: every language of the interface plus English (§2.1б): {"ru", "en"}.
QStringList helpLanguages();
// The guide of a language from the resources (":/help/guide.<lang>.md"); empty — no such resource.
QString helpGuide(const QString& lang);
// The command name as the user reads it: without a hotkey in parentheses, «▾» and a trailing «…» (as
// the check of the user guide reads them).
QString cleanCommandLabel(const QString& label);
// Paragraphs of the guide (blocks between empty lines; every list item is a block) that mention the label as a whole
// word, ignoring case, «ё»/«е» and «...»/«…»; in guide order, at most `limit`.
QStringList guideParagraphs(const QString& guide, const QString& label, int limit = 3);
// HTML of the command reference: an anchor «cmd-<object>» per command, the icon image «icon:<object>», the name,
// the tooltip and the guide paragraphs (simple markdown: **bold**, `code`, [text](link) → text).
// title — the heading (empty: «Command reference»).
QString helpCommandsHtml(const std::vector<HelpCommand>& commands, const QString& guide, const QString& lang,
                         const QString& title = QString());

// Slug of a guide heading as in the guide's contents links: lower case, letters, digits, «-» and «_» kept, spaces → «-».
QString guideSlug(const QString& heading);
// Topics of the guide: the introduction (before the first «## », without the contents line — the tree replaces it;
// title — the «# » heading) and one topic per «## » section (its «### » subsections inside).
std::vector<HelpTopic> guideTopics(const QString& guide);
// The name of a ribbon page in the help language: as on the interface; in English with a translation in parentheses.
QString helpGroupTitle(const QString& group, const QString& lang);
// Topics of the command reference: one per ribbon page in order of first appearance, then «Hotkeys»
// (id "hotkeys") — the hotkey table §7.3 (FX8-UI-003).
std::vector<HelpTopic> commandTopics(const std::vector<HelpCommand>& commands, const QString& guide, const QString& lang);
// HTML of the hotkey table (app-ux §7.3): key, command, context.
QString hotkeysHtml(const QString& lang);
// Every topic of the help in tree order: the guide, then the command reference.
std::vector<HelpTopic> helpTopics(const QString& guide, const std::vector<HelpCommand>& commands, const QString& lang);

}  // namespace construe::app
