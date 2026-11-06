// Built-in help content (app/help_content.h, spec/app-ux.md §2.1б, FX7-UI-007).
#include "app/help_content.h"

#include "app/hotkeys_table.h"
#include "app/messages.h"

#include <QFile>
#include <QRegularExpression>

#include <map>

namespace construe::app {

namespace {

QString normalized(QString s) {
    s = s.toLower();
    s.replace(QString::fromUtf8("ё"), QString::fromUtf8("е"));  // i18n-skip: search normalization
    s.replace("...", QString::fromUtf8("…"));
    static const QRegularExpression ws("\\s+");
    s.replace(ws, " ");
    return s;
}

// Simple inline markdown of the guide → HTML.
QString inlineHtml(const QString& md) {
    QString s = md.toHtmlEscaped();
    static const QRegularExpression link("\\[([^\\]]+)\\]\\([^)]*\\)"), bold("\\*\\*([^*]+)\\*\\*"), code("`([^`]+)`"),
        bullet("^\\s*[-*]\\s+"), image("!\\[[^\\]]*\\]\\([^)]*\\)");
    s.replace(image, QString());
    s.replace(link, "\\1");
    s.replace(bold, "<b>\\1</b>");
    s.replace(code, "<code>\\1</code>");
    s.replace(bullet, QString());
    return s;
}

}  // namespace

QStringList helpLanguages() { return {"ru", "en"}; }

QString helpGuide(const QString& lang) {
    QFile f(":/help/guide." + lang + ".md");
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).replace("\r\n", "\n") : QString();
}

QString cleanCommandLabel(const QString& label) {
    static const QRegularExpression key("\\s*\\((?:Ctrl|Shift|Alt|Esc|F\\d)[^)]*\\)\\s*$"), tail("(?:…|\\.\\.\\.)$");
    QString s = label;
    s.replace(key, QString());
    s.replace(QString::fromUtf8("▾"), QString());
    s = s.trimmed();
    s.replace(tail, QString());
    return s.trimmed();
}

QStringList guideParagraphs(const QString& guide, const QString& label, int limit) {
    const QString want = normalized(cleanCommandLabel(label));
    QStringList out;
    if (want.isEmpty()) return out;
    const QRegularExpression word("(?<![\\w])" + QRegularExpression::escape(want) + "(?![\\w])",
                                  QRegularExpression::UseUnicodePropertiesOption);
    // Blocks: paragraphs between empty lines; a list item starts a new block; headings and tables are skipped.
    QStringList blocks;
    QString cur;
    auto flush = [&] {
        if (!cur.trimmed().isEmpty()) blocks << cur.trimmed();
        cur.clear();
    };
    static const QRegularExpression item("^\\s*([-*]|\\d+\\.)\\s+");
    for (const QString& line : guide.split('\n')) {
        if (line.trimmed().isEmpty() || line.startsWith('#') || line.startsWith('|') || line.startsWith("---")) {
            flush();
            continue;
        }
        if (item.match(line).hasMatch()) flush();
        cur += (cur.isEmpty() ? QString() : QString(" ")) + line.trimmed();
    }
    flush();
    for (const QString& b : blocks) {
        if (out.size() >= limit) break;
        if (word.match(normalized(b)).hasMatch()) out << b;
    }
    return out;
}

QString helpCommandsHtml(const std::vector<HelpCommand>& commands, const QString& guide, const QString& lang, const QString& title) {
    const QString heading = !title.isEmpty() ? title : textIn(lang, "ui.help.commandReference");
    QString html = "<h1>" + heading.toHtmlEscaped() + "</h1>";
    html += "<p>" + textIn(lang, "ui.help.commandsIntro") + "</p>";
    for (const HelpCommand& c : commands) {
        html += "<h3><img src=\"icon:" + c.object.toHtmlEscaped() + "\" width=\"24\" height=\"24\"> <a name=\"cmd-" + c.object.toHtmlEscaped() +
                "\">" + cleanCommandLabel(c.label).toHtmlEscaped() + "</a></h3>";
        if (!c.tooltip.isEmpty() && c.tooltip != c.label) html += "<p><i>" + c.tooltip.toHtmlEscaped() + "</i></p>";
        const QStringList ps = guideParagraphs(guide, c.label);
        if (ps.isEmpty())
            html += "<p>" + textIn(lang, "ui.help.seeGuide") + "</p>";
        for (const QString& p : ps) html += "<p>" + inlineHtml(p) + "</p>";
    }
    return html;
}

QString guideSlug(const QString& heading) {
    QString out;
    for (const QChar c : heading.trimmed().toLower()) {
        if (c.isLetterOrNumber() || c == '-' || c == '_') out += c;
        else if (c.isSpace()) out += '-';
    }
    return out;
}

std::vector<HelpTopic> guideTopics(const QString& guide) {
    std::vector<HelpTopic> out;
    HelpTopic cur{"guide:0", "guide", QString(), QString(), true, QString()};
    static const QRegularExpression contents("^(Содержание|Contents):", QRegularExpression::UseUnicodePropertiesOption);  // i18n-skip: guide markup
    auto flush = [&] {
        while (cur.body.endsWith("\n\n")) cur.body.chop(1);
        if (!cur.title.isEmpty() || !cur.body.trimmed().isEmpty()) out.push_back(cur);
    };
    for (const QString& line : guide.split('\n')) {
        if (line.startsWith("## ")) {
            flush();
            const QString h = line.mid(3).trimmed();
            cur = {QString("guide:%1").arg(out.size()), "guide", h, line + "\n", true, guideSlug(h)};
            continue;
        }
        if (out.empty() && cur.title.isEmpty() && line.startsWith("# ")) cur.title = line.mid(2).trimmed();
        if (out.empty() && contents.match(line).hasMatch()) continue;  // the tree replaces the contents line
        cur.body += line + "\n";
    }
    flush();
    return out;
}

QString helpGroupTitle(const QString& group, const QString& lang) {
    if (lang == language()) return group;
    // The group is named as on the interface; in another help language the tab name gets a translation in brackets.
    static const char* const above[] = {"ui.main.aboveTheRibbonLeft", "ui.main.aboveTheRibbonRight"};
    static const char* const tabs[] = {"ui.common.part", "ui.common.sketch", "ui.common.assembly", "ui.ribbon.drawing"};
    for (const char* k : above)
        if (group == text(k)) return textIn(lang, k);
    for (const char* k : tabs)
        if (group == text(k)) return group + " (" + textIn(lang, k) + ")";
    return group;
}

QString hotkeysHtml(const QString& lang) {
    QString html = "<h1>" + textIn(lang, "ui.hotkeys.title").toHtmlEscaped() + "</h1>";
    html += "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\"><tr>";
    for (const char* k : {"ui.help.colKey", "ui.help.colCommand", "ui.help.colContext"}) html += "<th>" + textIn(lang, k).toHtmlEscaped() + "</th>";
    html += "</tr>";
    for (const auto& row : hotkeyTableKeys()) {
        html += "<tr>";
        for (const char* cell : row) html += "<td>" + textIn(lang, cell).toHtmlEscaped() + "</td>";
        html += "</tr>";
    }
    return html + "</table>";
}

std::vector<HelpTopic> commandTopics(const std::vector<HelpCommand>& commands, const QString& guide, const QString& lang) {
    std::vector<QString> order;
    std::map<QString, std::vector<HelpCommand>> groups;
    for (const HelpCommand& c : commands) {
        const QString g = c.group.isEmpty() ? textIn(lang, "ui.help.commands") : c.group;
        if (!groups.count(g)) order.push_back(g);
        groups[g].push_back(c);
    }
    std::vector<HelpTopic> out;
    for (const QString& g : order) {
        const QString title = helpGroupTitle(g, lang);
        out.push_back({QString("commands:%1").arg(out.size()), "commands", title, helpCommandsHtml(groups[g], guide, lang, title), false, QString()});
    }
    out.push_back({"hotkeys", "commands", textIn(lang, "ui.hotkeys.title"), hotkeysHtml(lang), false, QString()});
    return out;
}

std::vector<HelpTopic> helpTopics(const QString& guide, const std::vector<HelpCommand>& commands, const QString& lang) {
    std::vector<HelpTopic> out = guideTopics(guide);
    for (auto& t : commandTopics(commands, guide, lang)) out.push_back(std::move(t));
    return out;
}

}  // namespace construe::app
