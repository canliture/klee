#include "klee/Expr/SymbolicSource.h"
#include "klee/Expr/Expr.h"

#include "klee/Expr/ExprPPrinter.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"

DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
DISABLE_WARNING_POP

#include <vector>

namespace klee {
int SymbolicSource::compare(const SymbolicSource &b) const {
  return internalCompare(b);
}

bool SymbolicSource::equals(const SymbolicSource &b) const {
  return compare(b) == 0;
}

void SymbolicSource::print(llvm::raw_ostream &os) const {
  ExprPPrinter::printSignleSource(os, const_cast<SymbolicSource *>(this));
}

void SymbolicSource::dump() const {
  this->print(llvm::errs());
  llvm::errs() << "\n";
}

std::string SymbolicSource::toString() const {
  std::string str;
  llvm::raw_string_ostream output(str);
  this->print(output);
  return str;
}

std::set<const Array *> LazyInitializationSource::getRelatedArrays() const {
  std::vector<const Array *> objects;
  findObjects(pointer, objects);
  return std::set<const Array *>(objects.begin(), objects.end());
}

unsigned ConstantSource::computeHash() {
  unsigned res = 0;
  for (unsigned i = 0, e = constantValues.size(); i != e; ++i) {
    res =
        (res * SymbolicSource::MAGIC_HASH_CONSTANT) + constantValues[i]->hash();
  }
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) + getKind();
  hashValue = res;
  return hashValue;
}

unsigned SymbolicSizeConstantSource::computeHash() {
  unsigned res =
      (getKind() * SymbolicSource::MAGIC_HASH_CONSTANT) + defaultValue;
  hashValue = res;
  return hashValue;
}

unsigned SymbolicSizeConstantAddressSource::computeHash() {
  unsigned res =
      (getKind() * SymbolicSource::MAGIC_HASH_CONSTANT) + defaultValue;
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) + version;
  hashValue = res;
  return hashValue;
}

unsigned MakeSymbolicSource::computeHash() {
  unsigned res = (getKind() * SymbolicSource::MAGIC_HASH_CONSTANT) + version;
  for (unsigned i = 0, e = name.size(); i != e; ++i) {
    res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) + name[i];
  }
  hashValue = res;
  return hashValue;
}

unsigned LazyInitializationSource::computeHash() {
  unsigned res =
      (getKind() * SymbolicSource::MAGIC_HASH_CONSTANT) + pointer->hash();
  hashValue = res;
  return hashValue;
}

int ArgumentSource::internalCompare(const SymbolicSource &b) const {
  if (getKind() != b.getKind()) {
    return getKind() < b.getKind() ? -1 : 1;
  }
  const ArgumentSource &ab = static_cast<const ArgumentSource &>(b);
  if (index != ab.index) {
    return index < ab.index ? -1 : 1;
  }
  assert(km == ab.km);
  auto parent = allocSite.getParent();
  auto bParent = ab.allocSite.getParent();
  if (km->functionIDMap.at(parent) != km->functionIDMap.at(bParent)) {
    return km->functionIDMap.at(parent) < km->functionIDMap.at(bParent) ? -1
                                                                        : 1;
  }
  if (allocSite.getArgNo() != ab.allocSite.getArgNo()) {
    return allocSite.getArgNo() < ab.allocSite.getArgNo() ? -1 : 1;
  }
  return 0;
}

int InstructionSource::internalCompare(const SymbolicSource &b) const {
  if (getKind() != b.getKind()) {
    return getKind() < b.getKind() ? -1 : 1;
  }
  const InstructionSource &ib = static_cast<const InstructionSource &>(b);
  if (index != ib.index) {
    return index < ib.index ? -1 : 1;
  }
  assert(km == ib.km);
  auto function = allocSite.getParent()->getParent();
  auto bFunction = ib.allocSite.getParent()->getParent();
  if (km->functionIDMap.at(function) != km->functionIDMap.at(bFunction)) {
    return km->functionIDMap.at(function) < km->functionIDMap.at(bFunction) ? -1
                                                                            : 1;
  }
  auto kf = km->functionMap.at(function);
  auto block = allocSite.getParent();
  auto bBlock = ib.allocSite.getParent();
  if (kf->blockMap[block]->id != kf->blockMap[bBlock]->id) {
    return kf->blockMap[block]->id < kf->blockMap[bBlock]->id ? -1 : 1;
  }
  if (kf->instructionMap[&allocSite]->index !=
      kf->instructionMap[&ib.allocSite]->index) {
    return kf->instructionMap[&allocSite]->index <
                   kf->instructionMap[&ib.allocSite]->index
               ? -1
               : 1;
  }
  return 0;
}

unsigned ArgumentSource::computeHash() {
  unsigned res = (getKind() * SymbolicSource::MAGIC_HASH_CONSTANT) + index;
  auto parent = allocSite.getParent();
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) +
        km->functionIDMap.at(parent);
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) + allocSite.getArgNo();
  hashValue = res;
  return hashValue;
}

unsigned InstructionSource::computeHash() {
  unsigned res = (getKind() * SymbolicSource::MAGIC_HASH_CONSTANT) + index;
  auto function = allocSite.getParent()->getParent();
  auto kf = km->functionMap.at(function);
  auto block = allocSite.getParent();
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) +
        km->functionIDMap.at(function);
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) + kf->blockMap[block]->id;
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) +
        kf->instructionMap[&allocSite]->index;
  hashValue = res;
  return hashValue;
}

unsigned MockNaiveSource::computeHash() {
  unsigned res = (getKind() * SymbolicSource::MAGIC_HASH_CONSTANT) + version;
  unsigned funcID = km->functionIDMap.at(&function);
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) + funcID;
  hashValue = res;
  return res;
}

int MockNaiveSource::internalCompare(const SymbolicSource &b) const {
  if (getKind() != b.getKind()) {
    return getKind() < b.getKind() ? -1 : 1;
  }
  const MockNaiveSource &mnb = static_cast<const MockNaiveSource &>(b);
  if (version != mnb.version) {
    return version < mnb.version ? -1 : 1;
  }
  unsigned funcID = km->functionIDMap.at(&function);
  unsigned bFuncID = mnb.km->functionIDMap.at(&mnb.function);
  if (funcID != bFuncID) {
    return funcID < bFuncID ? -1 : 1;
  }
  return 0;
}

MockDeterministicSource::MockDeterministicSource(const KModule *km,
                                                 const llvm::Function &function,
                                                 const std::vector<ref<Expr>> &_args)
    : MockSource(km, function), args(_args) {}

unsigned MockDeterministicSource::computeHash() {
  unsigned res = getKind();
  res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) +
        km->functionIDMap.at(&function);
  for (const auto &arg : args) {
    res = (res * SymbolicSource::MAGIC_HASH_CONSTANT) + arg->hash();
  }
  hashValue = res;
  return res;
}

int MockDeterministicSource::internalCompare(const SymbolicSource &b) const {
  if (getKind() != b.getKind()) {
    return getKind() < b.getKind() ? -1 : 1;
  }
  const MockDeterministicSource &mdb =
      static_cast<const MockDeterministicSource &>(b);
  unsigned funcID = km->functionIDMap.at(&function);
  unsigned bFuncID = mdb.km->functionIDMap.at(&mdb.function);
  if (funcID != bFuncID) {
    return funcID < bFuncID ? -1 : 1;
  }
  assert(args.size() == mdb.args.size() &&
         "the same functions should have the same arguments number");
  for (unsigned i = 0; i < args.size(); i++) {
    if (args[i] != mdb.args[i]) {
      return args[i] < mdb.args[i] ? -1 : 1;
    }
  }
  return 0;
}

} // namespace klee
