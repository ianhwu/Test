//===--- ValueOwnership.cpp -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/SILBuiltinVisitor.h"
#include "swift/SIL/SILModule.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                 Interface
//===----------------------------------------------------------------------===//

namespace {

class ValueOwnershipKindClassifier
    : public SILValueVisitor<ValueOwnershipKindClassifier, ValueOwnershipKind> {

public:
  ValueOwnershipKindClassifier() = default;
  ~ValueOwnershipKindClassifier() = default;
  ValueOwnershipKindClassifier(const ValueOwnershipKindClassifier &) = delete;
  ValueOwnershipKindClassifier(ValueOwnershipKindClassifier &&) = delete;

  ValueOwnershipKind visitForwardingInst(SILInstruction *I,
                                         ArrayRef<Operand> Ops);
  ValueOwnershipKind visitForwardingInst(SILInstruction *I) {
    return visitForwardingInst(I, I->getAllOperands());
  }

#define VALUE(Id, Parent) ValueOwnershipKind visit##Id(Id *ID);
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

#define CONSTANT_OWNERSHIP_INST(OWNERSHIP, INST)                               \
  ValueOwnershipKind ValueOwnershipKindClassifier::visit##INST##Inst(          \
      INST##Inst *Arg) {                                                       \
    return ValueOwnershipKind::OWNERSHIP;                                      \
  }

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  CONSTANT_OWNERSHIP_INST(Owned, Load##Name)
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  CONSTANT_OWNERSHIP_INST(Unowned, RefTo##Name)                                \
  CONSTANT_OWNERSHIP_INST(Unowned, Name##ToRef)                                \
  CONSTANT_OWNERSHIP_INST(Owned, StrongCopy##Name##Value)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, "...") \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  CONSTANT_OWNERSHIP_INST(None, RefTo##Name)                                   \
  CONSTANT_OWNERSHIP_INST(Unowned, Name##ToRef)                                \
  CONSTANT_OWNERSHIP_INST(Owned, StrongCopy##Name##Value)
#include "swift/AST/ReferenceStorage.def"

CONSTANT_OWNERSHIP_INST(Guaranteed, BeginBorrow)
CONSTANT_OWNERSHIP_INST(Guaranteed, LoadBorrow)
CONSTANT_OWNERSHIP_INST(Owned, AllocBox)
CONSTANT_OWNERSHIP_INST(Owned, AllocExistentialBox)
CONSTANT_OWNERSHIP_INST(Owned, AllocRef)
CONSTANT_OWNERSHIP_INST(Owned, AllocRefDynamic)
CONSTANT_OWNERSHIP_INST(None, AllocValueBuffer)
CONSTANT_OWNERSHIP_INST(Owned, CopyBlock)
CONSTANT_OWNERSHIP_INST(Owned, CopyBlockWithoutEscaping)
CONSTANT_OWNERSHIP_INST(Owned, CopyValue)
CONSTANT_OWNERSHIP_INST(Owned, KeyPath)
CONSTANT_OWNERSHIP_INST(Owned, InitExistentialValue)
CONSTANT_OWNERSHIP_INST(Owned, GlobalValue) // TODO: is this correct?

// NOTE: Even though init_existential_ref from a reference counting perspective
// is not considered to be "owned" since it doesn't affect reference counts,
// conceptually we want to treat it as an owned value that produces owned
// things, rather than a forwarding thing since initialization is generally a
// consuming operation.
CONSTANT_OWNERSHIP_INST(Owned, InitExistentialRef)

// One would think that these /should/ be unowned. In truth they are owned since
// objc metatypes do not go through the retain/release fast path. In their
// implementations of retain/release nothing happens, so this is safe.
//
// You could even have an optimization that just always removed retain/release
// operations on these objects.
CONSTANT_OWNERSHIP_INST(Owned, ObjCExistentialMetatypeToObject)
CONSTANT_OWNERSHIP_INST(Owned, ObjCMetatypeToObject)

// All addresses have trivial ownership. The values stored at the address may
// not though.
CONSTANT_OWNERSHIP_INST(None, AddressToPointer)
CONSTANT_OWNERSHIP_INST(None, AllocStack)
CONSTANT_OWNERSHIP_INST(None, BeginAccess)
CONSTANT_OWNERSHIP_INST(None, BridgeObjectToWord)
CONSTANT_OWNERSHIP_INST(None, ClassMethod)
CONSTANT_OWNERSHIP_INST(None, ClassifyBridgeObject)
CONSTANT_OWNERSHIP_INST(None, ObjCMethod)
CONSTANT_OWNERSHIP_INST(None, ExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, FloatLiteral)
CONSTANT_OWNERSHIP_INST(None, FunctionRef)
CONSTANT_OWNERSHIP_INST(None, DynamicFunctionRef)
CONSTANT_OWNERSHIP_INST(None, PreviousDynamicFunctionRef)
CONSTANT_OWNERSHIP_INST(None, GlobalAddr)
CONSTANT_OWNERSHIP_INST(None, IndexAddr)
CONSTANT_OWNERSHIP_INST(None, IndexRawPointer)
CONSTANT_OWNERSHIP_INST(None, InitEnumDataAddr)
CONSTANT_OWNERSHIP_INST(None, InitExistentialAddr)
CONSTANT_OWNERSHIP_INST(None, InitExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, IntegerLiteral)
CONSTANT_OWNERSHIP_INST(None, IsUnique)
CONSTANT_OWNERSHIP_INST(None, IsEscapingClosure)
CONSTANT_OWNERSHIP_INST(None, Metatype)
CONSTANT_OWNERSHIP_INST(None, ObjCToThickMetatype)
CONSTANT_OWNERSHIP_INST(None, OpenExistentialAddr)
CONSTANT_OWNERSHIP_INST(None, OpenExistentialBox)
CONSTANT_OWNERSHIP_INST(None, OpenExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, PointerToAddress)
CONSTANT_OWNERSHIP_INST(None, PointerToThinFunction)
CONSTANT_OWNERSHIP_INST(None, ProjectBlockStorage)
CONSTANT_OWNERSHIP_INST(None, ProjectBox)
CONSTANT_OWNERSHIP_INST(None, ProjectExistentialBox)
CONSTANT_OWNERSHIP_INST(None, ProjectValueBuffer)
CONSTANT_OWNERSHIP_INST(None, RefElementAddr)
CONSTANT_OWNERSHIP_INST(None, RefTailAddr)
CONSTANT_OWNERSHIP_INST(None, RefToRawPointer)
CONSTANT_OWNERSHIP_INST(None, SelectEnumAddr)
CONSTANT_OWNERSHIP_INST(None, StringLiteral)
CONSTANT_OWNERSHIP_INST(None, StructElementAddr)
CONSTANT_OWNERSHIP_INST(None, SuperMethod)
CONSTANT_OWNERSHIP_INST(None, ObjCSuperMethod)
CONSTANT_OWNERSHIP_INST(None, TailAddr)
CONSTANT_OWNERSHIP_INST(None, ThickToObjCMetatype)
CONSTANT_OWNERSHIP_INST(None, ThinFunctionToPointer)
CONSTANT_OWNERSHIP_INST(None, TupleElementAddr)
CONSTANT_OWNERSHIP_INST(None, UncheckedAddrCast)
CONSTANT_OWNERSHIP_INST(None, UncheckedTakeEnumDataAddr)
CONSTANT_OWNERSHIP_INST(None, UncheckedTrivialBitCast)
CONSTANT_OWNERSHIP_INST(None, ValueMetatype)
CONSTANT_OWNERSHIP_INST(None, WitnessMethod)
CONSTANT_OWNERSHIP_INST(None, StoreBorrow)
CONSTANT_OWNERSHIP_INST(None, ConvertEscapeToNoEscape)
CONSTANT_OWNERSHIP_INST(Unowned, InitBlockStorageHeader)
// TODO: It would be great to get rid of these.
CONSTANT_OWNERSHIP_INST(Unowned, RawPointerToRef)
CONSTANT_OWNERSHIP_INST(Unowned, ObjCProtocol)
CONSTANT_OWNERSHIP_INST(Unowned, ValueToBridgeObject)
#undef CONSTANT_OWNERSHIP_INST

#define CONSTANT_OR_NONE_OWNERSHIP_INST(OWNERSHIP, INST)                       \
  ValueOwnershipKind ValueOwnershipKindClassifier::visit##INST##Inst(          \
      INST##Inst *I) {                                                         \
    if (I->getType().isTrivial(*I->getFunction())) {                           \
      return ValueOwnershipKind::None;                                         \
    }                                                                          \
    return ValueOwnershipKind::OWNERSHIP;                                      \
  }
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, StructExtract)
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, TupleExtract)
// OpenExistentialValue opens the boxed value inside an existential
// CoW box. The semantics of an existential CoW box implies that we
// can only consume the projected value inside the box if the box is
// unique. Since we do not know in general if the box is unique
// without additional work, in SIL we require opened archetypes to
// be borrowed sub-objects of the parent CoW box.
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, OpenExistentialValue)
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, OpenExistentialBoxValue)
CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, UnconditionalCheckedCastValue)

// unchecked_bitwise_cast is a bitwise copy. It produces a trivial or unowned
// result.
//
// If the operand is nontrivial and the result is trivial, then it is the
// programmer's responsibility to use Builtin.fixLifetime.
//
// If both the operand and the result are nontrivial, then either the types must
// be compatible so that TBAA doesn't allow the destroy to be hoisted above uses
// of the cast, or the programmer must use Builtin.fixLifetime.
//
// FIXME
// -----
//
// SR-7175: Since we model this as unowned, then we must copy the
// value before use. This directly contradicts the semantics mentioned
// above since we will copy the value upon any use lest we use an
// unowned value in an owned or guaranteed way. So really all we will
// do here is perhaps add a copy slightly earlier unless the unowned
// value immediately is cast to something trivial. In such a case, we
// should be able to simplify the cast to just a trivial value and
// then eliminate the copy. That being said, we should investigate
// this since this is used in reinterpret_cast which is important from
// a performance perspective.
CONSTANT_OR_NONE_OWNERSHIP_INST(Unowned, UncheckedBitwiseCast)

// A thin_to_thick instruction can return a trivial (@noescape) type.
CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, ThinToThickFunction)

#undef CONSTANT_OR_NONE_OWNERSHIP_INST

// For a forwarding instruction, we loop over all operands and make sure that
// all non-trivial values have the same ownership.
ValueOwnershipKind
ValueOwnershipKindClassifier::visitForwardingInst(SILInstruction *i,
                                                  ArrayRef<Operand> ops) {
  // A forwarding inst without operands must be trivial.
  if (ops.empty())
    return ValueOwnershipKind::None;

  auto mergedValue = ValueOwnershipKind::merge(makeOptionalTransformRange(
      ops, [&i](const Operand &op) -> Optional<ValueOwnershipKind> {
        if (i->isTypeDependentOperand(op))
          return None;
        return op.get().getOwnershipKind();
      }));

  if (!mergedValue.hasValue()) {
    // If we have mismatched SILOwnership and sil ownership is not enabled,
    // just return None for staging purposes. If SILOwnership is enabled, then
    // we must assert!
    if (!i->getModule().getOptions().VerifySILOwnership) {
      return ValueOwnershipKind::None;
    }
    llvm_unreachable("Forwarding inst with mismatching ownership kinds?!");
  }

  return mergedValue.getValue();
}

#define FORWARDING_OWNERSHIP_INST(INST)                                        \
  ValueOwnershipKind ValueOwnershipKindClassifier::visit##INST##Inst(          \
      INST##Inst *I) {                                                         \
    return I->getOwnershipKind();                                              \
  }
FORWARDING_OWNERSHIP_INST(BridgeObjectToRef)
FORWARDING_OWNERSHIP_INST(ConvertFunction)
FORWARDING_OWNERSHIP_INST(OpenExistentialRef)
FORWARDING_OWNERSHIP_INST(RefToBridgeObject)
FORWARDING_OWNERSHIP_INST(SelectValue)
FORWARDING_OWNERSHIP_INST(Object)
FORWARDING_OWNERSHIP_INST(Struct)
FORWARDING_OWNERSHIP_INST(Tuple)
FORWARDING_OWNERSHIP_INST(UncheckedRefCast)
FORWARDING_OWNERSHIP_INST(UnconditionalCheckedCast)
FORWARDING_OWNERSHIP_INST(Upcast)
FORWARDING_OWNERSHIP_INST(MarkUninitialized)
FORWARDING_OWNERSHIP_INST(UncheckedEnumData)
FORWARDING_OWNERSHIP_INST(SelectEnum)
FORWARDING_OWNERSHIP_INST(Enum)
#undef FORWARDING_OWNERSHIP_INST

ValueOwnershipKind
ValueOwnershipKindClassifier::visitUncheckedOwnershipConversionInst(
    UncheckedOwnershipConversionInst *I) {
  return I->getConversionOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitSILUndef(SILUndef *arg) {
  return arg->getOwnershipKind();
}

ValueOwnershipKind
ValueOwnershipKindClassifier::visitSILPhiArgument(SILPhiArgument *Arg) {
  return Arg->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitDestructureStructResult(
    DestructureStructResult *Result) {
  return Result->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitDestructureTupleResult(
    DestructureTupleResult *Result) {
  return Result->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitBeginApplyResult(
    BeginApplyResult *Result) {
  return Result->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitSILFunctionArgument(
    SILFunctionArgument *Arg) {
  return Arg->getOwnershipKind();
}

// This is a forwarding instruction through only one of its arguments.
ValueOwnershipKind
ValueOwnershipKindClassifier::visitMarkDependenceInst(MarkDependenceInst *MDI) {
  return MDI->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitApplyInst(ApplyInst *ai) {
  auto *f = ai->getFunction();
  bool isTrivial = ai->getType().isTrivial(*f);
  // Quick is trivial check.
  if (isTrivial)
    return ValueOwnershipKind::None;

  SILFunctionConventions fnConv(ai->getSubstCalleeType(), f->getModule());
  auto results = fnConv.getDirectSILResults();
  // No results => None.
  if (results.empty())
    return ValueOwnershipKind::None;

  // Otherwise, map our results to their ownership kinds and then merge them!
  auto resultOwnershipKinds =
      makeTransformRange(results, [&](const SILResultInfo &info) {
        return info.getOwnershipKind(*f);
      });
  auto mergedOwnershipKind = ValueOwnershipKind::merge(resultOwnershipKinds);
  if (!mergedOwnershipKind) {
    llvm_unreachable("Forwarding inst with mismatching ownership kinds?!");
  }

  return *mergedOwnershipKind;
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitLoadInst(LoadInst *LI) {
  switch (LI->getOwnershipQualifier()) {
  case LoadOwnershipQualifier::Take:
  case LoadOwnershipQualifier::Copy:
    return ValueOwnershipKind::Owned;
  case LoadOwnershipQualifier::Unqualified:
  case LoadOwnershipQualifier::Trivial:
    return ValueOwnershipKind::None;
  }

  llvm_unreachable("Unhandled LoadOwnershipQualifier in switch.");
}

ValueOwnershipKind
ValueOwnershipKindClassifier::visitPartialApplyInst(PartialApplyInst *PA) {
  if (PA->isOnStack())
    return ValueOwnershipKind::None;
  return ValueOwnershipKind::Owned;
}

//===----------------------------------------------------------------------===//
//                   Builtin OwnershipValueKind Computation
//===----------------------------------------------------------------------===//

namespace {

struct ValueOwnershipKindBuiltinVisitor
    : SILBuiltinVisitor<ValueOwnershipKindBuiltinVisitor, ValueOwnershipKind> {

  ValueOwnershipKind visitLLVMIntrinsic(BuiltinInst *BI,
                                        llvm::Intrinsic::ID ID) {
    // LLVM intrinsics do not traffic in ownership, so if we have a result, it
    // must be any.
    return ValueOwnershipKind::None;
  }

#define BUILTIN(ID, NAME, ATTRS)                                               \
  ValueOwnershipKind visit##ID(BuiltinInst *BI, StringRef Attr);
#include "swift/AST/Builtins.def"
};

} // end anonymous namespace

#define CONSTANT_OWNERSHIP_BUILTIN(OWNERSHIP, ID)                              \
  ValueOwnershipKind ValueOwnershipKindBuiltinVisitor::visit##ID(              \
      BuiltinInst *BI, StringRef Attr) {                                       \
    return ValueOwnershipKind::OWNERSHIP;                                      \
  }
// This returns a value at +1 that is destroyed strictly /after/ the
// UnsafeGuaranteedEnd. This provides the guarantee that we want.
CONSTANT_OWNERSHIP_BUILTIN(Owned, UnsafeGuaranteed)
CONSTANT_OWNERSHIP_BUILTIN(None, AShr)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericAShr)
CONSTANT_OWNERSHIP_BUILTIN(None, Add)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericAdd)
CONSTANT_OWNERSHIP_BUILTIN(None, And)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericAnd)
CONSTANT_OWNERSHIP_BUILTIN(None, AssumeNonNegative)
CONSTANT_OWNERSHIP_BUILTIN(None, AssumeTrue)
CONSTANT_OWNERSHIP_BUILTIN(None, BitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, CondFailMessage)
CONSTANT_OWNERSHIP_BUILTIN(None, ExactSDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericExactSDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, ExactUDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericExactUDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, FAdd)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFAdd)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OEQ)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OGE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OGT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OLE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OLT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ONE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UEQ)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UGE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UGT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ULE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ULT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UNE)
CONSTANT_OWNERSHIP_BUILTIN(None, FDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, FMul)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFMul)
CONSTANT_OWNERSHIP_BUILTIN(None, FNeg)
CONSTANT_OWNERSHIP_BUILTIN(None, FPExt)
CONSTANT_OWNERSHIP_BUILTIN(None, FPToSI)
CONSTANT_OWNERSHIP_BUILTIN(None, FPToUI)
CONSTANT_OWNERSHIP_BUILTIN(None, FPTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, FRem)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFRem)
CONSTANT_OWNERSHIP_BUILTIN(None, FSub)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFSub)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_EQ)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_NE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SGE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SGT)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SLE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SLT)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_UGE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_UGT)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_ULE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_ULT)
CONSTANT_OWNERSHIP_BUILTIN(None, IntToPtr)
CONSTANT_OWNERSHIP_BUILTIN(None, LShr)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericLShr)
CONSTANT_OWNERSHIP_BUILTIN(None, Mul)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericMul)
CONSTANT_OWNERSHIP_BUILTIN(None, Or)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericOr)
CONSTANT_OWNERSHIP_BUILTIN(None, PtrToInt)
CONSTANT_OWNERSHIP_BUILTIN(None, SAddOver)
CONSTANT_OWNERSHIP_BUILTIN(None, SDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericSDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, SExt)
CONSTANT_OWNERSHIP_BUILTIN(None, SExtOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, SIToFP)
CONSTANT_OWNERSHIP_BUILTIN(None, SMulOver)
CONSTANT_OWNERSHIP_BUILTIN(None, SRem)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericSRem)
CONSTANT_OWNERSHIP_BUILTIN(None, SSubOver)
CONSTANT_OWNERSHIP_BUILTIN(None, Expect)
CONSTANT_OWNERSHIP_BUILTIN(None, Shl)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericShl)
CONSTANT_OWNERSHIP_BUILTIN(None, Sub)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericSub)
CONSTANT_OWNERSHIP_BUILTIN(None, Trunc)
CONSTANT_OWNERSHIP_BUILTIN(None, TruncOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, UAddOver)
CONSTANT_OWNERSHIP_BUILTIN(None, UDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericUDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, UIToFP)
CONSTANT_OWNERSHIP_BUILTIN(None, UMulOver)
CONSTANT_OWNERSHIP_BUILTIN(None, URem)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericURem)
CONSTANT_OWNERSHIP_BUILTIN(None, USubOver)
CONSTANT_OWNERSHIP_BUILTIN(None, Xor)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericXor)
CONSTANT_OWNERSHIP_BUILTIN(None, ZExt)
CONSTANT_OWNERSHIP_BUILTIN(None, ZExtOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ORD)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UNO)
CONSTANT_OWNERSHIP_BUILTIN(None, OnFastPath)
CONSTANT_OWNERSHIP_BUILTIN(None, IsOptionalType)
CONSTANT_OWNERSHIP_BUILTIN(None, Sizeof)
CONSTANT_OWNERSHIP_BUILTIN(None, Strideof)
CONSTANT_OWNERSHIP_BUILTIN(None, StringObjectOr)
CONSTANT_OWNERSHIP_BUILTIN(None, IsPOD)
CONSTANT_OWNERSHIP_BUILTIN(None, IsConcrete)
CONSTANT_OWNERSHIP_BUILTIN(None, IsBitwiseTakable)
CONSTANT_OWNERSHIP_BUILTIN(None, IsSameMetatype)
CONSTANT_OWNERSHIP_BUILTIN(None, Alignof)
CONSTANT_OWNERSHIP_BUILTIN(None, AllocRaw)
CONSTANT_OWNERSHIP_BUILTIN(None, AssertConf)
CONSTANT_OWNERSHIP_BUILTIN(None, UToSCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, SToSCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, SToUCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, UToUCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, IntToFPWithOverflow)

// This is surprising, Builtin.unreachable returns a "Never" value which is
// trivially typed.
CONSTANT_OWNERSHIP_BUILTIN(None, Unreachable)

/// AtomicRMW has type (Builtin.RawPointer, T) -> T. But it provides overloads
/// for integer or rawpointer, so it should be trivial.
CONSTANT_OWNERSHIP_BUILTIN(None, AtomicRMW)

CONSTANT_OWNERSHIP_BUILTIN(None, CondUnreachable)
CONSTANT_OWNERSHIP_BUILTIN(None, UnsafeGuaranteedEnd)
CONSTANT_OWNERSHIP_BUILTIN(None, GetObjCTypeEncoding)
CONSTANT_OWNERSHIP_BUILTIN(None, CanBeObjCClass)
CONSTANT_OWNERSHIP_BUILTIN(None, WillThrow)
CONSTANT_OWNERSHIP_BUILTIN(None, StaticReport)

CONSTANT_OWNERSHIP_BUILTIN(None, DestroyArray)
CONSTANT_OWNERSHIP_BUILTIN(None, CopyArray)
CONSTANT_OWNERSHIP_BUILTIN(None, TakeArrayNoAlias)
CONSTANT_OWNERSHIP_BUILTIN(None, TakeArrayFrontToBack)
CONSTANT_OWNERSHIP_BUILTIN(None, TakeArrayBackToFront)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignCopyArrayNoAlias)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignCopyArrayFrontToBack)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignCopyArrayBackToFront)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignTakeArray)
CONSTANT_OWNERSHIP_BUILTIN(None, UnexpectedError)
CONSTANT_OWNERSHIP_BUILTIN(None, ErrorInMain)
CONSTANT_OWNERSHIP_BUILTIN(None, DeallocRaw)
CONSTANT_OWNERSHIP_BUILTIN(None, Fence)
CONSTANT_OWNERSHIP_BUILTIN(None, AtomicStore)
CONSTANT_OWNERSHIP_BUILTIN(None, Once)
CONSTANT_OWNERSHIP_BUILTIN(None, OnceWithContext)
CONSTANT_OWNERSHIP_BUILTIN(None, TSanInoutAccess)
CONSTANT_OWNERSHIP_BUILTIN(None, Swift3ImplicitObjCEntrypoint)
CONSTANT_OWNERSHIP_BUILTIN(None, PoundAssert)
CONSTANT_OWNERSHIP_BUILTIN(None, GlobalStringTablePointer)

#undef CONSTANT_OWNERSHIP_BUILTIN

// Check all of these...
#define UNOWNED_OR_NONE_DEPENDING_ON_RESULT(ID)                                \
  ValueOwnershipKind ValueOwnershipKindBuiltinVisitor::visit##ID(              \
      BuiltinInst *BI, StringRef Attr) {                                       \
    if (BI->getType().isTrivial(*BI->getFunction())) {                         \
      return ValueOwnershipKind::None;                                         \
    }                                                                          \
    return ValueOwnershipKind::Unowned;                                        \
  }
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(CmpXChg)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(AtomicLoad)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(ExtractElement)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(InsertElement)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(ZeroInitializer)
#undef UNOWNED_OR_NONE_DEPENDING_ON_RESULT

#define BUILTIN(X,Y,Z)
#define BUILTIN_SIL_OPERATION(ID, NAME, CATEGORY) \
  ValueOwnershipKind ValueOwnershipKindBuiltinVisitor::visit##ID( \
      BuiltinInst *BI, StringRef Attr) { \
    llvm_unreachable("builtin should have been lowered in SILGen"); \
  }

#include "swift/AST/Builtins.def"

ValueOwnershipKind
ValueOwnershipKindClassifier::visitBuiltinInst(BuiltinInst *BI) {
  // For now, just conservatively say builtins are None. We need to use a
  // builtin in here to guarantee correctness.
  return ValueOwnershipKindBuiltinVisitor().visit(BI);
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

ValueOwnershipKind SILValue::getOwnershipKind() const {
  ValueOwnershipKindClassifier Classifier;
  return Classifier.visit(const_cast<ValueBase *>(Value));
}
