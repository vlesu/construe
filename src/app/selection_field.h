// Selection set of one field (spec/selection.md §5.1, FX7-SEL-001): the picks of the set, the candidates the core
// derives from them (core-api selectionCandidates), the shown variant and the accepted value. No widgets.
#pragma once

#include "api/session.h"
#include "app/viewport_types.h"

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace construe::app {

// Pick reference of a view or tree object (selection.md §7.1): plane:* / axis:* / origin / feature ID / stable name /
// "<sketch>:<entity>".
std::string pickRef(const PickTarget& t);
// View object of a pick reference (highlight): a sketch entity, an origin plane/axis, origin; otherwise nullopt.
std::optional<PickTarget> pickTargetOf(const std::string& ref);
// Candidate label with picked element captions (ui.sel.*; objectCaption, style Candidate — app/captions.h).
QString candidateLabel(const api::Session& s, const api::SelectionCandidate& c, const api::StateRef& state);

class SelectionField {
public:
    SelectionField(api::Session& s, std::string input) : s_(&s), input_(std::move(input)) {}
    const std::string& input() const { return input_; }
    void setInput(std::string input);  // mode change of the panel (linear ↔ circular): the field is cleared
    // Every candidate waits for «Yes» (the sketch base set, selection.md §5.2: nothing is created before «Yes»).
    void setAlwaysConfirm(bool on) { alwaysConfirm_ = on; }

    enum class Outcome { Rejected, Removed, Prefix, Accepted, Pending };
    // A click on an element (selection.md §5.1 items 1–5) in state `state` with filter `filter`.
    Outcome click(const std::string& ref, const api::StateRef& state, const api::SelectionFilter& filter = {});

    // Candidate waiting for «Yes» (selection.md §2.4).
    bool pending() const { return pending_; }
    const api::SelectionCandidate* current() const;
    int variantCount() const { return int(result_.candidates.size()); }
    int variantIndex() const { return index_; }
    void confirm();                 // «Yes»
    void nextVariant(int step);     // «Other», Tab / Shift+Tab
    void reset();                   // «Reset», Esc: the set goes, the accepted value stays
    void clear();                   // the field becomes empty
    bool hasSet() const { return !picks_.empty(); }
    // The set can be continued (§5.1 item 6): the field keeps the clicks.
    bool extensible() const { return result_.extensible || pending_ || result_.status == "prefix"; }

    // Accepted value (PlaneRef / DirRef / AxisRef), its owned features and the label.
    bool hasValue() const { return value_.has_value(); }
    const doc::Json& value() const { return *value_; }
    const std::vector<doc::Json>& aux() const { return aux_; }
    const QString& label() const { return label_; }
    // Preview of the accepted value (the arrow of a direction is shown always, selection.md §3.2); none — loaded value.
    const std::optional<api::SelectionPreview>& valuePreview() const { return valuePreview_; }
    // A stored value (editing a feature): direct, without a set.
    void setValue(const doc::Json& value, const QString& label);

    const std::vector<std::string>& picks() const { return picks_; }
    // Hint: what to add (prefix), what fits (rejected).
    QString hint() const { return hint_; }

private:
    void evaluate(const api::StateRef& state, const api::SelectionFilter& filter);
    void acceptCurrent();

    api::Session* s_;
    std::string input_;
    std::vector<std::string> picks_;
    api::SelectionResult result_;
    api::StateRef state_ = api::StateRef::final();
    int index_ = 0;
    bool pending_ = false;
    bool alwaysConfirm_ = false;
    std::optional<api::SelectionPreview> valuePreview_;
    std::optional<doc::Json> value_;
    std::vector<doc::Json> aux_;
    QString label_;
    QString hint_;
};

}  // namespace construe::app
