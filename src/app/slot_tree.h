// Places tree (spec/app-ux.md §5.5б, S7F-UI-009): a command that waits for several different places («Face under the
// head», «Hole axes», …) shows them as a tree — a place node and under it the objects picked into it, each with
// ✕, hover highlight and centering (the unified selection field, selection_items.h). The places are described by data
// (SlotSpec, also a JSON description from a library package), not by the panel's code.
#pragma once

#include "app/selection_items.h"
#include "doc/json_text.h"

#include <QLabel>
#include <QString>
#include <QToolButton>
#include <QWidget>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::app {

// A place: its ID, caption, object type, how many objects and whether it is required (min > 0).
// Object types (§5.5б): "face", "face.plane", "face.cylinder", "edge", "edge.line", "edge.circle", "vertex", "point",
// "axis", "plane", "any".
struct SlotSpec {
    std::string id;
    QString title;
    std::string type = "any";
    int min = 1;  // required: at least min objects; 0 — optional
    int max = 1;  // at most; 0 — unlimited
    QString emptyText;  // caption of an empty place after the title (e.g. «auto (journal step)»); empty — none
    bool operator==(const SlotSpec&) const = default;
    // The same place apart from the captions (title, emptyText).
    bool samePlace(const SlotSpec& o) const { return id == o.id && type == o.type && min == o.min && max == o.max; }
};

// The description of places as JSON (§5.5б): [{"id", "title"?, "type"?, "min"?, "max"?, "empty"?}, …]; error — its text.
std::optional<std::vector<SlotSpec>> slotSpecsFromJson(const doc::Json& j, std::string* error = nullptr);
// Caption of an object type for hints: «cylindrical face», «edge», …
QString slotTypeCaption(const std::string& type);
// An object of the types (all its types, e.g. {"face", "face.cylinder"}) fits a place of type slotType.
bool slotTypeFits(const std::string& slotType, const std::set<std::string>& types);

// An object picked in the view: its unique reference, its types and its row of the list (caption, view objects).
struct SlotObject {
    std::string ref;
    std::set<std::string> types;
    SelectionItem item;
};

// Selection by places without widgets (§5.5б): the current place, the objects of each place, the click rules.
class SlotSet {
public:
    SlotSet() = default;
    explicit SlotSet(std::vector<SlotSpec> specs);
    const std::vector<SlotSpec>& specs() const { return specs_; }
    int count() const { return int(specs_.size()); }
    int current() const { return current_; }
    void setCurrent(int slot);
    const std::vector<SlotObject>& objects(int slot) const { return objects_.at(size_t(slot)); }
    // Place of the object with the reference, -1 — none.
    int slotOf(const std::string& ref) const;
    // Place by ID, -1 — none.
    int indexOf(const std::string& id) const;

    enum class Outcome { Added, Removed, Replaced, Rejected };
    // A click on an object in the view (§5.5б): in the current place — removed; fits the current place — added there
    // (a full place of one object — replaced; an object of another place moves here); in another place and does not
    // fit the current one — removed from there; otherwise — into the first place it fits that is not full (it becomes
    // current). Nothing fits — Rejected (hint()).
    Outcome click(const SlotObject& obj);
    void remove(int slot, int index);  // ✕
    // The objects of a place set by the panel that keeps its own model (the connection wizard, §8д); the current place
    // stays.
    void setObjects(int slot, std::vector<SlotObject> objs);
    // Titles and empty-place captions from `specs` (same places, another language — library.md §17.2); objects stay.
    void retitle(const std::vector<SlotSpec>& specs);
    void clear();
    // Every required place has its min objects.
    bool complete() const;
    // A required place with fewer than min objects.
    bool missing(int slot) const;
    // «Select: <place> (<type>)» for the status line; a rejected click — what is needed.
    QString hint() const;

private:
    bool full(int slot) const;
    void advance();

    std::vector<SlotSpec> specs_;
    std::vector<std::vector<SlotObject>> objects_;
    int current_ = 0;
    bool rejected_ = false;
};

// The widget (§5.5б): per place a node «<name>Slot<i>» (a button: caption, count, «required» when missing; the
// current node is marked as the active field) and under it the list «<name>Slot<i>Items» (SelectionItems: ✕, hover,
// centering). A click on a node makes the place current.
class SlotTree : public QWidget {
    Q_OBJECT
public:
    explicit SlotTree(const QString& name, QWidget* parent = nullptr);
    void setSpecs(std::vector<SlotSpec> specs);
    // The same places with new titles (a language switch): the objects and the current place stay; then refresh().
    void retitle(const std::vector<SlotSpec>& specs);
    SlotSet& places() { return set_; }
    const SlotSet& places() const { return set_; }
    // A click in the view: SlotSet::click, the tree is rebuilt, changed() is emitted (unless rejected).
    SlotSet::Outcome click(const SlotObject& obj);
    void setCurrent(int slot);
    void clear();
    // Captions of the nodes and their lists from the SlotSet (after a change of the set from outside).
    void refresh();
    QToolButton* node(int slot) const { return nodes_.at(size_t(slot)); }
    SelectionItems* items(int slot) const { return lists_.at(size_t(slot)); }

signals:
    void changed();         // objects of a place changed (click, ✕)
    void objectRemoved(int slot, const QString& ref);  // ✕ removed the object ref of the place (before changed())
    void currentChanged();  // the current place changed

private:
    QString name_;
    SlotSet set_;
    std::vector<QToolButton*> nodes_;
    std::vector<SelectionItems*> lists_;
};

}  // namespace construe::app
