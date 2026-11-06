// «Help» window (spec/app-ux.md §2.1б, FX7-UI-007, FX8-UI-002): modeless, one tree of topics — the user
// guide by sections with screenshots and the command reference of the ribbon by groups plus the hotkey table — and the
// chosen topic beside it, in every help language.
#pragma once

#include "app/help_content.h"

#include <QComboBox>
#include <QIcon>
#include <QPushButton>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QWidget>

#include <functional>
#include <map>
#include <vector>

namespace construe::app {

// The topic view: command icons are images «icon:<object>» of the current command list.
class HelpBrowser : public QTextBrowser {
public:
    QVariant loadResource(int type, const QUrl& name) override;
    std::map<QString, QIcon> icons;
};

class HelpWindow : public QWidget {
    Q_OBJECT
public:
    // commands — the ribbon commands (asked on every open: the ribbon is built by then).
    HelpWindow(QWidget* parent, std::function<std::vector<HelpCommand>()> commands);
    // Shows the window (raises an open one) in the current language on the topic (empty — the first topic).
    void open(const QString& topic = QString());
    void setLanguage(const QString& lang);
    QString language() const;
    // Shows a topic by id (HelpTopic::id); false — no such topic.
    bool openTopic(const QString& id);
    QString currentTopic() const { return current_; }
    const std::vector<HelpTopic>& topics() const { return topics_; }
    QTreeWidget* tree() const { return tree_; }
    QTextBrowser* topicView() const { return view_; }
    // The tree item of a topic (nullptr — none).
    QTreeWidgetItem* topicItem(const QString& id) const;

private:
    void fill();
    void showTopic(const HelpTopic& t);
    void followLink(const QUrl& url);

    std::function<std::vector<HelpCommand>()> commands_;
    std::vector<HelpTopic> topics_;
    QString current_;
    QComboBox* lang_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    HelpBrowser* view_ = nullptr;
};

}  // namespace construe::app
