// Places tree (spec/app-ux.md §5.5б, S7F-UI-009).
#include "app/slot_tree.h"

#include "app/messages.h"
#include "app/theme.h"

#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

namespace {
const std::set<std::string>& knownTypes() {
    static const std::set<std::string> t = {"face", "face.plane", "face.cylinder", "edge", "edge.line", "edge.circle",
                                            "vertex", "point", "axis", "plane", "any"};
    return t;
}

// A place node (§5.5б, FX10-WZ-001): the button frame of the style, the caption from the left edge — QToolButton itself
// centers its text and ignores text-align of a style sheet.
class SlotNode : public QToolButton {
public:
    explicit SlotNode(QWidget* parent) : QToolButton(parent) {}
    void setTextColor(const QColor& c) {
        color_ = c;
        update();
    }
    QSize sizeHint() const override {
        const QSize h = QToolButton::sizeHint();
        return {fontMetrics().horizontalAdvance(text()) + 2 * SLOT_NODE_PAD_PX + 8, h.height()};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter p(this);
        QStyleOptionToolButton opt;
        initStyleOption(&opt);
        opt.text.clear();
        p.drawComplexControl(QStyle::CC_ToolButton, opt);
        p.setPen(color_.isValid() ? color_ : palette().color(QPalette::ButtonText));
        p.drawText(rect().adjusted(SLOT_NODE_PAD_PX, 0, -SLOT_NODE_PAD_PX, 0), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                   fontMetrics().elidedText(text(), Qt::ElideRight, width() - 2 * SLOT_NODE_PAD_PX));
    }

private:
    QColor color_;
};
}  // namespace

std::optional<std::vector<SlotSpec>> slotSpecsFromJson(const doc::Json& j, std::string* error) {
    auto fail = [&](const std::string& e) -> std::optional<std::vector<SlotSpec>> {
        if (error) *error = e;
        return std::nullopt;
    };
    if (!j.is_array()) return fail("places: an array is expected");
    std::vector<SlotSpec> out;
    std::set<std::string> ids;
    for (size_t i = 0; i < j.size(); ++i) {
        const auto& e = j[i];
        const std::string at = "places[" + std::to_string(i) + "]";
        if (!e.is_object() || !e.contains("id") || !e["id"].is_string() || e["id"].get<std::string>().empty())
            return fail(at + ": id (a non-empty string) is required");
        SlotSpec s;
        s.id = e["id"].get<std::string>();
        if (!ids.insert(s.id).second) return fail(at + ": id «" + s.id + "» repeats");
        s.title = QString::fromStdString(e.value("title", s.id));
        s.type = e.value("type", std::string("any"));
        if (!knownTypes().count(s.type)) return fail(at + ": unknown type «" + s.type + "»");
        if (e.contains("min") && !e["min"].is_number_integer()) return fail(at + ": min is an integer");
        if (e.contains("max") && !e["max"].is_number_integer()) return fail(at + ": max is an integer");
        s.min = e.value("min", 1);
        s.max = e.value("max", 1);
        s.emptyText = QString::fromStdString(e.value("empty", std::string()));
        if (s.min < 0 || s.max < 0 || (s.max > 0 && s.min > s.max)) return fail(at + ": 0 <= min <= max (max 0 — unlimited)");
        out.push_back(std::move(s));
    }
    return out;
}

QString slotTypeCaption(const std::string& type) {
    return text("ui.slot.type." + (knownTypes().count(type) ? type : std::string("any")));
}

bool slotTypeFits(const std::string& slotType, const std::set<std::string>& types) {
    return slotType == "any" || types.count(slotType) > 0;
}

// ---------------------------------------------------------------- SlotSet

SlotSet::SlotSet(std::vector<SlotSpec> specs) : specs_(std::move(specs)), objects_(specs_.size()) {}

void SlotSet::setCurrent(int slot) {
    if (slot >= 0 && slot < count()) current_ = slot;
    rejected_ = false;
}

int SlotSet::slotOf(const std::string& ref) const {
    for (int s = 0; s < count(); ++s)
        for (const auto& o : objects_[size_t(s)])
            if (o.ref == ref) return s;
    return -1;
}

int SlotSet::indexOf(const std::string& id) const {
    for (int s = 0; s < count(); ++s)
        if (specs_[size_t(s)].id == id) return s;
    return -1;
}

bool SlotSet::full(int slot) const {
    const int mx = specs_[size_t(slot)].max;
    return mx > 0 && int(objects_[size_t(slot)].size()) >= mx;
}

bool SlotSet::missing(int slot) const { return int(objects_.at(size_t(slot)).size()) < specs_.at(size_t(slot)).min; }

bool SlotSet::complete() const {
    for (int s = 0; s < count(); ++s)
        if (missing(s)) return false;
    return true;
}

// After the current place is full: the next place that is not full (a missing required one first), as the next empty
// field of a panel (§5.5).
void SlotSet::advance() {
    if (!full(current_)) return;
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 1; k < count(); ++k) {
            const int s = (current_ + k) % count();
            if (!full(s) && (pass == 1 || missing(s))) {
                current_ = s;
                return;
            }
        }
}

SlotSet::Outcome SlotSet::click(const SlotObject& obj) {
    rejected_ = false;
    if (specs_.empty()) return rejected_ = true, Outcome::Rejected;
    const int was = slotOf(obj.ref);
    const bool fitsCurrent = slotTypeFits(specs_[size_t(current_)].type, obj.types);
    if (was == current_ || (was >= 0 && !fitsCurrent)) {
        auto& src = objects_[size_t(was)];
        src.erase(std::find_if(src.begin(), src.end(), [&](const SlotObject& o) { return o.ref == obj.ref; }));
        return Outcome::Removed;
    }
    // The place: the current one if the object fits it, otherwise the first fitting place that is not full.
    int target = -1;
    if (fitsCurrent) target = current_;
    for (int s = 0; target < 0 && s < count(); ++s)
        if (slotTypeFits(specs_[size_t(s)].type, obj.types) && !full(s)) target = s;
    if (target < 0) return rejected_ = true, Outcome::Rejected;
    auto& dst = objects_[size_t(target)];
    bool replaced = false;
    if (full(target)) {
        if (specs_[size_t(target)].max != 1) return rejected_ = true, Outcome::Rejected;
        dst.clear();
        replaced = true;
    }
    if (was >= 0) {
        auto& src = objects_[size_t(was)];
        src.erase(std::find_if(src.begin(), src.end(), [&](const SlotObject& o) { return o.ref == obj.ref; }));
    }
    dst.push_back(obj);
    current_ = target;
    advance();
    return replaced ? Outcome::Replaced : Outcome::Added;
}

void SlotSet::remove(int slot, int index) {
    auto& v = objects_.at(size_t(slot));
    if (index < 0 || index >= int(v.size())) return;
    v.erase(v.begin() + index);
    current_ = slot;  // ✕ makes the place current: the next click fills it again
    rejected_ = false;
}

void SlotSet::retitle(const std::vector<SlotSpec>& specs) {
    for (size_t i = 0; i < specs_.size() && i < specs.size(); ++i)
        if (specs_[i].samePlace(specs[i])) specs_[i].title = specs[i].title, specs_[i].emptyText = specs[i].emptyText;
}

void SlotTree::retitle(const std::vector<SlotSpec>& specs) {
    set_.retitle(specs);
    refresh();
}

void SlotSet::setObjects(int slot, std::vector<SlotObject> objs) {
    objects_.at(size_t(slot)) = std::move(objs);
    rejected_ = false;
}

void SlotSet::clear() {
    for (auto& v : objects_) v.clear();
    current_ = 0;
    rejected_ = false;
}

QString SlotSet::hint() const {
    if (specs_.empty()) return {};
    const SlotSpec& s = specs_[size_t(current_)];
    if (rejected_) {
        if (full(current_) && s.max > 1)
            return text("ui.slot.full", {{"place", s.title}, {"max", QString::number(s.max)}});
        return text("ui.slot.need", {{"type", slotTypeCaption(s.type)}});
    }
    return text("ui.slot.pick", {{"place", s.title}, {"type", slotTypeCaption(s.type)}});
}

// ---------------------------------------------------------------- SlotTree

SlotTree::SlotTree(const QString& name, QWidget* parent) : QWidget(parent), name_(name) {
    setObjectName(name + "Slots");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
}

void SlotTree::setSpecs(std::vector<SlotSpec> specs) {
    for (auto* n : nodes_) n->deleteLater();
    for (auto* l : lists_) l->deleteLater();
    nodes_.clear();
    lists_.clear();
    set_ = SlotSet(std::move(specs));
    for (int s = 0; s < set_.count(); ++s) {
        auto* node = new SlotNode(this);
        node->setObjectName(name_ + "Slot" + QString::number(s));
        node->setToolButtonStyle(Qt::ToolButtonTextOnly);
        node->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        node->setAutoRaise(true);
        node->setToolTip(txt("ui.slot.clickPickObjectsHere"));
        connect(node, &QToolButton::clicked, this, [this, s] { setCurrent(s); });
        // Children — the unified field (S7F-UI-001), indented under the node.
        auto* list = new SelectionItems(name_ + "Slot" + QString::number(s), this);
        list->setContentsMargins(16, 0, 0, 0);
        connect(list, &SelectionItems::removed, this, [this, s](int i) {
            if (i < 0 || i >= int(set_.objects(s).size())) return;
            const QString ref = QString::fromStdString(set_.objects(s)[size_t(i)].ref);
            set_.remove(s, i);
            refresh();
            emit objectRemoved(s, ref);
            emit changed();
            emit currentChanged();
        });
        layout()->addWidget(node);
        layout()->addWidget(list);
        nodes_.push_back(node);
        lists_.push_back(list);
    }
    refresh();
}

void SlotTree::refresh() {
    for (int s = 0; s < set_.count(); ++s) {
        const SlotSpec& sp = set_.specs()[size_t(s)];
        const int n = int(set_.objects(s).size());
        QString caption = QString::fromUtf8("▾ ") + sp.title;
        if (n > 0 && sp.max != 1) caption += QString(" (%1)").arg(n);
        const bool missing = set_.missing(s);
        if (missing) caption += " — " + text("ui.slot.required");
        else if (n == 0 && !sp.emptyText.isEmpty()) caption += " — " + sp.emptyText;
        QToolButton* node = nodes_[size_t(s)];
        node->setText(caption);
        node->setProperty("current", s == set_.current());
        node->setProperty("missing", missing);
        QString css = "QToolButton { padding: 2px 4px;";
        if (s == set_.current())
            css += QString(" background: %1; border: 1px solid %2;")
                       .arg(COLOR_FIELD_ACTIVE_BG.color.name(), COLOR_FIELD_ACTIVE_BORDER.color.name());
        node->setStyleSheet(css + " }");
        static_cast<SlotNode*>(node)->setTextColor(missing ? COLOR_ERROR_TEXT.color : QColor());
        std::vector<SelectionItem> items;
        for (const auto& o : set_.objects(s)) items.push_back(o.item);
        lists_[size_t(s)]->setItems(std::move(items));
    }
}

SlotSet::Outcome SlotTree::click(const SlotObject& obj) {
    const int was = set_.current();
    const auto r = set_.click(obj);
    refresh();
    if (r != SlotSet::Outcome::Rejected) emit changed();
    if (set_.current() != was) emit currentChanged();
    return r;
}

void SlotTree::setCurrent(int slot) {
    const int was = set_.current();
    set_.setCurrent(slot);
    refresh();
    if (set_.current() != was) emit currentChanged();
}

void SlotTree::clear() {
    set_.clear();
    refresh();
}

}  // namespace construe::app
