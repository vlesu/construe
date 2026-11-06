// «Help» window (app/help_window.h, spec/app-ux.md §2.1б, FX7-UI-007, FX8-UI-002).
#include "app/help_window.h"

#include "app/messages.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

namespace construe::app {

QVariant HelpBrowser::loadResource(int type, const QUrl& name) {
    if (type == QTextDocument::ImageResource && name.scheme() == "icon") {
        auto it = icons.find(name.path());
        return it != icons.end() ? QVariant(it->second.pixmap(24, 24).toImage()) : QVariant();
    }
    return QTextBrowser::loadResource(type, name);
}

HelpWindow::HelpWindow(QWidget* parent, std::function<std::vector<HelpCommand>()> commands)
    : QWidget(parent, Qt::Window), commands_(std::move(commands)) {
    setObjectName("helpWindow");
    setWindowTitle(text("ui.help.title"));
    resize(1000, 700);
    auto* col = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(text("ui.help.language")));
    lang_ = new QComboBox;
    lang_->setObjectName("helpLanguage");
    lang_->addItem(QString::fromUtf8("Русский"), "ru");  // i18n-skip: language autonym
    lang_->addItem("English", "en");
    setLanguage(construe::app::language());  // the interface language by default (§2.1б, §9.5)
    top->addWidget(lang_);
    top->addStretch(1);
    col->addLayout(top);
    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    tree_ = new QTreeWidget;
    tree_->setObjectName("helpTree");
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(1);
    view_ = new HelpBrowser;
    view_->setObjectName("helpTopic");
    view_->setOpenLinks(false);  // guide links «#…» open topics, web links — the browser (followLink)
    view_->setSearchPaths({":/help"});
    split->addWidget(tree_);
    split->addWidget(view_);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 700});
    col->addWidget(split, 1);
    auto* close = new QPushButton(text("ui.about.close"));
    close->setObjectName("helpClose");
    col->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(lang_, &QComboBox::currentIndexChanged, this, [this] {
        fill();
        if (!openTopic(current_) && !topics_.empty()) openTopic(topics_.front().id);
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (!it) return;
        if (it->childCount() > 0) it = it->child(0);  // a top level item shows its first topic
        openTopic(it->data(0, Qt::UserRole).toString());
    });
    connect(view_, &QTextBrowser::anchorClicked, this, &HelpWindow::followLink);
}

QString HelpWindow::language() const { return lang_->currentData().toString(); }

void HelpWindow::setLanguage(const QString& lang) {
    const int i = lang_->findData(lang);
    if (i >= 0) lang_->setCurrentIndex(i);
}

void HelpWindow::open(const QString& topic) {
    fill();
    if ((topic.isEmpty() || !openTopic(topic)) && !topics_.empty()) openTopic(topics_.front().id);
    show();
    raise();
    activateWindow();
}

void HelpWindow::fill() {
    const QString lang = language();
    const QString md = helpGuide(lang);
    const auto cmds = commands_ ? commands_() : std::vector<HelpCommand>{};
    view_->icons.clear();
    for (const HelpCommand& c : cmds)
        if (!c.icon.isNull()) view_->icons[c.object] = c.icon;
    topics_ = helpTopics(md, cmds, lang);
    const QSignalBlocker block(tree_);
    tree_->clear();
    QTreeWidgetItem* guide = new QTreeWidgetItem(tree_, {text("ui.help.tabGuide")});
    guide->setData(0, Qt::UserRole + 1, "guide");
    QTreeWidgetItem* commands = new QTreeWidgetItem(tree_, {text("ui.help.tabCommands")});
    commands->setData(0, Qt::UserRole + 1, "commands");
    for (const HelpTopic& t : topics_) {
        auto* it = new QTreeWidgetItem(t.section == "guide" ? guide : commands, {t.title});
        it->setData(0, Qt::UserRole, t.id);
    }
    tree_->expandAll();
}

QTreeWidgetItem* HelpWindow::topicItem(const QString& id) const {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree_->topLevelItem(i);
        for (int k = 0; k < top->childCount(); ++k)
            if (top->child(k)->data(0, Qt::UserRole).toString() == id) return top->child(k);
    }
    return nullptr;
}

bool HelpWindow::openTopic(const QString& id) {
    for (const HelpTopic& t : topics_) {
        if (t.id != id) continue;
        current_ = id;
        showTopic(t);
        if (QTreeWidgetItem* it = topicItem(id); it && tree_->currentItem() != it) {
            const QSignalBlocker block(tree_);
            tree_->setCurrentItem(it);
        }
        return true;
    }
    return false;
}

void HelpWindow::showTopic(const HelpTopic& t) {
    // Screenshots of the guide are resources «:/help/images/…»; relative links resolve against the resource folder.
    view_->document()->setBaseUrl(QUrl("qrc:/help/"));
    if (t.markdown) view_->setMarkdown(t.body);
    else view_->setHtml(t.body);
}

void HelpWindow::followLink(const QUrl& url) {
    if (url.scheme() == "http" || url.scheme() == "https" || url.scheme() == "mailto") {
        QDesktopServices::openUrl(url);
        return;
    }
    // A link of the guide to its section («#5-фичи»): the topic of that section.
    const QString frag = url.fragment(QUrl::FullyDecoded);
    if (frag.isEmpty()) return;
    for (const HelpTopic& t : topics_)
        if (!t.anchor.isEmpty() && t.anchor == frag) {
            openTopic(t.id);
            return;
        }
}

}  // namespace construe::app
