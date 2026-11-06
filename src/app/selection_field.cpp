// Selection set of a field (app/selection_field.h).
#include "app/selection_field.h"

#include "app/captions.h"
#include "app/messages.h"

#include <algorithm>

namespace construe::app {

std::string pickRef(const PickTarget& t) {
    switch (t.kind) {
        case PickTarget::Kind::Origin: return "origin";
        case PickTarget::Kind::SketchEntity: return t.body + ":" + t.name;
        case PickTarget::Kind::Plane:
        case PickTarget::Kind::Axis:
        case PickTarget::Kind::Face:
        case PickTarget::Kind::Edge:
        case PickTarget::Kind::Vertex: return t.name;
        default: return {};
    }
}

std::optional<PickTarget> pickTargetOf(const std::string& ref) {
    if (ref == "origin") return PickTarget{PickTarget::Kind::Origin, {}, "origin"};
    if (ref.rfind("plane:", 0) == 0) return PickTarget{PickTarget::Kind::Plane, {}, ref};
    if (ref.rfind("axis:", 0) == 0) return PickTarget{PickTarget::Kind::Axis, {}, ref};
    if (const size_t colon = ref.find(':'); colon != std::string::npos && ref.find('(') == std::string::npos && ref.find('.') == std::string::npos)
        return PickTarget{PickTarget::Kind::SketchEntity, ref.substr(0, colon), ref.substr(colon + 1)};
    return std::nullopt;
}

QString candidateLabel(const api::Session& s, const api::SelectionCandidate& c, const api::StateRef& state) {
    std::map<std::string, QString> sub;
    for (auto it = c.labelParams.begin(); it != c.labelParams.end(); ++it)
        if (it.value().is_string()) sub[it.key()] = objectCaption(s, it.value().get<std::string>(), CaptionStyle::Candidate, state);
    return text(c.labelKey, sub);
}

void SelectionField::setInput(std::string input) {
    input_ = std::move(input);
    clear();
}

const api::SelectionCandidate* SelectionField::current() const {
    if (!pending_ || result_.candidates.empty()) return nullptr;
    return &result_.candidates[size_t(index_)];
}

void SelectionField::evaluate(const api::StateRef& state, const api::SelectionFilter& filter) {
    auto r = s_->selectionCandidates(input_, picks_, state, filter);
    result_ = r ? *r : api::SelectionResult{};
    if (!r) result_.status = "invalid";
}

SelectionField::Outcome SelectionField::click(const std::string& ref, const api::StateRef& state, const api::SelectionFilter& filter) {
    if (ref.empty()) return Outcome::Rejected;
    state_ = state;
    hint_.clear();
    index_ = 0;
    pending_ = false;
    Outcome removed = Outcome::Rejected;
    // 1. A repeated click removes the element from the set; the last one removed clears the field.
    if (auto it = std::find(picks_.begin(), picks_.end(), ref); it != picks_.end()) {
        picks_.erase(it);
        if (picks_.empty()) {
            clear();
            return Outcome::Removed;
        }
        evaluate(state, filter);
        removed = Outcome::Removed;
    } else {
        // 2. The set grows when it gives candidates or a prefix; 3. otherwise the click starts a new set.
        const auto prev = picks_;
        const auto prevResult = result_;
        bool taken = false;
        if (picks_.size() < api::SEL_MAX_PICKS) {
            picks_.push_back(ref);
            evaluate(state, filter);
            taken = result_.status == "candidates" || result_.status == "prefix";
        }
        if (!taken) {
            const std::vector<std::string> grownNeed = result_.need;
            picks_ = {ref};
            evaluate(state, filter);
            taken = result_.status == "candidates" || result_.status == "prefix";
            if (!taken) {  // 4. the set does not change; the hint names what fits
                const auto& need = !result_.need.empty() ? result_.need : grownNeed;
                hint_ = need.empty() ? text("ui.sel.need." + input_) : text(need.front());
                picks_ = prev;
                result_ = prevResult;
                return Outcome::Rejected;
            }
        }
    }
    // 5. Parsing of the set.
    if (result_.status == "prefix") {
        hint_ = result_.need.empty() ? QString() : text(result_.need.front());
        return removed == Outcome::Removed ? removed : Outcome::Prefix;
    }
    if (result_.candidates.size() == 1 && result_.candidates[0].direct && !result_.candidates[0].confirm && !alwaysConfirm_) {
        pending_ = true;
        acceptCurrent();
        return removed == Outcome::Removed ? removed : Outcome::Accepted;
    }
    pending_ = true;
    return removed == Outcome::Removed ? removed : Outcome::Pending;
}

void SelectionField::acceptCurrent() {
    const auto* c = current();
    if (!c) return;
    value_ = c->value;
    aux_ = c->aux;
    valuePreview_ = c->preview;
    label_ = candidateLabel(*s_, *c, state_);
    pending_ = false;
}

void SelectionField::confirm() {
    if (!pending_) return;
    acceptCurrent();
    // The set is consumed by the constructed value: the next click starts a new one.
    picks_.clear();
    result_ = {};
}

void SelectionField::nextVariant(int step) {
    if (!pending_ || result_.candidates.empty()) return;
    const int n = int(result_.candidates.size());
    index_ = ((index_ + step) % n + n) % n;
}

void SelectionField::reset() {
    picks_.clear();
    result_ = {};
    pending_ = false;
    index_ = 0;
    hint_.clear();
}

void SelectionField::clear() {
    reset();
    value_.reset();
    aux_.clear();
    label_.clear();
    valuePreview_.reset();
}

void SelectionField::setValue(const doc::Json& value, const QString& label) {
    clear();
    value_ = value;
    label_ = label;
}

}  // namespace construe::app
