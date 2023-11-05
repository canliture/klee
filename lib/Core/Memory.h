//===-- Memory.h ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_MEMORY_H
#define KLEE_MEMORY_H

#include "MemoryManager.h"
#include "TimingSolver.h"
#include "klee/ADT/Ref.h"
#include "klee/ADT/SparseStorage.h"
#include "klee/Core/Context.h"

#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/SourceBuilder.h"

#include "klee/Support/CompilerWarning.h"
DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/ADT/StringExtras.h"
DISABLE_WARNING_POP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class Value;
}

namespace klee {

class BitArray;
class ExecutionState;
class KType;
class MemoryManager;
class Solver;

typedef uint64_t IDType;

extern llvm::cl::opt<bool> UseTypeBasedAliasAnalysis;

class MemoryObject {
  friend class STPBuilder;
  friend class ObjectState;
  friend class ExecutionState;
  friend class ref<MemoryObject>;
  friend class ref<const MemoryObject>;

private:
  // Counter is using for id's of MemoryObjects.
  //
  // Value 0 is reserved for erroneous objects.
  static IDType counter;
  static int time;
  /// @brief Required by klee::ref-managed objects
  mutable class ReferenceCounter _refCount;

public:
  IDType id;
  mutable unsigned timestamp;

  // std::shared_ptr<void> systemAddress;

  // ref<Expr> segment;
  // ref<Expr> address;
  // ref<Expr> size;

  ref<Expr> segment;

  uint64_t address;
  ref<Expr> addressExpr;

  /// size in bytes
  unsigned size;
  ref<Expr> sizeExpr;

  uint64_t alignment;

  mutable std::string name;

  bool isLocal;
  mutable bool isGlobal;
  bool isFixed;
  bool isLazyInitialized;

  bool isUserSpecified;

  MemoryManager *parent;

  /// "Location" for which this memory object was allocated. This
  /// should be either the allocating instruction or the global object
  /// it was allocated for (or whatever else makes sense).
  const llvm::Value *allocSite;

  // DO NOT IMPLEMENT
  MemoryObject(const MemoryObject &b);
  MemoryObject &operator=(const MemoryObject &b);

public:
  // XXX this is just a temp hack, should be removed
  explicit MemoryObject(uint64_t _address)
      : id(0), timestamp(0), address(_address), addressExpr(nullptr), size(0),
        sizeExpr(nullptr), alignment(0), isFixed(true),
        isLazyInitialized(false), parent(NULL), allocSite(0) {}

  MemoryObject(
      uint64_t _address, unsigned _size, uint64_t alignment, bool _isLocal,
      bool _isGlobal, bool _isFixed, bool _isLazyInitialized,
      const llvm::Value *_allocSite, MemoryManager *_parent,
      ref<Expr> _addressExpr = nullptr, ref<Expr> _sizeExpr = nullptr,
      unsigned _timestamp = 0 /* unused if _isLazyInitialized is false*/,
      ref<Expr> _segmentExpr = nullptr)
      : id(counter++), timestamp(_timestamp), address(_address),
        addressExpr(_addressExpr), size(_size), sizeExpr(_sizeExpr),
        alignment(alignment), name("unnamed"), isLocal(_isLocal),
        isGlobal(_isGlobal), isFixed(_isFixed),
        isLazyInitialized(_isLazyInitialized), isUserSpecified(false),
        parent(_parent), allocSite(_allocSite) {
    if (isLazyInitialized) {
      timestamp = _timestamp;
      segment = _segmentExpr;
    } else {
      timestamp = time++;
      segment = Expr::createPointer(id);
    }
  }

  ~MemoryObject();

  /// Get an identifying string for this allocation.
  void getAllocInfo(std::string &result) const;

  void setName(const std::string &_name) const { this->name = _name; }

  void updateTimestamp() const { this->timestamp = time++; }

  bool hasSymbolicSize() const { return !isa<ConstantExpr>(getSizeExpr()); }
  ref<ConstantExpr> getBaseConstantExpr() const {
    return ConstantExpr::create(address, Context::get().getPointerWidth());
  }
  ref<Expr> getBaseExpr() const {
    if (addressExpr) {
      return addressExpr;
    }
    return getBaseConstantExpr();
  }
  ref<Expr> getSizeExpr() const {
    if (sizeExpr) {
      return sizeExpr;
    }
    return Expr::createPointer(size);
  }
  ref<Expr> getSegmentDiff(ref<Expr> pointer) const {
    return SubExpr::create(pointer, getBaseExpr());
  }
  ref<Expr> getOffsetExpr(ref<Expr> pointer) const {
    return SubExpr::create(pointer, getBaseExpr());
  }
  ref<Expr> getOffsetExpr(ref<PointerExpr> pointer) const {
    return SubExpr::create(pointer->getValue(), getBaseExpr());
  }
  ref<Expr> getBoundsCheckPointer(ref<PointerExpr> pointer) const {
    ref<Expr> segment = pointer->getSegment();
    ref<Expr> address = pointer->getValue();
    return AndExpr::create(EqExpr::create(getBaseExpr(), segment),
                           getBoundsCheckOffset(getOffsetExpr(address)));
  }
  ref<Expr> getBoundsCheckPointer(ref<PointerExpr> pointer,
                                  unsigned bytes) const {
    ref<Expr> segment = pointer->getSegment();
    ref<Expr> address = pointer->getValue();
    return AndExpr::create(EqExpr::create(getBaseExpr(), segment),
                           getBoundsCheckOffset(getOffsetExpr(address), bytes));
  }

  ref<Expr> getBoundsCheckOffset(ref<Expr> offset) const {
    ref<Expr> isZeroSizeExpr =
        EqExpr::create(Expr::createPointer(0), getSizeExpr());
    ref<Expr> isZeroOffsetExpr = EqExpr::create(Expr::createPointer(0), offset);
    return SelectExpr::create(isZeroSizeExpr, isZeroOffsetExpr,
                              UltExpr::create(offset, getSizeExpr()));
  }

  ref<Expr> getBoundsCheckOffset(ref<Expr> offset, unsigned bytes) const {
    ref<Expr> offsetSizeCheck =
        UleExpr::create(Expr::createPointer(bytes), getSizeExpr());
    ref<Expr> trueExpr = UltExpr::create(
        offset, AddExpr::create(
                    SubExpr::create(getSizeExpr(), Expr::createPointer(bytes)),
                    Expr::createPointer(1)));
    return SelectExpr::create(offsetSizeCheck, trueExpr, Expr::createFalse());
  }

  /// Compare this object with memory object b.
  /// \param b memory object to compare with
  /// \return <0 if this is smaller, 0 if both are equal, >0 if b is smaller
  int compare(const MemoryObject &b) const {
    // Short-cut with id
    if (id == b.id)
      return 0;
    if (address != b.address)
      return (address < b.address ? -1 : 1);

    if (size != b.size)
      return (size < b.size ? -1 : 1);

    if (allocSite != b.allocSite)
      return (allocSite < b.allocSite ? -1 : 1);

    assert(isLazyInitialized == b.isLazyInitialized);
    return 0;
  }

  bool equals(const MemoryObject &b) const { return compare(b) == 0; }
};

class ObjectStage {
private:
  /// knownSymbolics[byte] holds the expression for byte,
  /// if byte is known
  mutable SparseStorage<ref<Expr>, OptionalRefEq<Expr>> knownSymbolics;

  /// unflushedMask[byte] is set if byte is unflushed
  /// mutable because may need flushed during read of const
  mutable SparseStorage<bool> unflushedMask;

  // mutable because we may need flush during read of const
  mutable UpdateList updates;

  ref<Expr> size;
  bool safeRead;

public:
  ObjectStage(const Array *array, ref<Expr> defaultValue, bool safe = true);
  ObjectStage(ref<Expr> size, ref<Expr> defaultValue, bool safe = true);

  ObjectStage(const ObjectStage &os);
  ~ObjectStage() = default;

  ref<Expr> read8(unsigned offset) const;
  ref<Expr> read8(ref<Expr> offset) const;
  void write8(unsigned offset, ref<Expr> value);
  void write8(ref<Expr> offset, ref<Expr> value);
  void write(const ObjectStage &os);

  void write(unsigned offset, ref<Expr> value);
  void write(ref<Expr> offset, ref<Expr> value);

  void write8(unsigned offset, uint8_t value);
  void write16(unsigned offset, uint16_t value);
  void write32(unsigned offset, uint32_t value);
  void write64(unsigned offset, uint64_t value);
  void print() const;

private:
  const UpdateList &getUpdates() const;

  void makeConcrete();

  void flushForRead() const;
  void flushForWrite();
};

class ObjectState {
private:
  friend class AddressSpace;
  friend class ref<ObjectState>;
  friend class ref<const ObjectState>;

  unsigned copyOnWriteOwner; // exclusively for AddressSpace

  /// @brief Required by klee::ref-managed objects
  mutable class ReferenceCounter _refCount;

  ref<const MemoryObject> object;

  ObjectStage valueOS;
  ObjectStage segmentOS;
  ObjectStage baseOS;

  ref<UpdateNode> lastUpdate;

  ref<Expr> size;

  KType *dynamicType;

public:
  bool readOnly;

  /// Create a new object state for the given memory
  // For objects in memory
  ObjectState(const MemoryObject *mo, const Array *array, KType *dt);
  ObjectState(const MemoryObject *mo, KType *dt);

  // For symbolic objects not in memory (hack)

  ObjectState(const ObjectState &os);
  ~ObjectState() = default;

  const MemoryObject *getObject() const { return object.get(); }

  void setReadOnly(bool ro) { readOnly = ro; }

  void swapObjectHack(MemoryObject *mo) { object = mo; }

  ref<Expr> read(ref<Expr> offset, Expr::Width width) const;
  ref<Expr> read(unsigned offset, Expr::Width width) const;
  ref<Expr> read8(unsigned offset) const;
  ref<Expr> readValue8(unsigned offset) const;

  void write(unsigned offset, ref<Expr> value);
  void write(ref<Expr> offset, ref<Expr> value);
  void write(ref<const ObjectState> os);

  void write8(unsigned offset, uint8_t value);
  void write16(unsigned offset, uint16_t value);
  void write32(unsigned offset, uint32_t value);
  void write64(unsigned offset, uint64_t value);
  void print() const;

  bool isAccessableFrom(KType *) const;

  KType *getDynamicType() const;

private:
  ref<Expr> read8(ref<Expr> offset) const;
  void write8(unsigned offset, ref<Expr> value);
  void write8(ref<Expr> offset, ref<Expr> value);
};

} // namespace klee

#endif /* KLEE_MEMORY_H */
