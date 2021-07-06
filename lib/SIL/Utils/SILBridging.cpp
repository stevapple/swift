//===--- SILBridgingUtils.cpp - Utilities for swift bridging --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILNode.h"
#include "swift/SIL/SILBridgingUtils.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILBuilder.h"

using namespace swift;

namespace {

bool nodeMetatypesInitialized = false;

// Filled in by class registration in initializeLibSwift().
SwiftMetatype nodeMetatypes[(unsigned)SILNodeKind::Last_SILNode + 1];

}

// Does return null, if libswift is not used, i.e. initializeLibSwift() is
// never called.
SwiftMetatype SILNode::getSILNodeMetatype(SILNodeKind kind) {
  SwiftMetatype metatype = nodeMetatypes[(unsigned)kind];
  assert((!nodeMetatypesInitialized || metatype) &&
        "no metatype for bridged SIL node");
  return metatype;
}

static_assert(sizeof(BridgedLocation) == sizeof(SILDebugLocation),
              "BridgedLocation has wrong size");

/// Fills \p storage with all Values from the bridged \p values array.
ArrayRef<SILValue> swift::getSILValues(BridgedValueArray values,
                                       SmallVectorImpl<SILValue> &storage) {
  auto *base = reinterpret_cast<const SwiftObject *>(values.data);

  // The bridged array contains class existentials, which have a layout of two
  // words. The first word is the actual object. Pick the objects and store them
  // into storage.
  for (unsigned idx = 0; idx < values.count; ++idx) {
    storage.push_back(castToSILValue({base[idx * 2]}));
  }
  return storage;
}

//===----------------------------------------------------------------------===//
//                          Class registration
//===----------------------------------------------------------------------===//

static llvm::StringMap<SILNodeKind> valueNamesToKind;
static llvm::SmallPtrSet<SwiftMetatype, 4> unimplementedTypes;

// Utility to fill in a metatype of an "unimplemented" class for a whole range
// of class types.
static void setUnimplementedRange(SwiftMetatype metatype,
                                  SILNodeKind from, SILNodeKind to) {
  unimplementedTypes.insert(metatype);
  for (unsigned kind = (unsigned)from; kind <= (unsigned)to; ++kind) {
    assert((!nodeMetatypes[kind] || unimplementedTypes.count(metatype)) &&
           "unimplemented nodes must be registered first");
    nodeMetatypes[kind] = metatype;
  }
}

/// Registers the metatype of a libswift class.
/// Called by initializeLibSwift().
void registerBridgedClass(BridgedStringRef className, SwiftMetatype metatype) {
  nodeMetatypesInitialized = true;

  // Handle the important non Node classes.
  StringRef clName = getStringRef(className);
  if (clName == "Function")
    return SILFunction::registerBridgedMetatype(metatype);
  if (clName == "BasicBlock")
    return SILBasicBlock::registerBridgedMetatype(metatype);
  if (clName == "GlobalVariable")
    return SILGlobalVariable::registerBridgedMetatype(metatype);
  if (clName == "BlockArgument") {
    nodeMetatypes[(unsigned)SILNodeKind::SILPhiArgument] = metatype;
    return;
  }
  if (clName == "FunctionArgument") {
    nodeMetatypes[(unsigned)SILNodeKind::SILFunctionArgument] = metatype;
    return;
  }

  // Pre-populate the "unimplemented" ranges of metatypes.
  // If a specifc class is not implemented yet in libswift, it bridges to an
  // "unimplemented" class. This ensures that optimizations handle _all_ kind of
  // instructions gracefully, without the need to define the not-yet-used
  // classes in libswift.
#define VALUE_RANGE(ID) SILNodeKind::First_##ID, SILNodeKind::Last_##ID
  if (clName == "UnimplementedRefCountingInst")
    return setUnimplementedRange(metatype, VALUE_RANGE(RefCountingInst));
  if (clName == "UnimplementedSingleValueInst")
    return setUnimplementedRange(metatype, VALUE_RANGE(SingleValueInstruction));
  if (clName == "UnimplementedInstruction")
    return setUnimplementedRange(metatype, VALUE_RANGE(SILInstruction));
#undef VALUE_RANGE

  if (valueNamesToKind.empty()) {
#define VALUE(ID, PARENT) \
    valueNamesToKind[#ID] = SILNodeKind::ID;
#define BRIDGED_NON_VALUE_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE) \
    VALUE(ID, NAME)
#define ARGUMENT(ID, PARENT) \
    VALUE(ID, NAME)
#define BRIDGED_SINGLE_VALUE_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE) \
    VALUE(ID, NAME)
#define MULTIPLE_VALUE_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE) \
    VALUE(ID, NAME)
#include "swift/SIL/SILNodes.def"
  }

  std::string prefixedName;
  auto iter = valueNamesToKind.find(clName);
  if (iter == valueNamesToKind.end()) {
    // Try again with a "SIL" prefix. For example Argument -> SILArgument.
    prefixedName = std::string("SIL") + std::string(clName);
    iter = valueNamesToKind.find(prefixedName);
    if (iter == valueNamesToKind.end()) {
      llvm::errs() << "Unknown bridged node class " << clName << '\n';
      abort();
    }
    clName = prefixedName;
  }
  SILNodeKind kind = iter->second;
  SwiftMetatype existingTy = nodeMetatypes[(unsigned)kind];
  if (existingTy && !unimplementedTypes.count(existingTy)) {
    llvm::errs() << "Double registration of class " << clName << '\n';
    abort();
  }
  nodeMetatypes[(unsigned)kind] = metatype;
}

//===----------------------------------------------------------------------===//
//                            Bridging C functions
//===----------------------------------------------------------------------===//

/// Frees a string which was allocated by getCopiedBridgedStringRef.
void freeBridgedStringRef(BridgedStringRef str) {
  llvm::MallocAllocator().Deallocate(str.data, str.length);
}

//===----------------------------------------------------------------------===//
//                                SILFunction
//===----------------------------------------------------------------------===//

BridgedStringRef SILFunction_getName(BridgedFunction function) {
  return getBridgedStringRef(castToFunction(function)->getName());
}

BridgedStringRef SILFunction_debugDescription(BridgedFunction function) {
  std::string str;
  llvm::raw_string_ostream os(str);
  castToFunction(function)->print(os);
  return getCopiedBridgedStringRef(str, /*removeTrailingNewline*/ true);
}

OptionalBridgedBasicBlock SILFunction_firstBlock(BridgedFunction function) {
  SILFunction *f = castToFunction(function);
  if (f->empty())
    return {nullptr};
  return {f->getEntryBlock()};
}

OptionalBridgedBasicBlock SILFunction_lastBlock(BridgedFunction function) {
  SILFunction *f = castToFunction(function);
  if (f->empty())
    return {nullptr};
  return {&*f->rbegin()};
}

//===----------------------------------------------------------------------===//
//                               SILBasicBlock
//===----------------------------------------------------------------------===//

static_assert(BridgedSuccessorSize == sizeof(SILSuccessor),
              "wrong bridged SILSuccessor size");

OptionalBridgedBasicBlock SILBasicBlock_next(BridgedBasicBlock block) {
  SILBasicBlock *b = castToBasicBlock(block);
  auto iter = std::next(b->getIterator());
  if (iter == b->getParent()->end())
    return {nullptr};
  return {&*iter};
}

OptionalBridgedBasicBlock SILBasicBlock_previous(BridgedBasicBlock block) {
  SILBasicBlock *b = castToBasicBlock(block);
  auto iter = std::next(b->getReverseIterator());
  if (iter == b->getParent()->rend())
    return {nullptr};
  return {&*iter};
}

BridgedFunction SILBasicBlock_getFunction(BridgedBasicBlock block) {
  return {castToBasicBlock(block)->getParent()};
}

BridgedStringRef SILBasicBlock_debugDescription(BridgedBasicBlock block) {
  std::string str;
  llvm::raw_string_ostream os(str);
  castToBasicBlock(block)->print(os);
  return getCopiedBridgedStringRef(str, /*removeTrailingNewline*/ true);
}

OptionalBridgedInstruction SILBasicBlock_firstInst(BridgedBasicBlock block) {
  SILBasicBlock *b = castToBasicBlock(block);
  if (b->empty())
    return {nullptr};
  return {b->front().asSILNode()};
}

OptionalBridgedInstruction SILBasicBlock_lastInst(BridgedBasicBlock block) {
  SILBasicBlock *b = castToBasicBlock(block);
  if (b->empty())
    return {nullptr};
  return {b->back().asSILNode()};
}

SwiftInt SILBasicBlock_getNumArguments(BridgedBasicBlock block) {
  return castToBasicBlock(block)->getNumArguments();
}

BridgedArgument SILBasicBlock_getArgument(BridgedBasicBlock block, SwiftInt index) {
  return {castToBasicBlock(block)->getArgument(index)};
}

OptionalBridgedSuccessor SILBasicBlock_getFirstPred(BridgedBasicBlock block) {
  return {castToBasicBlock(block)->pred_begin().getSuccessorRef()};
}

static SILSuccessor *castToSuccessor(BridgedSuccessor succ) {
  return const_cast<SILSuccessor *>(static_cast<const SILSuccessor *>(succ.succ));
}

OptionalBridgedSuccessor SILSuccessor_getNext(BridgedSuccessor succ) {
  return {castToSuccessor(succ)->getNext()};
}

BridgedBasicBlock SILSuccessor_getTargetBlock(BridgedSuccessor succ) {
  return {castToSuccessor(succ)->getBB()};
}

BridgedInstruction SILSuccessor_getContainingInst(BridgedSuccessor succ) {
  return {castToSuccessor(succ)->getContainingInst()};
}

//===----------------------------------------------------------------------===//
//                                SILArgument
//===----------------------------------------------------------------------===//

BridgedBasicBlock SILArgument_getParent(BridgedArgument argument) {
  return {static_cast<SILArgument *>(argument.obj)->getParent()};
}

//===----------------------------------------------------------------------===//
//                                SILValue
//===----------------------------------------------------------------------===//

static_assert(BridgedOperandSize == sizeof(Operand),
              "wrong bridged Operand size");

BridgedStringRef SILNode_debugDescription(BridgedNode node) {
  std::string str;
  llvm::raw_string_ostream os(str);
  castToSILNode(node)->print(os);
  return getCopiedBridgedStringRef(str, /*removeTrailingNewline*/ true);
}

static Operand *castToOperand(BridgedOperand operand) {
  return const_cast<Operand *>(static_cast<const Operand *>(operand.op));
}

BridgedValue Operand_getValue(BridgedOperand operand) {
  return {castToOperand(operand)->get()};
}

OptionalBridgedOperand Operand_nextUse(BridgedOperand operand) {
  return {castToOperand(operand)->getNextUse()};
}

BridgedInstruction Operand_getUser(BridgedOperand operand) {
  return {castToOperand(operand)->getUser()->asSILNode()};
}

OptionalBridgedOperand SILValue_firstUse(BridgedValue value) {
  return {*castToSILValue(value)->use_begin()};
}

BridgedType SILValue_getType(BridgedValue value) {
  return { castToSILValue(value)->getType().getOpaqueValue() };
}

//===----------------------------------------------------------------------===//
//                            SILType
//===----------------------------------------------------------------------===//

SwiftInt SILType_isAddress(BridgedType type) {
  return castToSILType(type).isAddress();
}

//===----------------------------------------------------------------------===//
//                            SILGlobalVariable
//===----------------------------------------------------------------------===//

BridgedStringRef SILGlobalVariable_getName(BridgedGlobalVar global) {
  return getBridgedStringRef(castToGlobal(global)->getName());
}

BridgedStringRef SILGlobalVariable_debugDescription(BridgedGlobalVar global) {
  std::string str;
  llvm::raw_string_ostream os(str);
  castToGlobal(global)->print(os);
  return getCopiedBridgedStringRef(str, /*removeTrailingNewline*/ true);
}

//===----------------------------------------------------------------------===//
//                               SILInstruction
//===----------------------------------------------------------------------===//

OptionalBridgedInstruction SILInstruction_next(BridgedInstruction inst) {
  SILInstruction *i = castToInst(inst);
  auto iter = std::next(i->getIterator());
  if (iter == i->getParent()->end())
    return {nullptr};
  return {iter->asSILNode()};
}

OptionalBridgedInstruction SILInstruction_previous(BridgedInstruction inst) {
  SILInstruction *i = castToInst(inst);
  auto iter = std::next(i->getReverseIterator());
  if (iter == i->getParent()->rend())
    return {nullptr};
  return {iter->asSILNode()};
}

BridgedBasicBlock SILInstruction_getParent(BridgedInstruction inst) {
  SILInstruction *i = castToInst(inst);
  assert(!i->isStaticInitializerInst() &&
         "cannot get the parent of a static initializer instruction");
  return {i->getParent()};
}

BridgedArrayRef SILInstruction_getOperands(BridgedInstruction inst) {
  auto operands = castToInst(inst)->getAllOperands();
  return {(const unsigned char *)operands.data(), operands.size()};
}

BridgedLocation SILInstruction_getLocation(BridgedInstruction inst) {
  SILDebugLocation loc = castToInst(inst)->getDebugLocation();
  return *reinterpret_cast<BridgedLocation *>(&loc);
}

BridgedMemoryBehavior SILInstruction_getMemBehavior(BridgedInstruction inst) {
  return (BridgedMemoryBehavior)castToInst(inst)->getMemoryBehavior();
}

BridgedInstruction MultiValueInstResult_getParent(BridgedMultiValueResult result) {
  return {static_cast<MultipleValueInstructionResult *>(result.obj)->getParent()};
}

SwiftInt MultipleValueInstruction_getNumResults(BridgedInstruction inst) {
  return castToInst<MultipleValueInstruction>(inst)->getNumResults();
}
BridgedMultiValueResult
MultipleValueInstruction_getResult(BridgedInstruction inst, SwiftInt index) {
  return {castToInst<MultipleValueInstruction>(inst)->getResult(index)};
}

BridgedArrayRef TermInst_getSuccessors(BridgedInstruction term) {
  auto successors = castToInst<TermInst>(term)->getSuccessors();
  return {(const unsigned char *)successors.data(), successors.size()};
}

//===----------------------------------------------------------------------===//
//                            Instruction classes
//===----------------------------------------------------------------------===//

BridgedStringRef CondFailInst_getMessage(BridgedInstruction cfi) {
  return getBridgedStringRef(castToInst<CondFailInst>(cfi)->getMessage());
}

BridgedGlobalVar GlobalAccessInst_getGlobal(BridgedInstruction globalInst) {
  return {castToInst<GlobalAccessInst>(globalInst)->getReferencedGlobal()};
}

SwiftInt TupleExtractInst_fieldIndex(BridgedInstruction tei) {
  return castToInst<TupleExtractInst>(tei)->getFieldIndex();
}

SwiftInt TupleElementAddrInst_fieldIndex(BridgedInstruction teai) {
  return castToInst<TupleElementAddrInst>(teai)->getFieldIndex();
}

SwiftInt StructExtractInst_fieldIndex(BridgedInstruction sei) {
  return castToInst<StructExtractInst>(sei)->getFieldIndex();
}

SwiftInt StructElementAddrInst_fieldIndex(BridgedInstruction seai) {
  return castToInst<StructElementAddrInst>(seai)->getFieldIndex();
}

SwiftInt EnumInst_caseIndex(BridgedInstruction ei) {
  return getCaseIndex(castToInst<EnumInst>(ei)->getElement());
}

SwiftInt UncheckedEnumDataInst_caseIndex(BridgedInstruction uedi) {
  return getCaseIndex(castToInst<UncheckedEnumDataInst>(uedi)->getElement());
}

SwiftInt RefElementAddrInst_fieldIndex(BridgedInstruction reai) {
  return castToInst<RefElementAddrInst>(reai)->getFieldIndex();
}

SwiftInt PartialApplyInst_numArguments(BridgedInstruction pai) {
  return castToInst<PartialApplyInst>(pai)->getNumArguments();
}

SwiftInt ApplyInst_numArguments(BridgedInstruction ai) {
  return castToInst<ApplyInst>(ai)->getNumArguments();
}

SwiftInt BeginApplyInst_numArguments(BridgedInstruction tai) {
  return castToInst<BeginApplyInst>(tai)->getNumArguments();
}

SwiftInt TryApplyInst_numArguments(BridgedInstruction tai) {
  return castToInst<TryApplyInst>(tai)->getNumArguments();
}

BridgedBasicBlock BranchInst_getTargetBlock(BridgedInstruction bi) {
  return {castToInst<BranchInst>(bi)->getDestBB()};
}

SwiftInt SwitchEnumInst_getNumCases(BridgedInstruction se) {
  return castToInst<SwitchEnumInst>(se)->getNumCases();
}

SwiftInt SwitchEnumInst_getCaseIndex(BridgedInstruction se, SwiftInt idx) {
  return getCaseIndex(castToInst<SwitchEnumInst>(se)->getCase(idx).first);
}

//===----------------------------------------------------------------------===//
//                                SILBuilder
//===----------------------------------------------------------------------===//

BridgedInstruction SILBuilder_createBuiltinBinaryFunction(
          BridgedInstruction insertionPoint,
          BridgedLocation loc, BridgedStringRef name,
          BridgedType operandType, BridgedType resultType,
          BridgedValueArray arguments) {
    SILBuilder builder(castToInst(insertionPoint), getSILDebugScope(loc));
    SmallVector<SILValue, 16> argValues;
    return {builder.createBuiltinBinaryFunction(getRegularLocation(loc),
      getStringRef(name), getSILType(operandType), getSILType(resultType),
      getSILValues(arguments, argValues))};
}

BridgedInstruction SILBuilder_createCondFail(BridgedInstruction insertionPoint,
          BridgedLocation loc, BridgedValue condition, BridgedStringRef messge) {
  SILBuilder builder(castToInst(insertionPoint), getSILDebugScope(loc));
  return {builder.createCondFail(getRegularLocation(loc),
    castToSILValue(condition), getStringRef(messge))};
}
