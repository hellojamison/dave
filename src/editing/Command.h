// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"

#include <memory>
#include <string>
#include <vector>

namespace dave::editing {

// Command is a single undoable user action. perform/undo both mutate the Edit;
// after each, Edit::notifyChanged() fires, which re-derives the engine graph.
//
// For multi-step actions (e.g. "drag clip" emitting many micro-moves), group
// them into one Command or use a transaction (RB-later). RB-2 keeps it simple:
// one logical action = one Command.
struct Command {
    virtual ~Command() = default;
    virtual void perform(document::Edit& e) = 0;
    virtual void undo(document::Edit& e) = 0;
    virtual std::string name() const = 0;
};

// UndoStack holds executed commands and supports undo/redo. Each execute()
// performs the command, pushes it, and clears the redo stack (standard behavior
// — a new action after an undo discards the redo branch).
class UndoStack {
public:
    explicit UndoStack(document::Edit& edit) : edit_(edit) {}

    void execute(std::unique_ptr<Command> cmd) {
        cmd->perform(edit_);
        done_.push_back(std::move(cmd));
        redo_.clear();
    }

    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    void undo() {
        if (done_.empty()) return;
        auto cmd = std::move(done_.back());
        done_.pop_back();
        cmd->undo(edit_);
        redo_.push_back(std::move(cmd));
    }

    void redo() {
        if (redo_.empty()) return;
        auto cmd = std::move(redo_.back());
        redo_.pop_back();
        cmd->perform(edit_);
        done_.push_back(std::move(cmd));
    }

    size_t undoDepth() const { return done_.size(); }

    // Clear the undo/redo stacks (used by New/Open Project). Keeps the Edit
    // reference (it's the same Edit, just emptied of history).
    void clear() {
        done_.clear();
        redo_.clear();
    }

private:
    document::Edit& edit_;
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<std::unique_ptr<Command>> redo_;
};

} // namespace dave::editing
