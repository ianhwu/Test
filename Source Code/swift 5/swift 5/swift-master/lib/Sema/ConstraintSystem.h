//===--- ConstraintSystem.h - Constraint-based Type Checking ----*- C++ -*-===//
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
//
// This file provides the constraint-based type checker, anchored by the
// \c ConstraintSystem class, which provides type checking and type
// inference for expressions.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_CONSTRAINT_SYSTEM_H
#define SWIFT_SEMA_CONSTRAINT_SYSTEM_H

#include "CSFix.h"
#include "Constraint.h"
#include "ConstraintGraph.h"
#include "ConstraintGraphScope.h"
#include "ConstraintLocator.h"
#include "OverloadChoice.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/TypeCheckerDebugConsumer.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/OptionSet.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/ilist.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <functional>

namespace swift {

class Expr;

namespace constraints {

class ConstraintGraph;
class ConstraintGraphNode;
class ConstraintSystem;

} // end namespace constraints

} // end namespace swift

/// Allocate memory within the given constraint system.
void *operator new(size_t bytes, swift::constraints::ConstraintSystem& cs,
                   size_t alignment = 8);

namespace swift {

namespace constraints {

/// A handle that holds the saved state of a type variable, which
/// can be restored.
class SavedTypeVariableBinding {
  /// The type variable that we saved the state of.
  TypeVariableType *TypeVar;

  /// The saved type variable options.
  unsigned Options;

  /// The parent or fixed type.
  llvm::PointerUnion<TypeVariableType *, TypeBase *> ParentOrFixed;

public:
  explicit SavedTypeVariableBinding(TypeVariableType *typeVar);

  /// Restore the state of the type variable to the saved state.
  void restore();
};

/// A set of saved type variable bindings.
using SavedTypeVariableBindings = SmallVector<SavedTypeVariableBinding, 16>;

class ConstraintLocator;

/// Describes a conversion restriction or a fix.
struct RestrictionOrFix {
  union {
    ConversionRestrictionKind Restriction;
    ConstraintFix *TheFix;
  };
  bool IsRestriction;

public:
  RestrictionOrFix(ConversionRestrictionKind restriction)
  : Restriction(restriction), IsRestriction(true) { }

  RestrictionOrFix(ConstraintFix *fix) : TheFix(fix), IsRestriction(false) {}

  Optional<ConversionRestrictionKind> getRestriction() const {
    if (IsRestriction)
      return Restriction;

    return None;
  }

  Optional<ConstraintFix *> getFix() const {
    if (!IsRestriction)
      return TheFix;

    return None;
  }
};


class ExpressionTimer {
  Expr* E;
  ASTContext &Context;
  llvm::TimeRecord StartTime;

  bool PrintDebugTiming;
  bool PrintWarning;

public:
  ExpressionTimer(Expr *E, ConstraintSystem &CS);

  ~ExpressionTimer();

  unsigned getWarnLimit() const {
    return Context.TypeCheckerOpts.WarnLongExpressionTypeChecking;
  }
  llvm::TimeRecord startedAt() const { return StartTime; }

  /// Return the elapsed process time (including fractional seconds)
  /// as a double.
  double getElapsedProcessTimeInFractionalSeconds() const {
    llvm::TimeRecord endTime = llvm::TimeRecord::getCurrentTime(false);

    return endTime.getProcessTime() - StartTime.getProcessTime();
  }

  // Disable emission of warnings about expressions that take longer
  // than the warning threshold.
  void disableWarning() { PrintWarning = false; }

  bool isExpired(unsigned thresholdInMillis) const {
    auto elapsed = getElapsedProcessTimeInFractionalSeconds();
    return unsigned(elapsed) > thresholdInMillis;
  }
};

} // end namespace constraints

/// Options that describe how a type variable can be used.
enum TypeVariableOptions {
  /// Whether the type variable can be bound to an lvalue type or not.
  TVO_CanBindToLValue = 0x01,

  /// Whether the type variable can be bound to an inout type or not.
  TVO_CanBindToInOut = 0x02,

  /// Whether the type variable can be bound to a non-escaping type or not.
  TVO_CanBindToNoEscape = 0x04,

  /// Whether the type variable can be bound to a hole type or not.
  TVO_CanBindToHole = 0x08,

  /// Whether a more specific deduction for this type variable implies a
  /// better solution to the constraint system.
  TVO_PrefersSubtypeBinding = 0x10,
};

/// The implementation object for a type variable used within the
/// constraint-solving type checker.
///
/// The implementation object for a type variable contains information about
/// the type variable, where it was generated, what protocols it must conform
/// to, what specific types it might be and, eventually, the fixed type to
/// which it is assigned.
class TypeVariableType::Implementation {
  /// The locator that describes where this type variable was generated.
  constraints::ConstraintLocator *locator;

  /// Either the parent of this type variable within an equivalence
  /// class of type variables, or the fixed type to which this type variable
  /// type is bound.
  llvm::PointerUnion<TypeVariableType *, TypeBase *> ParentOrFixed;

  /// The corresponding node in the constraint graph.
  constraints::ConstraintGraphNode *GraphNode = nullptr;

  ///  Index into the list of type variables, as used by the
  ///  constraint graph.
  unsigned GraphIndex;

  friend class constraints::SavedTypeVariableBinding;

public:
  /// Retrieve the type variable associated with this implementation.
  TypeVariableType *getTypeVariable() {
    return reinterpret_cast<TypeVariableType *>(this) - 1;
  }

  /// Retrieve the type variable associated with this implementation.
  const TypeVariableType *getTypeVariable() const {
    return reinterpret_cast<const TypeVariableType *>(this) - 1;
  }

  explicit Implementation(constraints::ConstraintLocator *locator,
                          unsigned options)
    : locator(locator), ParentOrFixed(getTypeVariable()) {
    getTypeVariable()->Bits.TypeVariableType.Options = options;
  }

  /// Retrieve the unique ID corresponding to this type variable.
  unsigned getID() const { return getTypeVariable()->getID(); }

  unsigned getRawOptions() const {
    return getTypeVariable()->Bits.TypeVariableType.Options;
  }

  void setRawOptions(unsigned bits) {
    getTypeVariable()->Bits.TypeVariableType.Options = bits;
    assert(getTypeVariable()->Bits.TypeVariableType.Options == bits
           && "Trucation");
  }

  /// Whether this type variable can bind to an lvalue type.
  bool canBindToLValue() const { return getRawOptions() & TVO_CanBindToLValue; }

  /// Whether this type variable can bind to an inout type.
  bool canBindToInOut() const { return getRawOptions() & TVO_CanBindToInOut; }

  /// Whether this type variable can bind to an inout type.
  bool canBindToNoEscape() const { return getRawOptions() & TVO_CanBindToNoEscape; }

  /// Whether this type variable can bind to a hole type.
  bool canBindToHole() const { return getRawOptions() & TVO_CanBindToHole; }

  /// Whether this type variable prefers a subtype binding over a supertype
  /// binding.
  bool prefersSubtypeBinding() const {
    return getRawOptions() & TVO_PrefersSubtypeBinding;
  }

  /// Retrieve the corresponding node in the constraint graph.
  constraints::ConstraintGraphNode *getGraphNode() const { return GraphNode; }

  /// Set the corresponding node in the constraint graph.
  void setGraphNode(constraints::ConstraintGraphNode *newNode) { 
    GraphNode = newNode; 
  }

  /// Retrieve the index into the constraint graph's list of type variables.
  unsigned getGraphIndex() const { 
    assert(GraphNode && "Graph node isn't set");
    return GraphIndex;
  }

  /// Set the index into the constraint graph's list of type variables.
  void setGraphIndex(unsigned newIndex) {
    GraphIndex = newIndex;
  }
  
  /// Check whether this type variable either has a representative that
  /// is not itself or has a fixed type binding.
  bool hasRepresentativeOrFixed() const {
    // If we have a fixed type, we're done.
    if (!ParentOrFixed.is<TypeVariableType *>())
      return true;

    // Check whether the representative is different from our own type
    // variable.
    return ParentOrFixed.get<TypeVariableType *>() != getTypeVariable();
  }

  /// Record the current type-variable binding.
  void recordBinding(constraints::SavedTypeVariableBindings &record) {
    record.push_back(constraints::SavedTypeVariableBinding(getTypeVariable()));
  }

  /// Retrieve the locator describing where this type variable was
  /// created.
  constraints::ConstraintLocator *getLocator() const {
    return locator;
  }

  /// Retrieve the generic parameter opened by this type variable.
  GenericTypeParamType *getGenericParameter() const;

  /// Retrieve the representative of the equivalence class to which this
  /// type variable belongs.
  ///
  /// \param record The record of changes made by retrieving the representative,
  /// which can happen due to path compression. If null, path compression is
  /// not performed.
  TypeVariableType *
  getRepresentative(constraints::SavedTypeVariableBindings *record) {
    // Find the representative type variable.
    auto result = getTypeVariable();
    Implementation *impl = this;
    while (impl->ParentOrFixed.is<TypeVariableType *>()) {
      // Extract the representative.
      auto nextTV = impl->ParentOrFixed.get<TypeVariableType *>();
      if (nextTV == result)
        break;

      result = nextTV;
      impl = &nextTV->getImpl();
    }

    if (impl == this || !record)
      return result;

    // Perform path compression.
    impl = this;
    while (impl->ParentOrFixed.is<TypeVariableType *>()) {
      // Extract the representative.
      auto nextTV = impl->ParentOrFixed.get<TypeVariableType *>();
      if (nextTV == result)
        break;

      // Record the state change.
      impl->recordBinding(*record);

      impl->ParentOrFixed = result;
      impl = &nextTV->getImpl();
    }

    return result;
  }

  /// Merge the equivalence class of this type variable with the
  /// equivalence class of another type variable.
  ///
  /// \param other The type variable to merge with.
  ///
  /// \param record The record of state changes.
  void mergeEquivalenceClasses(TypeVariableType *other,
                               constraints::SavedTypeVariableBindings *record) {
    // Merge the equivalence classes corresponding to these two type
    // variables. Always merge 'up' the constraint stack, because it is simpler.
    if (getID() > other->getImpl().getID()) {
      other->getImpl().mergeEquivalenceClasses(getTypeVariable(), record);
      return;
    }

    auto otherRep = other->getImpl().getRepresentative(record);
    if (record)
      otherRep->getImpl().recordBinding(*record);
    otherRep->getImpl().ParentOrFixed = getTypeVariable();

    if (canBindToLValue() && !otherRep->getImpl().canBindToLValue()) {
      if (record)
        recordBinding(*record);
      getTypeVariable()->Bits.TypeVariableType.Options &= ~TVO_CanBindToLValue;
    }

    if (canBindToInOut() && !otherRep->getImpl().canBindToInOut()) {
      if (record)
        recordBinding(*record);
      getTypeVariable()->Bits.TypeVariableType.Options &= ~TVO_CanBindToInOut;
    }

    if (canBindToNoEscape() && !otherRep->getImpl().canBindToNoEscape()) {
      if (record)
        recordBinding(*record);
      getTypeVariable()->Bits.TypeVariableType.Options &= ~TVO_CanBindToNoEscape;
    }
  }

  /// Retrieve the fixed type that corresponds to this type variable,
  /// if there is one.
  ///
  /// \returns the fixed type associated with this type variable, or a null
  /// type if there is no fixed type.
  ///
  /// \param record The record of changes made by retrieving the representative,
  /// which can happen due to path compression. If null, path compression is
  /// not performed.
  Type getFixedType(constraints::SavedTypeVariableBindings *record) {
    // Find the representative type variable.
    auto rep = getRepresentative(record);
    Implementation &repImpl = rep->getImpl();

    // Return the bound type if there is one, otherwise, null.
    return repImpl.ParentOrFixed.dyn_cast<TypeBase *>();
  }

  /// Assign a fixed type to this equivalence class.
  void assignFixedType(Type type,
                       constraints::SavedTypeVariableBindings *record) {
    assert((!getFixedType(0) || getFixedType(0)->isEqual(type)) &&
           "Already has a fixed type!");
    auto rep = getRepresentative(record);
    if (record)
      rep->getImpl().recordBinding(*record);
    rep->getImpl().ParentOrFixed = type.getPointer();
  }

  void setCanBindToLValue(constraints::SavedTypeVariableBindings *record,
                          bool enabled) {
    auto &impl = getRepresentative(record)->getImpl();
    if (record)
      impl.recordBinding(*record);

    if (enabled)
      impl.getTypeVariable()->Bits.TypeVariableType.Options |=
          TVO_CanBindToLValue;
    else
      impl.getTypeVariable()->Bits.TypeVariableType.Options &=
          ~TVO_CanBindToLValue;
  }

  void setCanBindToNoEscape(constraints::SavedTypeVariableBindings *record,
                            bool enabled) {
    auto &impl = getRepresentative(record)->getImpl();
    if (record)
      impl.recordBinding(*record);

    if (enabled)
      impl.getTypeVariable()->Bits.TypeVariableType.Options |=
          TVO_CanBindToNoEscape;
    else
      impl.getTypeVariable()->Bits.TypeVariableType.Options &=
          ~TVO_CanBindToNoEscape;
  }

  void enableCanBindToHole(constraints::SavedTypeVariableBindings *record) {
    auto &impl = getRepresentative(record)->getImpl();
    if (record)
      impl.recordBinding(*record);

    impl.getTypeVariable()->Bits.TypeVariableType.Options |= TVO_CanBindToHole;
  }

  /// Print the type variable to the given output stream.
  void print(llvm::raw_ostream &OS);
};

namespace constraints {

struct ResolvedOverloadSetListItem;

/// The result of comparing two constraint systems that are a solutions
/// to the given set of constraints.
enum class SolutionCompareResult {
  /// The two solutions are incomparable, because, e.g., because one
  /// solution has some better decisions and some worse decisions than the
  /// other.
  Incomparable,
  /// The two solutions are identical.
  Identical,
  /// The first solution is better than the second.
  Better,
  /// The second solution is better than the first.
  Worse
};

/// An overload that has been selected in a particular solution.
///
/// A selected overload captures the specific overload choice (e.g., a
/// particular declaration) as well as the type to which the reference to the
/// declaration was opened, which may involve type variables.
struct SelectedOverload {
  /// The overload choice.
  OverloadChoice choice;

  /// The opened type of the base of the reference to this overload, if
  /// we're referencing a member.
  Type openedFullType;

  /// The opened type produced by referring to this overload.
  Type openedType;
};

/// Describes an aspect of a solution that affects its overall score, i.e., a
/// user-defined conversions.
enum ScoreKind {
  // These values are used as indices into a Score value.

  /// A fix needs to be applied to the source.
  SK_Fix,
  /// A reference to an @unavailable declaration.
  SK_Unavailable,
  /// A use of a disfavored overload.
  SK_DisfavoredOverload,
  /// An implicit force of an implicitly unwrapped optional value.
  SK_ForceUnchecked,
  /// A user-defined conversion.
  SK_UserConversion,
  /// A non-trivial function conversion.
  SK_FunctionConversion,
  /// A literal expression bound to a non-default literal type.
  SK_NonDefaultLiteral,
  /// An implicit upcast conversion between collection types.
  SK_CollectionUpcastConversion,
  /// A value-to-optional conversion.
  SK_ValueToOptional,
  /// A conversion to an empty existential type ('Any' or '{}').
  SK_EmptyExistentialConversion,
  /// A key path application subscript.
  SK_KeyPathSubscript,
  /// A conversion from a string, array, or inout to a pointer.
  SK_ValueToPointerConversion,

  SK_LastScoreKind = SK_ValueToPointerConversion,
};

/// The number of score kinds.
const unsigned NumScoreKinds = SK_LastScoreKind + 1;

/// Describes what happened when a function builder transform was applied
/// to a particular closure.
struct AppliedBuilderTransform {
  /// The builder type that was applied to the closure.
  Type builderType;

  /// The single expression to which the closure was transformed.
  Expr *singleExpr;
};

/// Describes the fixed score of a solution to the constraint system.
struct Score {
  unsigned Data[NumScoreKinds] = {};

  friend Score &operator+=(Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      x.Data[i] += y.Data[i];
    }
    return x;
  }

  friend Score operator+(const Score &x, const Score &y) {
    Score result;
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      result.Data[i] = x.Data[i] + y.Data[i];
    }
    return result;
  }

  friend Score operator-(const Score &x, const Score &y) {
    Score result;
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      result.Data[i] = x.Data[i] - y.Data[i];
    }
    return result;
  }

  friend Score &operator-=(Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      x.Data[i] -= y.Data[i];
    }
    return x;
  }

  friend bool operator==(const Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      if (x.Data[i] != y.Data[i])
        return false;
    }

    return true;
  }

  friend bool operator!=(const Score &x, const Score &y) {
    return !(x == y);
  }

  friend bool operator<(const Score &x, const Score &y) {
    for (unsigned i = 0; i != NumScoreKinds; ++i) {
      if (x.Data[i] < y.Data[i])
        return true;

      if (x.Data[i] > y.Data[i])
        return false;
    }

    return false;
  }

  friend bool operator<=(const Score &x, const Score &y) {
    return !(y < x);
  }

  friend bool operator>(const Score &x, const Score &y) {
    return y < x;
  }

  friend bool operator>=(const Score &x, const Score &y) {
    return !(x < y);
  }

};

/// An AST node that can gain type information while solving.
using TypedNode =
    llvm::PointerUnion3<const Expr *, const TypeLoc *,
                        const VarDecl *>;

/// Display a score.
llvm::raw_ostream &operator<<(llvm::raw_ostream &out, const Score &score);

/// Describes a dependent type that has been opened to a particular type
/// variable.
using OpenedType = std::pair<GenericTypeParamType *, TypeVariableType *>;

using OpenedTypeMap =
    llvm::DenseMap<GenericTypeParamType *, TypeVariableType *>;

/// A complete solution to a constraint system.
///
/// A solution to a constraint system consists of type variable bindings to
/// concrete types for every type variable that is used in the constraint
/// system along with a set of mappings from each constraint locator
/// involving an overload set to the selected overload.
class Solution {
  /// The constraint system this solution solves.
  ConstraintSystem *constraintSystem;

  /// The fixed score for this solution.
  Score FixedScore;

public:
  /// Create a solution for the given constraint system.
  Solution(ConstraintSystem &cs, const Score &score)
    : constraintSystem(&cs), FixedScore(score) {}

  // Solution is a non-copyable type for performance reasons.
  Solution(const Solution &other) = delete;
  Solution &operator=(const Solution &other) = delete;

  Solution(Solution &&other) = default;
  Solution &operator=(Solution &&other) = default;

  size_t getTotalMemory() const;

  /// Retrieve the constraint system that this solution solves.
  ConstraintSystem &getConstraintSystem() const { return *constraintSystem; }

  /// The set of type bindings.
  llvm::DenseMap<TypeVariableType *, Type> typeBindings;
  
  /// The set of overload choices along with their types.
  llvm::DenseMap<ConstraintLocator *, SelectedOverload> overloadChoices;

  /// The set of constraint restrictions used to arrive at this restriction,
  /// which informs constraint application.
  llvm::DenseMap<std::pair<CanType, CanType>, ConversionRestrictionKind>
    ConstraintRestrictions;

  /// The list of fixes that need to be applied to the initial expression
  /// to make the solution work.
  llvm::SmallVector<ConstraintFix *, 4> Fixes;

  /// The set of disjunction choices used to arrive at this solution,
  /// which informs constraint application.
  llvm::DenseMap<ConstraintLocator *, unsigned> DisjunctionChoices;

  /// The set of opened types for a given locator.
  llvm::DenseMap<ConstraintLocator *, ArrayRef<OpenedType>> OpenedTypes;

  /// The opened existential type for a given locator.
  llvm::DenseMap<ConstraintLocator *, OpenedArchetypeType *>
    OpenedExistentialTypes;

  /// The locators of \c Defaultable constraints whose defaults were used.
  llvm::SmallPtrSet<ConstraintLocator *, 2> DefaultedConstraints;

  /// The node -> type mappings introduced by this solution.
  llvm::SmallVector<std::pair<TypedNode, Type>, 8> addedNodeTypes;

  std::vector<std::pair<ConstraintLocator *, ProtocolConformanceRef>>
      Conformances;

  /// The set of closures that have been transformed by a function builder.
  llvm::MapVector<ClosureExpr *, AppliedBuilderTransform>
      builderTransformedClosures;

  /// Simplify the given type by substituting all occurrences of
  /// type variables for their fixed types.
  Type simplifyType(Type type) const;

  /// Coerce the given expression to the given type.
  ///
  /// This operation cannot fail.
  ///
  /// \param expr The expression to coerce.
  /// \param toType The type to coerce the expression to.
  /// \param locator Locator used to describe the location of this expression.
  ///
  /// \param typeFromPattern Optionally, the caller can specify the pattern
  /// from where the toType is derived, so that we can deliver better fixit.
  ///
  /// \returns the coerced expression, which will have type \c ToType.
  Expr *coerceToType(Expr *expr, Type toType,
                     ConstraintLocator *locator,
                     Optional<Pattern*> typeFromPattern = None) const;

  /// Compute the set of substitutions for a generic signature opened at the
  /// given locator.
  ///
  /// \param sig The generic signature.
  ///
  /// \param locator The locator that describes where the substitutions came
  /// from.
  SubstitutionMap computeSubstitutions(GenericSignature sig,
                                       ConstraintLocator *locator) const;

  /// Resolves the contextual substitutions for a reference to a declaration
  /// at a given locator.
  ConcreteDeclRef
  resolveConcreteDeclRef(ValueDecl *decl, ConstraintLocator *locator) const;

  /// Return the disjunction choice for the given constraint location.
  unsigned getDisjunctionChoice(ConstraintLocator *locator) const {
    assert(DisjunctionChoices.count(locator));
    return DisjunctionChoices.find(locator)->second;
  }

  /// Retrieve the fixed score of this solution
  const Score &getFixedScore() const { return FixedScore; }

  /// Retrieve the fixed score of this solution
  Score &getFixedScore() { return FixedScore; }

  /// Retrieve the fixed type for the given type variable.
  Type getFixedType(TypeVariableType *typeVar) const;

  /// Try to resolve the given locator to a declaration within this
  /// solution. Note that this only returns a decl for a direct reference such
  /// as \c x.foo and will not return a decl for \c x.foo().
  ConcreteDeclRef resolveLocatorToDecl(ConstraintLocator *locator) const;

  /// Retrieve the overload choice associated with the given
  /// locator.
  SelectedOverload getOverloadChoice(ConstraintLocator *locator) const {
    return *getOverloadChoiceIfAvailable(locator);
  }

  /// Retrieve the overload choice associated with the given
  /// locator.
  Optional<SelectedOverload>
  getOverloadChoiceIfAvailable(ConstraintLocator *locator) const {
    auto known = overloadChoices.find(locator);
    if (known != overloadChoices.end())
      return known->second;
    return None;
  }

  void setExprTypes(Expr *expr) const;

  SWIFT_DEBUG_DUMP;

  /// Dump this solution.
  void dump(raw_ostream &OS) const LLVM_ATTRIBUTE_USED;
};

/// Describes the differences between several solutions to the same
/// constraint system.
class SolutionDiff {
public:
  /// A difference between two overloads.
  struct OverloadDiff {
    /// The locator that describes where the overload comes from.
    ConstraintLocator *locator;

    /// The choices that each solution made.
    SmallVector<OverloadChoice, 2> choices;
  };

  /// A difference between two type variable bindings.
  struct TypeBindingDiff {
    /// The type variable.
    TypeVariableType *typeVar;

    /// The bindings that each solution made.
    SmallVector<Type, 2> bindings;
  };

  /// The differences between the overload choices between the
  /// solutions.
  SmallVector<OverloadDiff, 4> overloads;

  /// The differences between the type variable bindings of the
  /// solutions.
  SmallVector<TypeBindingDiff, 4> typeBindings;

  /// Compute the differences between the given set of solutions.
  ///
  /// \param solutions The set of solutions.
  explicit SolutionDiff(ArrayRef<Solution> solutions);
};

/// Describes one resolved overload set within the list of overload sets
/// resolved by the solver.
struct ResolvedOverloadSetListItem {
  /// The previously resolved overload set in the list.
  ResolvedOverloadSetListItem *Previous;

  /// The type that this overload binds.
  Type BoundType;

  /// The overload choice.
  OverloadChoice Choice;

  /// The locator for this choice.
  ConstraintLocator *Locator;

  /// The type of the fully-opened base, if any.
  Type OpenedFullType;

  /// The type of the referenced choice.
  Type ImpliedType;

  // Make vanilla new/delete illegal for overload set items.
  void *operator new(size_t Bytes) = delete;
  void operator delete(void *Data) = delete;

  // Only allow allocation of list items using the allocator in the
  // constraint system.
  void *operator new(size_t bytes, ConstraintSystem &cs,
                     unsigned alignment
                       = alignof(ResolvedOverloadSetListItem));
};
  


/// Identifies a specific conversion from
struct SpecificConstraint {
  CanType First;
  CanType Second;
  ConstraintKind Kind;
};

/// An intrusive, doubly-linked list of constraints.
using ConstraintList = llvm::ilist<Constraint>;

enum class ConstraintSystemFlags {
  /// Whether we allow the solver to attempt fixes to the system.
  AllowFixes = 0x01,
  
  /// If set, this is going to prevent constraint system from erasing all
  /// discovered solutions except the best one.
  ReturnAllDiscoveredSolutions = 0x04,

  /// Set if the client wants diagnostics suppressed.
  SuppressDiagnostics = 0x08,

  /// If set, the client wants a best-effort solution to the constraint system,
  /// but can tolerate a solution where all of the constraints are solved, but
  /// not all type variables have been determined.  In this case, the constraint
  /// system is not applied to the expression AST, but the ConstraintSystem is
  /// left in-tact.
  AllowUnresolvedTypeVariables = 0x10,

  /// If set, constraint system always reuses type of pre-typechecked
  /// expression, and doesn't dig into its subexpressions.
  ReusePrecheckedType = 0x20,
  
  /// If set, the top-level expression may be able to provide an underlying
  /// type for the contextual opaque archetype.
  UnderlyingTypeForOpaqueReturnType = 0x40,

  /// FIXME(diagnostics): Once diagnostics are completely switched to new
  /// framework, this flag could be removed as obsolete.
  ///
  /// If set, this identifies constraint system as being used to re-typecheck
  /// one of the sub-expressions as part of the expression diagnostics, which
  /// is attempting to narrow down failure location.
  SubExpressionDiagnostics = 0x80,
};

/// Options that affect the constraint system as a whole.
using ConstraintSystemOptions = OptionSet<ConstraintSystemFlags>;

/// This struct represents the results of a member lookup of
struct MemberLookupResult {
  enum {
    /// This result indicates that we cannot begin to solve this, because the
    /// base expression is a type variable.
    Unsolved,
    
    /// This result indicates that the member reference is erroneous, but was
    /// already diagnosed.  Don't emit another error.
    ErrorAlreadyDiagnosed,
    
    /// This result indicates that the lookup produced candidate lists,
    /// potentially of viable results, potentially of error candidates, and
    /// potentially empty lists, indicating that there were no matches.
    HasResults
  } OverallResult;
  
  /// This is a list of viable candidates that were matched.
  ///
  SmallVector<OverloadChoice, 4> ViableCandidates;
  
  /// If there is a favored candidate in the viable list, this indicates its
  /// index.
  unsigned FavoredChoice = ~0U;
  
  
  /// This enum tracks reasons why a candidate is not viable.
  enum UnviableReason {
    /// This uses a type like Self in its signature that cannot be used on an
    /// existential box.
    UR_UnavailableInExistential,

    /// This is an instance member being accessed through something of metatype
    /// type.
    UR_InstanceMemberOnType,

    /// This is a static/class member being accessed through an instance.
    UR_TypeMemberOnInstance,

    /// This is a mutating member, being used on an rvalue.
    UR_MutatingMemberOnRValue,

    /// The getter for this subscript or computed property is mutating and we
    /// only have an rvalue base.  This is more specific than the former one.
    UR_MutatingGetterOnRValue,

    /// The member is inaccessible (e.g. a private member in another file).
    UR_Inaccessible,

    /// This is a `WritableKeyPath` being used to look up read-only member,
    /// used in situations involving dynamic member lookup via keypath,
    /// because it's not known upfront what access capability would the
    /// member have.
    UR_WritableKeyPathOnReadOnlyMember,

    /// This is a `ReferenceWritableKeyPath` being used to look up mutating
    /// member, used in situations involving dynamic member lookup via keypath,
    /// because it's not known upfront what access capability would the
    /// member have.
    UR_ReferenceWritableKeyPathOnMutatingMember,

    /// This is a KeyPath whose root type is AnyObject
    UR_KeyPathWithAnyObjectRootType
  };

  /// This is a list of considered (but rejected) candidates, along with a
  /// reason for their rejection. Split into separate collections to make
  /// it easier to use in conjunction with viable candidates.
  SmallVector<OverloadChoice, 4> UnviableCandidates;
  SmallVector<UnviableReason, 4> UnviableReasons;

  /// Mark this as being an already-diagnosed error and return itself.
  MemberLookupResult &markErrorAlreadyDiagnosed() {
    OverallResult = ErrorAlreadyDiagnosed;
    return *this;
  }
  
  void addViable(OverloadChoice candidate) {
    ViableCandidates.push_back(candidate);
  }
  
  void addUnviable(OverloadChoice candidate, UnviableReason reason) {
    UnviableCandidates.push_back(candidate);
    UnviableReasons.push_back(reason);
  }

  Optional<unsigned> getFavoredIndex() const {
    return (FavoredChoice == ~0U) ? Optional<unsigned>() : FavoredChoice;
  }
};

/// Stores the required methods for @dynamicCallable types.
struct DynamicCallableMethods {
  llvm::DenseSet<FuncDecl *> argumentsMethods;
  llvm::DenseSet<FuncDecl *> keywordArgumentsMethods;

  void addArgumentsMethod(FuncDecl *method) {
    argumentsMethods.insert(method);
  }

  void addKeywordArgumentsMethod(FuncDecl *method) {
    keywordArgumentsMethods.insert(method);
  }

  /// Returns true if type defines either of the @dynamicCallable
  /// required methods. Returns false iff type does not satisfy @dynamicCallable
  /// requirements.
  bool isValid() const {
    return !argumentsMethods.empty() || !keywordArgumentsMethods.empty();
  }
};

enum class ConstraintSystemPhase {
  ConstraintGeneration,
  Solving,
  Diagnostics,
  Finalization
};

/// Describes a system of constraints on type variables, the
/// solution of which assigns concrete types to each of the type variables.
/// Constraint systems are typically generated given an (untyped) expression.
class ConstraintSystem {
  ASTContext &Context;

public:
  DeclContext *DC;
  ConstraintSystemOptions Options;
  Optional<ExpressionTimer> Timer;

  friend class Solution;
  friend class ConstraintFix;
  friend class OverloadChoice;
  friend class ConstraintGraph;
  friend class DisjunctionChoice;
  friend class Component;
  friend class FailureDiagnostic;
  friend class TypeVarBindingProducer;
  friend class TypeVariableBinding;
  friend class StepScope;
  friend class SolverStep;
  friend class SplitterStep;
  friend class ComponentStep;
  friend class TypeVariableStep;
  friend class RequirementFailure;
  friend class MissingMemberFailure;

  class SolverScope;

  Constraint *failedConstraint = nullptr;

  /// Expressions that are known to be unevaluated.
  /// Note: this is only used to support ObjCSelectorExpr at the moment.
  llvm::SmallPtrSet<Expr *, 2> UnevaluatedRootExprs;

  /// The original CS if this CS was created as a simplification of another CS
  ConstraintSystem *baseCS = nullptr;

  /// The total number of disjunctions created.
  unsigned CountDisjunctions = 0;

private:
  /// Current phase of the constraint system lifetime.
  ConstraintSystemPhase Phase = ConstraintSystemPhase::ConstraintGeneration;

  /// The set of expressions for which we have generated constraints.
  llvm::SetVector<Expr *> InputExprs;

  /// The number of input expressions whose parents and depths have
  /// been entered into \c ExprWeights.
  unsigned NumInputExprsInWeights = 0;

  llvm::DenseMap<Expr *, std::pair<unsigned, Expr *>> ExprWeights;

  /// Allocator used for all of the related constraint systems.
  llvm::BumpPtrAllocator Allocator;

  /// Arena used for memory management of constraint-checker-related
  /// allocations.
  ConstraintCheckerArenaRAII Arena;

  /// Counter for type variables introduced.
  unsigned TypeCounter = 0;

  /// The number of scopes created so far during the solution
  /// of this constraint system.
  ///
  /// This is a measure of complexity of the solution space. A new
  /// scope is created every time we attempt a type variable binding
  /// or explore an option in a disjunction.
  unsigned CountScopes = 0;

  /// High-water mark of measured memory usage in any sub-scope we
  /// explored.
  size_t MaxMemory = 0;

  /// Cached member lookups.
  llvm::DenseMap<std::pair<Type, DeclName>, Optional<LookupResult>>
    MemberLookups;

  /// Cached sets of "alternative" literal types.
  static const unsigned NumAlternativeLiteralTypes = 13;
  Optional<ArrayRef<Type>> AlternativeLiteralTypes[NumAlternativeLiteralTypes];

  /// Folding set containing all of the locators used in this
  /// constraint system.
  llvm::FoldingSetVector<ConstraintLocator> ConstraintLocators;

  /// The overload sets that have been resolved along the current path.
  ResolvedOverloadSetListItem *resolvedOverloadSets = nullptr;

  /// The current fixed score for this constraint system and the (partial)
  /// solution it represents.
  Score CurrentScore;

  llvm::SetVector<TypeVariableType *> TypeVariables;

  /// Maps expressions to types for choosing a favored overload
  /// type in a disjunction constraint.
  llvm::DenseMap<Expr *, TypeBase *> FavoredTypes;

  /// Maps expression types used within all portions of the constraint
  /// system, instead of directly using the types on the expression
  /// nodes themselves. This allows us to typecheck an expression and
  /// run through various diagnostics passes without actually mutating
  /// the types on the expression nodes.
  llvm::DenseMap<const Expr *, TypeBase *> ExprTypes;
  llvm::DenseMap<const TypeLoc *, TypeBase *> TypeLocTypes;
  llvm::DenseMap<const VarDecl *, TypeBase *> VarTypes;
  llvm::DenseMap<std::pair<const KeyPathExpr *, unsigned>, TypeBase *>
      KeyPathComponentTypes;

  /// Maps closure parameters to type variables.
  llvm::DenseMap<const ParamDecl *, TypeVariableType *>
    OpenedParameterTypes;

  /// There can only be a single contextual type on the root of the expression
  /// being checked.  If specified, this holds its type along with the base
  /// expression, and the purpose of it.
  TypeLoc contextualType;
  Expr *contextualTypeNode = nullptr;
  ContextualTypePurpose contextualTypePurpose = CTP_Unused;
  
  /// The set of constraint restrictions used to reach the
  /// current constraint system.
  ///
  /// Constraint restrictions help describe which path the solver took when
  /// there are multiple ways in which one type could convert to another, e.g.,
  /// given class types A and B, the solver might choose either a superclass
  /// conversion or a user-defined conversion.
  std::vector<std::tuple<Type, Type, ConversionRestrictionKind>>
      ConstraintRestrictions;

  /// The set of fixes applied to make the solution work.
  llvm::SmallVector<ConstraintFix *, 4> Fixes;

  /// The set of remembered disjunction choices used to reach
  /// the current constraint system.
  std::vector<std::pair<ConstraintLocator*, unsigned>>
      DisjunctionChoices;

  /// The worklist of "active" constraints that should be revisited
  /// due to a change.
  ConstraintList ActiveConstraints;

  /// The list of "inactive" constraints that still need to be solved,
  /// but will not be revisited until one of their inputs changes.
  ConstraintList InactiveConstraints;

  /// The constraint graph.
  ConstraintGraph &CG;

  /// A mapping from constraint locators to the set of opened types associated
  /// with that locator.
  SmallVector<std::pair<ConstraintLocator *, ArrayRef<OpenedType>>, 4>
    OpenedTypes;

  /// The list of all generic requirements fixed along the current
  /// solver path.
  using FixedRequirement = std::tuple<TypeBase *, RequirementKind, TypeBase *>;
  SmallVector<FixedRequirement, 4> FixedRequirements;

  bool hasFixedRequirement(Type lhs, RequirementKind kind, Type rhs) {
    auto reqInfo = std::make_tuple(lhs.getPointer(), kind, rhs.getPointer());
    return llvm::any_of(
        FixedRequirements,
        [&reqInfo](const FixedRequirement &entry) { return entry == reqInfo; });
  }

  void recordFixedRequirement(Type lhs, RequirementKind kind, Type rhs) {
    FixedRequirements.push_back(
        std::make_tuple(lhs.getPointer(), kind, rhs.getPointer()));
  }

  /// A mapping from constraint locators to the opened existential archetype
  /// used for the 'self' of an existential type.
  SmallVector<std::pair<ConstraintLocator *, OpenedArchetypeType *>, 4>
    OpenedExistentialTypes;

  /// The node -> type mappings introduced by generating constraints.
  llvm::SmallVector<std::pair<TypedNode, Type>, 8> addedNodeTypes;

  std::vector<std::pair<ConstraintLocator *, ProtocolConformanceRef>>
      CheckedConformances;

  /// The set of closures that have been transformed by a function builder.
  std::vector<std::pair<ClosureExpr *, AppliedBuilderTransform>>
      builderTransformedClosures;

public:
  /// The locators of \c Defaultable constraints whose defaults were used.
  std::vector<ConstraintLocator *> DefaultedConstraints;

  /// A cache that stores the @dynamicCallable required methods implemented by
  /// types.
  llvm::DenseMap<CanType, DynamicCallableMethods> DynamicCallableCache;

  /// A cache that stores whether types are valid @dynamicMemberLookup types.
  llvm::DenseMap<CanType, bool> DynamicMemberLookupCache;

private:
  /// Describe the candidate expression for partial solving.
  /// This class used by shrink & solve methods which apply
  /// variation of directional path consistency algorithm in attempt
  /// to reduce scopes of the overload sets (disjunctions) in the system.
  class Candidate {
    Expr *E;
    DeclContext *DC;
    llvm::BumpPtrAllocator &Allocator;

    ConstraintSystem &BaseCS;

    // Contextual Information.
    Type CT;
    ContextualTypePurpose CTP;

  public:
    Candidate(ConstraintSystem &cs, Expr *expr, Type ct = Type(),
              ContextualTypePurpose ctp = ContextualTypePurpose::CTP_Unused)
        : E(expr), DC(cs.DC), Allocator(cs.Allocator), BaseCS(cs),
          CT(ct), CTP(ctp) {}

    /// Return underlying expression.
    Expr *getExpr() const { return E; }

    /// Try to solve this candidate sub-expression
    /// and re-write it's OSR domains afterwards.
    ///
    /// \param shrunkExprs The set of expressions which
    /// domains have been successfully shrunk so far.
    ///
    /// \returns true on solver failure, false otherwise.
    bool solve(llvm::SmallDenseSet<OverloadSetRefExpr *> &shrunkExprs);

    /// Apply solutions found by solver as reduced OSR sets for
    /// for current and all of it's sub-expressions.
    ///
    /// \param solutions The solutions found by running solver on the
    /// this candidate expression.
    ///
    /// \param shrunkExprs The set of expressions which
    /// domains have been successfully shrunk so far.
    void applySolutions(
        llvm::SmallVectorImpl<Solution> &solutions,
        llvm::SmallDenseSet<OverloadSetRefExpr *> &shrunkExprs) const;

    /// Check if attempt at solving of the candidate makes sense given
    /// the current conditions - number of shrunk domains which is related
    /// to the given candidate over the total number of disjunctions present.
    static bool
    isTooComplexGiven(ConstraintSystem *const cs,
                      llvm::SmallDenseSet<OverloadSetRefExpr *> &shrunkExprs) {
      SmallVector<Constraint *, 8> disjunctions;
      cs->collectDisjunctions(disjunctions);

      unsigned unsolvedDisjunctions = disjunctions.size();
      for (auto *disjunction : disjunctions) {
        auto *locator = disjunction->getLocator();
        if (!locator)
          continue;

        if (auto *anchor = locator->getAnchor()) {
          auto *OSR = dyn_cast<OverloadSetRefExpr>(anchor);
          if (!OSR)
            continue;

          if (shrunkExprs.count(OSR) > 0)
            --unsolvedDisjunctions;
        }
      }

      unsigned threshold =
          cs->getASTContext().TypeCheckerOpts.SolverShrinkUnsolvedThreshold;
      return unsolvedDisjunctions >= threshold;
    }
  };

  /// Describes the current solver state.
  struct SolverState {
    SolverState(ConstraintSystem &cs,
                FreeTypeVariableBinding allowFreeTypeVariables);
    ~SolverState();

    /// The constraint system.
    ConstraintSystem &CS;

    FreeTypeVariableBinding AllowFreeTypeVariables;

    /// Old value of DebugConstraintSolver.
    /// FIXME: Move the "debug constraint solver" bit into the constraint 
    /// system itself.
    bool OldDebugConstraintSolver;

    /// Depth of the solution stack.
    unsigned depth = 0;

    /// Maximum depth reached so far in exploring solutions.
    unsigned maxDepth = 0;

    /// Whether to record failures or not.
    bool recordFixes = false;

    /// The set of type variable bindings that have changed while
    /// processing this constraint system.
    SavedTypeVariableBindings savedBindings;

     /// The best solution computed so far.
    Optional<Score> BestScore;

    /// The number of the solution attempt we're looking at.
    unsigned SolutionAttempt;

    /// Refers to the innermost partial solution scope.
    SolverScope *PartialSolutionScope = nullptr;

    // Statistics
    #define CS_STATISTIC(Name, Description) unsigned Name = 0;
    #include "ConstraintSolverStats.def"

    /// Check whether there are any retired constraints present.
    bool hasRetiredConstraints() const {
      return !retiredConstraints.empty();
    }

    /// Mark given constraint as retired along current solver path.
    ///
    /// \param constraint The constraint to retire temporarily.
    void retireConstraint(Constraint *constraint) {
      retiredConstraints.push_front(constraint);
    }

    /// Iterate over all of the retired constraints registered with
    /// current solver state.
    ///
    /// \param processor The processor function to be applied to each of
    /// the constraints retrieved.
    void forEachRetired(llvm::function_ref<void(Constraint &)> processor) {
      for (auto &constraint : retiredConstraints)
        processor(constraint);
    }

    /// Add new "generated" constraint along the current solver path.
    ///
    /// \param constraint The newly generated constraint.
    void addGeneratedConstraint(Constraint *constraint) {
      assert(constraint && "Null generated constraint?");
      generatedConstraints.push_back(constraint);
    }

    /// Erase given constraint from the list of generated constraints
    /// along the current solver path. Note that this operation doesn't
    /// guarantee any ordering of the after it's application.
    ///
    /// \param constraint The constraint to erase.
    void removeGeneratedConstraint(Constraint *constraint) {
      for (auto *&generated : generatedConstraints) {
        // When we find the constraint we're erasing, overwrite its
        // value with the last element in the generated constraints
        // vector and then pop that element from the vector.
        if (generated == constraint) {
          generated = generatedConstraints.back();
          generatedConstraints.pop_back();
          return;
        }
      }
    }

    /// Register given scope to be tracked by the current solver state,
    /// this helps to make sure that all of the retired/generated constraints
    /// are dealt with correctly when the life time of the scope ends.
    ///
    /// \param scope The scope to associate with current solver state.
    void registerScope(SolverScope *scope) {
      ++depth;
      maxDepth = std::max(maxDepth, depth);
      scope->scopeNumber = NumStatesExplored++;

      CS.incrementScopeCounter();
      auto scopeInfo =
        std::make_tuple(scope, retiredConstraints.begin(),
                        generatedConstraints.size());
      scopes.push_back(scopeInfo);
    }

    /// Restore all of the retired/generated constraints to the state
    /// before given scope. This is required because retired constraints have
    /// to be re-introduced to the system in order of arrival (LIFO) and list
    /// of the generated constraints has to be truncated back to the
    /// original size.
    ///
    /// \param scope The solver scope to rollback.
    void rollback(SolverScope *scope) {
      --depth;

      unsigned countScopesExplored = NumStatesExplored - scope->scopeNumber;
      if (countScopesExplored == 1)
        CS.incrementLeafScopes();

      SolverScope *savedScope;
      // The position of last retired constraint before given scope.
      ConstraintList::iterator lastRetiredPos;
      // The original number of generated constraints before given scope.
      unsigned numGenerated;

      std::tie(savedScope, lastRetiredPos, numGenerated) =
        scopes.pop_back_val();

      assert(savedScope == scope && "Scope rollback not in LIFO order!");

      // Restore all of the retired constraints.
      CS.InactiveConstraints.splice(CS.InactiveConstraints.end(),
                                    retiredConstraints,
                                    retiredConstraints.begin(), lastRetiredPos);

      // And remove all of the generated constraints.
      auto genStart = generatedConstraints.begin() + numGenerated,
           genEnd = generatedConstraints.end();
      for (auto genI = genStart; genI != genEnd; ++genI) {
        CS.InactiveConstraints.erase(ConstraintList::iterator(*genI));
      }

      generatedConstraints.erase(genStart, genEnd);

      for (unsigned constraintIdx :
             range(scope->numDisabledConstraints, disabledConstraints.size())) {
        if (disabledConstraints[constraintIdx]->isDisabled())
          disabledConstraints[constraintIdx]->setEnabled();
      }
      disabledConstraints.erase(
          disabledConstraints.begin() + scope->numDisabledConstraints,
          disabledConstraints.end());

      for (unsigned constraintIdx :
             range(scope->numFavoredConstraints, favoredConstraints.size())) {
        if (favoredConstraints[constraintIdx]->isFavored())
          favoredConstraints[constraintIdx]->setFavored(false);
      }
      favoredConstraints.erase(
          favoredConstraints.begin() + scope->numFavoredConstraints,
          favoredConstraints.end());
    }

    /// Check whether constraint system is allowed to form solutions
    /// even with unbound type variables present.
    bool allowsFreeTypeVariables() const {
      return AllowFreeTypeVariables != FreeTypeVariableBinding::Disallow;
    }

    unsigned getNumDisabledConstraints() const {
      return disabledConstraints.size();
    }

    /// Disable the given constraint; this change will be rolled back
    /// when we exit the current solver scope.
    void disableContraint(Constraint *constraint) {
      constraint->setDisabled();
      disabledConstraints.push_back(constraint);
    }

    unsigned getNumFavoredConstraints() const {
      return favoredConstraints.size();
    }

    /// Favor the given constraint; this change will be rolled back
    /// when we exit the current solver scope.
    void favorConstraint(Constraint *constraint) {
      assert(!constraint->isFavored());

      constraint->setFavored();
      favoredConstraints.push_back(constraint);
    }

  private:
    /// The list of constraints that have been retired along the
    /// current path, this list is used in LIFO fashion when constraints
    /// are added back to the circulation.
    ConstraintList retiredConstraints;

    /// The set of constraints which were active at the time of this state
    /// creating, it's used to re-activate them on destruction.
    SmallVector<Constraint *, 4> activeConstraints;

    /// The current set of generated constraints.
    SmallVector<Constraint *, 4> generatedConstraints;

    /// The collection which holds association between solver scope
    /// and position of the last retired constraint and number of
    /// constraints generated before registration of given scope,
    /// this helps to rollback all of the constraints retired/generated
    /// each of the registered scopes correct (LIFO) order.
    llvm::SmallVector<
      std::tuple<SolverScope *, ConstraintList::iterator, unsigned>, 4> scopes;

    SmallVector<Constraint *, 4> disabledConstraints;
    SmallVector<Constraint *, 4> favoredConstraints;
  };

  class CacheExprTypes : public ASTWalker {
    Expr *RootExpr;
    ConstraintSystem &CS;
    bool ExcludeRoot;

  public:
    CacheExprTypes(Expr *expr, ConstraintSystem &cs, bool excludeRoot)
        : RootExpr(expr), CS(cs), ExcludeRoot(excludeRoot) {}

    Expr *walkToExprPost(Expr *expr) override {
      if (ExcludeRoot && expr == RootExpr) {
        assert(!expr->getType() && "Unexpected type in root of expression!");
        return expr;
      }

      if (expr->getType())
        CS.cacheType(expr);

      if (auto kp = dyn_cast<KeyPathExpr>(expr))
        for (auto i : indices(kp->getComponents()))
          if (kp->getComponents()[i].getComponentType())
            CS.cacheType(kp, i);

      return expr;
    }

    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
  };

public:
  ConstraintSystemPhase getPhase() const { return Phase; }

  /// Move constraint system to a new phase of its lifetime.
  void setPhase(ConstraintSystemPhase newPhase) {
    if (Phase == newPhase)
      return;

#ifndef NDEBUG
    switch (Phase) {
    case ConstraintSystemPhase::ConstraintGeneration:
      assert(newPhase == ConstraintSystemPhase::Solving);
      break;

    case ConstraintSystemPhase::Solving:
      // We can come back to constraint generation phase while
      // processing function builder body.
      assert(newPhase == ConstraintSystemPhase::ConstraintGeneration ||
             newPhase == ConstraintSystemPhase::Diagnostics ||
             newPhase == ConstraintSystemPhase::Finalization);
      break;

    case ConstraintSystemPhase::Diagnostics:
      assert(newPhase == ConstraintSystemPhase::Solving ||
             newPhase == ConstraintSystemPhase::Finalization);
      break;

    case ConstraintSystemPhase::Finalization:
      assert(newPhase == ConstraintSystemPhase::Diagnostics);
      break;
    }
#endif

    Phase = newPhase;
  }

  /// Cache the types of the given expression and all subexpressions.
  void cacheExprTypes(Expr *expr) {
    bool excludeRoot = false;
    expr->walk(CacheExprTypes(expr, *this, excludeRoot));
  }

  /// Cache the types of the expressions under the given expression
  /// (but not the type of the given expression).
  void cacheSubExprTypes(Expr *expr) {
    bool excludeRoot = true;
    expr->walk(CacheExprTypes(expr, *this, excludeRoot));
  }

  /// The current solver state.
  ///
  /// This will be non-null when we're actively solving the constraint
  /// system, and carries temporary state related to the current path
  /// we're exploring.
  SolverState *solverState = nullptr;

  struct ArgumentInfo {
    ArrayRef<Identifier> Labels;
    bool HasTrailingClosure;
  };

  /// A mapping from the constraint locators for references to various
  /// names (e.g., member references, normal name references, possible
  /// constructions) to the argument labels provided in the call to
  /// that locator.
  llvm::DenseMap<ConstraintLocator *, ArgumentInfo> ArgumentInfos;

  /// Form a locator that can be used to retrieve argument information cached in
  /// the constraint system for the callee described by the anchor of the
  /// passed locator.
  ConstraintLocator *getArgumentInfoLocator(ConstraintLocator *locator);

  /// Retrieve the argument info that is associated with a member
  /// reference at the given locator.
  Optional<ArgumentInfo> getArgumentInfo(ConstraintLocator *locator);

  ResolvedOverloadSetListItem *getResolvedOverloadSets() const {
    return resolvedOverloadSets;
  }

  ResolvedOverloadSetListItem *
  findSelectedOverloadFor(ConstraintLocator *locator) const {
    auto resolvedOverload = getResolvedOverloadSets();
    while (resolvedOverload) {
      if (resolvedOverload->Locator == locator)
        return resolvedOverload;
      resolvedOverload = resolvedOverload->Previous;
    }
    return nullptr;
  }

  ResolvedOverloadSetListItem *findSelectedOverloadFor(Expr *expr) const {
    auto resolvedOverload = getResolvedOverloadSets();
    while (resolvedOverload) {
      if (resolvedOverload->Locator->getAnchor() == expr)
        return resolvedOverload;
      resolvedOverload = resolvedOverload->Previous;
    }
    return nullptr;
  }

private:
  unsigned assignTypeVariableID() {
    return TypeCounter++;
  }

  void incrementScopeCounter();
  void incrementLeafScopes();

public:
  /// Introduces a new solver scope, which any changes to the
  /// solver state or constraint system are temporary and will be undone when
  /// this object is destroyed.
  ///
  ///
  class SolverScope {
    ConstraintSystem &cs;

    /// The current resolved overload set list.
    ResolvedOverloadSetListItem *resolvedOverloadSets;

    /// The length of \c TypeVariables.
    unsigned numTypeVariables;

    /// The length of \c SavedBindings.
    unsigned numSavedBindings;

    /// The length of \c ConstraintRestrictions.
    unsigned numConstraintRestrictions;

    /// The length of \c Fixes.
    unsigned numFixes;

    /// The length of \c FixedRequirements.
    unsigned numFixedRequirements;

    /// The length of \c DisjunctionChoices.
    unsigned numDisjunctionChoices;

    /// The length of \c OpenedTypes.
    unsigned numOpenedTypes;

    /// The length of \c OpenedExistentialTypes.
    unsigned numOpenedExistentialTypes;

    /// The length of \c DefaultedConstraints.
    unsigned numDefaultedConstraints;

    unsigned numAddedNodeTypes;

    unsigned numCheckedConformances;

    unsigned numDisabledConstraints;

    unsigned numFavoredConstraints;

    unsigned numBuilderTransformedClosures;

    /// The previous score.
    Score PreviousScore;

    /// The scope number of this scope. Set when the scope is registered.
    unsigned scopeNumber = 0;

    /// Constraint graph scope associated with this solver scope.
    ConstraintGraphScope CGScope;

    SolverScope(const SolverScope &) = delete;
    SolverScope &operator=(const SolverScope &) = delete;

    friend class ConstraintSystem;

  public:
    explicit SolverScope(ConstraintSystem &cs);
    ~SolverScope();
  };

  ConstraintSystem(DeclContext *dc,
                   ConstraintSystemOptions options);
  ~ConstraintSystem();

  /// Retrieve the constraint graph associated with this constraint system.
  ConstraintGraph &getConstraintGraph() const { return CG; }

  /// Retrieve the AST context.
  ASTContext &getASTContext() const { return Context; }

  /// Determine whether this constraint system has any free type
  /// variables.
  bool hasFreeTypeVariables();

private:
  /// Indicates if the constraint system should retain all of the
  /// solutions it has deduced regardless of their score.
  bool retainAllSolutions() const {
    return Options.contains(
        ConstraintSystemFlags::ReturnAllDiscoveredSolutions);
  }

  /// Finalize this constraint system; we're done attempting to solve
  /// it.
  ///
  /// \returns the solution.
  Solution finalize();

  /// Apply the given solution to the current constraint system.
  ///
  /// This operation is used to take a solution computed based on some
  /// subset of the constraints and then apply it back to the
  /// constraint system for further exploration.
  void applySolution(const Solution &solution);

  // FIXME: Allows the type checker to apply solutions.
  friend class swift::TypeChecker;

  /// Emit the fixes computed as part of the solution, returning true if we were
  /// able to emit an error message, or false if none of the fixits worked out.
  bool applySolutionFixes(const Solution &solution);

  /// If there is more than one viable solution,
  /// attempt to pick the best solution and remove all of the rest.
  ///
  /// \param solutions The set of solutions to filter.
  ///
  /// \param minimize The flag which idicates if the
  /// set of solutions should be filtered even if there is
  /// no single best solution, see `findBestSolution` for
  /// more details.
  void
  filterSolutions(SmallVectorImpl<Solution> &solutions,
                  bool minimize = false) {
    if (solutions.size() < 2)
      return;

    if (auto best = findBestSolution(solutions, minimize)) {
      if (*best != 0)
        solutions[0] = std::move(solutions[*best]);
      solutions.erase(solutions.begin() + 1, solutions.end());
    }
  }

  /// Restore the type variable bindings to what they were before
  /// we attempted to solve this constraint system.
  ///
  /// \param numBindings The number of bindings to restore, from the end of
  /// the saved-binding stack.
  void restoreTypeVariableBindings(unsigned numBindings);

  /// Retrieve the set of saved type variable bindings, if available.
  ///
  /// \returns null when we aren't currently solving the system.
  SavedTypeVariableBindings *getSavedBindings() const {
    return solverState ? &solverState->savedBindings : nullptr;
  }

  /// Add a new type variable that was already created.
  void addTypeVariable(TypeVariableType *typeVar);
  
  /// Add a constraint from the subscript base to the root of the key
  /// path literal to the constraint system.
  void addKeyPathApplicationRootConstraint(Type root, ConstraintLocatorBuilder locator);

public:
  /// Lookup for a member with the given name in the given base type.
  ///
  /// This routine caches the results of member lookups in the top constraint
  /// system, to avoid.
  ///
  /// FIXME: This caching should almost certainly be performed at the
  /// module level, since type checking occurs after name binding,
  /// and no new names are introduced after name binding.
  ///
  /// \returns A reference to the member-lookup result.
  LookupResult &lookupMember(Type base, DeclName name);

  /// Retrieve the set of "alternative" literal types that we'll explore
  /// for a given literal protocol kind.
  ArrayRef<Type> getAlternativeLiteralTypes(KnownProtocolKind kind);

  /// Create a new type variable.
  TypeVariableType *createTypeVariable(ConstraintLocator *locator,
                                       unsigned options);

  /// Retrieve the set of active type variables.
  ArrayRef<TypeVariableType *> getTypeVariables() const {
    return TypeVariables.getArrayRef();
  }

  /// Whether the given type variable is active in the constraint system at
  /// the moment.
  bool isActiveTypeVariable(TypeVariableType *typeVar) const {
    return TypeVariables.count(typeVar) > 0;
  }

  TypeBase* getFavoredType(Expr *E) {
    assert(E != nullptr);
    return this->FavoredTypes[E];
  }
  void setFavoredType(Expr *E, TypeBase *T) {
    assert(E != nullptr);
    this->FavoredTypes[E] = T;
  }

  /// Set the type in our type map for the given node.
  ///
  /// The side tables are used through the expression type checker to avoid mutating nodes until
  /// we know we have successfully type-checked them.
  void setType(TypedNode node, Type type) {
    assert(!node.isNull() && "Cannot set type information on null node");
    assert(type && "Expected non-null type");

    // Record the type.
    if (auto expr = node.dyn_cast<const Expr *>()) {
      ExprTypes[expr] = type.getPointer();
    } else if (auto typeLoc = node.dyn_cast<const TypeLoc *>()) {
      TypeLocTypes[typeLoc] = type.getPointer();
    } else {
      auto var = node.get<const VarDecl *>();
      VarTypes[var] = type.getPointer();
    }

    // Record the fact that we ascribed a type to this node.
    if (solverState && solverState->depth > 0) {
      addedNodeTypes.push_back({node, type});
    }
  }

  /// Set the type in our type map for a given expression. The side
  /// map is used throughout the expression type checker in order to
  /// avoid mutating expressions until we know we have successfully
  /// type-checked them.
  void setType(TypeLoc &L, Type T) {
    setType(TypedNode(&L), T);
  }

  /// Erase the type for the given node.
  void eraseType(TypedNode node) {
    if (auto expr = node.dyn_cast<const Expr *>()) {
      ExprTypes.erase(expr);
    } else if (auto typeLoc = node.dyn_cast<const TypeLoc *>()) {
      TypeLocTypes.erase(typeLoc);
    } else {
      auto var = node.get<const VarDecl *>();
      VarTypes.erase(var);
    }
  }

  void setType(KeyPathExpr *KP, unsigned I, Type T) {
    assert(KP && "Expected non-null key path parameter!");
    assert(T && "Expected non-null type!");
    KeyPathComponentTypes[std::make_pair(KP, I)] = T.getPointer();
  }

  /// Check to see if we have a type for an expression.
  bool hasType(const Expr *E) const {
    assert(E != nullptr && "Expected non-null expression!");
    return ExprTypes.find(E) != ExprTypes.end();
  }

  bool hasType(const TypeLoc &L) const {
    return hasType(TypedNode(&L));
  }

  /// Check to see if we have a type for a node.
  bool hasType(TypedNode node) const {
    assert(!node.isNull() && "Expected non-null node");
    if (auto expr = node.dyn_cast<const Expr *>()) {
      return ExprTypes.find(expr) != ExprTypes.end();
    } else if (auto typeLoc = node.dyn_cast<const TypeLoc *>()) {
      return TypeLocTypes.find(typeLoc) != TypeLocTypes.end();
    } else {
      auto var = node.get<const VarDecl *>();
      return VarTypes.find(var) != VarTypes.end();
    }
  }

  bool hasType(const KeyPathExpr *KP, unsigned I) const {
    assert(KP && "Expected non-null key path parameter!");
    return KeyPathComponentTypes.find(std::make_pair(KP, I))
              != KeyPathComponentTypes.end();
  }

  /// Get the type for an expression.
  Type getType(const Expr *E) const {
    assert(hasType(E) && "Expected type to have been set!");
    // FIXME: lvalue differences
    //    assert((!E->getType() ||
    //            E->getType()->isEqual(ExprTypes.find(E)->second)) &&
    //           "Mismatched types!");
    return ExprTypes.find(E)->second;
  }

  Type getType(const TypeLoc &L) const {
    assert(hasType(L) && "Expected type to have been set!");
    return TypeLocTypes.find(&L)->second;
  }

  Type getType(const VarDecl *VD) const {
    assert(hasType(VD) && "Expected type to have been set!");
    return VarTypes.find(VD)->second;
  }

  Type getType(const KeyPathExpr *KP, unsigned I) const {
    assert(hasType(KP, I) && "Expected type to have been set!");
    return KeyPathComponentTypes.find(std::make_pair(KP, I))->second;
  }

  /// Cache the type of the expression argument and return that same
  /// argument.
  template <typename T>
  T *cacheType(T *E) {
    assert(E->getType() && "Expected a type!");
    setType(E, E->getType());
    return E;
  }

  /// Cache the type of the expression argument and return that same
  /// argument.
  KeyPathExpr *cacheType(KeyPathExpr *E, unsigned I) {
    auto componentTy = E->getComponents()[I].getComponentType();
    assert(componentTy && "Expected a type!");
    setType(E, I, componentTy);
    return E;
  }

  void setContextualType(Expr *E, TypeLoc T, ContextualTypePurpose purpose) {
    assert(E != nullptr && "Expected non-null expression!");
    contextualTypeNode = E;
    contextualType = T;
    contextualTypePurpose = purpose;
  }

  Type getContextualType(Expr *E) const {
    assert(E != nullptr && "Expected non-null expression!");
    return E == contextualTypeNode ? contextualType.getType() : Type();
  }
  Type getContextualType() const {
    return contextualType.getType();
  }

  TypeLoc getContextualTypeLoc() const {
    return contextualType;
  }

  const Expr *getContextualTypeNode() const {
    return contextualTypeNode;
  }

  ContextualTypePurpose getContextualTypePurpose() const {
    return contextualTypePurpose;
  }
  
  /// Retrieve the constraint locator for the given anchor and
  /// path, uniqued.
  ConstraintLocator *
  getConstraintLocator(Expr *anchor,
                       ArrayRef<ConstraintLocator::PathElement> path,
                       unsigned summaryFlags);

  /// Retrive the constraint locator for the given anchor and
  /// path, uniqued and automatically infer the summary flags
  ConstraintLocator *
  getConstraintLocator(Expr *anchor,
                       ArrayRef<ConstraintLocator::PathElement> path);

  /// Retrieve the constraint locator for the given anchor and
  /// an empty path, uniqued.
  ConstraintLocator *getConstraintLocator(Expr *anchor) {
    return getConstraintLocator(anchor, {}, 0);
  }

  /// Retrieve the constraint locator for the given anchor and
  /// path element.
  ConstraintLocator *
  getConstraintLocator(Expr *anchor, ConstraintLocator::PathElement pathElt) {
    return getConstraintLocator(anchor, llvm::makeArrayRef(pathElt),
                                pathElt.getNewSummaryFlags());
  }

  ConstraintLocator *
  getConstraintLocator(const Expr *anchor,
                       ConstraintLocator::PathElement pathElt) {
    return getConstraintLocator(const_cast<Expr *>(anchor), pathElt);
  }

  /// Extend the given constraint locator with a path element.
  ConstraintLocator *
  getConstraintLocator(ConstraintLocator *locator,
                       ConstraintLocator::PathElement pathElt) {
    return getConstraintLocator(ConstraintLocatorBuilder(locator)
                                  .withPathElement(pathElt));
  }

  /// Retrieve the constraint locator described by the given
  /// builder.
  ConstraintLocator *
  getConstraintLocator(const ConstraintLocatorBuilder &builder);

  /// Lookup and return parent associated with given expression.
  Expr *getParentExpr(Expr *expr) {
    if (auto result = getExprDepthAndParent(expr))
      return result->second;
    return nullptr;
  }

  /// Retrieve the depth of the given expression.
  Optional<unsigned> getExprDepth(Expr *expr) {
    if (auto result = getExprDepthAndParent(expr))
      return result->first;
    return None;
  }

  /// Retrieve the depth and parent expression of the given expression.
  Optional<std::pair<unsigned, Expr *>> getExprDepthAndParent(Expr *expr);

  /// Returns a locator describing the callee for the anchor of a given locator.
  ///
  /// - For an unresolved dot/member anchor, this will be a locator describing
  /// the member.
  ///
  /// - For a subscript anchor, this will be a locator describing the subscript
  /// member.
  ///
  /// - For a key path anchor with a property/subscript component path element,
  /// this will be a locator describing the decl referenced by the component.
  ///
  /// - For a function application anchor, this will be a locator describing the
  /// 'direct callee' of the call. For example, for the expression \c x.foo?()
  /// the returned locator will describe the member \c foo.
  ///
  /// Note that because this function deals with the anchor, given a locator
  /// anchored on \c functionA(functionB()) with path elements pointing to the
  /// argument \c functionB(), the returned callee locator will describe
  /// \c functionA rather than \c functionB.
  ///
  /// \param locator The input locator.
  /// \param lookThroughApply Whether to look through applies. If false, a
  /// callee locator will only be returned for a direct reference such as
  /// \c x.foo rather than \c x.foo().
  ConstraintLocator *getCalleeLocator(ConstraintLocator *locator,
                                      bool lookThroughApply = true);

public:

  /// Whether we should attempt to fix problems.
  bool shouldAttemptFixes() const {
    if (!(Options & ConstraintSystemFlags::AllowFixes))
      return false;

    return !solverState || solverState->recordFixes;
  }

  ArrayRef<ConstraintFix *> getFixes() const { return Fixes; }

  bool shouldSuppressDiagnostics() const {
    return Options.contains(ConstraintSystemFlags::SuppressDiagnostics);
  }

  bool shouldReusePrecheckedType() const {
    return Options.contains(ConstraintSystemFlags::ReusePrecheckedType);
  }

  /// Log and record the application of the fix. Return true iff any
  /// subsequent solution would be worse than the best known solution.
  bool recordFix(ConstraintFix *fix, unsigned impact = 1);

  void recordPotentialHole(TypeVariableType *typeVar);

  /// Determine whether constraint system already has a fix recorded
  /// for a particular location.
  bool hasFixFor(ConstraintLocator *locator,
                 Optional<FixKind> expectedKind = None) const {
    return llvm::any_of(
        Fixes, [&locator, &expectedKind](const ConstraintFix *fix) {
          if (fix->getLocator() == locator) {
            return !expectedKind || fix->getKind() == *expectedKind;
          }
          return false;
        });
  }

  /// If an UnresolvedDotExpr, SubscriptMember, etc has been resolved by the
  /// constraint system, return the decl that it references.
  ValueDecl *findResolvedMemberRef(ConstraintLocator *locator);

  /// Try to salvage the constraint system by applying (speculative)
  /// fixes.
  SolutionResult salvage();
  
  /// Mine the active and inactive constraints in the constraint
  /// system to generate a plausible diagnosis of why the system could not be
  /// solved.
  ///
  /// \param expr The expression whose constraints we're investigating for a
  /// better diagnostic.
  ///
  /// Assuming that this constraint system is actually erroneous, this *always*
  /// emits an error message.
  void diagnoseFailureForExpr(Expr *expr);

  bool diagnoseAmbiguity(ArrayRef<Solution> solutions);
  bool diagnoseAmbiguityWithFixes(ArrayRef<Solution> solutions);

  /// Give the deprecation warning for referring to a global function
  /// when there's a method from a conditional conformance in a smaller/closer
  /// scope.
  void
  diagnoseDeprecatedConditionalConformanceOuterAccess(UnresolvedDotExpr *UDE,
                                                      ValueDecl *choice);

  /// Add a constraint to the constraint system.
  void addConstraint(ConstraintKind kind, Type first, Type second,
                     ConstraintLocatorBuilder locator,
                     bool isFavored = false);

  /// Add a requirement as a constraint to the constraint system.
  void addConstraint(Requirement req, ConstraintLocatorBuilder locator,
                     bool isFavored = false);

  /// Add a "join" constraint between a set of types, producing the common
  /// supertype.
  ///
  /// Currently, a "join" is modeled by a set of conversion constraints to
  /// a new type variable. At some point, we may want a new constraint kind
  /// to cover the join.
  ///
  /// \returns the joined type, which is generally a new type variable.
  Type addJoinConstraint(ConstraintLocator *locator,
                         ArrayRef<std::pair<Type, ConstraintLocator *>> inputs);

  /// Add a constraint to the constraint system with an associated fix.
  void addFixConstraint(ConstraintFix *fix, ConstraintKind kind,
                        Type first, Type second,
                        ConstraintLocatorBuilder locator,
                        bool isFavored = false);

  /// Add a key path application constraint to the constraint system.
  void addKeyPathApplicationConstraint(Type keypath, Type root, Type value,
                                       ConstraintLocatorBuilder locator,
                                       bool isFavored = false);

  /// Add a key path constraint to the constraint system.
  void addKeyPathConstraint(Type keypath, Type root, Type value,
                            ArrayRef<TypeVariableType *> componentTypeVars,
                            ConstraintLocatorBuilder locator,
                            bool isFavored = false);

  /// Add a new constraint with a restriction on its application.
  void addRestrictedConstraint(ConstraintKind kind,
                               ConversionRestrictionKind restriction,
                               Type first, Type second,
                               ConstraintLocatorBuilder locator);

  /// Add a constraint that binds an overload set to a specific choice.
  void addBindOverloadConstraint(Type boundTy, OverloadChoice choice,
                                 ConstraintLocator *locator,
                                 DeclContext *useDC) {
    resolveOverload(locator, boundTy, choice, useDC);
  }

  /// Add a value member constraint to the constraint system.
  void addValueMemberConstraint(Type baseTy, DeclName name, Type memberTy,
                                DeclContext *useDC,
                                FunctionRefKind functionRefKind,
                                ArrayRef<OverloadChoice> outerAlternatives,
                                ConstraintLocatorBuilder locator) {
    assert(baseTy);
    assert(memberTy);
    assert(name);
    assert(useDC);
    switch (simplifyMemberConstraint(
        ConstraintKind::ValueMember, baseTy, name, memberTy, useDC,
        functionRefKind, outerAlternatives, TMF_GenerateConstraints, locator)) {
    case SolutionKind::Unsolved:
      llvm_unreachable("Unsolved result when generating constraints!");

    case SolutionKind::Solved:
      break;

    case SolutionKind::Error:
      if (shouldAddNewFailingConstraint()) {
        addNewFailingConstraint(Constraint::createMemberOrOuterDisjunction(
            *this, ConstraintKind::ValueMember, baseTy, memberTy, name, useDC,
            functionRefKind, outerAlternatives, getConstraintLocator(locator)));
      }
      break;
    }
  }

  /// Add a value member constraint for an UnresolvedMemberRef
  /// to the constraint system.
  void addUnresolvedValueMemberConstraint(Type baseTy, DeclName name,
                                          Type memberTy, DeclContext *useDC,
                                          FunctionRefKind functionRefKind,
                                          ConstraintLocatorBuilder locator) {
    assert(baseTy);
    assert(memberTy);
    assert(name);
    assert(useDC);
    switch (simplifyMemberConstraint(ConstraintKind::UnresolvedValueMember,
                                     baseTy, name, memberTy,
                                     useDC, functionRefKind,
                                     /*outerAlternatives=*/{},
                                     TMF_GenerateConstraints, locator)) {
    case SolutionKind::Unsolved:
      llvm_unreachable("Unsolved result when generating constraints!");

    case SolutionKind::Solved:
      break;

    case SolutionKind::Error:
      if (shouldAddNewFailingConstraint()) {
        addNewFailingConstraint(
          Constraint::createMember(*this, ConstraintKind::UnresolvedValueMember,
                                   baseTy, memberTy, name,
                                   useDC, functionRefKind,
                                   getConstraintLocator(locator)));
      }
      break;
    }
  }

  /// Add an explicit conversion constraint (e.g., \c 'x as T').
  void addExplicitConversionConstraint(Type fromType, Type toType,
                                       bool allowFixes,
                                       ConstraintLocatorBuilder locator);

  /// Add a disjunction constraint.
  void
  addDisjunctionConstraint(ArrayRef<Constraint *> constraints,
                           ConstraintLocatorBuilder locator,
                           RememberChoice_t rememberChoice = ForgetChoice) {
    auto constraint =
      Constraint::createDisjunction(*this, constraints,
                                    getConstraintLocator(locator),
                                    rememberChoice);

    addUnsolvedConstraint(constraint);
  }

  /// Whether we should add a new constraint to capture a failure.
  bool shouldAddNewFailingConstraint() const {
    // Only do this at the top level.
    return !failedConstraint;
  }

  /// Add a new constraint that we know fails.
  void addNewFailingConstraint(Constraint *constraint) {
    assert(shouldAddNewFailingConstraint());
    failedConstraint = constraint;
    failedConstraint->setActive(false);

    // Record this as a newly-generated constraint.
    if (solverState) {
      solverState->addGeneratedConstraint(constraint);
      solverState->retireConstraint(constraint);
    }
  }

  /// Add a newly-generated constraint that is known not to be solvable
  /// right now.
  void addUnsolvedConstraint(Constraint *constraint) {
    // We couldn't solve this constraint; add it to the pile.
    InactiveConstraints.push_back(constraint);

    // Add this constraint to the constraint graph.
    CG.addConstraint(constraint);

    // Record this as a newly-generated constraint.
    if (solverState)
      solverState->addGeneratedConstraint(constraint);
  }

  /// Remove an inactive constraint from the current constraint graph.
  void removeInactiveConstraint(Constraint *constraint) {
    CG.removeConstraint(constraint);
    InactiveConstraints.erase(constraint);

    if (solverState)
      solverState->retireConstraint(constraint);
  }

  /// Transfer given constraint from to active list
  /// for solver to attempt its simplification.
  void activateConstraint(Constraint *constraint) {
    assert(!constraint->isActive() && "Constraint is already active");
    ActiveConstraints.splice(ActiveConstraints.end(), InactiveConstraints,
                             constraint);
    constraint->setActive(true);
  }

  void deactivateConstraint(Constraint *constraint) {
    assert(constraint->isActive() && "Constraint is already inactive");
    InactiveConstraints.splice(InactiveConstraints.end(),
                               ActiveConstraints, constraint);
    constraint->setActive(false);
  }

  void retireConstraint(Constraint *constraint) {
    if (constraint->isActive())
      deactivateConstraint(constraint);
    removeInactiveConstraint(constraint);
  }

  /// Note that this constraint is "favored" within its disjunction, and
  /// should be tried first to the exclusion of non-favored constraints in
  /// the same disjunction.
  void favorConstraint(Constraint *constraint) {
    if (constraint->isFavored())
      return;

    if (solverState) {
      solverState->favorConstraint(constraint);
    } else {
      constraint->setFavored();
    }
  }

  /// Retrieve the list of inactive constraints.
  ConstraintList &getConstraints() { return InactiveConstraints; }

  /// The worklist of "active" constraints that should be revisited
  /// due to a change.
  ConstraintList &getActiveConstraints() { return ActiveConstraints; }

  void findConstraints(SmallVectorImpl<Constraint *> &found,
                       llvm::function_ref<bool(const Constraint &)> pred) {
    filterConstraints(ActiveConstraints, pred, found);
    filterConstraints(InactiveConstraints, pred, found);
  }

  /// Retrieve the representative of the equivalence class containing
  /// this type variable.
  TypeVariableType *getRepresentative(TypeVariableType *typeVar) const {
    return typeVar->getImpl().getRepresentative(getSavedBindings());
  }

  /// Gets the VarDecl associateed with resolvedOverload, and the type of the
  /// storage wrapper if the decl has an associated storage wrapper.
  Optional<std::pair<VarDecl *, Type>>
  getStorageWrapperInformation(ResolvedOverloadSetListItem *resolvedOverload) {
    assert(resolvedOverload);
    if (resolvedOverload->Choice.isDecl()) {
      if (auto *decl = dyn_cast<VarDecl>(resolvedOverload->Choice.getDecl())) {
        if (decl->hasAttachedPropertyWrapper()) {
          if (auto storageWrapper = decl->getPropertyWrapperStorageWrapper()) {
            Type type = storageWrapper->getInterfaceType();
            if (Type baseType = resolvedOverload->Choice.getBaseType()) {
              type = baseType->getTypeOfMember(DC->getParentModule(),
                                               storageWrapper, type);
            }
            return std::make_pair(decl, type);
          }
        }
      }
    }
    return None;
  }

  /// Gets the VarDecl associateed with resolvedOverload, and the type of the
  /// backing storage if the decl has an associated property wrapper.
  Optional<std::pair<VarDecl *, Type>>
  getPropertyWrapperInformation(ResolvedOverloadSetListItem *resolvedOverload) {
    assert(resolvedOverload);
    if (resolvedOverload->Choice.isDecl()) {
      if (auto *decl = dyn_cast<VarDecl>(resolvedOverload->Choice.getDecl())) {
        if (decl->hasAttachedPropertyWrapper()) {
          auto wrapperTy = decl->getPropertyWrapperBackingPropertyType();
          if (Type baseType = resolvedOverload->Choice.getBaseType()) {
            wrapperTy = baseType->getTypeOfMember(DC->getParentModule(),
                                                  decl, wrapperTy);
          }
          return std::make_pair(decl, wrapperTy);
        }
      }
    }
    return None;
  }

  /// Gets the VarDecl, and the type of the type property that it wraps if
  /// resolved overload has a decl which is the backing storage for a
  /// property wrapper.
  Optional<std::pair<VarDecl *, Type>>
  getWrappedPropertyInformation(ResolvedOverloadSetListItem *resolvedOverload) {
    assert(resolvedOverload);
    if (resolvedOverload->Choice.isDecl()) {
      if (auto *decl = dyn_cast<VarDecl>(resolvedOverload->Choice.getDecl())) {
        if (auto wrapped = decl->getOriginalWrappedProperty()) {
          Type type = wrapped->getInterfaceType();
          if (Type baseType = resolvedOverload->Choice.getBaseType()) {
            type = baseType->getTypeOfMember(DC->getParentModule(),
                                             wrapped, type);
          }
          return std::make_pair(decl, type);
        }
      }
    }
    return None;
  }

  /// Merge the equivalence sets of the two type variables.
  ///
  /// Note that both \c typeVar1 and \c typeVar2 must be the
  /// representatives of their equivalence classes, and must be
  /// distinct.
  void mergeEquivalenceClasses(TypeVariableType *typeVar1,
                               TypeVariableType *typeVar2,
                               bool updateWorkList = true);

  /// Flags that direct type matching.
  enum TypeMatchFlags {
    /// Indicates that we are in a context where we should be
    /// generating constraints for any unsolvable problems.
    ///
    /// This flag is automatically introduced when type matching destructures
    /// a type constructor (tuple, function type, etc.), solving that
    /// constraint while potentially generating others.
    TMF_GenerateConstraints = 0x01,

    /// Indicates that we are applying a fix.
    TMF_ApplyingFix = 0x02,
  };

  /// Options that govern how type matching should proceed.
  using TypeMatchOptions = OptionSet<TypeMatchFlags>;

  /// Retrieve the fixed type corresponding to the given type variable,
  /// or a null type if there is no fixed type.
  Type getFixedType(TypeVariableType *typeVar) const {
    return typeVar->getImpl().getFixedType(getSavedBindings());
  }

  /// Retrieve the fixed type corresponding to a given type variable,
  /// recursively, until we hit something that isn't a type variable
  /// or a type variable that doesn't have a fixed type.
  ///
  /// \param type The type to simplify.
  ///
  /// \param wantRValue Whether this routine should look through
  /// lvalues at each step.
  Type getFixedTypeRecursive(Type type, bool wantRValue) const {
    TypeMatchOptions flags = None;
    return getFixedTypeRecursive(type, flags, wantRValue);
  }

  /// Retrieve the fixed type corresponding to a given type variable,
  /// recursively, until we hit something that isn't a type variable
  /// or a type variable that doesn't have a fixed type.
  ///
  /// \param type The type to simplify.
  ///
  /// \param flags When simplifying one of the types that is part of a
  /// constraint we are examining, the set of flags that governs the
  /// simplification. The set of flags may be both queried and mutated.
  ///
  /// \param wantRValue Whether this routine should look through
  /// lvalues at each step.
  Type getFixedTypeRecursive(Type type, TypeMatchOptions &flags,
                             bool wantRValue) const;

  /// Determine whether the given type variable occurs within the given type.
  ///
  /// This routine assumes that the type has already been fully simplified.
  ///
  /// \param involvesOtherTypeVariables if non-null, records whether any other
  /// type variables are present in the type.
  static bool typeVarOccursInType(TypeVariableType *typeVar, Type type,
                                  bool *involvesOtherTypeVariables = nullptr);

  /// Assign a fixed type to the given type variable.
  ///
  /// \param typeVar The type variable to bind.
  ///
  /// \param type The fixed type to which the type variable will be bound.
  ///
  /// \param updateState Whether to update the state based on this binding.
  /// False when we're only assigning a type as part of reconstructing 
  /// a complete solution from partial solutions.
  void assignFixedType(TypeVariableType *typeVar, Type type,
                       bool updateState = true);
  
  /// Determine if the type in question is an Array<T> and, if so, provide the
  /// element type of the array.
  static Optional<Type> isArrayType(Type type);

  /// Determine whether the given type is a dictionary and, if so, provide the
  /// key and value types for the dictionary.
  static Optional<std::pair<Type, Type>> isDictionaryType(Type type);

  /// Determine if the type in question is a Set<T> and, if so, provide the
  /// element type of the set.
  static Optional<Type> isSetType(Type t);

  /// Determine if the type in question is one of the known collection types.
  static bool isCollectionType(Type t);

  /// Determine if the type in question is AnyHashable.
  static bool isAnyHashableType(Type t);

  /// Call Expr::isTypeReference on the given expression, using a
  /// custom accessor for the type on the expression that reads the
  /// type from the ConstraintSystem expression type map.
  bool isTypeReference(const Expr *E);

  /// Call Expr::isIsStaticallyDerivedMetatype on the given
  /// expression, using a custom accessor for the type on the
  /// expression that reads the type from the ConstraintSystem
  /// expression type map.
  bool isStaticallyDerivedMetatype(const Expr *E);

  /// Call TypeExpr::getInstanceType on the given expression, using a
  /// custom accessor for the type on the expression that reads the
  /// type from the ConstraintSystem expression type map.
  Type getInstanceType(const TypeExpr *E);

  /// Call AbstractClosureExpr::getResultType on the given expression,
  /// using a custom accessor for the type on the expression that
  /// reads the type from the ConstraintSystem expression type map.
  Type getResultType(const AbstractClosureExpr *E);

private:
  /// Introduce the constraints associated with the given type variable
  /// into the worklist.
  void addTypeVariableConstraintsToWorkList(TypeVariableType *typeVar);

  static void
  filterConstraints(ConstraintList &constraints,
                    llvm::function_ref<bool(const Constraint &)> pred,
                    SmallVectorImpl<Constraint *> &found) {
    for (auto &constraint : constraints) {
      if (pred(constraint))
        found.push_back(&constraint);
    }
  }

public:

  /// Coerce the given expression to an rvalue, if it isn't already.
  Expr *coerceToRValue(Expr *expr);

  /// "Open" the given unbound type by introducing fresh type
  /// variables for generic parameters and constructing a bound generic
  /// type from these type variables.
  ///
  /// \param unbound The type to open.
  ///
  /// \returns The opened type.
  Type openUnboundGenericType(UnboundGenericType *unbound,
                              ConstraintLocatorBuilder locator,
                              OpenedTypeMap &replacements);

  /// "Open" the given type by replacing any occurrences of unbound
  /// generic types with bound generic types with fresh type variables as
  /// generic arguments.
  ///
  /// \param type The type to open.
  ///
  /// \returns The opened type.
  Type openUnboundGenericType(Type type, ConstraintLocatorBuilder locator);

  /// "Open" the given type by replacing any occurrences of generic
  /// parameter types and dependent member types with fresh type variables.
  ///
  /// \param type The type to open.
  ///
  /// \returns The opened type, or \c type if there are no archetypes in it.
  Type openType(Type type, OpenedTypeMap &replacements);

  /// "Open" the given function type.
  ///
  /// If the function type is non-generic, this is equivalent to calling
  /// openType(). Otherwise, it calls openGeneric() on the generic
  /// function's signature first.
  ///
  /// \param funcType The function type to open.
  ///
  /// \param replacements The mapping from opened types to the type
  /// variables to which they were opened.
  ///
  /// \param outerDC The generic context containing the declaration.
  ///
  /// \returns The opened type, or \c type if there are no archetypes in it.
  FunctionType *openFunctionType(AnyFunctionType *funcType,
                                 ConstraintLocatorBuilder locator,
                                 OpenedTypeMap &replacements,
                                 DeclContext *outerDC);

  /// Open the generic parameter list and its requirements,
  /// creating type variables for each of the type parameters.
  void openGeneric(DeclContext *outerDC,
                   GenericSignature signature,
                   ConstraintLocatorBuilder locator,
                   OpenedTypeMap &replacements);

  /// Open the generic parameter list creating type variables for each of the
  /// type parameters.
  void openGenericParameters(DeclContext *outerDC,
                             GenericSignature signature,
                             OpenedTypeMap &replacements,
                             ConstraintLocatorBuilder locator);

  /// Given generic signature open its generic requirements,
  /// using substitution function, and record them in the
  /// constraint system for further processing.
  void openGenericRequirements(DeclContext *outerDC,
                               GenericSignature signature,
                               bool skipProtocolSelfConstraint,
                               ConstraintLocatorBuilder locator,
                               llvm::function_ref<Type(Type)> subst);

  /// Record the set of opened types for the given locator.
  void recordOpenedTypes(
         ConstraintLocatorBuilder locator,
         const OpenedTypeMap &replacements);

  /// Retrieve the type of a reference to the given value declaration.
  ///
  /// For references to polymorphic function types, this routine "opens up"
  /// the type by replacing each instance of an archetype with a fresh type
  /// variable.
  ///
  /// \param decl The declarations whose type is being computed.
  ///
  /// \returns a pair containing the full opened type (if applicable) and
  /// opened type of a reference to declaration.
  std::pair<Type, Type> getTypeOfReference(
                          ValueDecl *decl,
                          FunctionRefKind functionRefKind,
                          ConstraintLocatorBuilder locator,
                          DeclContext *useDC);

  /// Return the type-of-reference of the given value.
  ///
  /// \param baseType if non-null, return the type of a member reference to
  ///   this value when the base has the given type
  ///
  /// \param UseDC The context of the access.  Some variables have different
  ///   types depending on where they are used.
  ///
  /// \param base The optional base expression of this value reference
  ///
  /// \param wantInterfaceType Whether we want the interface type, if available.
  Type getUnopenedTypeOfReference(VarDecl *value, Type baseType,
                                  DeclContext *UseDC,
                                  const DeclRefExpr *base = nullptr,
                                  bool wantInterfaceType = false);

  /// Retrieve the type of a reference to the given value declaration,
  /// as a member with a base of the given type.
  ///
  /// For references to generic function types or members of generic types,
  /// this routine "opens up" the type by replacing each instance of a generic
  /// parameter with a fresh type variable.
  ///
  /// \param isDynamicResult Indicates that this declaration was found via
  /// dynamic lookup.
  ///
  /// \returns a pair containing the full opened type (which includes the opened
  /// base) and opened type of a reference to this member.
  std::pair<Type, Type> getTypeOfMemberReference(
                          Type baseTy, ValueDecl *decl, DeclContext *useDC,
                          bool isDynamicResult,
                          FunctionRefKind functionRefKind,
                          ConstraintLocatorBuilder locator,
                          const DeclRefExpr *base = nullptr,
                          OpenedTypeMap *replacements = nullptr);

private:
  /// Adjust the constraint system to accomodate the given selected overload, and
  /// recompute the type of the referenced declaration.
  ///
  /// \returns a pair containing the adjusted opened type of a reference to
  /// this member and a bit indicating whether or not a bind constraint was added.
  std::pair<Type, bool> adjustTypeOfOverloadReference(
      const OverloadChoice &choice, ConstraintLocator *locator, Type boundType,
      Type refType, DeclContext *useDC,
      llvm::function_ref<void(unsigned int, Type, ConstraintLocator *)>
          verifyThatArgumentIsHashable);

public:
  /// Attempt to simplify the set of overloads corresponding to a given
  /// function application constraint.
  ///
  /// \param fnTypeVar The type variable that describes the set of
  /// overloads for the function.
  ///
  /// \param argFnType The call signature, which includes the call arguments
  /// (as the function parameters) and the expected result type of the
  /// call.
  ///
  /// \returns \c fnType, or some simplified form of it if this function
  /// was able to find a single overload or derive some common structure
  /// among the overloads.
  Type simplifyAppliedOverloads(TypeVariableType *fnTypeVar,
                                const FunctionType *argFnType,
                                ConstraintLocatorBuilder locator);

  /// Retrieve the type that will be used when matching the given overload.
  Type getEffectiveOverloadType(const OverloadChoice &overload,
                                bool allowMembers,
                                DeclContext *useDC);

  /// Add a new overload set to the list of unresolved overload
  /// sets.
  void addOverloadSet(Type boundType, ArrayRef<OverloadChoice> choices,
                      DeclContext *useDC, ConstraintLocator *locator,
                      Optional<unsigned> favoredIndex = None);

  void addOverloadSet(ArrayRef<Constraint *> choices,
                      ConstraintLocator *locator);

  /// Retrieve the allocator used by this constraint system.
  llvm::BumpPtrAllocator &getAllocator() { return Allocator; }

  template <typename It>
  ArrayRef<typename std::iterator_traits<It>::value_type>
  allocateCopy(It start, It end) {
    using T = typename std::iterator_traits<It>::value_type;
    T *result = (T*)getAllocator().Allocate(sizeof(T)*(end-start), alignof(T));
    unsigned i;
    for (i = 0; start != end; ++start, ++i)
      new (result+i) T(*start);
    return ArrayRef<T>(result, i);
  }

  template<typename T>
  ArrayRef<T> allocateCopy(ArrayRef<T> array) {
    return allocateCopy(array.begin(), array.end());
  }

  template<typename T>
  ArrayRef<T> allocateCopy(SmallVectorImpl<T> &vec) {
    return allocateCopy(vec.begin(), vec.end());
  }

  /// Generate constraints for the given (unchecked) expression.
  ///
  /// \returns a possibly-sanitized expression, or null if an error occurred.
  Expr *generateConstraints(Expr *E, DeclContext *dc = nullptr);

  /// Generate constraints for binding the given pattern to the
  /// value of the given expression.
  ///
  /// \returns a possibly-sanitized initializer, or null if an error occurred.
  Type generateConstraints(Pattern *P, ConstraintLocatorBuilder locator);

  /// Generate constraints for a given set of overload choices.
  ///
  /// \param constraints The container of generated constraint choices.
  ///
  /// \param type The type each choice should be bound to.
  ///
  /// \param choices The set of choices to convert into bind overload
  /// constraints so solver could attempt each one.
  ///
  /// \param useDC The declaration context where each choice is used.
  ///
  /// \param locator The locator to use when generating constraints.
  ///
  /// \param favoredIndex If there is a "favored" or preferred choice
  /// this is its index in the set of choices.
  ///
  /// \param requiresFix Determines whether choices require a fix to
  /// be included in the result. If the fix couldn't be provided by
  /// `getFix` for any given choice, such choice would be filtered out.
  ///
  /// \param getFix Optional callback to determine a fix for a given
  /// choice (first argument is a position of current choice,
  /// second - the choice in question).
  void generateConstraints(
      SmallVectorImpl<Constraint *> &constraints, Type type,
      ArrayRef<OverloadChoice> choices, DeclContext *useDC,
      ConstraintLocator *locator, Optional<unsigned> favoredIndex = None,
      bool requiresFix = false,
      llvm::function_ref<ConstraintFix *(unsigned, const OverloadChoice &)>
          getFix = [](unsigned, const OverloadChoice &) { return nullptr; });

  /// Propagate constraints in an effort to enforce local
  /// consistency to reduce the time to solve the system.
  ///
  /// \returns true if the system is known to be inconsistent (have no
  /// solutions).
  bool propagateConstraints();

  /// The result of attempting to resolve a constraint or set of
  /// constraints.
  enum class SolutionKind : char {
    /// The constraint has been solved completely, and provides no
    /// more information.
    Solved,
    /// The constraint could not be solved at this point.
    Unsolved,
    /// The constraint uncovers an inconsistency in the system.
    Error
  };

  class TypeMatchResult {
    SolutionKind Kind;

  public:
    inline bool isSuccess() const { return Kind == SolutionKind::Solved; }
    inline bool isFailure() const { return Kind == SolutionKind::Error; }
    inline bool isAmbiguous() const { return Kind == SolutionKind::Unsolved; }

    static TypeMatchResult success(ConstraintSystem &cs) {
      return {SolutionKind::Solved};
    }

    static TypeMatchResult failure(ConstraintSystem &cs,
                                   ConstraintLocatorBuilder location) {
      return {SolutionKind::Error};
    }

    static TypeMatchResult ambiguous(ConstraintSystem &cs) {
      return {SolutionKind::Unsolved};
    }

    operator SolutionKind() { return Kind; }
  private:
    TypeMatchResult(SolutionKind result) : Kind(result) {}
  };

  /// Attempt to repair typing failures and record fixes if needed.
  /// \return true if at least some of the failures has been repaired
  /// successfully, which allows type matcher to continue.
  bool repairFailures(Type lhs, Type rhs, ConstraintKind matchKind,
                      SmallVectorImpl<RestrictionOrFix> &conversionsOrFixes,
                      ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up two tuple types.
  ///
  /// \returns the result of performing the tuple-to-tuple conversion.
  TypeMatchResult matchTupleTypes(TupleType *tuple1, TupleType *tuple2,
                                  ConstraintKind kind, TypeMatchOptions flags,
                                  ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches a scalar type to
  /// a tuple type.
  ///
  /// \returns the result of performing the scalar-to-tuple conversion.
  TypeMatchResult matchScalarToTupleTypes(Type type1, TupleType *tuple2,
                                          ConstraintKind kind,
                                          TypeMatchOptions flags,
                                          ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up two function
  /// types.
  TypeMatchResult matchFunctionTypes(FunctionType *func1, FunctionType *func2,
                                     ConstraintKind kind, TypeMatchOptions flags,
                                     ConstraintLocatorBuilder locator);
  
  /// Subroutine of \c matchTypes(), which matches up a value to a
  /// superclass.
  TypeMatchResult matchSuperclassTypes(Type type1, Type type2,
                                       TypeMatchOptions flags,
                                       ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up two types that
  /// refer to the same declaration via their generic arguments.
  TypeMatchResult matchDeepEqualityTypes(Type type1, Type type2,
                                         ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), which matches up a value to an
  /// existential type.
  ///
  /// \param kind Either ConstraintKind::SelfObjectOfProtocol or
  /// ConstraintKind::ConformsTo. Usually this uses SelfObjectOfProtocol,
  /// but when matching the instance type of a metatype with the instance type
  /// of an existential metatype, since we want an actual conformance check.
  TypeMatchResult matchExistentialTypes(Type type1, Type type2,
                                        ConstraintKind kind,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator);

  /// Subroutine of \c matchTypes(), used to bind a type to a
  /// type variable.
  TypeMatchResult matchTypesBindTypeVar(
      TypeVariableType *typeVar, Type type, ConstraintKind kind,
      TypeMatchOptions flags, ConstraintLocatorBuilder locator,
      llvm::function_ref<TypeMatchResult()> formUnsolvedResult);

public: // FIXME: public due to statics in CSSimplify.cpp
  /// Attempt to match up types \c type1 and \c type2, which in effect
  /// is solving the given type constraint between these two types.
  ///
  /// \param type1 The first type, which is on the left of the type relation.
  ///
  /// \param type2 The second type, which is on the right of the type relation.
  ///
  /// \param kind The kind of type match being performed, e.g., exact match,
  /// trivial subtyping, subtyping, or conversion.
  ///
  /// \param flags A set of flags composed from the TMF_* constants, which
  /// indicates how the constraint should be simplified.
  ///
  /// \param locator The locator that will be used to track the location of
  /// the specific types being matched.
  ///
  /// \returns the result of attempting to solve this constraint.
  TypeMatchResult matchTypes(Type type1, Type type2, ConstraintKind kind,
                             TypeMatchOptions flags,
                             ConstraintLocatorBuilder locator);

  TypeMatchResult getTypeMatchSuccess() {
    return TypeMatchResult::success(*this);
  }

  TypeMatchResult getTypeMatchFailure(ConstraintLocatorBuilder locator) {
    return TypeMatchResult::failure(*this, locator);
  }

  TypeMatchResult getTypeMatchAmbiguous() {
    return TypeMatchResult::ambiguous(*this);
  }

public:
  /// Given a function type where the eventual result type is an optional,
  /// where "eventual result type" is defined as:
  ///   1. The result type is an optional
  ///   2. The result type is a function type with an eventual result
  ///      type that is an optional.
  ///
  /// return the same function type but with the eventual result type
  /// replaced by its underlying type.
  ///
  /// i.e. return (S) -> T for (S) -> T?
  //       return (X) -> () -> Y for (X) -> () -> Y?
  Type replaceFinalResultTypeWithUnderlying(AnyFunctionType *fnTy) {
    auto resultTy = fnTy->getResult();
    if (auto *resultFnTy = resultTy->getAs<AnyFunctionType>())
      resultTy = replaceFinalResultTypeWithUnderlying(resultFnTy);
    else
      resultTy = resultTy->getWithoutSpecifierType()->getOptionalObjectType();

    assert(resultTy);

    if (auto *genericFn = fnTy->getAs<GenericFunctionType>()) {
      return GenericFunctionType::get(genericFn->getGenericSignature(),
                                      genericFn->getParams(), resultTy,
                                      genericFn->getExtInfo());
    }

    return FunctionType::get(fnTy->getParams(), resultTy, fnTy->getExtInfo());
  }

  // Build a disjunction that attempts both T? and T for a particular
  // type binding. The choice of T? is preferred, and we will not
  // attempt T if we can type check with T?
  void
  buildDisjunctionForOptionalVsUnderlying(Type boundTy, Type type,
                                          ConstraintLocator *locator) {
    // NOTE: If we use other locator kinds for these disjunctions, we
    // need to account for it in solution scores for forced-unwraps.
    assert(locator->getPath().back().getKind() ==
               ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice ||
           locator->getPath().back().getKind() ==
               ConstraintLocator::DynamicLookupResult);

    // Create the constraint to bind to the optional type and make it
    // the favored choice.
    auto *bindToOptional =
      Constraint::create(*this, ConstraintKind::Bind, boundTy, type, locator);
    bindToOptional->setFavored();

    Type underlyingType;
    if (auto *fnTy = type->getAs<AnyFunctionType>())
      underlyingType = replaceFinalResultTypeWithUnderlying(fnTy);
    else
      underlyingType = type->getWithoutSpecifierType()->getOptionalObjectType();

    assert(underlyingType);

    if (type->is<LValueType>())
      underlyingType = LValueType::get(underlyingType);
    assert(!type->is<InOutType>());

    auto *bindToUnderlying = Constraint::create(
        *this, ConstraintKind::Bind, boundTy, underlyingType, locator);

    llvm::SmallVector<Constraint *, 2> choices = {bindToOptional,
                                                  bindToUnderlying};

    // Create the disjunction
    addDisjunctionConstraint(choices, locator, RememberChoice);
  }

  // Build a disjunction for types declared IUO.
  void
  buildDisjunctionForImplicitlyUnwrappedOptional(Type boundTy, Type type,
                                                 ConstraintLocator *locator) {
    auto *disjunctionLocator = getConstraintLocator(
        locator, ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice);
    buildDisjunctionForOptionalVsUnderlying(boundTy, type, disjunctionLocator);
  }

  // Build a disjunction for dynamic lookup results, which are
  // implicitly unwrapped if needed.
  void buildDisjunctionForDynamicLookupResult(Type boundTy, Type type,
                                              ConstraintLocator *locator) {
    auto *dynamicLocator =
        getConstraintLocator(locator, ConstraintLocator::DynamicLookupResult);
    buildDisjunctionForOptionalVsUnderlying(boundTy, type, dynamicLocator);
  }

  /// Resolve the given overload set to the given choice.
  void resolveOverload(ConstraintLocator *locator, Type boundType,
                       OverloadChoice choice, DeclContext *useDC);

  /// Simplify a type, by replacing type variables with either their
  /// fixed types (if available) or their representatives.
  ///
  /// The resulting types can be compared canonically, so long as additional
  /// type equivalence requirements aren't introduced between comparisons.
  Type simplifyType(Type type) const;

  /// Simplify a type, by replacing type variables with either their
  /// fixed types (if available) or their representatives.
  ///
  /// \param flags If the simplified type has changed, this will be updated
  /// to include \c TMF_GenerateConstraints.
  ///
  /// The resulting types can be compared canonically, so long as additional
  /// type equivalence requirements aren't introduced between comparisons.
  Type simplifyType(Type type, TypeMatchOptions &flags) {
    Type result = simplifyType(type);
    if (result.getPointer() != type.getPointer())
      flags |= TMF_GenerateConstraints;
    return result;
  }

  /// Given a ValueMember, UnresolvedValueMember, or TypeMember constraint,
  /// perform a lookup into the specified base type to find a candidate list.
  /// The list returned includes the viable candidates as well as the unviable
  /// ones (along with reasons why they aren't viable).
  ///
  /// If includeInaccessibleMembers is set to true, this burns compile time to
  /// try to identify and classify inaccessible members that may be being
  /// referenced.
  MemberLookupResult performMemberLookup(ConstraintKind constraintKind,
                                         DeclName memberName, Type baseTy,
                                         FunctionRefKind functionRefKind,
                                         ConstraintLocator *memberLocator,
                                         bool includeInaccessibleMembers);

  /// Build implicit autoclosure expression wrapping a given expression.
  /// Given expression represents computed result of the closure.
  Expr *buildAutoClosureExpr(Expr *expr, FunctionType *closureType);

private:
  /// Determines whether or not a given conversion at a given locator requires
  /// the creation of a temporary value that's only valid for a limited scope.
  /// Such ephemeral conversions, such as array-to-pointer, cannot be passed to
  /// non-ephemeral parameters.
  ConversionEphemeralness
  isConversionEphemeral(ConversionRestrictionKind conversion,
                        ConstraintLocatorBuilder locator);

  /// Simplifies a type by replacing type variables with the result of
  /// \c getFixedTypeFn and performing lookup on dependent member types.
  Type simplifyTypeImpl(Type type,
      llvm::function_ref<Type(TypeVariableType *)> getFixedTypeFn) const;

  /// Attempt to simplify the given construction constraint.
  ///
  /// \param valueType The type being constructed.
  ///
  /// \param fnType The argument type that will be the input to the
  /// valueType initializer and the result type will be the result of
  /// calling that initializer.
  ///
  /// \param flags A set of flags composed from the TMF_* constants, which
  /// indicates how the constraint should be simplified.
  /// 
  /// \param locator Locator describing where this construction
  /// occurred.
  SolutionKind simplifyConstructionConstraint(Type valueType, 
                                              FunctionType *fnType,
                                              TypeMatchOptions flags,
                                              DeclContext *DC,
                                              FunctionRefKind functionRefKind,
                                              ConstraintLocator *locator);

  /// Attempt to simplify the given conformance constraint.
  ///
  /// \param type The type being tested.
  /// \param protocol The protocol to which the type should conform.
  /// \param kind Either ConstraintKind::SelfObjectOfProtocol or
  /// ConstraintKind::ConformsTo.
  /// \param locator Locator describing where this constraint occurred.
  SolutionKind simplifyConformsToConstraint(Type type, ProtocolDecl *protocol,
                                            ConstraintKind kind,
                                            ConstraintLocatorBuilder locator,
                                            TypeMatchOptions flags);

  /// Attempt to simplify the given conformance constraint.
  ///
  /// \param type The type being tested.
  /// \param protocol The protocol or protocol composition type to which the
  /// type should conform.
  /// \param locator Locator describing where this constraint occurred.
  ///
  /// \param kind If this is SelfTypeOfProtocol, we allow an existential type
  /// that contains the protocol but does not conform to it (eg, due to
  /// associated types).
  SolutionKind simplifyConformsToConstraint(Type type, Type protocol,
                                            ConstraintKind kind,
                                            ConstraintLocatorBuilder locator,
                                            TypeMatchOptions flags);

  /// Attempt to simplify a checked-cast constraint.
  SolutionKind simplifyCheckedCastConstraint(Type fromType, Type toType,
                                             TypeMatchOptions flags,
                                             ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given member constraint.
  SolutionKind simplifyMemberConstraint(
      ConstraintKind kind, Type baseType, DeclName member, Type memberType,
      DeclContext *useDC, FunctionRefKind functionRefKind,
      ArrayRef<OverloadChoice> outerAlternatives, TypeMatchOptions flags,
      ConstraintLocatorBuilder locator);

  /// Attempt to simplify the optional object constraint.
  SolutionKind simplifyOptionalObjectConstraint(
                                          Type first, Type second,
                                          TypeMatchOptions flags,
                                          ConstraintLocatorBuilder locator);

  /// Attempt to simplify a function input or result constraint.
  SolutionKind simplifyFunctionComponentConstraint(
                                          ConstraintKind kind,
                                          Type first, Type second,
                                          TypeMatchOptions flags,
                                          ConstraintLocatorBuilder locator);

  /// Attempt to simplify an OpaqueUnderlyingType constraint.
  SolutionKind simplifyOpaqueUnderlyingTypeConstraint(Type type1,
                                              Type type2,
                                              TypeMatchOptions flags,
                                              ConstraintLocatorBuilder locator);
  
  /// Attempt to simplify the BridgingConversion constraint.
  SolutionKind simplifyBridgingConstraint(Type type1,
                                         Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the ApplicableFunction constraint.
  SolutionKind simplifyApplicableFnConstraint(
                                      Type type1,
                                      Type type2,
                                      TypeMatchOptions flags,
                                      ConstraintLocatorBuilder locator);

  /// Attempt to simplify the DynamicCallableApplicableFunction constraint.
  SolutionKind simplifyDynamicCallableApplicableFnConstraint(
                                      Type type1,
                                      Type type2,
                                      TypeMatchOptions flags,
                                      ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given DynamicTypeOf constraint.
  SolutionKind simplifyDynamicTypeOfConstraint(
                                         Type type1, Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given EscapableFunctionOf constraint.
  SolutionKind simplifyEscapableFunctionOfConstraint(
                                         Type type1, Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given OpenedExistentialOf constraint.
  SolutionKind simplifyOpenedExistentialOfConstraint(
                                         Type type1, Type type2,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given KeyPathApplication constraint.
  SolutionKind simplifyKeyPathApplicationConstraint(
                                         Type keyPath,
                                         Type root,
                                         Type value,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given KeyPath constraint.
  SolutionKind simplifyKeyPathConstraint(
      Type keyPath,
      Type root,
      Type value,
      ArrayRef<TypeVariableType *> componentTypeVars,
      TypeMatchOptions flags,
      ConstraintLocatorBuilder locator);

  /// Attempt to simplify the given defaultable constraint.
  SolutionKind simplifyDefaultableConstraint(Type first, Type second,
                                             TypeMatchOptions flags,
                                             ConstraintLocatorBuilder locator);

  /// Attempt to simplify a one-way constraint.
  SolutionKind simplifyOneWayConstraint(ConstraintKind kind,
                                        Type first, Type second,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator);

  /// Simplify a conversion constraint by applying the given
  /// reduction rule, which is known to apply at the outermost level.
  SolutionKind simplifyRestrictedConstraintImpl(
                 ConversionRestrictionKind restriction,
                 Type type1, Type type2,
                 ConstraintKind matchKind,
                 TypeMatchOptions flags,
                 ConstraintLocatorBuilder locator);

  /// Simplify a conversion constraint by applying the given
  /// reduction rule, which is known to apply at the outermost level.
  SolutionKind simplifyRestrictedConstraint(
                 ConversionRestrictionKind restriction,
                 Type type1, Type type2,
                 ConstraintKind matchKind,
                 TypeMatchOptions flags,
                 ConstraintLocatorBuilder locator);

public: // FIXME: Public for use by static functions.
  /// Simplify a conversion constraint with a fix applied to it.
  SolutionKind simplifyFixConstraint(ConstraintFix *fix, Type type1, Type type2,
                                     ConstraintKind matchKind,
                                     TypeMatchOptions flags,
                                     ConstraintLocatorBuilder locator);

public:
  /// Simplify the system of constraints, by breaking down complex
  /// constraints into simpler constraints.
  ///
  /// The result of simplification is a constraint system consisting of
  /// only simple constraints relating type variables to each other or
  /// directly to fixed types. There are no constraints that involve
  /// type constructors on both sides. The simplified constraint system may,
  /// of course, include type variables for which we have constraints but
  /// no fixed type. Such type variables are left to the solver to bind.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool simplify(bool ContinueAfterFailures = false);

  /// Simplify the given constraint.
  SolutionKind simplifyConstraint(const Constraint &constraint);
  /// Simplify the given disjunction choice.
  void simplifyDisjunctionChoice(Constraint *choice);

  /// Apply the given function builder to the closure expression.
  TypeMatchResult applyFunctionBuilder(ClosureExpr *closure, Type builderType,
                                       ConstraintLocator *calleeLocator,
                                       ConstraintLocatorBuilder locator);

private:
  /// The kind of bindings that are permitted.
  enum class AllowedBindingKind : uint8_t {
    /// Only the exact type.
    Exact,
    /// Supertypes of the specified type.
    Supertypes,
    /// Subtypes of the specified type.
    Subtypes
  };

  /// The kind of literal binding found.
  enum class LiteralBindingKind : uint8_t {
    None,
    Collection,
    Float,
    Atom,
  };

  /// A potential binding from the type variable to a particular type,
  /// along with information that can be used to construct related
  /// bindings, e.g., the supertypes of a given type.
  struct PotentialBinding {
    /// The type to which the type variable can be bound.
    Type BindingType;

    /// The kind of bindings permitted.
    AllowedBindingKind Kind;

    /// The kind of the constraint this binding came from.
    ConstraintKind BindingSource;

    /// The defaulted protocol associated with this binding.
    ProtocolDecl *DefaultedProtocol;

    /// If this is a binding that comes from a \c Defaultable constraint,
    /// the locator of that constraint.
    ConstraintLocator *DefaultableBinding = nullptr;

    PotentialBinding(Type type, AllowedBindingKind kind,
                     ConstraintKind bindingSource,
                     ProtocolDecl *defaultedProtocol = nullptr,
                     ConstraintLocator *defaultableBinding = nullptr)
        : BindingType(type->getWithoutParens()), Kind(kind),
          BindingSource(bindingSource), DefaultedProtocol(defaultedProtocol),
          DefaultableBinding(defaultableBinding) {}

    bool isDefaultableBinding() const { return DefaultableBinding != nullptr; }

    PotentialBinding withType(Type type) const {
      return {type, Kind, BindingSource, DefaultedProtocol, DefaultableBinding};
    }
  };

  struct PotentialBindings {
    using BindingScore =
        std::tuple<bool, bool, bool, bool, bool, unsigned char, int>;

    TypeVariableType *TypeVar;

    /// The set of potential bindings.
    SmallVector<PotentialBinding, 4> Bindings;

    /// Whether this type variable is fully bound by one of its constraints.
    bool FullyBound = false;

    /// Whether the bindings of this type involve other type variables.
    bool InvolvesTypeVariables = false;

    /// Whether this type variable is considered a hole in the constraint system.
    bool IsHole = false;

    /// Whether the bindings represent (potentially) incomplete set,
    /// there is no way to say with absolute certainty if that's the
    /// case, but that could happen when certain constraints like
    /// `bind param` are present in the system.
    bool PotentiallyIncomplete = false;

    /// Whether this type variable has literal bindings.
    LiteralBindingKind LiteralBinding = LiteralBindingKind::None;

    /// Whether this type variable is only bound above by existential types.
    bool SubtypeOfExistentialType = false;

    /// The number of defaultable bindings.
    unsigned NumDefaultableBindings = 0;

    /// Tracks the position of the last known supertype in the group.
    Optional<unsigned> lastSupertypeIndex;

    /// A set of all constraints which contribute to pontential bindings.
    llvm::SmallPtrSet<Constraint *, 8> Sources;

    PotentialBindings(TypeVariableType *typeVar)
        : TypeVar(typeVar), PotentiallyIncomplete(isGenericParameter()) {}

    /// Determine whether the set of bindings is non-empty.
    explicit operator bool() const { return !Bindings.empty(); }

    /// Whether there are any non-defaultable bindings.
    bool hasNonDefaultableBindings() const {
      return Bindings.size() > NumDefaultableBindings;
    }

    static BindingScore formBindingScore(const PotentialBindings &b) {
      return std::make_tuple(b.IsHole,
                             !b.hasNonDefaultableBindings(),
                             b.FullyBound,
                             b.SubtypeOfExistentialType,
                             b.InvolvesTypeVariables,
                             static_cast<unsigned char>(b.LiteralBinding),
                             -(b.Bindings.size() - b.NumDefaultableBindings));
    }

    /// Compare two sets of bindings, where \c x < y indicates that
    /// \c x is a better set of bindings that \c y.
    friend bool operator<(const PotentialBindings &x,
                          const PotentialBindings &y) {
      if (formBindingScore(x) < formBindingScore(y))
        return true;

      if (formBindingScore(y) < formBindingScore(x))
        return false;

      // If there is a difference in number of default types,
      // prioritize bindings with fewer of them.
      if (x.NumDefaultableBindings != y.NumDefaultableBindings)
        return x.NumDefaultableBindings < y.NumDefaultableBindings;

      // As a last resort, let's check if the bindings are
      // potentially incomplete, and if so, let's de-prioritize them.
      return x.PotentiallyIncomplete < y.PotentiallyIncomplete;
    }

    void foundLiteralBinding(ProtocolDecl *proto) {
      switch (*proto->getKnownProtocolKind()) {
      case KnownProtocolKind::ExpressibleByDictionaryLiteral:
      case KnownProtocolKind::ExpressibleByArrayLiteral:
      case KnownProtocolKind::ExpressibleByStringInterpolation:
        LiteralBinding = LiteralBindingKind::Collection;
        break;

      case KnownProtocolKind::ExpressibleByFloatLiteral:
        LiteralBinding = LiteralBindingKind::Float;
        break;

      default:
        if (LiteralBinding != LiteralBindingKind::Collection)
          LiteralBinding = LiteralBindingKind::Atom;
        break;
      }
    }

    /// Add a potential binding to the list of bindings,
    /// coalescing supertype bounds when we are able to compute the meet.
    void addPotentialBinding(PotentialBinding binding,
                             bool allowJoinMeet = true);

    /// Check if this binding is viable for inclusion in the set.
    bool isViable(PotentialBinding &binding) const;

    bool isGenericParameter() const {
      if (auto *locator = TypeVar->getImpl().getLocator()) {
        auto path = locator->getPath();
        return path.empty() ? false
                            : path.back().getKind() ==
                                  ConstraintLocator::GenericParameter;
      }
      return false;
    }

    void dump(llvm::raw_ostream &out,
              unsigned indent = 0) const LLVM_ATTRIBUTE_USED {
      out.indent(indent);
      if (PotentiallyIncomplete)
        out << "potentially_incomplete ";
      if (FullyBound)
        out << "fully_bound ";
      if (SubtypeOfExistentialType)
        out << "subtype_of_existential ";
      if (LiteralBinding != LiteralBindingKind::None)
        out << "literal=" << static_cast<int>(LiteralBinding) << " ";
      if (InvolvesTypeVariables)
        out << "involves_type_vars ";
      if (NumDefaultableBindings > 0)
        out << "#defaultable_bindings=" << NumDefaultableBindings << " ";

      PrintOptions PO;
      PO.PrintTypesForDebugging = true;
      out << "bindings={";
      interleave(Bindings,
                 [&](const PotentialBinding &binding) {
                   auto type = binding.BindingType;
                   switch (binding.Kind) {
                   case AllowedBindingKind::Exact:
                     break;

                   case AllowedBindingKind::Subtypes:
                     out << "(subtypes of) ";
                     break;

                   case AllowedBindingKind::Supertypes:
                     out << "(supertypes of) ";
                     break;
                   }
                   if (binding.DefaultedProtocol)
                     out << "(default from "
                         << binding.DefaultedProtocol->getName() << ") ";
                   out << type.getString(PO);
                 },
                 [&]() { out << "; "; });
      out << "}";
    }

    void dump(ConstraintSystem *cs,
              unsigned indent = 0) const LLVM_ATTRIBUTE_USED {
      dump(cs->getASTContext().TypeCheckerDebug->getStream());
    }

    void dump(TypeVariableType *typeVar, llvm::raw_ostream &out,
              unsigned indent = 0) const LLVM_ATTRIBUTE_USED {
      out.indent(indent);
      out << "(";
      if (typeVar)
        out << "$T" << typeVar->getImpl().getID();
      dump(out, 1);
      out << ")\n";
    }
  };

  Optional<Type> checkTypeOfBinding(TypeVariableType *typeVar, Type type) const;
  Optional<PotentialBindings> determineBestBindings();
  Optional<ConstraintSystem::PotentialBinding>
  getPotentialBindingForRelationalConstraint(
      PotentialBindings &result, Constraint *constraint,
      bool &hasDependentMemberRelationalConstraints,
      bool &hasNonDependentMemberRelationalConstraints,
      bool &addOptionalSupertypeBindings) const;
  PotentialBindings getPotentialBindings(TypeVariableType *typeVar) const;

private:
  /// Add a constraint to the constraint system.
  SolutionKind addConstraintImpl(ConstraintKind kind, Type first, Type second,
                                 ConstraintLocatorBuilder locator,
                                 bool isFavored);

  /// Collect the current inactive disjunction constraints.
  void collectDisjunctions(SmallVectorImpl<Constraint *> &disjunctions);

  /// Record a particular disjunction choice of
  void recordDisjunctionChoice(ConstraintLocator *disjunctionLocator,
                               unsigned index) {
    DisjunctionChoices.push_back({disjunctionLocator, index});
  }

  /// Filter the set of disjunction terms, keeping only those where the
  /// predicate returns \c true.
  ///
  /// The terms of the disjunction that are filtered out will be marked as
  /// "disabled" so they won't be visited later. If only one term remains
  /// enabled, the disjunction itself will be returned and that term will
  /// be made active.
  ///
  /// \param restoreOnFail If true, then all of the disabled terms will
  /// be re-enabled when this function returns \c Error.
  ///
  /// \returns One of \c Solved (only a single term remained),
  /// \c Unsolved (more than one disjunction terms remain), or
  /// \c Error (all terms were filtered out).
  SolutionKind filterDisjunction(Constraint *disjunction,
                                  bool restoreOnFail,
                                  llvm::function_ref<bool(Constraint *)> pred);

  bool isReadOnlyKeyPathComponent(const AbstractStorageDecl *storage) {
    // See whether key paths can store to this component. (Key paths don't
    // get any special power from being formed in certain contexts, such
    // as the ability to assign to `let`s in initialization contexts, so
    // we pass null for the DC to `isSettable` here.)
    if (!getASTContext().isSwiftVersionAtLeast(5)) {
      // As a source-compatibility measure, continue to allow
      // WritableKeyPaths to be formed in the same conditions we did
      // in previous releases even if we should not be able to set
      // the value in this context.
      if (!storage->isSettable(DC)) {
        // A non-settable component makes the key path read-only, unless
        // a reference-writable component shows up later.
        return true;
      }
    } else if (!storage->isSettable(nullptr) ||
               !storage->isSetterAccessibleFrom(DC)) {
      // A non-settable component makes the key path read-only, unless
      // a reference-writable component shows up later.
      return true;
    }

    return false;
  }

public:
  // Given a type variable, attempt to find the disjunction of
  // bind overloads associated with it. This may return null in cases where
  // the disjunction has either not been created or binds the type variable
  // in some manner other than by binding overloads.
  ///
  /// \param numOptionalUnwraps If non-null, this will receive the number
  /// of "optional object of" constraints that this function looked through
  /// to uncover the disjunction. The actual overloads will have this number
  /// of optionals wrapping the type.
  Constraint *getUnboundBindOverloadDisjunction(
    TypeVariableType *tyvar,
    unsigned *numOptionalUnwraps = nullptr);

private:
  /// Solve the system of constraints after it has already been
  /// simplified.
  ///
  /// \param solutions The set of solutions to this system of constraints.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool solveSimplified(SmallVectorImpl<Solution> &solutions);

  /// Find reduced domains of disjunction constraints for given
  /// expression, this is achieved to solving individual sub-expressions
  /// and combining resolving types. Such algorithm is called directional
  /// path consistency because it goes from children to parents for all
  /// related sub-expressions taking union of their domains.
  ///
  /// \param expr The expression to find reductions for.
  void shrink(Expr *expr);

  /// Pick a disjunction from the InactiveConstraints list.
  ///
  /// \returns The selected disjunction.
  Constraint *selectDisjunction();

  Constraint *selectApplyDisjunction();

  /// Solve the system of constraints generated from provided expression.
  ///
  /// \param expr The expression to generate constraints from.
  /// \param convertType The expected type of the expression.
  /// \param listener The callback to check solving progress.
  /// \param solutions The set of solutions to the system of constraints.
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  ///
  /// \returns Error is an error occurred, Solved is system is consistent
  /// and solutions were found, Unsolved otherwise.
  SolutionKind solveImpl(Expr *&expr,
                         Type convertType,
                         ExprTypeCheckListener *listener,
                         SmallVectorImpl<Solution> &solutions,
                         FreeTypeVariableBinding allowFreeTypeVariables
                          = FreeTypeVariableBinding::Disallow);

public:
  /// Pre-check the expression, validating any types that occur in the
  /// expression and folding sequence expressions.
  static bool preCheckExpression(Expr *&expr, DeclContext *dc,
                                 ConstraintSystem *baseCS = nullptr);
        
  /// Solve the system of constraints generated from provided expression.
  ///
  /// The expression should have already been pre-checked with
  /// preCheckExpression().
  ///
  /// \param expr The expression to generate constraints from.
  /// \param convertType The expected type of the expression.
  /// \param listener The callback to check solving progress.
  /// \param solutions The set of solutions to the system of constraints.
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  ///
  /// \returns true is an error occurred, false is system is consistent
  /// and solutions were found.
  bool solve(Expr *&expr,
             Type convertType,
             ExprTypeCheckListener *listener,
             SmallVectorImpl<Solution> &solutions,
             FreeTypeVariableBinding allowFreeTypeVariables
             = FreeTypeVariableBinding::Disallow);

  /// Solve the system of constraints.
  ///
  /// \param solutions The set of solutions to this system of constraints.
  ///
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  ///
  /// \returns true if an error occurred, false otherwise.  Note that multiple
  /// ambiguous solutions for the same constraint system are considered to be
  /// success by this API.
  bool solve(SmallVectorImpl<Solution> &solutions,
             FreeTypeVariableBinding allowFreeTypeVariables =
                 FreeTypeVariableBinding::Disallow);

  /// Solve the system of constraints.
  ///
  /// \param allowFreeTypeVariables How to bind free type variables in
  /// the solution.
  ///
  /// \param allowFixes Whether to allow fixes in the solution.
  ///
  /// \returns a solution if a single unambiguous one could be found, or None if
  /// ambiguous or unsolvable.
  Optional<Solution> solveSingle(FreeTypeVariableBinding allowFreeTypeVariables
                                 = FreeTypeVariableBinding::Disallow,
                                 bool allowFixes = false);

private:
  /// Solve the system of constraints.
  ///
  /// This method responsible for running search/solver algorithm.
  /// It doesn't filter solutions, that's the job of top-level `solve` methods.
  ///
  /// \param solutions The set of solutions to this system of constraints.
  void solveImpl(SmallVectorImpl<Solution> &solutions);

  /// Compare two solutions to the same set of constraints.
  ///
  /// \param cs The constraint system.
  /// \param solutions All of the solutions to the system.
  /// \param diff The differences among the solutions.
  /// \param idx1 The index of the first solution.
  /// \param idx2 The index of the second solution.
  static SolutionCompareResult
  compareSolutions(ConstraintSystem &cs, ArrayRef<Solution> solutions,
                   const SolutionDiff &diff, unsigned idx1, unsigned idx2);

public:
  /// Increase the score of the given kind for the current (partial) solution
  /// along the.
  void increaseScore(ScoreKind kind, unsigned value = 1);

  /// Determine whether this solution is guaranteed to be worse than the best
  /// solution found so far.
  bool worseThanBestSolution() const;

  /// Given a set of viable solutions, find the best
  /// solution.
  ///
  /// \param solutions The set of viable solutions to consider.
  ///
  /// \param minimize If true, then in the case where there is no single
  /// best solution, minimize the set of solutions by removing any solutions
  /// that are identical to or worse than some other solution. This operation
  /// is quadratic.
  ///
  /// \returns The index of the best solution, or nothing if there was no
  /// best solution.
  Optional<unsigned>
  findBestSolution(SmallVectorImpl<Solution> &solutions,
                   bool minimize);

  /// Apply a given solution to the expression, producing a fully
  /// type-checked expression.
  ///
  /// \param convertType the contextual type to which the
  /// expression should be converted, if any.
  /// \param discardedExpr if true, the result of the expression
  /// is contextually ignored.
  /// \param skipClosures if true, don't descend into bodies of
  /// non-single expression closures.
  Expr *applySolution(Solution &solution, Expr *expr,
                      Type convertType, bool discardedExpr,
                      bool skipClosures);

  /// Reorder the disjunctive clauses for a given expression to
  /// increase the likelihood that a favored constraint will be successfully
  /// resolved before any others.
  void optimizeConstraints(Expr *e);
  
  /// Determine if we've already explored too many paths in an
  /// attempt to solve this expression.
  bool isExpressionAlreadyTooComplex = false;
  bool getExpressionTooComplex(SmallVectorImpl<Solution> const &solutions) {
    if (isExpressionAlreadyTooComplex)
      return true;

    auto used = getASTContext().getSolverMemory();
    for (auto const& s : solutions) {
      used += s.getTotalMemory();
    }
    MaxMemory = std::max(used, MaxMemory);
    auto threshold = getASTContext().TypeCheckerOpts.SolverMemoryThreshold;
    if (MaxMemory > threshold) {
      return isExpressionAlreadyTooComplex= true;
    }

    const auto timeoutThresholdInMillis =
        getASTContext().TypeCheckerOpts.ExpressionTimeoutThreshold;
    if (Timer && Timer->isExpired(timeoutThresholdInMillis)) {
      // Disable warnings about expressions that go over the warning
      // threshold since we're arbitrarily ending evaluation and
      // emitting an error.
      Timer->disableWarning();

      return isExpressionAlreadyTooComplex = true;
    }

    // Bail out once we've looked at a really large number of
    // choices.
    if (CountScopes > getASTContext().TypeCheckerOpts.SolverBindingThreshold) {
      return isExpressionAlreadyTooComplex = true;
    }

    return false;
  }

  // Utility class that can collect information about the type of an
  // argument in an apply.
  //
  // For example, when given a type variable type that represents the
  // argument of a function call, it will walk the constraint graph
  // finding any concrete types that are reachable through various
  // subtype constraints and will also collect all the literal types
  // conformed to by the types it finds on the walk.
  //
  // This makes it possible to get an idea of the kinds of literals
  // and types of arguments that are used in the subexpression rooted
  // in this argument, which we can then use to make better choices
  // for how we partition the operators in a disjunction (in order to
  // avoid visiting all the options).
  class ArgumentInfoCollector {
    ConstraintSystem &CS;
    llvm::SetVector<Type> Types;
    llvm::SetVector<ProtocolDecl *> LiteralProtocols;

    void addType(Type ty) {
      assert(!ty->is<TypeVariableType>());
      Types.insert(ty);
    }

    void addLiteralProtocol(ProtocolDecl *proto) {
      LiteralProtocols.insert(proto);
    }

    void walk(Type argType);
    void minimizeLiteralProtocols();

  public:
    ArgumentInfoCollector(ConstraintSystem &cs, FunctionType *fnTy) : CS(cs) {
      for (auto &param : fnTy->getParams())
        walk(param.getPlainType());

      minimizeLiteralProtocols();
    }

    ArgumentInfoCollector(ConstraintSystem &cs, AnyFunctionType::Param param)
        : CS(cs) {
      walk(param.getPlainType());
      minimizeLiteralProtocols();
    }

    const llvm::SetVector<Type> &getTypes() const { return Types; }
    const llvm::SetVector<ProtocolDecl *> &getLiteralProtocols() const {
      return LiteralProtocols;
    }

    SWIFT_DEBUG_DUMP;
  };

  bool haveTypeInformationForAllArguments(FunctionType *fnType);

  typedef std::function<bool(unsigned index, Constraint *)> ConstraintMatcher;
  typedef std::function<void(ArrayRef<Constraint *>, ConstraintMatcher)>
      ConstraintMatchLoop;
  typedef std::function<void(SmallVectorImpl<unsigned> &options)>
      PartitionAppendCallback;

  // Attempt to sort nominalTypes based on what we can discover about
  // calls into the overloads in the disjunction that bindOverload is
  // a part of.
  void sortDesignatedTypes(SmallVectorImpl<NominalTypeDecl *> &nominalTypes,
                           Constraint *bindOverload);

  // Partition the choices in a disjunction based on those that match
  // the designated types for the operator that the disjunction was
  // formed for.
  void partitionForDesignatedTypes(ArrayRef<Constraint *> Choices,
                                   ConstraintMatchLoop forEachChoice,
                                   PartitionAppendCallback appendPartition);

  // Partition the choices in the disjunction into groups that we will
  // iterate over in an order appropriate to attempt to stop before we
  // have to visit all of the options.
  void partitionDisjunction(ArrayRef<Constraint *> Choices,
                            SmallVectorImpl<unsigned> &Ordering,
                            SmallVectorImpl<unsigned> &PartitionBeginning);

private:
  /// The set of expressions currently being analyzed for failures.
  llvm::DenseMap<Expr*, Expr*> DiagnosedExprs;

public:
  void addExprForDiagnosis(Expr *E1, Expr *Result) {
    DiagnosedExprs[E1] = Result;
  }
  bool isExprBeingDiagnosed(Expr *E) {
    if (DiagnosedExprs.count(E)) {
      return true;
    }
    
    if (baseCS && baseCS != this) {
      return baseCS->isExprBeingDiagnosed(E);
    }
    return false;
  }
  Expr *getExprBeingDiagnosed(Expr *E) {
    if (auto *expr = DiagnosedExprs[E]) {
      return expr;
    }
    
    if (baseCS && baseCS != this) {
      return baseCS->getExprBeingDiagnosed(E);
    }
    return nullptr;
  }
        
public:
  SWIFT_DEBUG_DUMP;
  SWIFT_DEBUG_DUMPER(dump(Expr *));

  void print(raw_ostream &out) const;
  void print(raw_ostream &out, Expr *) const;
};

/// Compute the shuffle required to map from a given tuple type to
/// another tuple type.
///
/// \param fromTuple The tuple type we're converting from, as represented by its
/// TupleTypeElt members.
///
/// \param toTuple The tuple type we're converting to, as represented by its
/// TupleTypeElt members.
///
/// \param sources Will be populated with information about the source of each
/// of the elements for the result tuple. The indices into this array are the
/// indices of the tuple type we're converting to, while the values are
/// an index into the source tuple.
///
/// \returns true if no tuple conversion is possible, false otherwise.
bool computeTupleShuffle(ArrayRef<TupleTypeElt> fromTuple,
                         ArrayRef<TupleTypeElt> toTuple,
                         SmallVectorImpl<unsigned> &sources);
static inline bool computeTupleShuffle(TupleType *fromTuple,
                                       TupleType *toTuple,
                                       SmallVectorImpl<unsigned> &sources){
  return computeTupleShuffle(fromTuple->getElements(), toTuple->getElements(),
                             sources);
}

/// Describes the arguments to which a parameter binds.
/// FIXME: This is an awful data structure. We want the equivalent of a
/// TinyPtrVector for unsigned values.
using ParamBinding = SmallVector<unsigned, 1>;

/// Class used as the base for listeners to the \c matchCallArguments process.
///
/// By default, none of the callbacks do anything.
class MatchCallArgumentListener {
public:
  virtual ~MatchCallArgumentListener();

  /// Indicates that the argument at the given index does not match any
  /// parameter.
  ///
  /// \param argIdx The index of the extra argument.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool extraArgument(unsigned argIdx);

  /// Indicates that no argument was provided for the parameter at the given
  /// indices.
  ///
  /// \param paramIdx The index of the parameter that is missing an argument.
  virtual Optional<unsigned> missingArgument(unsigned paramIdx);

  /// Indicate that there was no label given when one was expected by parameter.
  ///
  /// \param paramIndex The index of the parameter that is missing a label.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool missingLabel(unsigned paramIndex);

  /// Indicate that there was label given when none was expected by parameter.
  ///
  /// \param paramIndex The index of the parameter that wasn't expecting a label.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool extraneousLabel(unsigned paramIndex);

  /// Indicate that there was a label given with a typo(s) in it.
  ///
  /// \param paramIndex The index of the parameter with misspelled label.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool incorrectLabel(unsigned paramIndex);

  /// Indicates that an argument is out-of-order with respect to a previously-
  /// seen argument.
  ///
  /// \param argIdx The argument that came too late in the argument list.
  /// \param prevArgIdx The argument that the \c argIdx should have preceded.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool outOfOrderArgument(unsigned argIdx, unsigned prevArgIdx);

  /// Indicates that the arguments need to be relabeled to match the parameters.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool relabelArguments(ArrayRef<Identifier> newNames);

  /// Indicates that the trailing closure argument at the given \c argIdx
  /// cannot be passed to the last parameter at \c paramIdx.
  ///
  /// \returns true to indicate that this should cause a failure, false
  /// otherwise.
  virtual bool trailingClosureMismatch(unsigned paramIdx, unsigned argIdx);
};

/// Match the call arguments (as described by the given argument type) to
/// the parameters (as described by the given parameter type).
///
/// \param args The arguments.
/// \param params The parameters.
/// \param paramInfo Declaration-level information about the parameters.
/// \param hasTrailingClosure Whether the last argument is a trailing closure.
/// \param allowFixes Whether to allow fixes when matching arguments.
///
/// \param listener Listener that will be notified when certain problems occur,
/// e.g., to produce a diagnostic.
///
/// \param parameterBindings Will be populated with the arguments that are
/// bound to each of the parameters.
/// \returns true if the call arguments could not be matched to the parameters.
bool matchCallArguments(SmallVectorImpl<AnyFunctionType::Param> &args,
                        ArrayRef<AnyFunctionType::Param> params,
                        const ParameterListInfo &paramInfo,
                        bool hasTrailingClosure,
                        bool allowFixes,
                        MatchCallArgumentListener &listener,
                        SmallVectorImpl<ParamBinding> &parameterBindings);

ConstraintSystem::TypeMatchResult
matchCallArguments(ConstraintSystem &cs,
                   FunctionType *contextualType,
                   ArrayRef<AnyFunctionType::Param> args,
                   ArrayRef<AnyFunctionType::Param> params,
                   ConstraintKind subKind,
                   ConstraintLocatorBuilder locator);

/// Given an expression that is the target of argument labels (for a call,
/// subscript, etc.), find the underlying target expression.
Expr *getArgumentLabelTargetExpr(Expr *fn);

/// Returns true if a reference to a member on a given base type will apply
/// its curried self parameter, assuming it has one.
///
/// This is true for most member references, however isn't true for things
/// like an instance member being referenced on a metatype, where the
/// curried self parameter remains unapplied.
bool doesMemberRefApplyCurriedSelf(Type baseTy, const ValueDecl *decl);

/// Simplify the given locator by zeroing in on the most specific
/// subexpression described by the locator.
///
/// This routine can also find the corresponding "target" locator, which
/// typically provides the other end of a relational constraint. For example,
/// if the primary locator refers to a function argument, the target locator
/// will be set to refer to the corresponding function parameter.
///
/// \param cs The constraint system in which the locator will be simplified.
///
/// \param locator The locator to simplify.
///
/// \param range Will be populated with an "interesting" range.
///
/// \return the simplified locator.
ConstraintLocator *simplifyLocator(ConstraintSystem &cs,
                                   ConstraintLocator *locator,
                                   SourceRange &range);

void simplifyLocator(Expr *&anchor,
                     ArrayRef<LocatorPathElt> &path,
                     SourceRange &range);

/// Simplify the given locator down to a specific anchor expression,
/// if possible.
///
/// \returns the anchor expression if it fully describes the locator, or
/// null otherwise.
Expr *simplifyLocatorToAnchor(ConstraintLocator *locator);

/// Retrieve argument at specified index from given expression.
/// The expression could be "application", "subscript" or "member" call.
///
/// \returns argument expression or `nullptr` if given "base" expression
/// wasn't of one of the kinds listed above.
Expr *getArgumentExpr(Expr *expr, unsigned index);

/// Determine whether given locator points to one of the arguments
/// associated with the call to an operator. If the operator name
/// is empty `true` is returned for any kind of operator.
bool isOperatorArgument(ConstraintLocator *locator,
                        StringRef expectedOperator = "");

/// Determine whether given locator points to one of the arguments
/// associated with implicit `~=` (pattern-matching) operator
bool isArgumentOfPatternMatchingOperator(ConstraintLocator *locator);

/// Determine whether given locator points to one of the arguments
/// associated with `===` and `!==` operators.
bool isArgumentOfReferenceEqualityOperator(ConstraintLocator *locator);

/// Determine whether given expression is a reference to a
/// pattern-matching operator `~=`
bool isPatternMatchingOperator(Expr *expr);

/// If given expression references operator overlaod(s)
/// extract and produce name of the operator.
Optional<Identifier> getOperatorName(Expr *expr);

// Check whether argument of the call at given position refers to
// parameter marked as `@autoclosure`. This function is used to
// maintain source compatibility with Swift versions < 5,
// previously examples like following used to type-check:
//
// func foo(_ x: @autoclosure () -> Int) {}
// func bar(_ y: @autoclosure () -> Int) {
//   foo(y)
// }
bool isAutoClosureArgument(Expr *argExpr);

/// Checks whether referencing the given overload choice results in the self
/// parameter being applied, meaning that it's dropped from the type of the
/// reference.
bool hasAppliedSelf(ConstraintSystem &cs, const OverloadChoice &choice);

/// Check whether type conforms to a given known protocol.
bool conformsToKnownProtocol(ConstraintSystem &cs, Type type,
                             KnownProtocolKind protocol);

/// Check whether given type conforms to `RawPepresentable` protocol
/// and return witness type.
Type isRawRepresentable(ConstraintSystem &cs, Type type);
/// Check whether given type conforms to a specific known kind
/// `RawPepresentable` protocol and return witness type.
Type isRawRepresentable(ConstraintSystem &cs, Type type,
                        KnownProtocolKind rawRepresentableProtocol);

class DisjunctionChoice {
  unsigned Index;
  Constraint *Choice;
  bool ExplicitConversion;
  bool IsBeginningOfPartition;

public:
  DisjunctionChoice(unsigned index, Constraint *choice, bool explicitConversion,
                    bool isBeginningOfPartition)
      : Index(index), Choice(choice), ExplicitConversion(explicitConversion),
        IsBeginningOfPartition(isBeginningOfPartition) {}

  unsigned getIndex() const { return Index; }

  bool attempt(ConstraintSystem &cs) const;

  bool isDisabled() const { return Choice->isDisabled(); }

  bool hasFix() const {
    return bool(Choice->getFix());
  }

  bool isUnavailable() const {
    if (auto *decl = getDecl(Choice))
      return decl->getAttrs().isUnavailable(decl->getASTContext());
    return false;
  }

  bool isBeginningOfPartition() const { return IsBeginningOfPartition; }

  // FIXME: Both of the accessors below are required to support
  //        performance optimization hacks in constraint solver.

  bool isGenericOperator() const;
  bool isSymmetricOperator() const;

  void print(llvm::raw_ostream &Out, SourceManager *SM) const {
    Out << "disjunction choice ";
    Choice->print(Out, SM);
  }

  operator Constraint *() { return Choice; }
  operator Constraint *() const { return Choice; }

private:
  /// If associated disjunction is an explicit conversion,
  /// let's try to propagate its type early to prune search space.
  void propagateConversionInfo(ConstraintSystem &cs) const;

  static ValueDecl *getOperatorDecl(Constraint *choice) {
    auto *decl = getDecl(choice);
    if (!decl)
      return nullptr;

    return decl->isOperator() ? decl : nullptr;
  }

  static ValueDecl *getDecl(Constraint *constraint) {
    if (constraint->getKind() != ConstraintKind::BindOverload)
      return nullptr;

    auto choice = constraint->getOverloadChoice();
    if (choice.getKind() != OverloadChoiceKind::Decl)
      return nullptr;

    return choice.getDecl();
  }
};

class TypeVariableBinding {
  TypeVariableType *TypeVar;
  ConstraintSystem::PotentialBinding Binding;

public:
  TypeVariableBinding(TypeVariableType *typeVar,
                      ConstraintSystem::PotentialBinding &binding)
      : TypeVar(typeVar), Binding(binding) {}

  bool isDefaultable() const { return Binding.isDefaultableBinding(); }

  bool hasDefaultedProtocol() const { return Binding.DefaultedProtocol; }

  bool attempt(ConstraintSystem &cs) const;

  void print(llvm::raw_ostream &Out, SourceManager *) const {
    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    Out << "type variable " << TypeVar->getString(PO)
        << " := " << Binding.BindingType->getString(PO);
  }
};

template<typename Choice>
class BindingProducer {
  ConstraintLocator *Locator;

protected:
  ConstraintSystem &CS;

public:
  BindingProducer(ConstraintSystem &cs, ConstraintLocator *locator)
      : Locator(locator), CS(cs) {}

  virtual ~BindingProducer() {}
  virtual Optional<Choice> operator()() = 0;

  ConstraintLocator *getLocator() const { return Locator; }

  /// Check whether generator would have to compute next
  /// batch of bindings because it freshly ran out of current one.
  /// This is useful to be able to exhaustively attempt bindings
  /// for type variables found at one level, before proceeding to
  /// supertypes or literal defaults etc.
  virtual bool needsToComputeNext() const { return false; }
};

class TypeVarBindingProducer : public BindingProducer<TypeVariableBinding> {
  using BindingKind = ConstraintSystem::AllowedBindingKind;
  using Binding = ConstraintSystem::PotentialBinding;

  TypeVariableType *TypeVar;
  llvm::SmallVector<Binding, 8> Bindings;

  // The index pointing to the offset in the bindings
  // generator is currently at, `numTries` represents
  // the number of times bindings have been recomputed.
  unsigned Index = 0, NumTries = 0;

  llvm::SmallPtrSet<CanType, 4> ExploredTypes;
  llvm::SmallPtrSet<TypeBase *, 4> BoundTypes;

public:
  using Element = TypeVariableBinding;

  TypeVarBindingProducer(ConstraintSystem &cs,
                         ConstraintSystem::PotentialBindings &bindings)
      : BindingProducer(cs, bindings.TypeVar->getImpl().getLocator()),
        TypeVar(bindings.TypeVar),
        Bindings(bindings.Bindings.begin(), bindings.Bindings.end()) {}

  Optional<Element> operator()() override {
    // Once we reach the end of the current bindings
    // let's try to compute new ones, e.g. supertypes,
    // literal defaults, if that fails, we are done.
    if (needsToComputeNext() && !computeNext())
      return None;

    return TypeVariableBinding(TypeVar, Bindings[Index++]);
  }

  bool needsToComputeNext() const override { return Index >= Bindings.size(); }

private:
  /// Compute next batch of bindings if possible, this could
  /// be supertypes extracted from one of the current bindings
  /// or default literal types etc.
  ///
  /// \returns true if some new bindings were sucessfully computed,
  /// false otherwise.
  bool computeNext();
};

/// Iterator over disjunction choices, makes it
/// easy to work with disjunction and encapsulates
/// some other important information such as locator.
class DisjunctionChoiceProducer : public BindingProducer<DisjunctionChoice> {
  // The disjunction choices that this producer will iterate through.
  ArrayRef<Constraint *> Choices;

  // The ordering of disjunction choices. We index into Choices
  // through this vector in order to visit the disjunction choices in
  // the order we want to visit them.
  SmallVector<unsigned, 8> Ordering;

  // The index of the first element in a partition of the disjunction
  // choices. The choices are split into partitions where we will
  // visit all elements within a single partition before moving to the
  // elements of the next partition. If we visit all choices within a
  // single partition and have found a successful solution with one of
  // the choices in that partition, we stop looking for other
  // solutions.
  SmallVector<unsigned, 4> PartitionBeginning;

  // The index in the current partition of disjunction choices that we
  // are iterating over.
  unsigned PartitionIndex = 0;

  bool IsExplicitConversion;

  unsigned Index = 0;

public:
  using Element = DisjunctionChoice;

  DisjunctionChoiceProducer(ConstraintSystem &cs, Constraint *disjunction)
      : BindingProducer(cs, disjunction->shouldRememberChoice()
                                ? disjunction->getLocator()
                                : nullptr),
        Choices(disjunction->getNestedConstraints()),
        IsExplicitConversion(disjunction->isExplicitConversion()) {
    assert(disjunction->getKind() == ConstraintKind::Disjunction);
    assert(!disjunction->shouldRememberChoice() || disjunction->getLocator());

    // Order and partition the disjunction choices.
    CS.partitionDisjunction(Choices, Ordering, PartitionBeginning);
  }

  DisjunctionChoiceProducer(ConstraintSystem &cs,
                            ArrayRef<Constraint *> choices,
                            ConstraintLocator *locator, bool explicitConversion)
      : BindingProducer(cs, locator), Choices(choices),
        IsExplicitConversion(explicitConversion) {

    // Order and partition the disjunction choices.
    CS.partitionDisjunction(Choices, Ordering, PartitionBeginning);
  }

  Optional<Element> operator()() override {
    unsigned currIndex = Index;
    if (currIndex >= Choices.size())
      return None;

    bool isBeginningOfPartition = PartitionIndex < PartitionBeginning.size() &&
                                  PartitionBeginning[PartitionIndex] == Index;
    if (isBeginningOfPartition)
      ++PartitionIndex;

    ++Index;

    return DisjunctionChoice(currIndex, Choices[Ordering[currIndex]],
                             IsExplicitConversion, isBeginningOfPartition);
  }
};

/// Determine whether given type is a known one
/// for a key path `{Writable, ReferenceWritable}KeyPath`.
bool isKnownKeyPathType(Type type);

/// Determine whether given declaration is one for a key path
/// `{Writable, ReferenceWritable}KeyPath`.
bool isKnownKeyPathDecl(ASTContext &ctx, ValueDecl *decl);
} // end namespace constraints

template<typename ...Args>
TypeVariableType *TypeVariableType::getNew(const ASTContext &C, unsigned ID,
                                           Args &&...args) {
  // Allocate memory
  void *mem = C.Allocate(sizeof(TypeVariableType) + sizeof(Implementation),
                         alignof(TypeVariableType),
                         AllocationArena::ConstraintSolver);

  // Construct the type variable.
  auto *result = ::new (mem) TypeVariableType(C, ID);

  // Construct the implementation object.
  new (result+1) TypeVariableType::Implementation(std::forward<Args>(args)...);

  return result;
}

/// If the expression has the effect of a forced downcast, find the
/// underlying forced downcast expression.
ForcedCheckedCastExpr *findForcedDowncast(ASTContext &ctx, Expr *expr);


// Erases any opened existentials from the given expression.
// Note: this may update the provided expr pointer.
void eraseOpenedExistentials(constraints::ConstraintSystem &CS, Expr *&expr);

// Count the number of overload sets present
// in the expression and all of the children.
class OverloadSetCounter : public ASTWalker {
  unsigned &NumOverloads;

public:
  OverloadSetCounter(unsigned &overloads)
  : NumOverloads(overloads)
  {}

  std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
    if (auto applyExpr = dyn_cast<ApplyExpr>(expr)) {
      // If we've found function application and it's
      // function is an overload set, count it.
      if (isa<OverloadSetRefExpr>(applyExpr->getFn()))
        ++NumOverloads;
    }

    // Always recur into the children.
    return { true, expr };
  }
};

/// Matches array of function parameters to candidate inputs,
/// which can be anything suitable (e.g., parameters, arguments).
///
/// It claims inputs sequentially and tries to pair between an input
/// and the next appropriate parameter. The detailed matching behavior
/// of each pair is specified by a custom function (i.e., pairMatcher).
/// It considers variadic and defaulted arguments when forming proper
/// input-parameter pairs; however, other information like label and
/// type information is not directly used here. It can be implemented
/// in the custom function when necessary.
class InputMatcher {
  size_t NumSkippedParameters;
  const ParameterListInfo &ParamInfo;
  const ArrayRef<AnyFunctionType::Param> Params;

public:
  enum Result {
    /// The specified inputs are successfully matched.
    IM_Succeeded,
    /// There are input(s) left unclaimed while all parameters are matched.
    IM_HasUnclaimedInput,
    /// There are parateter(s) left unmatched while all inputs are claimed.
    IM_HasUnmatchedParam,
    /// Custom pair matcher function failure.
    IM_CustomPairMatcherFailed,
  };

  InputMatcher(const ArrayRef<AnyFunctionType::Param> params,
               const ParameterListInfo &paramInfo);

  /// Matching a given array of inputs.
  ///
  /// \param numInputs The number of inputs.
  /// \param pairMatcher Custom matching behavior of an input-parameter pair.
  /// \return the matching result.
  Result
  match(int numInputs,
        std::function<bool(unsigned inputIdx, unsigned paramIdx)> pairMatcher);

  size_t getNumSkippedParameters() const { return NumSkippedParameters; }
};

// Return true if, when replacing "<expr>" with "<expr> ?? T", parentheses need
// to be added around <expr> first in order to maintain the correct precedence.
bool exprNeedsParensBeforeAddingNilCoalescing(DeclContext *DC,
                                              Expr *expr);

// Return true if, when replacing "<expr>" with "<expr> as T", parentheses need
// to be added around the new expression in order to maintain the correct
// precedence.
bool exprNeedsParensAfterAddingNilCoalescing(DeclContext *DC,
                                             Expr *expr,
                                             Expr *rootExpr);

/// Return true if, when replacing "<expr>" with "<expr> op <something>",
/// parentheses must be added around "<expr>" to allow the new operator
/// to bind correctly.
bool exprNeedsParensInsideFollowingOperator(DeclContext *DC,
                                            Expr *expr,
                                            PrecedenceGroupDecl *followingPG);

/// Return true if, when replacing "<expr>" with "<expr> op <something>"
/// within the given root expression, parentheses must be added around
/// the new operator to prevent it from binding incorrectly in the
/// surrounding context.
bool exprNeedsParensOutsideFollowingOperator(
    DeclContext *DC, Expr *expr, Expr *rootExpr,
    PrecedenceGroupDecl *followingPG);

/// Determine whether this is a SIMD operator.
bool isSIMDOperator(ValueDecl *value);

} // end namespace swift

#endif // LLVM_SWIFT_SEMA_CONSTRAINT_SYSTEM_H
