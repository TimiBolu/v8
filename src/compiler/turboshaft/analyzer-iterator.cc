// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/turboshaft/analyzer-iterator.h"

namespace v8::internal::compiler::turboshaft {

void AnalyzerIterator::PopOutdated() {
  while (!stack_.empty()) {
    if (IsOutdated(stack_.back())) {
      stack_.pop_back();
    } else {
      return;
    }
  }
}

Block* AnalyzerIterator::Next() {
  DCHECK(HasNext());
  DCHECK(!IsOutdated(stack_.back()));
  curr_ = stack_.back();
  stack_.pop_back();

  Block* curr_header = curr_.block->IsLoop()
                           ? curr_.block
                           : loop_finder_.GetLoopHeader(curr_.block);

  // Pushing on the stack the children that are not in the same loop as Next
  // (remember that since we're doing a DFS with a Last-In-First-Out stack,
  // pushing them first on the stack means that they will be visited last).
  for (Block* child = curr_.block->LastChild(); child != nullptr;
       child = child->NeighboringChild()) {
    if (loop_finder_.GetLoopHeader(child) != curr_header) {
      stack_.push_back({child, curr_.generation});
    }
  }

  // Pushing on the stack the children that are in the same loop as Next (they
  // are pushed last, so that they will be visited first).
  for (Block* child = curr_.block->LastChild(); child != nullptr;
       child = child->NeighboringChild()) {
    if (loop_finder_.GetLoopHeader(child) == curr_header) {
      stack_.push_back({child, curr_.generation});
    }
  }

  visited_[curr_.block->index()] = curr_.generation;

  // Note that PopOutdated must be called after updating {visited_}, because
  // this way, if the stack contained initially [{Bx, 1}, {Bx, 2}] (where `Bx`
  // is the same block both time and it hasn't been visited before), then we
  // popped the second entry at the begining of this function, but if we call
  // PopOutdate before updating {visited_}, then it won't pop the first entry.
  PopOutdated();

  return curr_.block;
}

void AnalyzerIterator::MarkLoopForRevisit() {
  DCHECK_NOT_NULL(curr_.block);
  DCHECK_NE(curr_.generation, kNotVisitedGeneration);
  DCHECK(curr_.block->HasBackedge(graph_));
  Block* header = curr_.block->LastOperation(graph_).Cast<GotoOp>().destination;
  stack_.push_back({header, curr_.generation + 1});
}

}  // namespace v8::internal::compiler::turboshaft
