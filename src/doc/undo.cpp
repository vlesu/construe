#include "doc/undo.h"

#include <iterator>

namespace construe::doc {

namespace {
// Diagnostic builds ("Dangling document references"): every change of the history moves the
// snapshots to a new buffer, so a reference into the previous current document used after Session::commit() is
// caught by AddressSanitizer / _GLIBCXX_DEBUG at once instead of only when the vector happens to grow.
void stressRealloc([[maybe_unused]] std::vector<Snapshot>& v) {
#ifdef CONSTRUE_UNDO_STRESS
    std::vector<Snapshot> moved(std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
    v.swap(moved);
#endif
}
}  // namespace

UndoStack::UndoStack(Snapshot initial) {
    snapshots_.push_back(std::move(initial));
    saved_ = 0;
}

void UndoStack::push(Snapshot s) {
    // The discarded redo branch contains the saved snapshot → the saved snapshot no longer exists.
    if (saved_ && *saved_ > index_) saved_.reset();
    snapshots_.resize(index_ + 1);
    snapshots_.push_back(std::move(s));
    index_ = snapshots_.size() - 1;
    if (snapshots_.size() > UNDO_DEPTH) {
        snapshots_.erase(snapshots_.begin());
        --index_;
        if (saved_) {
            if (*saved_ == 0) saved_.reset();
            else --*saved_;
        }
    }
    stressRealloc(snapshots_);
}

void UndoStack::replaceCurrent(Snapshot s) {
    snapshots_[index_] = std::move(s);
    if (saved_ && *saved_ == index_) saved_.reset();
    stressRealloc(snapshots_);
}

bool UndoStack::undo() {
    if (!canUndo()) return false;
    --index_;
    return true;
}

bool UndoStack::redo() {
    if (!canRedo()) return false;
    ++index_;
    return true;
}

}  // namespace construe::doc
