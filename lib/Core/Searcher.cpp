//===-- Searcher.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Searcher.h"

#include "CoreStats.h"
#include "ExecutionState.h"
#include "Executor.h"
#include "PTree.h"
#include "StatsTracker.h"
#include "TargetCalculator.h"

#include "klee/ADT/DiscretePDF.h"
#include "klee/ADT/RNG.h"
#include "klee/ADT/WeightedQueue.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/Module/Target.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/System/Time.h"
#include "klee/Utilities/Math.h"

#include "klee/Support/CompilerWarning.h"
DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
DISABLE_WARNING_POP

#include <cassert>
#include <cmath>
#include <set>

using namespace klee;
using namespace llvm;

///

ExecutionState &DFSSearcher::selectState() { return *states.back(); }

void DFSSearcher::update(ExecutionState *current,
                         const StateIterable &addedStates,
                         const StateIterable &removedStates) {
  // insert states
  states.insert(states.end(), addedStates.begin(), addedStates.end());

  // remove states
  for (const auto state : removedStates) {
    if (state == states.back()) {
      states.pop_back();
    } else {
      auto it = std::find(states.begin(), states.end(), state);
      assert(it != states.end() && "invalid state removed");
      states.erase(it);
    }
  }
}

bool DFSSearcher::empty() { return states.empty(); }

void DFSSearcher::printName(llvm::raw_ostream &os) { os << "DFSSearcher\n"; }

///

ExecutionState &BFSSearcher::selectState() { return *states.front(); }

void BFSSearcher::update(ExecutionState *current,
                         const StateIterable &addedStates,
                         const StateIterable &removedStates) {
  // update current state
  // Assumption: If new states were added KLEE forked, therefore states evolved.
  // constraints were added to the current state, it evolved.
  if (!addedStates.empty() && current &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end()) {
    auto pos = std::find(states.begin(), states.end(), current);
    assert(pos != states.end());
    states.erase(pos);
    states.push_back(current);
  }

  // insert states
  states.insert(states.end(), addedStates.begin(), addedStates.end());

  // remove states
  for (const auto state : removedStates) {
    if (state == states.front()) {
      states.pop_front();
    } else {
      auto it = std::find(states.begin(), states.end(), state);
      assert(it != states.end() && "invalid state removed");
      states.erase(it);
    }
  }
}

bool BFSSearcher::empty() { return states.empty(); }

void BFSSearcher::printName(llvm::raw_ostream &os) { os << "BFSSearcher\n"; }

///

RandomSearcher::RandomSearcher(RNG &rng) : theRNG{rng} {}

ExecutionState &RandomSearcher::selectState() {
  return *states[theRNG.getInt32() % states.size()];
}

void RandomSearcher::update(ExecutionState *current,
                            const StateIterable &addedStates,
                            const StateIterable &removedStates) {
  // insert states
  states.insert(states.end(), addedStates.begin(), addedStates.end());

  // remove states
  for (const auto state : removedStates) {
    auto it = std::find(states.begin(), states.end(), state);
    assert(it != states.end() && "invalid state removed");
    states.erase(it);
  }
}

bool RandomSearcher::empty() { return states.empty(); }

void RandomSearcher::printName(llvm::raw_ostream &os) {
  os << "RandomSearcher\n";
}

///

TargetedSearcher::~TargetedSearcher() {}

bool TargetedSearcher::empty() { return states->empty(); }

void TargetedSearcher::printName(llvm::raw_ostream &os) {
  os << "TargetedSearcher";
}

TargetedSearcher::TargetedSearcher(ref<Target> target,
                                   DistanceCalculator &_distanceCalculator)
    : states(std::make_unique<
             WeightedQueue<ExecutionState *, ExecutionStateIDCompare>>()),
      target(target), distanceCalculator(_distanceCalculator) {}

ExecutionState &TargetedSearcher::selectState() { return *states->choose(0); }

void TargetedSearcher::update(ExecutionState *current,
                              const StateIterable &addedStates,
                              const StateIterable &removedStates) {

  // update current
  if (current && std::find(removedStates.begin(), removedStates.end(),
                           current) == removedStates.end())
    states->update(current, getWeight(current));

  // insert states
  for (const auto state : addedStates)
    states->insert(state, getWeight(state));

  // remove states
  for (const auto state : removedStates)
    states->remove(state);
}

weight_type TargetedSearcher::getWeight(ExecutionState *es) {
  KBlock *kb = es->pc->parent;
  KInstruction *ki = es->pc;
  weight_type weight;
  if (!target->shouldFailOnThisTarget() && kb->getNumInstructions() &&
      !isa<KCallBlock>(kb) && kb->getFirstInstruction() != ki &&
      states->tryGetWeight(es, weight)) {
    return weight;
  }
  auto distRes = distanceCalculator.getDistance(*es, target->getBlock());
  weight = klee::util::ulog2(distRes.weight + 1); // [0, 32)
  if (!distRes.isInsideFunction) {
    weight += 32; // [32, 64)
  }
  return weight;
}

///

ExecutionState &GuidedSearcher::selectState() {
  unsigned size = historiesAndTargets.size();
  interleave ^= 1;
  ExecutionState *state = nullptr;
  if (interleave || !size) {
    state = &baseSearcher->selectState();
  } else {
    index = theRNG.getInt32() % size;
    auto &historyTargetPair = historiesAndTargets[index];
    ref<const TargetsHistory> history = historyTargetPair.first;
    ref<Target> target = historyTargetPair.second;
    assert(targetedSearchers.find({history, target}) !=
               targetedSearchers.end() &&
           targetedSearchers.at({history, target}) &&
           !targetedSearchers.at({history, target})->empty());
    state = &targetedSearchers.at({history, target})->selectState();
  }
  return *state;
}

void GuidedSearcher::update(ExecutionState *current,
                            const StateIterable &addedStates,
                            const StateIterable &removedStates) {

  if (current) {
    ref<const TargetsHistory> history = current->history();
    const auto &targets = current->targets();
    for (auto target : targets) {
      localHistoryTargets.insert({history, target});
      currTargets.insert({history, target});
    }
  }

  for (const auto state : addedStates) {
    ref<const TargetsHistory> history = state->history();
    const auto &targets = state->targets();
    for (auto target : targets) {
      localHistoryTargets.insert({history, target});
      addedTStates[{history, target}].push_back(state);
    }
  }

  for (const auto state : removedStates) {
    ref<const TargetsHistory> history = state->history();
    const auto &targets = state->targets();
    for (auto target : targets) {
      localHistoryTargets.insert({history, target});
      removedTStates[{history, target}].push_back(state);
    }
  }

  for (auto historyTarget : localHistoryTargets) {
    ref<const TargetsHistory> history = historyTarget.first;
    ref<Target> target = historyTarget.second;

    ExecutionState *currTState =
        currTargets.count({history, target}) != 0 ? current : nullptr;

    if (!isThereTarget(history, target)) {
      addTarget(history, target);
    }

    targetedSearchers.at({history, target})
        ->update(currTState, addedTStates[{history, target}],
                 removedTStates[{history, target}]);
    addedTStates.at({history, target}).clear();
    removedTStates.at({history, target}).clear();
    if (targetedSearchers.at({history, target})->empty()) {
      removeTarget(history, target);
    }
  }
  localHistoryTargets.clear();
  currTargets.clear();

  if (baseSearcher) {
    baseSearcher->update(current, addedStates, removedStates);
  }
}

void GuidedSearcher::update(const TargetHistoryTargetPairToStatesMap &added,
                            const TargetHistoryTargetPairToStatesMap &removed) {
  for (const auto &pair : added) {
    if (!pair.second.empty())
      localHistoryTargets.insert(pair.first);
  }
  for (const auto &pair : removed) {
    if (!pair.second.empty())
      localHistoryTargets.insert(pair.first);
  }

  for (auto historyTarget : localHistoryTargets) {
    ref<const TargetsHistory> history = historyTarget.first;
    ref<Target> target = historyTarget.second;

    if (!isThereTarget(history, target)) {
      addTarget(history, target);
    }

    targetedSearchers.at({history, target})
        ->update(nullptr, added.at({history, target}),
                 removed.at({history, target}));
    if (targetedSearchers.at({history, target})->empty()) {
      removeTarget(history, target);
    }
  }
  localHistoryTargets.clear();
}

bool GuidedSearcher::isThereTarget(ref<const TargetsHistory> history,
                                   ref<Target> target) {
  return targetedSearchers.count({history, target}) != 0;
}

void GuidedSearcher::addTarget(ref<const TargetsHistory> history,
                               ref<Target> target) {
  assert(targetedSearchers.count({history, target}) == 0);
  targetedSearchers[{history, target}] =
      std::make_unique<TargetedSearcher>(target, distanceCalculator);
  assert(std::find_if(
             historiesAndTargets.begin(), historiesAndTargets.end(),
             [&history, &target](const std::pair<ref<const TargetsHistory>,
                                                 ref<Target>> &element) {
               return element.first.get() == history.get() &&
                      element.second.get() == target.get();
             }) == historiesAndTargets.end());
  historiesAndTargets.push_back({history, target});
}

void GuidedSearcher::removeTarget(ref<const TargetsHistory> history,
                                  ref<Target> target) {
  targetedSearchers.erase({history, target});
  auto it = std::find_if(
      historiesAndTargets.begin(), historiesAndTargets.end(),
      [&history, &target](
          const std::pair<ref<const TargetsHistory>, ref<Target>> &element) {
        return element.first.get() == history.get() &&
               element.second.get() == target.get();
      });
  assert(it != historiesAndTargets.end());
  historiesAndTargets.erase(it);
}

bool GuidedSearcher::empty() { return baseSearcher->empty(); }

void GuidedSearcher::printName(llvm::raw_ostream &os) {
  os << "GuidedSearcher\n";
}

///

WeightedRandomSearcher::WeightedRandomSearcher(WeightType type, RNG &rng)
    : states(std::make_unique<
             DiscretePDF<ExecutionState *, ExecutionStateIDCompare>>()),
      theRNG{rng}, type(type) {

  switch (type) {
  case Depth:
  case RP:
    updateWeights = false;
    break;
  case InstCount:
  case CPInstCount:
  case QueryCost:
  case MinDistToUncovered:
  case CoveringNew:
    updateWeights = true;
    break;
  default:
    assert(0 && "invalid weight type");
  }
}

ExecutionState &WeightedRandomSearcher::selectState() {
  return *states->choose(theRNG.getDoubleL());
}

double WeightedRandomSearcher::getWeight(ExecutionState *es) {
  switch (type) {
  default:
  case Depth:
    return es->depth;
  case RP:
    return std::pow(0.5, es->depth);
  case InstCount: {
    uint64_t count = theStatisticManager->getIndexedValue(
        stats::instructions, es->pc->getGlobalIndex());
    double inv = 1. / std::max((uint64_t)1, count);
    return inv * inv;
  }
  case CPInstCount: {
    const InfoStackFrame &sf = es->stack.infoStack().back();
    uint64_t count = sf.callPathNode->statistics.getValue(stats::instructions);
    double inv = 1. / std::max((uint64_t)1, count);
    return inv;
  }
  case QueryCost:
    return (es->queryMetaData.queryCost.toSeconds() < .1)
               ? 1.
               : 1. / es->queryMetaData.queryCost.toSeconds();
  case CoveringNew:
  case MinDistToUncovered: {
    uint64_t md2u = computeMinDistToUncovered(
        es->pc, es->stack.infoStack().back().minDistToUncoveredOnReturn);

    double invMD2U = 1. / (md2u ? md2u : 10000);
    if (type == CoveringNew) {
      double invCovNew = 0.;
      if (es->instsSinceCovNew)
        invCovNew = 1. / std::max(1, (int)es->instsSinceCovNew - 1000);
      return (invCovNew * invCovNew + invMD2U * invMD2U);
    } else {
      return invMD2U * invMD2U;
    }
  }
  }
}

void WeightedRandomSearcher::update(ExecutionState *current,
                                    const StateIterable &addedStates,
                                    const StateIterable &removedStates) {

  // update current
  if (current && updateWeights &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end())
    states->update(current, getWeight(current));

  // insert states
  for (const auto state : addedStates)
    states->insert(state, getWeight(state));

  // remove states
  for (const auto state : removedStates)
    states->remove(state);
}

bool WeightedRandomSearcher::empty() { return states->empty(); }

void WeightedRandomSearcher::printName(llvm::raw_ostream &os) {
  os << "WeightedRandomSearcher::";
  switch (type) {
  case Depth:
    os << "Depth\n";
    return;
  case RP:
    os << "RandomPath\n";
    return;
  case QueryCost:
    os << "QueryCost\n";
    return;
  case InstCount:
    os << "InstCount\n";
    return;
  case CPInstCount:
    os << "CPInstCount\n";
    return;
  case MinDistToUncovered:
    os << "MinDistToUncovered\n";
    return;
  case CoveringNew:
    os << "CoveringNew\n";
    return;
  default:
    os << "<unknown type>\n";
    return;
  }
}

///

// Check if n is a valid pointer and a node belonging to us
#define IS_OUR_NODE_VALID(n)                                                   \
  (((n).getPointer() != nullptr) && (((n).getInt() & idBitMask) != 0))

RandomPathSearcher::RandomPathSearcher(PForest &processForest, RNG &rng)
    : processForest{processForest}, theRNG{rng},
      idBitMask{processForest.getNextId()} {};

ExecutionState &RandomPathSearcher::selectState() {
  unsigned flips = 0, bits = 0, range = 0;
  PTreeNodePtr *root = nullptr;
  while (!root || !IS_OUR_NODE_VALID(*root))
    root = &processForest.getPTrees()
                .at(range++ % processForest.getPTrees().size() + 1)
                ->root;
  assert(root->getInt() & idBitMask && "Root should belong to the searcher");
  PTreeNode *n = root->getPointer();
  while (!n->state) {
    if (!IS_OUR_NODE_VALID(n->left)) {
      assert(IS_OUR_NODE_VALID(n->right) &&
             "Both left and right nodes invalid");
      assert(n != n->right.getPointer());
      n = n->right.getPointer();
    } else if (!IS_OUR_NODE_VALID(n->right)) {
      assert(IS_OUR_NODE_VALID(n->left) && "Both right and left nodes invalid");
      assert(n != n->left.getPointer());
      n = n->left.getPointer();
    } else {
      if (bits == 0) {
        flips = theRNG.getInt32();
        bits = 32;
      }
      --bits;
      n = ((flips & (1U << bits)) ? n->left : n->right).getPointer();
    }
  }

  return *n->state;
}

void RandomPathSearcher::update(ExecutionState *current,
                                const StateIterable &addedStates,
                                const StateIterable &removedStates) {
  // insert states
  for (auto es : addedStates) {
    PTreeNode *pnode = es->ptreeNode, *parent = pnode->parent;
    PTreeNodePtr &root = processForest.getPTrees().at(pnode->getTreeID())->root;
    PTreeNodePtr *childPtr;

    childPtr = parent ? ((parent->left.getPointer() == pnode) ? &parent->left
                                                              : &parent->right)
                      : &root;
    while (pnode && !IS_OUR_NODE_VALID(*childPtr)) {
      childPtr->setInt(childPtr->getInt() | idBitMask);
      pnode = parent;
      if (pnode)
        parent = pnode->parent;

      childPtr = parent
                     ? ((parent->left.getPointer() == pnode) ? &parent->left
                                                             : &parent->right)
                     : &root;
    }
  }

  // remove states
  for (auto es : removedStates) {
    PTreeNode *pnode = es->ptreeNode, *parent = pnode->parent;
    PTreeNodePtr &root = processForest.getPTrees().at(pnode->getTreeID())->root;

    while (pnode && !IS_OUR_NODE_VALID(pnode->left) &&
           !IS_OUR_NODE_VALID(pnode->right)) {
      auto childPtr =
          parent ? ((parent->left.getPointer() == pnode) ? &parent->left
                                                         : &parent->right)
                 : &root;
      assert(IS_OUR_NODE_VALID(*childPtr) && "Removing pTree child not ours");
      childPtr->setInt(childPtr->getInt() & ~idBitMask);
      pnode = parent;
      if (pnode)
        parent = pnode->parent;
    }
  }
}

bool RandomPathSearcher::empty() {
  bool res = true;
  for (const auto &ntree : processForest.getPTrees())
    res = res && !IS_OUR_NODE_VALID(ntree.second->root);
  return res;
}

void RandomPathSearcher::printName(llvm::raw_ostream &os) {
  os << "RandomPathSearcher\n";
}

///

BatchingSearcher::BatchingSearcher(Searcher *baseSearcher,
                                   time::Span timeBudget,
                                   unsigned instructionBudget)
    : baseSearcher{baseSearcher}, timeBudgetEnabled{timeBudget},
      timeBudget{timeBudget}, instructionBudgetEnabled{instructionBudget > 0},
      instructionBudget{instructionBudget} {};

bool BatchingSearcher::withinTimeBudget() const {
  return !timeBudgetEnabled ||
         (time::getWallTime() - lastStartTime) <= timeBudget;
}

bool BatchingSearcher::withinInstructionBudget() const {
  return !instructionBudgetEnabled ||
         (stats::instructions - lastStartInstructions) <= instructionBudget;
}

ExecutionState &BatchingSearcher::selectState() {
  if (lastState && withinTimeBudget() && withinInstructionBudget()) {
    // return same state for as long as possible
    return *lastState;
  }

  // ensure time budget is larger than time between two calls (for same state)
  if (lastState && timeBudgetEnabled) {
    time::Span delta = time::getWallTime() - lastStartTime;
    auto t = timeBudget;
    t *= 1.1;
    if (delta > t) {
      klee_message("increased time budget from %f to %f\n",
                   timeBudget.toSeconds(), delta.toSeconds());
      timeBudget = delta;
    }
  }

  // pick a new state
  lastState = &baseSearcher->selectState();
  if (timeBudgetEnabled) {
    lastStartTime = time::getWallTime();
  }
  if (instructionBudgetEnabled) {
    lastStartInstructions = stats::instructions;
  }
  return *lastState;
}

void BatchingSearcher::update(ExecutionState *current,
                              const StateIterable &addedStates,
                              const StateIterable &removedStates) {
  // drop memoized state if it is marked for deletion
  if (std::find(removedStates.begin(), removedStates.end(), lastState) !=
      removedStates.end())
    lastState = nullptr;
  // update underlying searcher
  baseSearcher->update(current, addedStates, removedStates);
}

bool BatchingSearcher::empty() { return baseSearcher->empty(); }

void BatchingSearcher::printName(llvm::raw_ostream &os) {
  os << "<BatchingSearcher> timeBudget: " << timeBudget
     << ", instructionBudget: " << instructionBudget << ", baseSearcher:\n";
  baseSearcher->printName(os);
  os << "</BatchingSearcher>\n";
}

///

class TimeMetric final : public IterativeDeepeningSearcher::Metric {
  time::Point startTime;
  time::Span time{time::seconds(1)};

public:
  void selectState() final { startTime = time::getWallTime(); }
  bool exceeds(const ExecutionState &state) const final {
    return time::getWallTime() - startTime > time;
  }
  void increaseLimit() final {
    time *= 2U;
    klee_message("increased time budget to %f seconds", time.toSeconds());
  }
};

class MaxCyclesMetric final : public IterativeDeepeningSearcher::Metric {
public:
  using ty = unsigned long long;

private:
  ty maxCycles;

public:
  explicit MaxCyclesMetric(ty maxCycles) : maxCycles(maxCycles){};
  explicit MaxCyclesMetric() : MaxCyclesMetric(1ULL){};

  bool exceeds(const ExecutionState &state) const final {
    return state.isCycled(maxCycles);
  }
  void increaseLimit() final {
    maxCycles *= 4ULL;
    klee_message("increased max-cycles to %llu", maxCycles);
  }
};

IterativeDeepeningSearcher::IterativeDeepeningSearcher(
    Searcher *baseSearcher, TargetManagerSubscriber *tms,
    HaltExecution::Reason m)
    : baseSearcher{baseSearcher}, tms{tms} {
  switch (m) {
  case HaltExecution::Reason::MaxTime:
    metric = std::make_unique<TimeMetric>();
    return;
  case HaltExecution::Reason::MaxCycles:
    metric = std::make_unique<MaxCyclesMetric>();
    return;
  default:
    klee_error("Illegal metric for iterative deepening searcher: %d", m);
  }
}

ExecutionState &IterativeDeepeningSearcher::selectState() {
  ExecutionState &res = baseSearcher->selectState();
  metric->selectState();
  return res;
}

void IterativeDeepeningSearcher::update(
    const TargetHistoryTargetPairToStatesMap &added,
    const TargetHistoryTargetPairToStatesMap &removed) {
  if (!tms)
    return;
  added.setWithout(&pausedStates);
  removed.setWithout(&pausedStates);
  tms->update(added, removed);
  added.clearWithout();
  removed.clearWithout();
}

void IterativeDeepeningSearcher::update(ExecutionState *current,
                                        const StateIterable &added,
                                        const StateIterable &removed) {
  removed.setWithout(&pausedStates);
  baseSearcher->update(current, added, removed);
  removed.clearWithout();

  for (auto state : removed)
    pausedStates.erase(state);

  if (current &&
      std::find(removed.begin(), removed.end(), current) == removed.end() &&
      metric->exceeds(*current)) {
    pausedStates.insert(current);
    baseSearcher->update(nullptr, {}, {current});
  }

  // no states left in underlying searcher: fill with paused states
  if (baseSearcher->empty() && !pausedStates.empty()) {
    metric->increaseLimit();
    baseSearcher->update(nullptr, pausedStates, {});
    pausedStates.clear();
  }
}

bool IterativeDeepeningSearcher::empty() {
  return baseSearcher->empty() && pausedStates.empty();
}

void IterativeDeepeningSearcher::printName(llvm::raw_ostream &os) {
  os << "IterativeDeepeningSearcher\n";
}

///

InterleavedSearcher::InterleavedSearcher(
    const std::vector<Searcher *> &_searchers) {
  searchers.reserve(_searchers.size());
  for (auto searcher : _searchers)
    searchers.emplace_back(searcher);
}

ExecutionState &InterleavedSearcher::selectState() {
  Searcher *s = searchers[--index].get();
  if (index == 0)
    index = searchers.size();
  return s->selectState();
}

void InterleavedSearcher::update(ExecutionState *current,
                                 const StateIterable &addedStates,
                                 const StateIterable &removedStates) {

  // update underlying searchers
  for (auto &searcher : searchers)
    searcher->update(current, addedStates, removedStates);
}

bool InterleavedSearcher::empty() { return searchers[0]->empty(); }

void InterleavedSearcher::printName(llvm::raw_ostream &os) {
  os << "<InterleavedSearcher> containing " << searchers.size()
     << " searchers:\n";
  for (const auto &searcher : searchers)
    searcher->printName(os);
  os << "</InterleavedSearcher>\n";
}

///

BlockLevelSearcher::BlockLevelSearcher(RNG &rng) : theRNG{rng} {}

ExecutionState &BlockLevelSearcher::selectState() {
  unsigned rnd = 0;
  unsigned index = 0;
  unsigned mod = 10;
  unsigned border = 9;

  auto kfi = data.begin();
  index = theRNG.getInt32() % data.size();
  std::advance(kfi, index);
  auto &sizesTo = kfi->second;

  for (auto &sizesSize : sizesTo) {
    rnd = theRNG.getInt32();
    if (rnd % mod < border) {
      for (auto &size : sizesSize.second) {
        rnd = theRNG.getInt32();
        if (rnd % mod < border) {
          auto lbi = size.second.begin();
          index = theRNG.getInt32() % size.second.size();
          std::advance(lbi, index);
          auto &level = *lbi;
          auto si = level.second.begin();
          index = theRNG.getInt32() % level.second.size();
          std::advance(si, index);
          auto &state = *si;
          return *state;
        }
      }
    }
  }

  return **(sizesTo.begin()->second.begin()->second.begin()->second.begin());
}

void BlockLevelSearcher::clear(ExecutionState &state) {
  KFunction *kf = state.initPC->parent->parent;
  BlockLevel &bl = stateToBlockLevel[&state];
  auto &sizeTo = data[kf];
  auto &sizesTo = sizeTo[bl.sizeOfLevel];
  auto &levelTo = sizesTo[bl.sizesOfFrameLevels];
  auto &states = levelTo[bl.maxMultilevel];

  states.erase(&state);
  if (states.size() == 0) {
    levelTo.erase(bl.maxMultilevel);
  }
  if (levelTo.size() == 0) {
    sizesTo.erase(bl.sizesOfFrameLevels);
  }
  if (sizesTo.size() == 0) {
    sizeTo.erase(bl.sizeOfLevel);
  }
  if (sizeTo.size() == 0) {
    data.erase(kf);
  }
}

void BlockLevelSearcher::update(ExecutionState *current,
                                const StateIterable &addedStates,
                                const StateIterable &removedStates) {
  if (current && std::find(removedStates.begin(), removedStates.end(),
                           current) == removedStates.end()) {
    KFunction *kf = current->initPC->parent->parent;
    BlockLevel &bl = stateToBlockLevel[current];
    sizes.clear();
    unsigned long long maxMultilevel = 0u;
    for (auto &infoFrame : current->stack.infoStack()) {
      sizes.push_back(infoFrame.level.size());
      maxMultilevel = std::max(maxMultilevel, infoFrame.maxMultilevel);
    }
    for (auto &kfLevel : current->stack.multilevel) {
      maxMultilevel = std::max(maxMultilevel, kfLevel.second);
    }
    if (sizes != bl.sizesOfFrameLevels ||
        current->level.size() != bl.sizeOfLevel ||
        maxMultilevel != bl.maxMultilevel) {
      clear(*current);

      data[kf][current->level.size()][sizes][maxMultilevel].insert(current);

      stateToBlockLevel[current] =
          BlockLevel(kf, current->level.size(), sizes, maxMultilevel);
    }
  }

  for (const auto state : addedStates) {
    KFunction *kf = state->initPC->parent->parent;

    sizes.clear();
    unsigned long long maxMultilevel = 0u;
    for (auto &infoFrame : state->stack.infoStack()) {
      sizes.push_back(infoFrame.level.size());
      maxMultilevel = std::max(maxMultilevel, infoFrame.maxMultilevel);
    }
    for (auto &kfLevel : state->stack.multilevel) {
      maxMultilevel = std::max(maxMultilevel, kfLevel.second);
    }

    data[kf][state->level.size()][sizes][maxMultilevel].insert(state);

    stateToBlockLevel[state] =
        BlockLevel(kf, state->level.size(), sizes, maxMultilevel);
  }

  // remove states
  for (const auto state : removedStates) {
    clear(*state);
    stateToBlockLevel.erase(state);
  }
}

bool BlockLevelSearcher::empty() { return stateToBlockLevel.empty(); }

void BlockLevelSearcher::printName(llvm::raw_ostream &os) {
  os << "BlockLevelSearcher\n";
}
