//===--- SILGenFunction.h - Function Specific AST lower context -*- C++ -*-===//
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

#ifndef SWIFT_SILGEN_SILGENFUNCTION_H
#define SWIFT_SILGEN_SILGENFUNCTION_H

#include "FormalEvaluation.h"
#include "Initialization.h"
#include "JumpDest.h"
#include "RValue.h"
#include "SGFContext.h"
#include "SILGen.h"
#include "SILGenBuilder.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/Basic/Statistic.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/PointerIntPair.h"

namespace swift {

class ParameterList;

namespace Lowering {

class ArgumentSource;
class Condition;
class Conversion;
class ConsumableManagedValue;
class LogicalPathComponent;
class LValue;
class ManagedValue;
class PreparedArguments;
class RValue;
class CalleeTypeInfo;
class ResultPlan;
using ResultPlanPtr = std::unique_ptr<ResultPlan>;
class ArgumentScope;
class Scope;

enum class ApplyOptions : unsigned {
  /// No special treatment is required.
  None = 0,

  /// Suppress the error-handling edge out of the call.  This should
  /// be used carefully; it's used to implement features like 'rethrows'.
  DoesNotThrow = 0x1,
};
inline ApplyOptions operator|(ApplyOptions lhs, ApplyOptions rhs) {
  return ApplyOptions(unsigned(lhs) | unsigned(rhs));
}
inline ApplyOptions &operator|=(ApplyOptions &lhs, ApplyOptions rhs) {
  return (lhs = (lhs | rhs));
}
inline bool operator&(ApplyOptions lhs, ApplyOptions rhs) {
  return ((unsigned(lhs) & unsigned(rhs)) != 0);
}
inline ApplyOptions operator-(ApplyOptions lhs, ApplyOptions rhs) {
  return ApplyOptions(unsigned(lhs) & ~unsigned(rhs));
}
inline ApplyOptions &operator-=(ApplyOptions &lhs, ApplyOptions rhs) {
  return (lhs = (lhs - rhs));
}

struct LValueOptions {
  bool IsNonAccessing = false;

  /// Derive options for accessing the base of an l-value, given that
  /// applying the derived component might touch the memory.
  LValueOptions forComputedBaseLValue() const {
    auto copy = *this;

    // Assume we're going to access the base.
    copy.IsNonAccessing = false;

    return copy;
  }

  /// Derive options for accessing the base of an l-value, given that
  /// applying the derived component will not touch the memory.
  LValueOptions forProjectedBaseLValue() const {
    auto copy = *this;
    return copy;
  }
};

class PatternMatchContext;

/// A formal section of the function.  This is a SILGen-only concept,
/// meant to improve locality.  It's only reflected in the generated
/// SIL implicitly.
enum class FunctionSection : bool {
  /// The section of the function dedicated to ordinary control flow.
  Ordinary,

  /// The section of the function dedicated to error-handling and
  /// similar things.
  Postmatter,
};

/// Parameter to \c SILGenFunction::emitCaptures that indicates what the
/// capture parameters are being emitted for.
enum class CaptureEmission {
  /// Captures are being emitted for immediate application to a local function.
  ImmediateApplication,
  /// Captures are being emitted for partial application to form a closure
  /// value.
  PartialApplication,
};

/// Different ways in which an l-value can be emitted.
enum class SGFAccessKind : uint8_t {
  /// The access is a read whose result will be ignored.
  IgnoredRead,

  /// The access is a read that would prefer the address of a borrowed value.
  /// This should only be used when it is semantically acceptable to borrow
  /// the value, not just because the caller would benefit from a borrowed
  /// value.  See shouldEmitSelfAsRValue.
  ///
  /// The caller will be calling emitAddressOfLValue or emitLoadOfLValue
  /// on the l-value.  The latter may be less efficient than an access
  /// would be if the l-value had been emitted with an owned-read kind.
  BorrowedAddressRead,

  /// The access is a read that would prefer a loaded borrowed value.
  /// This should only be used when it is semantically acceptable to borrow
  /// the value, not just because the caller would benefit from a borrowed
  /// value.  See shouldEmitSelfAsRValue.
  ///
  /// There isn't yet a way to emit the access that takes advantage of this.
  BorrowedObjectRead,

  /// The access is a read that would prefer the address of an owned value.
  ///
  /// The caller will be calling emitAddressOfLValue or emitLoadOfLValue
  /// on the l-value.
  OwnedAddressRead,

  /// The access is a read that would prefer a loaded owned value.
  ///
  /// The caller will be calling emitLoadOfLValue on the l-value.
  OwnedObjectRead,

  /// The access is an assignment (or maybe an initialization).
  ///
  /// The caller will be calling emitAssignToLValue on the l-value.
  Write,

  /// The access is a read-modify-write.
  ///
  /// The caller will be calling emitAddressOfLValue on the l-value.
  ReadWrite
};

static inline bool isReadAccess(SGFAccessKind kind) {
  return uint8_t(kind) <= uint8_t(SGFAccessKind::OwnedObjectRead);
}

/// Given a read access kind, does it require an owned result?
static inline bool isReadAccessResultOwned(SGFAccessKind kind) {
  assert(isReadAccess(kind));
  return uint8_t(kind) >= uint8_t(SGFAccessKind::OwnedAddressRead);
}

/// Given a read access kind, does it require an address result?
static inline bool isReadAccessResultAddress(SGFAccessKind kind) {
  assert(isReadAccess(kind));
  return kind == SGFAccessKind::BorrowedAddressRead ||
         kind == SGFAccessKind::OwnedAddressRead;
}

/// Return an address-preferring version of the given access kind.
static inline SGFAccessKind getAddressAccessKind(SGFAccessKind kind) {
  switch (kind) {
  case SGFAccessKind::BorrowedObjectRead:
    return SGFAccessKind::BorrowedAddressRead;
  case SGFAccessKind::OwnedObjectRead:
    return SGFAccessKind::OwnedAddressRead;
  case SGFAccessKind::IgnoredRead:
  case SGFAccessKind::BorrowedAddressRead:
  case SGFAccessKind::OwnedAddressRead:
  case SGFAccessKind::Write:
  case SGFAccessKind::ReadWrite:
    return kind;
  }
  llvm_unreachable("bad kind");
}

static inline AccessKind getFormalAccessKind(SGFAccessKind kind) {
  switch (kind) {
  case SGFAccessKind::IgnoredRead:
  case SGFAccessKind::BorrowedAddressRead:
  case SGFAccessKind::BorrowedObjectRead:
  case SGFAccessKind::OwnedAddressRead:
  case SGFAccessKind::OwnedObjectRead:
    return AccessKind::Read;
  case SGFAccessKind::Write:
    return AccessKind::Write;
  case SGFAccessKind::ReadWrite:
    return AccessKind::ReadWrite;
  }
  llvm_unreachable("bad kind");
}

/// Parameter to \c SILGenFunction::emitAddressOfLValue that indicates
/// what kind of instrumentation should be emitted when compiling under
/// Thread Sanitizer.
enum class TSanKind : bool {
  None = 0,

  /// Instrument the LValue access as an inout access.
  InoutAccess
};

/// Represents an LValue opened for mutating access.
///
/// This is used by LogicalPathComponent::projectAsBase().
struct MaterializedLValue {
  ManagedValue temporary;

  // Only set if a callback is required
  CanType origSelfType;
  CanGenericSignature genericSig;
  SILValue callback;
  SILValue callbackStorage;

  MaterializedLValue() {}
  explicit MaterializedLValue(ManagedValue temporary)
    : temporary(temporary) {}
  MaterializedLValue(ManagedValue temporary,
                     CanType origSelfType,
                     CanGenericSignature genericSig,
                     SILValue callback,
                     SILValue callbackStorage)
    : temporary(temporary),
      origSelfType(origSelfType),
      genericSig(genericSig),
      callback(callback),
      callbackStorage(callbackStorage) {}
};

/// SILGenFunction - an ASTVisitor for producing SIL from function bodies.
class LLVM_LIBRARY_VISIBILITY SILGenFunction
  : public ASTVisitor<SILGenFunction>
{ // style violation because Xcode <rdar://problem/13065676>
public:
  /// The SILGenModule this function belongs to.
  SILGenModule &SGM;
    
  /// The SILFunction being constructed.
  SILFunction &F;

  /// The SILModuleConventions for this SIL module.
  SILModuleConventions silConv;

  /// The DeclContext corresponding to the function currently being emitted.
  DeclContext * const FunctionDC;

  /// The name of the function currently being emitted, as presented to user
  /// code by #function.
  DeclName MagicFunctionName;
  std::string MagicFunctionString;

  ASTContext &getASTContext() const { return SGM.M.getASTContext(); }

  /// The first block in the postmatter section of the function, if
  /// anything has been built there.
  ///
  /// (This field must precede B because B's initializer calls
  /// createBasicBlock().)
  SILFunction::iterator StartOfPostmatter;

  /// The current section of the function that we're emitting code in.
  ///
  /// The postmatter section is a part of the function intended for
  /// things like error-handling that don't need to be mixed into the
  /// normal code sequence.
  ///
  /// If the current function section is Ordinary, and
  /// StartOfPostmatter does not point to the function end, the current
  /// insertion block should be ordered before that.
  ///
  /// If the current function section is Postmatter, StartOfPostmatter
  /// does not point to the function end and the current insertion block is
  /// ordered after that (inclusive).
  ///
  /// (This field must precede B because B's initializer calls
  /// createBasicBlock().)
  FunctionSection CurFunctionSection = FunctionSection::Ordinary;

  /// Does this function require a non-void direct return?
  bool NeedsReturn = false;

  /// Is emission currently within a formal modification?
  bool isInFormalEvaluationScope() const {
    return FormalEvalContext.isInFormalEvaluationScope();
  }

  /// Is emission currently within an inout conversion?
  bool InInOutConversionScope = false;

  /// The SILGenBuilder used to construct the SILFunction.  It is what maintains
  /// the notion of the current block being emitted into.
  SILGenBuilder B;

  SILOpenedArchetypesTracker OpenedArchetypesTracker;

  struct BreakContinueDest {
    LabeledStmt *Target;
    JumpDest BreakDest;
    JumpDest ContinueDest;
  };
  
  std::vector<BreakContinueDest> BreakContinueDestStack;
  std::vector<PatternMatchContext*> SwitchStack;
  /// Keep track of our current nested scope.
  std::vector<SILDebugScope*> DebugScopeStack;

  /// The cleanup depth and BB for when the operand of a
  /// BindOptionalExpr is a missing value.
  SmallVector<JumpDest, 2> BindOptionalFailureDests;

  /// The cleanup depth and epilog BB for "return" statements.
  JumpDest ReturnDest = JumpDest::invalid();
  /// The cleanup depth and epilog BB for "fail" statements.
  JumpDest FailDest = JumpDest::invalid();

  /// The destination for throws.  The block will always be in the
  /// postmatter and takes a BB argument of the exception type.
  JumpDest ThrowDest = JumpDest::invalid();

  /// The destination for coroutine unwinds.  The block will always
  /// be in the postmatter.
  JumpDest CoroutineUnwindDest = JumpDest::invalid();
    
  /// The SIL location corresponding to the AST node being processed.
  SILLocation CurrentSILLoc;

  /// This records information about the currently active cleanups.
  CleanupManager Cleanups;

  /// The current context where formal evaluation cleanups are managed.
  FormalEvaluationContext FormalEvalContext;

  /// VarLoc - representation of an emitted local variable or constant.  There
  /// are three scenarios here:
  ///
  ///  1) This could be a simple "var" or "let" emitted into an alloc_box.  In
  ///     this case, 'value' contains a pointer (it is always an address) to the
  ///     value, and 'box' contains a pointer to the retain count for the box.
  ///  2) This could be a simple non-address-only "let" represented directly. In
  ///     this case, 'value' is the value of the let and is never of address
  ///     type.  'box' is always nil.
  ///  3) This could be an address-only "let" emitted into an alloc_stack, or
  ///     passed in from somewhere else that has guaranteed lifetime (e.g. an
  ///     incoming argument of 'in_guaranteed' convention).  In this case,
  ///     'value' is a pointer to the memory (and thus, its type is always an
  ///     address) and the 'box' is nil.
  ///
  /// Generally, code shouldn't be written to enumerate these three cases, it
  /// should just handle the case of "box or not" or "address or not", depending
  /// on what the code cares about.
  struct VarLoc {
    /// value - the value of the variable, or the address the variable is
    /// stored at (if "value.getType().isAddress()" is true).
    SILValue value;

    /// box - This is the retainable box for something emitted to an alloc_box.
    /// It may be invalid if no box was made for the value (e.g., because it was
    /// an inout value, or constant emitted to an alloc_stack).
    SILValue box;

    static VarLoc get(SILValue value, SILValue box = SILValue()) {
      VarLoc Result;
      Result.value = value;
      Result.box = box;
      return Result;
    }
  };
  
  /// VarLocs - Entries in this map are generated when a PatternBindingDecl is
  /// emitted. The map is queried to produce the lvalue for a DeclRefExpr to
  /// a local variable.
  llvm::DenseMap<ValueDecl*, VarLoc> VarLocs;
 
  /// When rebinding 'self' during an initializer delegation, we have to be
  /// careful to preserve the object at 1 retain count during the delegation
  /// because of assumptions in framework code. This enum tracks the state of
  /// 'self' during the delegation.
  enum SelfInitDelegationStates {
    // 'self' is a normal variable.
    NormalSelf,

    /// 'self' needs to be shared borrowed next time self is used.
    ///
    /// At this point we do not know if:
    ///
    /// 1. 'self' is used at all. In such a case, the borrow scope for self will
    ///     end before the delegating init call and we will overwrite the value
    ///     in
    ///     the self box.
    ///
    /// 2. If there is a consuming self use, will self be borrowed in an
    ///    exclusive manner or a shared manner. If we need to perform an
    ///    exclusive borrow, we will transition to WillExclusiveBorrowSelf in
    ///    SILGenApply.
    WillSharedBorrowSelf,

    /// 'self' needs to be exclusively borrowed next time self is used.
    ///
    /// We only advance to this state in SILGenApply when we know that we are
    /// going to be passing self to a delegating initializer that will consume
    /// it. We will always evaluate self before any other uses of self in the
    /// self.init call, so we know that we will never move from
    /// WillExclusiveBorrowSelf to WillSharedBorrowSelf.
    ///
    /// Once we are in this point, all other uses of self must be borrows until
    /// we use self in the delegating init call. All of the borrow scopes /must/
    /// end before the delegating init call.
    WillExclusiveBorrowSelf,

    /// 'self' was shared borrowed to compute the self argument of the
    /// delegating init call.
    ///
    /// This means that the delegating init uses a metatype or the like as its
    /// self argument instead of 'self'. Thus we are able to perform a shared
    /// borrow of self to compute that value and end the shared borrow scope
    /// before the delegating initializer apply.
    DidSharedBorrowSelf,

    // 'self' was exclusively borrowed for the delegating init call. All further
    // uses of self until the actual delegating init must be done via shared
    // borrows that end strictly before the delegating init call.
    DidExclusiveBorrowSelf,
  };
  SelfInitDelegationStates SelfInitDelegationState = NormalSelf;
  ManagedValue InitDelegationSelf;
  SILValue InitDelegationSelfBox;
  Optional<SILLocation> InitDelegationLoc;
  ManagedValue SuperInitDelegationSelf;

  RValue emitRValueForSelfInDelegationInit(SILLocation loc, CanType refType,
                                           SILValue result, SGFContext C);

  /// A version of emitRValueForSelfInDelegationInit that uses formal evaluation
  /// operations instead of normal scoped operations.
  RValue emitFormalEvaluationRValueForSelfInDelegationInit(SILLocation loc,
                                                           CanType refType,
                                                           SILValue addr,
                                                           SGFContext C);
  /// The metatype argument to an allocating constructor, if we're emitting one.
  SILValue AllocatorMetatype;

  /// True if 'return' without an operand or falling off the end of the current
  /// function is valid.
  bool allowsVoidReturn() const { return ReturnDest.getBlock()->args_empty(); }

  /// Emit code to increment a counter for profiling.
  void emitProfilerIncrement(ASTNode Node);

  /// Load the profiled execution count corresponding to \p Node, if one is
  /// available.
  ProfileCounter loadProfilerCount(ASTNode Node) const;

  /// Get the PGO node's parent.
  Optional<ASTNode> getPGOParent(ASTNode Node) const;

  /// Tracer object for counting SIL (and other events) caused by this instance.
  FrontendStatsTracer StatsTracer;

  SILGenFunction(SILGenModule &SGM, SILFunction &F, DeclContext *DC);
  ~SILGenFunction();
  
  /// Return a stable reference to the current cleanup.
  CleanupsDepth getCleanupsDepth() const {
    return Cleanups.getCleanupsDepth();
  }
  CleanupHandle getTopCleanup() const {
    return Cleanups.getTopCleanup();
  }
  
  SILFunction &getFunction() { return F; }
  SILModule &getModule() { return F.getModule(); }
  SILGenBuilder &getBuilder() { return B; }
  SILOptions &getOptions() { return getModule().getOptions(); }

  // Returns the type expansion context for types in this function.
  TypeExpansionContext getTypeExpansionContext() {
    return TypeExpansionContext(getFunction());
  }

  const TypeLowering &getTypeLowering(AbstractionPattern orig, Type subst) {
    return F.getTypeLowering(orig, subst);
  }
  const TypeLowering &getTypeLowering(Type t) {
    return F.getTypeLowering(t);
  }
  CanSILFunctionType getSILFunctionType(TypeExpansionContext context,
                                        AbstractionPattern orig,
                                        CanFunctionType substFnType) {
    return SGM.Types.getSILFunctionType(context, orig, substFnType);
  }
  SILType getLoweredType(AbstractionPattern orig,
                         Type subst) {
    return F.getLoweredType(orig, subst);
  }
  SILType getLoweredType(Type t) {
    return F.getLoweredType(t);
  }
  SILType getLoweredTypeForFunctionArgument(Type t) {
    auto typeForConv =
        SGM.Types.getLoweredType(t, TypeExpansionContext::minimal());
    return getLoweredType(t).getCategoryType(typeForConv.getCategory());
  }

  SILType getLoweredLoadableType(Type t) {
    return F.getLoweredLoadableType(t);
  }
  const TypeLowering &getTypeLowering(SILType type) {
    return F.getTypeLowering(type);
  }

  SILType getSILType(SILParameterInfo param, CanSILFunctionType fnTy) const {
    return silConv.getSILType(param, fnTy);
  }
  SILType getSILType(SILResultInfo result, CanSILFunctionType fnTy) const {
    return silConv.getSILType(result, fnTy);
  }

  SILType getSILTypeInContext(SILResultInfo result, CanSILFunctionType fnTy) {
    auto t = F.mapTypeIntoContext(getSILType(result, fnTy));
    return getTypeLowering(t).getLoweredType().getCategoryType(t.getCategory());
  }

  SILType getSILTypeInContext(SILParameterInfo param, CanSILFunctionType fnTy) {
    auto t = F.mapTypeIntoContext(getSILType(param, fnTy));
    return getTypeLowering(t).getLoweredType().getCategoryType(t.getCategory());
  }

  const SILConstantInfo &getConstantInfo(TypeExpansionContext context,
                                         SILDeclRef constant) {
    return SGM.Types.getConstantInfo(context, constant);
  }

  Optional<SILAccessEnforcement> getStaticEnforcement(VarDecl *var = nullptr);
  Optional<SILAccessEnforcement> getDynamicEnforcement(VarDecl *var = nullptr);
  Optional<SILAccessEnforcement> getUnknownEnforcement(VarDecl *var = nullptr);

  SourceManager &getSourceManager() { return SGM.M.getASTContext().SourceMgr; }

  /// Push a new debug scope and set its parent pointer.
  void enterDebugScope(SILLocation Loc) {
    auto *Parent =
        DebugScopeStack.size() ? DebugScopeStack.back() : F.getDebugScope();
    auto *DS = new (SGM.M)
        SILDebugScope(Loc.getAsRegularLocation(), &getFunction(), Parent);
    DebugScopeStack.push_back(DS);
    B.setCurrentDebugScope(DS);
  }

  /// Return to the previous debug scope.
  void leaveDebugScope() {
    DebugScopeStack.pop_back();
    if (DebugScopeStack.size())
      B.setCurrentDebugScope(DebugScopeStack.back());
    // Don't reset the debug scope after leaving the outermost scope,
    // because the debugger is not expecting the function epilogue to
    // be in a different scope.
  }

  std::unique_ptr<Initialization>
  prepareIndirectResultInit(CanType formalResultType,
                            SmallVectorImpl<SILValue> &directResultsBuffer,
                            SmallVectorImpl<CleanupHandle> &cleanups);

  //===--------------------------------------------------------------------===//
  // Entry points for codegen
  //===--------------------------------------------------------------------===//
  
  /// Generates code for a FuncDecl.
  void emitFunction(FuncDecl *fd);
  /// Emits code for a ClosureExpr.
  void emitClosure(AbstractClosureExpr *ce);
  /// Generates code for a class destroying destructor. This
  /// emits the body code from the DestructorDecl, calls the base class 
  /// destructor, then implicitly releases the elements of the class.
  void emitDestroyingDestructor(DestructorDecl *dd);

  /// Generates code for an artificial top-level function that starts an
  /// application based on a main class.
  void emitArtificialTopLevel(ClassDecl *mainClass);
  
  /// Generates code for a class deallocating destructor. This
  /// calls the destroying destructor and then deallocates 'self'.
  void emitDeallocatingDestructor(DestructorDecl *dd);
  
  /// Generates code for a struct constructor.
  /// This allocates the new 'self' value, emits the
  /// body code, then returns the final initialized 'self'.
  void emitValueConstructor(ConstructorDecl *ctor);
  /// Generates code for an enum case constructor.
  /// This allocates the new 'self' value, injects the enum case,
  /// then returns the final initialized 'self'.
  void emitEnumConstructor(EnumElementDecl *element);
  /// Generates code for a class constructor's
  /// allocating entry point. This allocates the new 'self' value, passes it to
  /// the initializer entry point, then returns the initialized 'self'.
  void emitClassConstructorAllocator(ConstructorDecl *ctor);
  /// Generates code for a class constructor's
  /// initializing entry point. This takes 'self' and the constructor arguments
  /// as parameters and executes the constructor body to initialize 'self'.
  void emitClassConstructorInitializer(ConstructorDecl *ctor);
  /// Generates code to initialize instance variables from their
  /// initializers.
  ///
  /// \param dc The DeclContext containing the current function.
  /// \param selfDecl The 'self' declaration within the current function.
  /// \param nominal The type whose members are being initialized.
  void emitMemberInitializers(DeclContext *dc, VarDecl *selfDecl,
                              NominalTypeDecl *nominal);

  /// Emit a method that initializes the ivars of a class.
  void emitIVarInitializer(SILDeclRef ivarInitializer);

  /// Emit a method that destroys the ivars of a class.
  void emitIVarDestroyer(SILDeclRef ivarDestroyer);

  /// Generates code to destroy the instance variables of a class.
  ///
  /// \param selfValue The 'self' value.
  /// \param cd The class declaration whose members are being destroyed.
  void emitClassMemberDestruction(ManagedValue selfValue, ClassDecl *cd,
                                  CleanupLocation cleanupLoc);

  /// Generates code for a curry thunk from one uncurry level
  /// of a function to another.
  void emitCurryThunk(SILDeclRef thunk);
  /// Generates a thunk from a foreign function to the native Swift convention.
  void emitForeignToNativeThunk(SILDeclRef thunk);
  /// Generates a thunk from a native function to the conventions.
  void emitNativeToForeignThunk(SILDeclRef thunk);
  
  /// Generate a nullary function that returns the given value.
  /// If \p emitProfilerIncrement is set, emit a profiler increment for
  /// \p value.
  void emitGeneratorFunction(SILDeclRef function, Expr *value,
                             bool emitProfilerIncrement = false);

  /// Generate a nullary function that returns the value of the given variable's
  /// expression initializer.
  void emitGeneratorFunction(SILDeclRef function, VarDecl *var);

  /// Generate an ObjC-compatible destructor (-dealloc).
  void emitObjCDestructor(SILDeclRef dtor);

  ManagedValue emitGlobalVariableRef(SILLocation loc, VarDecl *var);

  /// Generate a lazy global initializer.
  void emitLazyGlobalInitializer(PatternBindingDecl *binding,
                                 unsigned pbdEntry);
  
  /// Generate a global accessor, using the given initializer token and
  /// function
  void emitGlobalAccessor(VarDecl *global,
                          SILGlobalVariable *onceToken,
                          SILFunction *onceFunc);

  /// Generate a protocol witness entry point, invoking 'witness' at the
  /// abstraction level of 'requirement'.
  ///
  /// This is used for both concrete witness thunks and default witness
  /// thunks.
  void emitProtocolWitness(AbstractionPattern reqtOrigTy,
                           CanAnyFunctionType reqtSubstTy,
                           SILDeclRef requirement,
                           SubstitutionMap reqtSubs,
                           SILDeclRef witness,
                           SubstitutionMap witnessSubs,
                           IsFreeFunctionWitness_t isFree,
                           bool isSelfConformance);

  /// Generates subscript arguments for keypath. This function handles lowering
  /// of all index expressions including default arguments.
  ///
  /// \returns Lowered index arguments.
  /// \param subscript - The subscript decl who's arguments are being lowered.
  /// \param subs - Used to get subscript function type and to substitute generic args.
  /// \param indexExpr - An expression holding the indices of the
  /// subscript (either a TupleExpr or a ParenExpr).
  SmallVector<ManagedValue, 4>
  emitKeyPathSubscriptOperands(SubscriptDecl *subscript, SubstitutionMap subs,
                               Expr *indexExpr);

  /// Convert a block to a native function with a thunk.
  ManagedValue emitBlockToFunc(SILLocation loc,
                               ManagedValue block,
                               CanAnyFunctionType blockTy,
                               CanAnyFunctionType funcTy,
                               CanSILFunctionType loweredFuncTy);

  /// Convert a native function to a block with a thunk.
  ManagedValue emitFuncToBlock(SILLocation loc,
                               ManagedValue block,
                               CanAnyFunctionType funcTy,
                               CanAnyFunctionType blockTy,
                               CanSILFunctionType loweredBlockTy);

  /// Thunk with the signature of a base class method calling a derived class
  /// method.
  ///
  /// \param inputOrigType Abstraction pattern of base class method
  /// \param inputSubstType Formal AST type of base class method
  /// \param outputSubstType Formal AST type of derived class method
  /// \param baseLessVisibleThanDerived If true, the thunk does a
  /// double dispatch to the derived method's vtable entry, so that if
  /// the derived method has an override that cannot access the base,
  /// calls to the base dispatch to the correct method.
  void emitVTableThunk(SILDeclRef base,
                       SILDeclRef derived,
                       SILFunction *implFn,
                       AbstractionPattern inputOrigType,
                       CanAnyFunctionType inputSubstType,
                       CanAnyFunctionType outputSubstType,
                       bool baseLessVisibleThanDerived);
  
  //===--------------------------------------------------------------------===//
  // Control flow
  //===--------------------------------------------------------------------===//

  /// emitCondition - Emit a boolean expression as a control-flow condition.
  ///
  /// \param E - The expression to be evaluated as a condition.
  /// \param invertValue - true if this routine should invert the value before
  ///        testing true/false.
  /// \param contArgs - the types of the arguments to the continuation BB.
  ///        Matching argument values must be passed to exitTrue and exitFalse
  ///        of the resulting Condition object.
  /// \param NumTrueTaken - The number of times the condition evaluates to true.
  /// \param NumFalseTaken - The number of times the condition evaluates to
  /// false.
  ///
  /// If `contArgs` is nonempty, then both Condition::exitTrue() and
  /// Condition::exitFalse() must be called.
  Condition emitCondition(Expr *E, bool invertValue = false,
                          ArrayRef<SILType> contArgs = {},
                          ProfileCounter NumTrueTaken = ProfileCounter(),
                          ProfileCounter NumFalseTaken = ProfileCounter());

  Condition emitCondition(SILValue V, SILLocation Loc, bool invertValue = false,
                          ArrayRef<SILType> contArgs = {},
                          ProfileCounter NumTrueTaken = ProfileCounter(),
                          ProfileCounter NumFalseTaken = ProfileCounter());

  /// Create a new basic block.
  ///
  /// The block can be explicitly placed after a particular block.
  /// Otherwise, if the current insertion point is valid, it will be
  /// placed immediately after it.  Otherwise, it will be placed at the
  /// end of the current function section.
  ///
  /// Because basic blocks are generally constructed with an insertion
  /// point active, users should be aware that this behavior leads to
  /// an emergent LIFO ordering: if code generation requires multiple
  /// blocks, the second block created will be positioned before the
  /// first block.  (This is clearly desirable behavior when blocks
  /// are created by different emissions; it's just a little
  /// counter-intuitive within a single emission.)
  SILBasicBlock *createBasicBlock();
  SILBasicBlock *createBasicBlockAfter(SILBasicBlock *afterBB);
  SILBasicBlock *createBasicBlockBefore(SILBasicBlock *beforeBB);

  /// Create a new basic block at the end of the given function
  /// section.
  SILBasicBlock *createBasicBlock(FunctionSection section);

  SILBasicBlock *createBasicBlockAndBranch(SILLocation loc,
                                           SILBasicBlock *destBB);

  /// Erase a basic block that was speculatively created and turned
  /// out to be unneeded.
  ///
  /// This should be called instead of eraseFromParent() in order to
  /// keep SILGen's internal bookkeeping consistent. 
  ///
  /// The block should be empty and have no predecessors.
  void eraseBasicBlock(SILBasicBlock *block);

  void mergeCleanupBlocks();

  //===--------------------------------------------------------------------===//
  // Memory management
  //===--------------------------------------------------------------------===//

  /// Emit debug info for the artificial error inout argument.
  void emitErrorArgument(SILLocation Loc, unsigned ArgNo);

  /// emitProlog - Generates prolog code to allocate and clean up mutable
  /// storage for closure captures and local arguments.
  void emitProlog(CaptureInfo captureInfo,
                  ParameterList *paramList, ParamDecl *selfParam,
                  DeclContext *DC, Type resultType,
                  bool throws, SourceLoc throwsLoc);
  /// returns the number of variables in paramPatterns.
  uint16_t emitProlog(ParameterList *paramList, ParamDecl *selfParam,
                      Type resultType, DeclContext *DC,
                      bool throws, SourceLoc throwsLoc);

  /// Create SILArguments in the entry block that bind a single value
  /// of the given parameter suitably for being forwarded.
  void bindParameterForForwarding(ParamDecl *param,
                                  SmallVectorImpl<SILValue> &parameters);

  /// Create SILArguments in the entry block that bind all the values
  /// of the given parameter list suitably for being forwarded.
  void bindParametersForForwarding(const ParameterList *params,
                                   SmallVectorImpl<SILValue> &parameters);

  /// Create (but do not emit) the epilog branch, and save the
  /// current cleanups depth as the destination for return statement branches.
  ///
  /// \param returnType  If non-null, the epilog block will be created with an
  ///                    argument of this type to receive the return value for
  ///                    the function.
  /// \param isThrowing  If true, create an error epilog block.
  /// \param L           The SILLocation which should be associated with
  ///                    cleanup instructions.
  void prepareEpilog(Type returnType, bool isThrowing, CleanupLocation L);
  void prepareRethrowEpilog(CleanupLocation l);
  void prepareCoroutineUnwindEpilog(CleanupLocation l);
  
  /// Branch to and emit the epilog basic block. This will fuse
  /// the epilog to the current basic block if the epilog bb has no predecessor.
  /// The insertion point will be moved into the epilog block if it is
  /// reachable.
  ///
  /// \param TopLevelLoc The location of the top level AST node for which we are
  ///            constructing the epilog, such as a AbstractClosureExpr.
  /// \returns None if the epilog block is unreachable. Otherwise, returns
  ///          the epilog block's return value argument, or a null SILValue if
  ///          the epilog doesn't take a return value. Also returns the location
  ///          of the return instruction if the epilog block is supposed to host
  ///          the ReturnLocation (This happens in case the predecessor block is
  ///          merged with the epilog block.)
  std::pair<Optional<SILValue>, SILLocation>
    emitEpilogBB(SILLocation TopLevelLoc);
  
  /// Emits a standard epilog which runs top-level cleanups then returns
  /// the function return value, if any.  This can be customized by clients, who
  /// set UsesCustomEpilog to true, and optionally inject their own code into
  /// the epilog block before calling this.  If they do this, their code is run
  /// before the top-level cleanups, and the epilog block to continue is
  /// returned as the insertion point of this function.  They must provide the
  /// final exit sequence for the block as well.
  ///
  /// \param TopLevelLoc The location of the top-level expression during whose
  ///        evaluation the epilog is being produced, for example, the
  ///        AbstractClosureExpr.
  /// \param UsesCustomEpilog True if the client wants to manage its own epilog
  ///        logic.
  SILLocation emitEpilog(SILLocation TopLevelLoc,bool UsesCustomEpilog = false);

  /// Emits the standard rethrow epilog using a Swift error result.
  void emitRethrowEpilog(SILLocation topLevelLoc);

  /// Emits the coroutine-unwind epilog.
  void emitCoroutineUnwindEpilog(SILLocation topLevelLoc);

  /// emitSelfDecl - Emit a SILArgument for 'self', register it in varlocs, set
  /// up debug info, etc.  This returns the 'self' value.
  SILValue emitSelfDecl(VarDecl *selfDecl);
  
  /// Emits a temporary allocation that will be deallocated automatically at the
  /// end of the current scope. Returns the address of the allocation.
  SILValue emitTemporaryAllocation(SILLocation loc, SILType ty);
  
  /// Prepares a buffer to receive the result of an expression, either using the
  /// 'emit into' initialization buffer if available, or allocating a temporary
  /// allocation if not.
  ///
  /// The caller should call manageBufferForExprResult at the instant
  /// that the buffer has been initialized.
  SILValue getBufferForExprResult(SILLocation loc, SILType ty, SGFContext C);

  /// Flag that the buffer for an expression result has been properly
  /// initialized.
  ///
  /// Returns an empty value if the buffer was taken from the context.
  ManagedValue manageBufferForExprResult(SILValue buffer,
                                         const TypeLowering &bufferTL,
                                         SGFContext C);
  
  //===--------------------------------------------------------------------===//
  // Type conversions for expr emission and thunks
  //===--------------------------------------------------------------------===//

  ManagedValue emitInjectEnum(SILLocation loc,
                              ArgumentSource &&payload,
                              SILType enumTy,
                              EnumElementDecl *element,
                              SGFContext C);

  ManagedValue emitInjectOptional(SILLocation loc,
                                  const TypeLowering &expectedTL,
                                  SGFContext ctxt,
                       llvm::function_ref<ManagedValue(SGFContext)> generator);

  /// Initialize a memory location with an optional value.
  ///
  /// \param loc   The location to use for the resulting optional.
  /// \param value The value to inject into an optional.
  /// \param dest  The uninitialized memory in which to store the result value.
  /// \param optTL Type lowering information for the optional to create.
  void emitInjectOptionalValueInto(SILLocation loc,
                                   ArgumentSource &&value,
                                   SILValue dest,
                                   const TypeLowering &optTL);

  /// Initialize a memory location with an optional "nothing"
  /// value.
  ///
  /// \param loc   The location to use for the resulting optional.
  /// \param dest  The uninitialized memory in which to store the result value.
  /// \param optTL Type lowering information for the optional to create.
  void emitInjectOptionalNothingInto(SILLocation loc,
                                     SILValue dest,
                                     const TypeLowering &optTL);

  /// Return a value for an optional ".None" of the specified type. This only
  /// works for loadable enum types.
  SILValue getOptionalNoneValue(SILLocation loc, const TypeLowering &optTL);

  /// Return a value for an optional ".Some(x)" of the specified type. This only
  /// works for loadable enum types.
  ManagedValue getOptionalSomeValue(SILLocation loc, ManagedValue value,
                                    const TypeLowering &optTL);


  struct SourceLocArgs {
    ManagedValue filenameStartPointer,
                 filenameLength,
                 filenameIsAscii,
                 line,
                 column;
  };

  /// Emit raw lowered arguments for a runtime diagnostic to report the given
  /// source location:
  /// - The first three arguments are the components necessary to construct
  ///   a StaticString for the filename: start pointer, length, and
  ///   "is ascii" bit.
  /// - The fourth argument is the line number.
  SourceLocArgs
  emitSourceLocationArgs(SourceLoc loc, SILLocation emitLoc);

  /// Emit a call to the library intrinsic _doesOptionalHaveValue.
  ///
  /// The result is a Builtin.Int1.
  SILValue emitDoesOptionalHaveValue(SILLocation loc, SILValue addrOrValue);

  /// Emit a switch_enum to call the library intrinsic
  /// _diagnoseUnexpectedNilOptional if the optional has no value. Return the
  /// MangedValue resulting from the success case.
  ManagedValue emitPreconditionOptionalHasValue(SILLocation loc,
                                                ManagedValue optional,
                                                bool isImplicitUnwrap);

  /// Emit a call to the library intrinsic _getOptionalValue
  /// given the address of the optional, which checks that an optional contains
  /// some value and either returns the value or traps if there is none.
  ManagedValue emitCheckedGetOptionalValueFrom(SILLocation loc,
                                               ManagedValue addr,
                                               bool isImplicitUnwrap,
                                               const TypeLowering &optTL,
                                               SGFContext C);
  
  /// Extract the value from an optional, which must be known to contain
  /// a value.
  ManagedValue emitUncheckedGetOptionalValueFrom(SILLocation loc,
                                                 ManagedValue addrOrValue,
                                                 const TypeLowering &optTL,
                                                 SGFContext C = SGFContext());

  typedef llvm::function_ref<ManagedValue(SILGenFunction &SGF,
                                    SILLocation loc,
                                    ManagedValue input,
                                    SILType loweredResultTy,
                                    SGFContext context)> ValueTransformRef;

  /// Emit a transformation on the value of an optional type.
  ManagedValue emitOptionalToOptional(SILLocation loc,
                                      ManagedValue input,
                                      SILType loweredResultTy,
                                      ValueTransformRef transform,
                                      SGFContext C = SGFContext());

  ManagedValue emitOptionalSome(SILLocation loc, SILType optionalTy,
                                ValueProducerRef injector,
                                SGFContext C = SGFContext());

  /// Emit a reinterpret-cast from one pointer type to another, using a library
  /// intrinsic.
  RValue emitPointerToPointer(SILLocation loc,
                              ManagedValue input,
                              CanType inputTy,
                              CanType outputTy,
                              SGFContext C = SGFContext());

  ManagedValue emitClassMetatypeToObject(SILLocation loc,
                                         ManagedValue v,
                                         SILType resultTy);

  ManagedValue emitExistentialMetatypeToObject(SILLocation loc,
                                               ManagedValue v,
                                               SILType resultTy);

  ManagedValue emitProtocolMetatypeToObject(SILLocation loc,
                                            CanType inputTy,
                                            SILType resultTy);

  ManagedValue manageOpaqueValue(ManagedValue value,
                                 SILLocation loc,
                                 SGFContext C);

  /// Open up the given existential value and project its payload.
  ///
  /// \param existentialValue The existential value.
  /// \param loweredOpenedType The lowered type of the projection, which in
  /// practice will be the openedArchetype, possibly wrapped in a metatype.
  ManagedValue emitOpenExistential(SILLocation loc,
                                   ManagedValue existentialValue,
                                   SILType loweredOpenedType,
                                   AccessKind accessKind);

  /// Wrap the given value in an existential container.
  ///
  /// \param concreteFormalType AST type of value.
  /// \param concreteTL Type lowering of value.
  /// \param existentialTL Type lowering of existential type.
  /// \param F Function reference to emit the existential contents with the
  /// given context.
  ManagedValue emitExistentialErasure(
                            SILLocation loc,
                            CanType concreteFormalType,
                            const TypeLowering &concreteTL,
                            const TypeLowering &existentialTL,
                            ArrayRef<ProtocolConformanceRef> conformances,
                            SGFContext C,
                            llvm::function_ref<ManagedValue (SGFContext)> F,
                            bool allowEmbeddedNSError = true);

  RValue emitCollectionConversion(SILLocation loc,
                                  FuncDecl *fn,
                                  CanType fromCollection,
                                  CanType toCollection,
                                  ManagedValue mv,
                                  SGFContext C);

  //===--------------------------------------------------------------------===//
  // Recursive entry points
  //===--------------------------------------------------------------------===//

  using ASTVisitorType::visit;
  
  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visit(Stmt *S) = delete;

  void emitStmt(Stmt *S);

  void emitBreakOutOf(SILLocation loc, Stmt *S);

  void emitCatchDispatch(DoCatchStmt *S, ManagedValue exn,
                         ArrayRef<CatchStmt*> clauses,
                         JumpDest catchFallthroughDest);

  /// Emit code for the throw expr. If \p emitWillThrow is set then emit a
  /// call to swift_willThrow, that will allow the debugger to place a
  /// breakpoint on throw sites.
  void emitThrow(SILLocation loc, ManagedValue exn, bool emitWillThrow = false);
  
  //===--------------------------------------------------------------------===//
  // Patterns
  //===--------------------------------------------------------------------===//

  SILValue emitOSVersionRangeCheck(SILLocation loc, const VersionRange &range);
  void emitStmtCondition(StmtCondition Cond, JumpDest FalseDest, SILLocation loc,
                         ProfileCounter NumTrueTaken = ProfileCounter(),
                         ProfileCounter NumFalseTaken = ProfileCounter());

  void emitConditionalPBD(PatternBindingDecl *PBD, SILBasicBlock *FailBB);

  void usingImplicitVariablesForPattern(Pattern *pattern, CaseStmt *stmt,
                                        const llvm::function_ref<void(void)> &f);
  void emitSwitchStmt(SwitchStmt *S);
  void emitSwitchFallthrough(FallthroughStmt *S);

  //===--------------------------------------------------------------------===//
  // Expressions
  //===--------------------------------------------------------------------===//
 
  RValue visit(Expr *E) = delete;
 
  /// Generate SIL for the given expression, storing the final result into the
  /// specified Initialization buffer(s). This avoids an allocation and copy if
  /// the result would be allocated into temporary memory normally.
  /// The location defaults to \c E.
  void emitExprInto(Expr *E, Initialization *I, Optional<SILLocation> L = None);

  /// Emit the given expression as an r-value.
  RValue emitRValue(Expr *E, SGFContext C = SGFContext());

  /// Emit the given expression as a +1 r-value.
  ///
  /// *NOTE* This creates the +1 r-value and then pushes that +1 r-value through
  /// a scope. So all temporaries resulting will be cleaned up.
  ///
  /// *NOTE* +0 vs +1 is ignored by this function. The only reason to use the
  /// SGFContext argument is to pass in an initialization.
  RValue emitPlusOneRValue(Expr *E, SGFContext C = SGFContext());

  /// Emit the given expression as a +0 r-value.
  ///
  /// *NOTE* This does not scope the creation of the +0 r-value. The reason why
  /// this is done is that +0 r-values can not be pushed through scopes.
  RValue emitPlusZeroRValue(Expr *E);

  /// Emit the given expression as an r-value with the given conversion
  /// context.  This may be more efficient --- and, in some cases,
  /// semantically different --- than emitting the expression and then
  /// converting the result.
  ///
  /// \param C a context into which to emit the converted result
  ManagedValue emitConvertedRValue(Expr *E, const Conversion &conversion,
                                   SGFContext C = SGFContext());
  ManagedValue emitConvertedRValue(SILLocation loc,
                                   const Conversion &conversion,
                                   SGFContext C,
                                   ValueProducerRef produceValue);

  /// Emit the given expression as an r-value that follows the
  /// abstraction patterns of the original type.
  ManagedValue emitRValueAsOrig(Expr *E, AbstractionPattern origPattern,
                                const TypeLowering &origTL,
                                SGFContext C = SGFContext());

  /// Emit an r-value into temporary memory and return the managed address.
  ManagedValue
  emitMaterializedRValueAsOrig(Expr *E, AbstractionPattern origPattern);
  
  /// Emit the given expression, ignoring its result.
  void emitIgnoredExpr(Expr *E);
  
  /// Emit the given expression as an r-value, then (if it is a tuple), combine
  /// it together into a single ManagedValue.
  ManagedValue emitRValueAsSingleValue(Expr *E, SGFContext C = SGFContext());

  /// Emit 'undef' in a particular formal type.
  ManagedValue emitUndef(Type type);
  ManagedValue emitUndef(SILType type);
  RValue emitUndefRValue(SILLocation loc, Type type);
  
  std::pair<ManagedValue, SILValue>
  emitUninitializedArrayAllocation(Type ArrayTy,
                                   SILValue Length,
                                   SILLocation Loc);

  CleanupHandle enterDeallocateUninitializedArrayCleanup(SILValue array);
  void emitUninitializedArrayDeallocation(SILLocation loc, SILValue array);

  CleanupHandle enterDelegateInitSelfWritebackCleanup(SILLocation loc,
                                                      SILValue address,
                                                      SILValue newValue);

  SILValue emitConversionToSemanticRValue(SILLocation loc, SILValue value,
                                          const TypeLowering &valueTL);

  ManagedValue emitConversionToSemanticRValue(SILLocation loc,
                                              ManagedValue value,
                                              const TypeLowering &valueTL);

  /// Emit the empty tuple value by emitting
  SILValue emitEmptyTuple(SILLocation loc);
  /// "Emit" an RValue representing an empty tuple.
  RValue emitEmptyTupleRValue(SILLocation loc, SGFContext C);

  /// Returns a reference to a constant in global context. For local func decls
  /// this returns the function constant with unapplied closure context.
  SILValue emitGlobalFunctionRef(SILLocation loc, SILDeclRef constant) {
    return emitGlobalFunctionRef(
        loc, constant, getConstantInfo(getTypeExpansionContext(), constant));
  }
  SILValue
  emitGlobalFunctionRef(SILLocation loc, SILDeclRef constant,
                        SILConstantInfo constantInfo,
                        bool callPreviousDynamicReplaceableImpl = false);

  /// Returns a reference to a function value that dynamically dispatches
  /// the function in a runtime-modifiable way.
  ManagedValue emitDynamicMethodRef(SILLocation loc, SILDeclRef constant,
                                    CanSILFunctionType constantTy);

  /// Returns a reference to a vtable-dispatched method.
  SILValue emitClassMethodRef(SILLocation loc, SILValue selfPtr,
                              SILDeclRef constant,
                              CanSILFunctionType constantTy);

  /// Given that a variable is a local stored variable, return its address.
  ManagedValue emitAddressOfLocalVarDecl(SILLocation loc, VarDecl *var,
                                         CanType formalRValueType,
                                         SGFAccessKind accessKind);

  // FIXME: demote this to private state.
  ManagedValue maybeEmitValueOfLocalVarDecl(VarDecl *var);

  /// Produce an RValue for a reference to the specified declaration,
  /// with the given type and in response to the specified expression.  Try to
  /// emit into the specified SGFContext to avoid copies (when provided).
  RValue emitRValueForDecl(SILLocation loc, ConcreteDeclRef decl, Type ty,
                           AccessSemantics semantics,
                           SGFContext C = SGFContext());

  /// Produce a singular RValue for a load from the specified property.
  ///
  /// This is designed to work with RValue ManagedValue bases that are either +0
  /// or +1.
  ///
  /// \arg isBaseGuaranteed This should /only/ be set to true if we know that
  /// the base value will stay alive as long as the returned RValue implying
  /// that it is safe to load/use values as +0.
  RValue emitRValueForStorageLoad(SILLocation loc,
                                  ManagedValue base,
                                  CanType baseFormalType,
                                  bool isSuper, AbstractStorageDecl *storage,
                                  PreparedArguments &&indices,
                                  SubstitutionMap substitutions,
                                  AccessSemantics semantics, Type propTy,
                                  SGFContext C,
                                  bool isBaseGuaranteed = false);

  void emitCaptures(SILLocation loc,
                    SILDeclRef closure,
                    CaptureEmission purpose,
                    SmallVectorImpl<ManagedValue> &captures);

  /// Produce a reference to a function, which may be a local function
  /// with captures. If the function is generic, substitutions must be
  /// given. The result is re-abstracted to the given expected type.
  ManagedValue emitClosureValue(SILLocation loc,
                                SILDeclRef function,
                                CanType expectedType,
                                SubstitutionMap subs);

  PreparedArguments prepareSubscriptIndices(SubscriptDecl *subscript,
                                            SubstitutionMap subs,
                                            AccessStrategy strategy,
                                            Expr *indices);

  ArgumentSource prepareAccessorBaseArg(SILLocation loc, ManagedValue base,
                                        CanType baseFormalType,
                                        SILDeclRef accessor);

  RValue emitGetAccessor(SILLocation loc, SILDeclRef getter,
                         SubstitutionMap substitutions,
                         ArgumentSource &&optionalSelfValue, bool isSuper,
                         bool isDirectAccessorUse,
                         PreparedArguments &&optionalSubscripts, SGFContext C,
                         bool isOnSelfParameter);

  void emitSetAccessor(SILLocation loc, SILDeclRef setter,
                       SubstitutionMap substitutions,
                       ArgumentSource &&optionalSelfValue,
                       bool isSuper, bool isDirectAccessorUse,
                       PreparedArguments &&optionalSubscripts,
                       ArgumentSource &&value,
                       bool isOnSelfParameter);

  bool maybeEmitMaterializeForSetThunk(ProtocolConformanceRef conformance,
                                       SILLinkage linkage,
                                       Type selfInterfaceType, Type selfType,
                                       GenericEnvironment *genericEnv,
                                       AccessorDecl *requirement,
                                       AccessorDecl *witness,
                                       SubstitutionMap witnessSubs);

  ManagedValue emitAddressorAccessor(
      SILLocation loc, SILDeclRef addressor, SubstitutionMap substitutions,
      ArgumentSource &&optionalSelfValue, bool isSuper,
      bool isDirectAccessorUse, PreparedArguments &&optionalSubscripts,
      SILType addressType, bool isOnSelfParameter);

  CleanupHandle emitCoroutineAccessor(SILLocation loc, SILDeclRef accessor,
                                      SubstitutionMap substitutions,
                                      ArgumentSource &&optionalSelfValue,
                                      bool isSuper, bool isDirectAccessorUse,
                                      PreparedArguments &&optionalSubscripts,
                                      SmallVectorImpl<ManagedValue> &yields,
                                      bool isOnSelfParameter);

  RValue emitApplyConversionFunction(SILLocation loc,
                                     Expr *funcExpr,
                                     Type resultType,
                                     RValue &&operand);

  ManagedValue emitManagedRetain(SILLocation loc, SILValue v);
  ManagedValue emitManagedRetain(SILLocation loc, SILValue v,
                                 const TypeLowering &lowering);

  ManagedValue emitManagedLoadCopy(SILLocation loc, SILValue v);
  ManagedValue emitManagedLoadCopy(SILLocation loc, SILValue v,
                                   const TypeLowering &lowering);

  ManagedValue emitManagedStoreBorrow(SILLocation loc, SILValue v,
                                      SILValue addr);
  ManagedValue emitManagedStoreBorrow(SILLocation loc, SILValue v,
                                      SILValue addr,
                                      const TypeLowering &lowering);

  ManagedValue emitManagedLoadBorrow(SILLocation loc, SILValue v);
  ManagedValue emitManagedLoadBorrow(SILLocation loc, SILValue v,
                                     const TypeLowering &lowering);

  ManagedValue emitManagedBeginBorrow(SILLocation loc, SILValue v,
                                      const TypeLowering &lowering);
  ManagedValue emitManagedBeginBorrow(SILLocation loc, SILValue v);

  ManagedValue emitManagedBorrowedRValueWithCleanup(SILValue original,
                                                    SILValue borrowedValue);
  ManagedValue emitManagedBorrowedRValueWithCleanup(
      SILValue original, SILValue borrowedValue, const TypeLowering &lowering);
  ManagedValue emitManagedBorrowedArgumentWithCleanup(SILPhiArgument *arg);
  ManagedValue emitFormalEvaluationManagedBorrowedRValueWithCleanup(
      SILLocation loc, SILValue original, SILValue borrowedValue);
  ManagedValue emitFormalEvaluationManagedBorrowedRValueWithCleanup(
      SILLocation loc, SILValue original, SILValue borrowedValue,
      const TypeLowering &lowering);

  ManagedValue emitFormalEvaluationManagedBeginBorrow(SILLocation loc,
                                                      SILValue v);
  ManagedValue
  emitFormalEvaluationManagedBeginBorrow(SILLocation loc, SILValue v,
                                         const TypeLowering &lowering);

  ManagedValue emitManagedRValueWithCleanup(SILValue v);
  ManagedValue emitManagedRValueWithCleanup(SILValue v,
                                            const TypeLowering &lowering);

  ManagedValue emitManagedBufferWithCleanup(SILValue addr);
  ManagedValue emitManagedBufferWithCleanup(SILValue addr,
                                            const TypeLowering &lowering);

  ManagedValue emitFormalAccessManagedRValueWithCleanup(SILLocation loc,
                                                        SILValue value);
  ManagedValue emitFormalAccessManagedBufferWithCleanup(SILLocation loc,
                                                        SILValue addr);

  void emitSemanticLoadInto(SILLocation loc, SILValue src,
                            const TypeLowering &srcLowering,
                            SILValue dest,
                            const TypeLowering &destLowering,
                            IsTake_t isTake, IsInitialization_t isInit);

  SILValue emitSemanticLoad(SILLocation loc, SILValue src,
                            const TypeLowering &srcLowering,
                            const TypeLowering &rvalueLowering,
                            IsTake_t isTake);

  void emitSemanticStore(SILLocation loc, SILValue value,
                         SILValue dest, const TypeLowering &destTL,
                         IsInitialization_t isInit);
  
  SILValue emitConversionFromSemanticValue(SILLocation loc,
                                           SILValue semanticValue,
                                           SILType storageType);

  SILValue emitUnwrapIntegerResult(SILLocation loc, SILValue value);
  
  /// Load an r-value out of the given address. This does not handle
  /// reabstraction or bridging. If that is needed, use the other emit load
  /// entry point.
  ///
  /// \param rvalueTL - the type lowering for the type-of-rvalue
  ///   of the address
  /// \param isAddrGuaranteed - true if the value in this address
  ///   is guaranteed to be valid for the duration of the current
  ///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
  ManagedValue emitLoad(SILLocation loc, SILValue addr,
                        const TypeLowering &rvalueTL,
                        SGFContext C, IsTake_t isTake,
                        bool isAddrGuaranteed = false);

  /// Load an r-value out of the given address handling re-abstraction and
  /// bridging if required.
  ///
  /// \param rvalueTL - the type lowering for the type-of-rvalue
  ///   of the address
  /// \param isAddrGuaranteed - true if the value in this address
  ///   is guaranteed to be valid for the duration of the current
  ///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
  ManagedValue emitLoad(SILLocation loc, SILValue addr,
                        AbstractionPattern origFormalType,
                        CanType substFormalType,
                        const TypeLowering &rvalueTL,
                        SGFContext C, IsTake_t isTake,
                        bool isAddrGuaranteed = false);

  ManagedValue emitFormalAccessLoad(SILLocation loc, SILValue addr,
                                    const TypeLowering &rvalueTL, SGFContext C,
                                    IsTake_t isTake,
                                    bool isAddrGuaranteed = false);

  void emitAssignToLValue(SILLocation loc, ArgumentSource &&src, LValue &&dest);
  void emitAssignToLValue(SILLocation loc, RValue &&src, LValue &&dest);
  void emitAssignLValueToLValue(SILLocation loc,
                                LValue &&src, LValue &&dest);
  void emitCopyLValueInto(SILLocation loc, LValue &&src,
                          Initialization *dest);
  ManagedValue emitAddressOfLValue(SILLocation loc, LValue &&src,
                                   TSanKind tsanKind = TSanKind::None);
  ManagedValue emitBorrowedLValue(SILLocation loc, LValue &&src,
                                  TSanKind tsanKind = TSanKind::None);
  LValue emitOpenExistentialLValue(SILLocation loc,
                                   LValue &&existentialLV,
                                   CanArchetypeType openedArchetype,
                                   CanType formalRValueType,
                                   SGFAccessKind accessKind);

  RValue emitLoadOfLValue(SILLocation loc, LValue &&src, SGFContext C,
                          bool isBaseLValueGuaranteed = false);

  /// Emit a reference to a method from within another method of the type.
  std::tuple<ManagedValue, SILType>
  emitSiblingMethodRef(SILLocation loc,
                       SILValue selfValue,
                       SILDeclRef methodConstant,
                       SubstitutionMap subMap);
  
  SILValue emitMetatypeOfValue(SILLocation loc, Expr *baseExpr);
  
  void emitReturnExpr(SILLocation loc, Expr *ret);

  void emitYield(SILLocation loc, MutableArrayRef<ArgumentSource> yieldValues,
                 ArrayRef<AbstractionPattern> origTypes,
                 JumpDest unwindDest);
  void emitRawYield(SILLocation loc, ArrayRef<ManagedValue> yieldArgs,
                    JumpDest unwindDest, bool isUniqueYield);

  RValue emitAnyHashableErasure(SILLocation loc,
                                ManagedValue value,
                                Type type,
                                ProtocolConformanceRef conformance,
                                SGFContext C);

  /// Turn a consumable managed value into a +1 managed value.
  ManagedValue getManagedValue(SILLocation loc,
                               ConsumableManagedValue value);

  //
  // Helpers for emitting ApplyExpr chains.
  //

  RValue emitApplyExpr(ApplyExpr *e, SGFContext c);

  /// Emit a function application, assuming that the arguments have been
  /// lowered appropriately for the abstraction level but that the
  /// result does need to be turned back into something matching a
  /// formal type.
  RValue emitApply(ResultPlanPtr &&resultPlan, ArgumentScope &&argScope,
                   SILLocation loc, ManagedValue fn, SubstitutionMap subs,
                   ArrayRef<ManagedValue> args,
                   const CalleeTypeInfo &calleeTypeInfo, ApplyOptions options,
                   SGFContext evalContext);

  RValue emitApplyOfDefaultArgGenerator(SILLocation loc,
                                        ConcreteDeclRef defaultArgsOwner,
                                        unsigned destIndex,
                                        CanType resultType,
                                        AbstractionPattern origResultType,
                                        SGFContext C = SGFContext());

  RValue emitApplyOfStoredPropertyInitializer(
      SILLocation loc,
      VarDecl *anchoringVar,
      SubstitutionMap subs,
      CanType resultType,
      AbstractionPattern origResultType,
      SGFContext C);

  RValue emitApplyOfPropertyWrapperBackingInitializer(
      SILLocation loc,
      VarDecl *var,
      RValue &&originalValue,
      SGFContext C = SGFContext());

  /// A convenience method for emitApply that just handles monomorphic
  /// applications.
  RValue emitMonomorphicApply(SILLocation loc,
                              ManagedValue fn,
                              ArrayRef<ManagedValue> args,
                              CanType foreignResultType,
                              CanType nativeResultType,
                              ApplyOptions options,
                    Optional<SILFunctionTypeRepresentation> overrideRep,
                    const Optional<ForeignErrorConvention> &foreignError,
                              SGFContext ctx = SGFContext());

  RValue emitApplyOfLibraryIntrinsic(SILLocation loc,
                                     FuncDecl *fn,
                                     SubstitutionMap subMap,
                                     ArrayRef<ManagedValue> args,
                                     SGFContext ctx);

  RValue emitApplyAllocatingInitializer(SILLocation loc, ConcreteDeclRef init,
                                        PreparedArguments &&args, Type overriddenSelfType,
                                        SGFContext ctx);

  RValue emitApplyMethod(SILLocation loc, ConcreteDeclRef declRef,
                         ArgumentSource &&self, PreparedArguments &&args,
                         SGFContext C);

  CleanupHandle emitBeginApply(SILLocation loc, ManagedValue fn,
                               SubstitutionMap subs, ArrayRef<ManagedValue> args,
                               CanSILFunctionType substFnType,
                               ApplyOptions options,
                               SmallVectorImpl<ManagedValue> &yields);

  SILValue emitApplyWithRethrow(SILLocation loc, SILValue fn,
                                SILType substFnType,
                                SubstitutionMap subs,
                                ArrayRef<SILValue> args);

  std::pair<SILValue, CleanupHandle>
  emitBeginApplyWithRethrow(SILLocation loc, SILValue fn, SILType substFnType,
                            SubstitutionMap subs, ArrayRef<SILValue> args,
                            SmallVectorImpl<SILValue> &yields);
  void emitEndApplyWithRethrow(SILLocation loc, SILValue token);

  /// Emit a literal that applies the various initializers.
  RValue emitLiteral(LiteralExpr *literal, SGFContext C);

  SILBasicBlock *getTryApplyErrorDest(SILLocation loc,
                                      CanSILFunctionType fnTy,
                                      SILResultInfo exnResult,
                                      bool isSuppressed);

  /// Emit a dynamic member reference.
  RValue emitDynamicMemberRefExpr(DynamicMemberRefExpr *e, SGFContext c);

  /// Emit a dynamic subscript.
  RValue emitDynamicSubscriptExpr(DynamicSubscriptExpr *e, SGFContext c);

  /// Open up the given existential expression and emit its
  /// subexpression in a caller-specified manner.
  ///
  /// \param e The expression.
  ///
  /// \param emitSubExpr A function to call to emit the subexpression
  /// (which will be passed in).
  void emitOpenExistentialExprImpl(OpenExistentialExpr *e,
                                  llvm::function_ref<void(Expr *)> emitSubExpr);

  /// Open up the given existential expression and emit its
  /// subexpression in a caller-specified manner.
  ///
  /// \param e The expression.
  ///
  /// \param emitSubExpr A function to call to emit the subexpression
  /// (which will be passed in).
  template<typename R, typename F>
  R emitOpenExistentialExpr(OpenExistentialExpr *e, F emitSubExpr) {
    Optional<R> result;
    emitOpenExistentialExprImpl(e,
                            [&](Expr *subExpr) {
                              result.emplace(emitSubExpr(subExpr));
                            });
    return std::move(*result);
  }

  /// Open up the given existential expression and emit its
  /// subexpression in a caller-specified manner.
  ///
  /// \param e The expression.
  ///
  /// \param emitSubExpr A function to call to emit the subexpression
  /// (which will be passed in).
  template<typename F>
  void emitOpenExistentialExpr(OpenExistentialExpr *e, F emitSubExpr) {
    emitOpenExistentialExprImpl(e, emitSubExpr);
  }

  /// Mapping from active opaque value expressions to their values.
  llvm::SmallDenseMap<OpaqueValueExpr *, ManagedValue>
    OpaqueValues;

  /// A mapping from opaque value expressions to the open-existential
  /// expression that determines them, used while lowering lvalues.
  llvm::SmallDenseMap<OpaqueValueExpr *, OpenExistentialExpr *>
    OpaqueValueExprs;

  /// RAII object that introduces a temporary binding for an opaque value.
  ///
  /// Each time the opaque value expression is referenced, it will be
  /// retained/released separately. When this RAII object goes out of
  /// scope, the value will be destroyed if requested.
  class OpaqueValueRAII {
    SILGenFunction &Self;
    OpaqueValueExpr *OpaqueValue;

    OpaqueValueRAII(const OpaqueValueRAII &) = delete;
    OpaqueValueRAII &operator=(const OpaqueValueRAII &) = delete;

  public:
    OpaqueValueRAII(SILGenFunction &self, OpaqueValueExpr *opaqueValue,
                    ManagedValue value)
    : Self(self), OpaqueValue(opaqueValue) {
      assert(Self.OpaqueValues.count(OpaqueValue) == 0 &&
             "Opaque value already has a binding");
      Self.OpaqueValues[OpaqueValue] = value;
    }

    ~OpaqueValueRAII();
  };

  /// Emit a conditional checked cast branch. Does not
  /// re-abstract the argument to the success branch. Terminates the
  /// current BB.
  ///
  /// \param loc          The AST location associated with the operation.
  /// \param src          The abstract value to cast.
  /// \param sourceType   The formal source type.
  /// \param targetType   The formal target type.
  /// \param C            Information about the result of the cast.
  /// \param handleTrue   A callback to invoke with the result of the cast
  ///                     in the success path.  The current BB should be
  ///                     terminated.
  /// \param handleFalse  A callback to invoke in the failure path.  The
  ///                     current BB should be terminated.
  void emitCheckedCastBranch(
      SILLocation loc, ConsumableManagedValue src, Type sourceType,
      CanType targetType, SGFContext C,
      llvm::function_ref<void(ManagedValue)> handleTrue,
      llvm::function_ref<void(Optional<ManagedValue>)> handleFalse,
      ProfileCounter TrueCount = ProfileCounter(),
      ProfileCounter FalseCount = ProfileCounter());

  /// Emit a conditional checked cast branch, starting from an
  /// expression.  Terminates the current BB.
  ///
  /// \param loc          The AST location associated with the operation.
  /// \param src          An expression which will generate the value to cast.
  /// \param targetType   The formal target type.
  /// \param C            Information about the result of the cast.
  /// \param handleTrue   A callback to invoke with the result of the cast
  ///                     in the success path.  The current BB should be
  ///                     terminated.
  /// \param handleFalse  A callback to invoke in the failure path.  The
  ///                     current BB should be terminated.
  void emitCheckedCastBranch(
      SILLocation loc, Expr *src, Type targetType, SGFContext C,
      llvm::function_ref<void(ManagedValue)> handleTrue,
      llvm::function_ref<void(Optional<ManagedValue>)> handleFalse,
      ProfileCounter TrueCount = ProfileCounter(),
      ProfileCounter FalseCount = ProfileCounter());

  /// Emit the control flow for an optional 'bind' operation, branching to the
  /// active failure destination if the optional value addressed by optionalAddr
  /// is nil, and leaving the insertion point on the success branch.
  ///
  /// NOTE: This operation does consume the managed value.
  ManagedValue emitBindOptional(SILLocation loc,
                                ManagedValue optionalAddrOrValue,
                                unsigned depth);

  /// Emit the control flow for an optional 'bind' operation, branching to the
  /// active failure destination if the optional value addressed by optionalAddr
  /// is nil, and leaving the insertion point on the success branch.
  ///
  /// NOTE: This operation does not consume the managed address.
  void emitBindOptionalAddress(SILLocation loc, ManagedValue optionalAddr,
                               unsigned depth);

  void emitOptionalEvaluation(SILLocation loc, Type optionalType,
                              SmallVectorImpl<ManagedValue> &results,
                              SGFContext C,
                      llvm::function_ref<void(SmallVectorImpl<ManagedValue> &,
                                              SGFContext primaryC)>
                                generateNormalResults);

  //===--------------------------------------------------------------------===//
  // Bridging thunks
  //===--------------------------------------------------------------------===//

  /// Convert a native Swift value to a value that can be passed as an argument
  /// to or returned as the result of a function with the given calling
  /// convention.
  ManagedValue emitNativeToBridgedValue(SILLocation loc, ManagedValue v,
                                        CanType nativeType,
                                        CanType bridgedType,
                                        SILType loweredBridgedType,
                                        SGFContext C = SGFContext());
  
  /// Convert a value received as the result or argument of a function with
  /// the given calling convention to a native Swift value of the given type.
  ManagedValue emitBridgedToNativeValue(SILLocation loc, ManagedValue v,
                                        CanType bridgedType,
                                        CanType nativeType,
                                        SILType loweredNativeType,
                                        SGFContext C = SGFContext(),
                                        bool isCallResult = false);

  /// Convert a bridged error type to the native Swift Error
  /// representation.  The value may be optional.
  ManagedValue emitBridgedToNativeError(SILLocation loc, ManagedValue v);

  /// Convert a value in the native Swift Error representation to
  /// a bridged error type representation.
  ManagedValue emitNativeToBridgedError(SILLocation loc, ManagedValue v,
                                        CanType nativeType,
                                        CanType bridgedType);
  
  SILValue emitBridgeErrorForForeignError(SILLocation loc,
                                          SILValue nativeError,
                                          SILType bridgedResultType,
                                          SILValue foreignErrorSlot,
                                    const ForeignErrorConvention &foreignError);

  SILValue
  emitBridgeReturnValueForForeignError(SILLocation loc,
                                       SILValue result,
                                       CanType formalNativeType,
                                       CanType formalBridgedType,
                                       SILType bridgedType,
                                       SILValue foreignErrorSlot,
                                 const ForeignErrorConvention &foreignError);

  void emitForeignErrorBlock(SILLocation loc, SILBasicBlock *errorBB,
                             Optional<ManagedValue> errorSlot);

  void emitForeignErrorCheck(SILLocation loc,
                             SmallVectorImpl<ManagedValue> &directResults,
                             ManagedValue errorSlot,
                             bool suppressErrorCheck,
                             const ForeignErrorConvention &foreignError);

  //===--------------------------------------------------------------------===//
  // Re-abstraction thunks
  //===--------------------------------------------------------------------===//

  /// Convert a value with the abstraction patterns of the original type
  /// to a value with the abstraction patterns of the substituted type.
  ManagedValue emitOrigToSubstValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern origType,
                                    CanType substType,
                                    SGFContext ctx = SGFContext());
  RValue emitOrigToSubstValue(SILLocation loc, RValue &&input,
                              AbstractionPattern origType,
                              CanType substType,
                              SGFContext ctx = SGFContext());

  /// Convert a value with the abstraction patterns of the substituted
  /// type to a value with the abstraction patterns of the original type.
  ManagedValue emitSubstToOrigValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern origType,
                                    CanType substType,
                                    SGFContext ctx = SGFContext());
  RValue emitSubstToOrigValue(SILLocation loc, RValue &&input,
                              AbstractionPattern origType,
                              CanType substType,
                              SGFContext ctx = SGFContext());

  /// Transform the AST-level types in the function signature without an
  /// abstraction or representation change.
  ManagedValue emitTransformedValue(SILLocation loc, ManagedValue input,
                                    CanType inputType,
                                    CanType outputType,
                                    SGFContext ctx = SGFContext());

  /// Most general form of the above.
  ManagedValue emitTransformedValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern inputOrigType,
                                    CanType inputSubstType,
                                    AbstractionPattern outputOrigType,
                                    CanType outputSubstType,
                                    SGFContext ctx = SGFContext());
  RValue emitTransformedValue(SILLocation loc, RValue &&input,
                              AbstractionPattern inputOrigType,
                              CanType inputSubstType,
                              AbstractionPattern outputOrigType,
                              CanType outputSubstType,
                              SGFContext ctx = SGFContext());

  /// Used for emitting SILArguments of bare functions, such as thunks.
  void collectThunkParams(
      SILLocation loc, SmallVectorImpl<ManagedValue> &params,
      SmallVectorImpl<SILArgument *> *indirectResultParams = nullptr);

  /// Build the type of a function transformation thunk.
  CanSILFunctionType buildThunkType(CanSILFunctionType &sourceType,
                                    CanSILFunctionType &expectedType,
                                    CanType &inputSubstType,
                                    CanType &outputSubstType,
                                    GenericEnvironment *&genericEnv,
                                    SubstitutionMap &interfaceSubs,
                                    CanType &dynamicSelfType,
                                    bool withoutActuallyEscaping=false);

  //===--------------------------------------------------------------------===//
  // NoEscaping to Escaping closure thunk
  //===--------------------------------------------------------------------===//
  ManagedValue
  createWithoutActuallyEscapingClosure(SILLocation loc,
                                       ManagedValue noEscapingFunctionValue,
                                       SILType escapingFnTy);

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//
  
  void visitDecl(Decl *D) {
    llvm_unreachable("Not yet implemented");
  }

  // Emitted as part of its storage.
  void visitAccessorDecl(AccessorDecl *D) {}

  void visitFuncDecl(FuncDecl *D);
  void visitPatternBindingDecl(PatternBindingDecl *D);

  void emitPatternBinding(PatternBindingDecl *D, unsigned entry);
  
  std::unique_ptr<Initialization>
  emitPatternBindingInitialization(Pattern *P, JumpDest failureDest);
    
  void visitNominalTypeDecl(NominalTypeDecl *D) {
    // No lowering support needed.
  }

  void visitTypeAliasDecl(TypeAliasDecl *D) {
    // No lowering support needed.
  }

  void visitGenericTypeParamDecl(GenericTypeParamDecl *D) {
    // No lowering support needed.
  }
  void visitAssociatedTypeDecl(AssociatedTypeDecl *D) {
    // No lowering support needed.
  }

  void visitPoundDiagnosticDecl(PoundDiagnosticDecl *D) {
    // No lowering support needed.
  }

  void visitVarDecl(VarDecl *D);

  /// Emit an Initialization for a 'var' or 'let' decl in a pattern.
  std::unique_ptr<Initialization> emitInitializationForVarDecl(VarDecl *vd,
                                                               bool immutable);
  
  /// Emit the allocation for a local variable, provides an Initialization
  /// that can be used to initialize it, and registers cleanups in the active
  /// scope.
  /// \param ArgNo optionally describes this function argument's
  /// position for debug info.
  std::unique_ptr<Initialization>
  emitLocalVariableWithCleanup(VarDecl *D,
                               Optional<MarkUninitializedInst::Kind> kind,
                               unsigned ArgNo = 0);

  /// Emit the allocation for a local temporary, provides an
  /// Initialization that can be used to initialize it, and registers
  /// cleanups in the active scope.
  ///
  /// The initialization is guaranteed to be a single buffer.
  std::unique_ptr<TemporaryInitialization>
  emitTemporary(SILLocation loc, const TypeLowering &tempTL);

  /// Emit the allocation for a local temporary, provides an
  /// Initialization that can be used to initialize it, and registers
  /// cleanups in the current active formal evaluation scope.
  ///
  /// The initialization is guaranteed to be a single buffer.
  std::unique_ptr<TemporaryInitialization>
  emitFormalAccessTemporary(SILLocation loc, const TypeLowering &tempTL);

  /// Provides an Initialization that can be used to initialize an already-
  /// allocated temporary, and registers cleanups in the active scope.
  ///
  /// The initialization is guaranteed to be a single buffer.
  std::unique_ptr<TemporaryInitialization>
  useBufferAsTemporary(SILValue addr, const TypeLowering &tempTL);

  /// Enter a currently-dormant cleanup to destroy the value in the
  /// given address.
  CleanupHandle enterDormantTemporaryCleanup(SILValue temp,
                                             const TypeLowering &tempTL);

  CleanupHandle enterDeallocBoxCleanup(SILValue box);

  /// Enter a currently-dormant cleanup to destroy the value in the
  /// given address.
  CleanupHandle
  enterDormantFormalAccessTemporaryCleanup(SILValue temp, SILLocation loc,
                                           const TypeLowering &tempTL);

  /// Destroy and deallocate an initialized local variable.
  void destroyLocalVariable(SILLocation L, VarDecl *D);
  
  /// Deallocate an uninitialized local variable.
  void deallocateUninitializedLocalVariable(SILLocation L, VarDecl *D);

  /// Enter a cleanup to deallocate a stack variable.
  CleanupHandle enterDeallocStackCleanup(SILValue address);
  
  /// Enter a cleanup to emit a ReleaseValue/DestroyAddr of the specified value.
  CleanupHandle enterDestroyCleanup(SILValue valueOrAddr);
  
  /// Enter a cleanup to emit a DeinitExistentialAddr or DeinitExistentialBox
  /// of the specified value.
  CleanupHandle enterDeinitExistentialCleanup(CleanupState state,
                                              SILValue addr,
                                              CanType concreteFormalType,
                                              ExistentialRepresentation repr);

  /// Evaluate an Expr as an lvalue.
  LValue emitLValue(Expr *E, SGFAccessKind accessKind,
                    LValueOptions options = LValueOptions());

  RValue emitRValueForNonMemberVarDecl(SILLocation loc,
                                       ConcreteDeclRef declRef,
                                       CanType formalRValueType,
                                       AccessSemantics semantics,
                                       SGFContext C);

  /// Emit an lvalue that directly refers to the given instance variable
  /// (without going through getters or setters).
  LValue emitPropertyLValue(SILLocation loc, ManagedValue base,
                            CanType baseFormalType, VarDecl *var,
                            LValueOptions options,
                            SGFAccessKind accessKind,
                            AccessSemantics semantics);

  struct PointerAccessInfo {
    CanType PointerType;
    PointerTypeKind PointerKind;
    SGFAccessKind AccessKind;
  };

  PointerAccessInfo getPointerAccessInfo(Type pointerType);
  ManagedValue emitLValueToPointer(SILLocation loc, LValue &&lvalue,
                                   PointerAccessInfo accessInfo);

  struct ArrayAccessInfo {
    Type PointerType;
    Type ArrayType;
    SGFAccessKind AccessKind;
  };
  ArrayAccessInfo getArrayAccessInfo(Type pointerType, Type arrayType);
  std::pair<ManagedValue,ManagedValue>
  emitArrayToPointer(SILLocation loc, LValue &&lvalue,
                     ArrayAccessInfo accessInfo);

  std::pair<ManagedValue,ManagedValue>
  emitArrayToPointer(SILLocation loc, ManagedValue arrayValue,
                     ArrayAccessInfo accessInfo);

  std::pair<ManagedValue,ManagedValue>
  emitStringToPointer(SILLocation loc, ManagedValue stringValue,
                      Type pointerType);

  class ForceTryEmission {
    SILGenFunction &SGF;
    ForceTryExpr *Loc;
    JumpDest OldThrowDest;

  public:
    ForceTryEmission(SILGenFunction &SGF, ForceTryExpr *loc);

    ForceTryEmission(const ForceTryEmission &) = delete;
    ForceTryEmission &operator=(const ForceTryEmission &) = delete;

    void finish();

    ~ForceTryEmission() {
      if (Loc) finish();
    }
  };

  /// Return forwarding substitutions for the archetypes in the current
  /// function.
  SubstitutionMap getForwardingSubstitutionMap();

  /// Get the _Pointer protocol used for pointer argument operations.
  ProtocolDecl *getPointerProtocol();
};


/// A utility class for saving and restoring the insertion point.
class SILGenSavedInsertionPoint {
  SILGenFunction &SGF;
  SILBasicBlock *SavedIP;
  FunctionSection SavedSection;
public:
  SILGenSavedInsertionPoint(SILGenFunction &SGF, SILBasicBlock *newIP,
                            Optional<FunctionSection> optSection = None)
      : SGF(SGF), SavedIP(SGF.B.getInsertionBB()),
        SavedSection(SGF.CurFunctionSection) {
    FunctionSection section = (optSection ? *optSection : SavedSection);
    assert((section != FunctionSection::Postmatter ||
            SGF.StartOfPostmatter != SGF.F.end()) &&
           "trying to move to postmatter without a registered start "
           "of postmatter?");

    SGF.B.setInsertionPoint(newIP);
    SGF.CurFunctionSection = section;
  }

  SILGenSavedInsertionPoint(const SILGenSavedInsertionPoint &) = delete;
  SILGenSavedInsertionPoint &
  operator=(const SILGenSavedInsertionPoint &) = delete;

  ~SILGenSavedInsertionPoint() {
    if (SavedIP) {
      SGF.B.setInsertionPoint(SavedIP);
    } else {
      SGF.B.clearInsertionPoint();
    }
    SGF.CurFunctionSection = SavedSection;
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
