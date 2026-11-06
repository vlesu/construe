// Undo/redo snapshot stack (spec/document.md §6).
#pragma once

#include "doc/document.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::doc {

inline constexpr size_t UNDO_DEPTH = 200;

struct Snapshot {
    Document document;
    std::string label;               // key undo.<command>
    std::optional<std::string> editSketch;  // editing context
    std::string labelName;           // params.name of the step title: name of the affected feature/variable or empty
};

class UndoStack {
public:
    explicit UndoStack(Snapshot initial);

    const Snapshot& current() const { return snapshots_[index_]; }
    // New snapshot: the redo branch is discarded; on overflow — the oldest one.
    void push(Snapshot s);
    // Replace the content of the current snapshot (within a command group).
    void replaceCurrent(Snapshot s);
    // Active element of the current snapshot (document.md §6): changing the element does not create a snapshot.
    void setCurrentActiveElement(const std::string& id) { snapshots_[index_].document.activeElement = id; }
    bool canUndo() const { return index_ > 0; }
    bool canRedo() const { return index_ + 1 < snapshots_.size(); }
    bool undo();
    bool redo();
    std::string undoLabel() const { return canUndo() ? snapshots_[index_].label : std::string(); }
    std::string undoLabelName() const { return canUndo() ? snapshots_[index_].labelName : std::string(); }
    std::string redoLabel() const { return canRedo() ? snapshots_[index_ + 1].label : std::string(); }
    std::string redoLabelName() const { return canRedo() ? snapshots_[index_ + 1].labelName : std::string(); }

    void markSaved() { saved_ = index_; }
    void markUnsaved() { saved_.reset(); }
    bool modified() const { return !saved_ || *saved_ != index_; }
    size_t size() const { return snapshots_.size(); }
    size_t index() const { return index_; }  // steps that can be undone

private:
    std::vector<Snapshot> snapshots_;
    size_t index_ = 0;
    std::optional<size_t> saved_;
};

}  // namespace construe::doc
