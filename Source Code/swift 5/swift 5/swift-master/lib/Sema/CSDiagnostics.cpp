//===--- CSDiagnostics.cpp - Constraint Diagnostics -----------------------===//
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
// This file implements diagnostics for constraint system.
//
//===----------------------------------------------------------------------===//

#include "CSDiagnostics.h"
#include "ConstraintSystem.h"
#include "MiscDiagnostics.h"
#include "TypoCorrection.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include <string>

using namespace swift;
using namespace constraints;

FailureDiagnostic::~FailureDiagnostic() {}

bool FailureDiagnostic::diagnose(bool asNote) {
  return asNote ? diagnoseAsNote() : diagnoseAsError();
}

bool FailureDiagnostic::diagnoseAsNote() {
  return false;
}

std::pair<Expr *, bool> FailureDiagnostic::computeAnchor() const {
  auto &cs = getConstraintSystem();

  auto *locator = getLocator();
  // Resolve the locator to a specific expression.
  SourceRange range;
  bool isSubscriptMember =
      (!locator->getPath().empty() && locator->getPath().back().getKind() ==
                                          ConstraintLocator::SubscriptMember);

  ConstraintLocator *resolved = simplifyLocator(cs, locator, range);
  if (!resolved || !resolved->getAnchor())
    return {locator->getAnchor(), true};

  Expr *anchor = resolved->getAnchor();
  // FIXME: Work around an odd locator representation that doesn't separate the
  // base of a subscript member from the member access.
  if (isSubscriptMember) {
    if (auto subscript = dyn_cast<SubscriptExpr>(anchor))
      anchor = subscript->getBase();
  }

  return {anchor, !resolved->getPath().empty()};
}

Type FailureDiagnostic::getType(Expr *expr, bool wantRValue) const {
  return resolveType(CS.getType(expr), /*reconstituteSugar=*/false,
                     wantRValue);
}

Type FailureDiagnostic::getType(const TypeLoc &loc, bool wantRValue) const {
  return resolveType(CS.getType(loc), /*reconstituteSugar=*/false,
                     wantRValue);
}

template <typename... ArgTypes>
InFlightDiagnostic
FailureDiagnostic::emitDiagnostic(ArgTypes &&... Args) const {
  auto &cs = getConstraintSystem();
  return cs.getASTContext().Diags.diagnose(std::forward<ArgTypes>(Args)...);
}

Expr *FailureDiagnostic::findParentExpr(Expr *subExpr) const {
  return CS.getParentExpr(subExpr);
}

Expr *
FailureDiagnostic::getArgumentListExprFor(ConstraintLocator *locator) const {
  auto path = locator->getPath();
  auto iter = path.begin();
  if (!locator->findFirst<LocatorPathElt::ApplyArgument>(iter))
    return nullptr;

  // Form a new locator that ends at the ApplyArgument element, then simplify
  // to get the argument list.
  auto newPath = ArrayRef<LocatorPathElt>(path.begin(), iter + 1);
  auto &cs = getConstraintSystem();
  auto argListLoc = cs.getConstraintLocator(locator->getAnchor(), newPath);
  return simplifyLocatorToAnchor(argListLoc);
}

Expr *FailureDiagnostic::getBaseExprFor(Expr *anchor) const {
  if (!anchor)
    return nullptr;

  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor))
    return UDE->getBase();
  else if (auto *SE = dyn_cast<SubscriptExpr>(anchor))
    return SE->getBase();
  else if (auto *MRE = dyn_cast<MemberRefExpr>(anchor))
    return MRE->getBase();

  return nullptr;
}

Optional<SelectedOverload>
FailureDiagnostic::getChoiceFor(ConstraintLocator *locator) const {
  auto &cs = getConstraintSystem();
  return getOverloadChoiceIfAvailable(cs.getCalleeLocator(locator));
}

Type FailureDiagnostic::resolveInterfaceType(Type type,
                                             bool reconstituteSugar) const {
  auto &cs = getConstraintSystem();
  auto resolvedType = type.transform([&](Type type) -> Type {
    if (auto *tvt = type->getAs<TypeVariableType>()) {
      // If this type variable is for a generic parameter, return that.
      if (auto *gp = tvt->getImpl().getGenericParameter())
        return gp;

      // Otherwise resolve its fixed type, mapped out of context.
      if (auto fixed = cs.getFixedType(tvt))
        return resolveInterfaceType(fixed->mapTypeOutOfContext());

      return cs.getRepresentative(tvt);
    }
    if (auto *dmt = type->getAs<DependentMemberType>()) {
      // For a dependent member, first resolve the base.
      auto newBase = resolveInterfaceType(dmt->getBase());

      // Then reconstruct using its associated type.
      assert(dmt->getAssocType());
      return DependentMemberType::get(newBase, dmt->getAssocType());
    }
    return type;
  });

  assert(!resolvedType->hasArchetype());
  return reconstituteSugar ? resolvedType->reconstituteSugar(/*recursive*/ true)
                           : resolvedType;
}

/// Given an apply expr, returns true if it is expected to have a direct callee
/// overload, resolvable using `getChoiceFor`. Otherwise, returns false.
static bool shouldHaveDirectCalleeOverload(const CallExpr *callExpr) {
  auto *fnExpr = callExpr->getDirectCallee();

  // An apply of an apply/subscript doesn't have a direct callee.
  if (isa<ApplyExpr>(fnExpr) || isa<SubscriptExpr>(fnExpr))
    return false;

  // Applies of closures don't have callee overloads.
  if (isa<ClosureExpr>(fnExpr))
    return false;

  // No direct callee for a try!/try?.
  if (isa<ForceTryExpr>(fnExpr) || isa<OptionalTryExpr>(fnExpr))
    return false;

  // If we have an intermediate cast, there's no direct callee.
  if (isa<ExplicitCastExpr>(fnExpr))
    return false;

  // No direct callee for an if expr.
  if (isa<IfExpr>(fnExpr))
    return false;

  // Assume that anything else would have a direct callee.
  return true;
}

Optional<FunctionArgApplyInfo>
FailureDiagnostic::getFunctionArgApplyInfo(ConstraintLocator *locator) const {
  auto &cs = getConstraintSystem();
  auto *anchor = locator->getAnchor();
  auto path = locator->getPath();

  // Look for the apply-arg-to-param element in the locator's path. We may
  // have to look through other elements that are generated from an argument
  // conversion such as GenericArgument for an optional-to-optional conversion,
  // and OptionalPayload for a value-to-optional conversion.
  auto iter = path.rbegin();
  auto applyArgElt = locator->findLast<LocatorPathElt::ApplyArgToParam>(iter);
  if (!applyArgElt)
    return None;

  auto nextIter = iter + 1;
  assert(!locator->findLast<LocatorPathElt::ApplyArgToParam>(nextIter) &&
         "Multiple ApplyArgToParam components?");

  // Form a new locator that ends at the apply-arg-to-param element, and
  // simplify it to get the full argument expression.
  auto argPath = path.drop_back(iter - path.rbegin());
  auto *argLocator = cs.getConstraintLocator(
      anchor, argPath, ConstraintLocator::getSummaryFlagsForPath(argPath));

  auto *argExpr = simplifyLocatorToAnchor(argLocator);

  // If we were unable to simplify down to the argument expression, we don't
  // know what this is.
  if (!argExpr)
    return None;

  Optional<OverloadChoice> choice;
  Type rawFnType;
  if (auto overload = getChoiceFor(argLocator)) {
    // If we have resolved an overload for the callee, then use that to get the
    // function type and callee.
    choice = overload->choice;
    rawFnType = overload->openedType;
  } else {
    // If we didn't resolve an overload for the callee, we should be dealing
    // with a call of an arbitrary function expr.
    if (auto *call = dyn_cast<CallExpr>(anchor)) {
      assert(!shouldHaveDirectCalleeOverload(call) &&
             "Should we have resolved a callee for this?");
      rawFnType = cs.getType(call->getFn());
    } else {
      // FIXME: ArgumentMismatchFailure is currently used from CSDiag, meaning
      // we can end up a BinaryExpr here with an unresolved callee. It should be
      // possible to remove this once we've gotten rid of the old CSDiag logic
      // and just assert that we have a CallExpr.
      auto *apply = cast<ApplyExpr>(anchor);
      rawFnType = cs.getType(apply->getFn());
    }
  }

  // Try to resolve the function type by loading lvalues and looking through
  // optional types, which can occur for expressions like `fn?(5)`.
  auto *fnType = resolveType(rawFnType)
                     ->lookThroughAllOptionalTypes()
                     ->getAs<FunctionType>();
  if (!fnType)
    return None;

  // Resolve the interface type for the function. Note that this may not be a
  // function type, for example it could be a generic parameter.
  Type fnInterfaceType;
  auto *callee = choice ? choice->getDeclOrNull() : nullptr;
  if (callee && callee->hasInterfaceType()) {
    // If we have a callee with an interface type, we can use it. This is
    // preferable to resolveInterfaceType, as this will allow us to get a
    // GenericFunctionType for generic decls.
    //
    // Note that it's possible to find a callee without an interface type. This
    // can happen for example with closure parameters, where the interface type
    // isn't set until the solution is applied. In that case, use
    // resolveInterfaceType.
    fnInterfaceType = callee->getInterfaceType();

    // Strip off the curried self parameter if necessary.
    if (hasAppliedSelf(cs, *choice))
      fnInterfaceType = fnInterfaceType->castTo<AnyFunctionType>()->getResult();

    if (auto *fn = fnInterfaceType->getAs<AnyFunctionType>()) {
      assert(fn->getNumParams() == fnType->getNumParams() &&
             "Parameter mismatch?");
      (void)fn;
    }
  } else {
    fnInterfaceType = resolveInterfaceType(rawFnType);
  }

  auto argIdx = applyArgElt->getArgIdx();
  auto paramIdx = applyArgElt->getParamIdx();

  return FunctionArgApplyInfo(cs, argExpr, argIdx, getType(argExpr),
                              paramIdx, fnInterfaceType, fnType, callee);
}

Type FailureDiagnostic::restoreGenericParameters(
    Type type,
    llvm::function_ref<void(GenericTypeParamType *, Type)> substitution) {
  llvm::SmallPtrSet<GenericTypeParamType *, 4> processed;
  return type.transform([&](Type type) -> Type {
    if (auto *typeVar = type->getAs<TypeVariableType>()) {
      type = resolveType(typeVar);
      if (auto *GP = typeVar->getImpl().getGenericParameter()) {
        if (processed.insert(GP).second)
          substitution(GP, type);
        return GP;
      }
    }

    return type;
  });
}

Type RequirementFailure::getOwnerType() const {
  auto *anchor = getRawAnchor();

  // If diagnostic is anchored at assignment expression
  // it means that requirement failure happend while trying
  // to convert source to destination, which means that
  // owner type is actually not an assignment expression
  // itself but its source.
  if (auto *assignment = dyn_cast<AssignExpr>(anchor))
    anchor = assignment->getSrc();

  return getType(anchor)->getInOutObjectType()->getMetatypeInstanceType();
}

const GenericContext *RequirementFailure::getGenericContext() const {
  if (auto *genericCtx = AffectedDecl->getAsGenericContext())
    return genericCtx;

  auto parentDecl = AffectedDecl->getDeclContext()->getAsDecl();
  if (!parentDecl)
    return nullptr;

  return parentDecl->getAsGenericContext();
}

const Requirement &RequirementFailure::getRequirement() const {
  // If this is a conditional requirement failure we need to
  // fetch conformance from constraint system associated with
  // type requirement this conditional conformance belongs to.
  auto requirements = isConditional()
                          ? Conformance->getConditionalRequirements()
                          : Signature->getRequirements();
  return requirements[getRequirementIndex()];
}

ProtocolConformance *RequirementFailure::getConformanceForConditionalReq(
    ConstraintLocator *locator) {
  auto &cs = getConstraintSystem();
  auto reqElt = locator->castLastElementTo<LocatorPathElt::AnyRequirement>();
  if (!reqElt.isConditionalRequirement())
    return nullptr;

  auto path = locator->getPath();
  auto *typeReqLoc = getConstraintLocator(getRawAnchor(), path.drop_back());

  auto result = llvm::find_if(
      cs.CheckedConformances,
      [&](const std::pair<ConstraintLocator *, ProtocolConformanceRef>
              &conformance) { return conformance.first == typeReqLoc; });
  assert(result != cs.CheckedConformances.end());

  auto conformance = result->second;
  assert(conformance.isConcrete());
  return conformance.getConcrete();
}

ValueDecl *RequirementFailure::getDeclRef() const {
  auto &cs = getConstraintSystem();

  // Get a declaration associated with given type (if any).
  // This is used to retrieve affected declaration when
  // failure is in any way contextual, and declaration can't
  // be fetched directly from constraint system.
  auto getAffectedDeclFromType = [](Type type) -> ValueDecl * {
    assert(type);
    // If problem is related to a typealias, let's point this
    // diagnostic directly to its declaration without desugaring.
    if (auto *alias = dyn_cast<TypeAliasType>(type.getPointer()))
      return alias->getDecl();

    if (auto *opaque = type->getAs<OpaqueTypeArchetypeType>())
      return opaque->getDecl();

    return type->getAnyGeneric();
  };

  if (isFromContextualType())
    return getAffectedDeclFromType(cs.getContextualType());

  if (auto overload = getChoiceFor(getLocator())) {
    // If there is a declaration associated with this
    // failure e.g. an overload choice of the call
    // expression, let's see whether failure is
    // associated with it directly or rather with
    // one of its parents.
    if (auto *decl = overload->choice.getDeclOrNull()) {
      auto *DC = decl->getDeclContext();

      do {
        if (auto *parent = DC->getAsDecl()) {
          if (auto *GC = parent->getAsGenericContext()) {
            // FIXME: Is this intending an exact match?
            if (GC->getGenericSignature().getPointer() != Signature.getPointer())
              continue;

            // If this is a signature if an extension
            // then it means that code has referenced
            // something incorrectly and diagnostic
            // should point to the referenced declaration.
            if (isa<ExtensionDecl>(parent))
              break;

            return cast<ValueDecl>(parent);
          }
        }
      } while ((DC = DC->getParent()));

      return decl;
    }
  }

  return getAffectedDeclFromType(getOwnerType());
}

GenericSignature RequirementFailure::getSignature(ConstraintLocator *locator) {
  if (isConditional())
    return Conformance->getGenericSignature();

  if (auto genericElt = locator->findLast<LocatorPathElt::OpenedGeneric>())
    return genericElt->getSignature();

  llvm_unreachable("Type requirement failure should always have signature");
}

bool RequirementFailure::isFromContextualType() const {
  auto path = getLocator()->getPath();
  assert(!path.empty());
  return path.front().getKind() == ConstraintLocator::ContextualType;
}

const DeclContext *RequirementFailure::getRequirementDC() const {
  // In case of conditional requirement failure, we don't
  // have to guess where the it comes from.
  if (isConditional())
    return Conformance->getDeclContext();

  const auto &req = getRequirement();
  auto *DC = AffectedDecl->getDeclContext();

  do {
    if (auto sig = DC->getGenericSignatureOfContext()) {
      if (sig->isRequirementSatisfied(req))
        return DC;
    }
  } while ((DC = DC->getParent()));

  return AffectedDecl->getAsGenericContext();
}

bool RequirementFailure::isStaticOrInstanceMember(const ValueDecl *decl) {
  if (decl->isInstanceMember())
    return true;

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(decl))
    return AFD->isStatic() && !AFD->isOperator();

  return decl->isStatic();
}

bool RequirementFailure::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  const auto *reqDC = getRequirementDC();
  auto *genericCtx = getGenericContext();

  auto lhs = getLHS();
  auto rhs = getRHS();

  if (auto *OTD = dyn_cast<OpaqueTypeDecl>(AffectedDecl)) {
    auto *namingDecl = OTD->getNamingDecl();
    emitDiagnostic(
        anchor->getLoc(), diag::type_does_not_conform_in_opaque_return,
        namingDecl->getDescriptiveKind(), namingDecl->getFullName(), lhs, rhs,
        rhs->isAnyObject());

    if (auto *repr = namingDecl->getOpaqueResultTypeRepr()) {
      emitDiagnostic(repr->getLoc(), diag::opaque_return_type_declared_here)
          .highlight(repr->getSourceRange());
    }
    return true;
  }

  if (genericCtx != reqDC && (genericCtx->isChildContextOf(reqDC) ||
                              isStaticOrInstanceMember(AffectedDecl))) {
    auto *NTD = reqDC->getSelfNominalTypeDecl();
    emitDiagnostic(anchor->getLoc(), getDiagnosticInRereference(),
                   AffectedDecl->getDescriptiveKind(),
                   AffectedDecl->getFullName(), NTD->getDeclaredType(), lhs,
                   rhs);
  } else {
    emitDiagnostic(anchor->getLoc(), getDiagnosticOnDecl(),
                   AffectedDecl->getDescriptiveKind(),
                   AffectedDecl->getFullName(), lhs, rhs);
  }

  emitRequirementNote(reqDC->getAsDecl(), lhs, rhs);
  return true;
}

bool RequirementFailure::diagnoseAsNote() {
  const auto &req = getRequirement();
  const auto *reqDC = getRequirementDC();

  emitDiagnostic(reqDC->getAsDecl(), getDiagnosticAsNote(), getLHS(), getRHS(),
                 req.getFirstType(), req.getSecondType(), "");
  return true;
}

void RequirementFailure::emitRequirementNote(const Decl *anchor, Type lhs,
                                             Type rhs) const {
  auto &req = getRequirement();

  if (req.getKind() != RequirementKind::SameType) {
    if (auto wrappedType = lhs->getOptionalObjectType()) {
      auto kind = (req.getKind() == RequirementKind::Superclass ?
                   ConstraintKind::Subtype : ConstraintKind::ConformsTo);
      if (TypeChecker::typesSatisfyConstraint(wrappedType, rhs,
                                              /*openArchetypes=*/false,
                                              kind, getDC()))
        emitDiagnostic(getAnchor()->getLoc(),
                       diag::wrapped_type_satisfies_requirement, wrappedType);
    }
  }

  if (isConditional()) {
    emitDiagnostic(anchor, diag::requirement_implied_by_conditional_conformance,
                   resolveType(Conformance->getType()),
                   Conformance->getProtocol()->getDeclaredInterfaceType());
    return;
  }

  if (req.getKind() == RequirementKind::Layout ||
      rhs->isEqual(req.getSecondType())) {
    emitDiagnostic(anchor, diag::where_requirement_failure_one_subst,
                   req.getFirstType(), lhs);
    return;
  }

  if (lhs->isEqual(req.getFirstType())) {
    emitDiagnostic(anchor, diag::where_requirement_failure_one_subst,
                   req.getSecondType(), rhs);
    return;
  }

  emitDiagnostic(anchor, diag::where_requirement_failure_both_subst,
                 req.getFirstType(), lhs, req.getSecondType(), rhs);
}

bool MissingConformanceFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto nonConformingType = getLHS();
  auto protocolType = getRHS();

  // If this is a requirement of a pattern-matching operator,
  // let's see whether argument already has a fix associated
  // with it and if so skip conformance error, otherwise we'd
  // produce an unrelated `<type> doesn't conform to Equatable protocol`
  // diagnostic.
  if (isPatternMatchingOperator(anchor)) {
    if (auto *binaryOp = dyn_cast_or_null<BinaryExpr>(findParentExpr(anchor))) {
      auto *caseExpr = binaryOp->getArg()->getElement(0);

      auto &cs = getConstraintSystem();
      llvm::SmallPtrSet<Expr *, 4> anchors;
      for (const auto *fix : cs.getFixes())
        anchors.insert(fix->getAnchor());

      bool hasFix = false;
      caseExpr->forEachChildExpr([&](Expr *expr) -> Expr * {
        hasFix |= anchors.count(expr);
        return hasFix ? nullptr : expr;
      });

      if (hasFix)
        return false;
    }
  }

  if (diagnoseAsAmbiguousOperatorRef())
    return true;

  if (nonConformingType->isObjCExistentialType()) {
    emitDiagnostic(anchor->getLoc(), diag::protocol_does_not_conform_static,
                   nonConformingType, protocolType);
    return true;
  }

  if (diagnoseTypeCannotConform(anchor, nonConformingType, protocolType))
    return true;

  // If none of the special cases could be diagnosed,
  // let's fallback to the most general diagnostic.
  return RequirementFailure::diagnoseAsError();
}

bool MissingConformanceFailure::diagnoseTypeCannotConform(Expr *anchor,
    Type nonConformingType, Type protocolType) const {
  if (getRequirement().getKind() == RequirementKind::Layout ||
      !(nonConformingType->is<AnyFunctionType>() ||
      nonConformingType->is<TupleType>() ||
      nonConformingType->isExistentialType() ||
      nonConformingType->is<AnyMetatypeType>())) {
    return false;
  }

  emitDiagnostic(anchor->getLoc(), diag::type_cannot_conform,
                 nonConformingType->isExistentialType(), nonConformingType,
                 protocolType);

  if (auto *OTD = dyn_cast<OpaqueTypeDecl>(AffectedDecl)) {
    auto *namingDecl = OTD->getNamingDecl();
    if (auto *repr = namingDecl->getOpaqueResultTypeRepr()) {
      emitDiagnostic(repr->getLoc(), diag::required_by_opaque_return,
                     namingDecl->getDescriptiveKind(), namingDecl->getFullName())
          .highlight(repr->getSourceRange());
    }
    return true;
  }

  auto &req = getRequirement();
  auto *reqDC = getRequirementDC();
  auto *genericCtx = getGenericContext();
  auto noteLocation = reqDC->getAsDecl()->getLoc();

  if (!noteLocation.isValid())
    noteLocation = anchor->getLoc();

  if (isConditional()) {
    emitDiagnostic(noteLocation, diag::requirement_implied_by_conditional_conformance,
                   resolveType(Conformance->getType()),
                   Conformance->getProtocol()->getDeclaredInterfaceType());
  } else if (genericCtx != reqDC && (genericCtx->isChildContextOf(reqDC) ||
                                     isStaticOrInstanceMember(AffectedDecl))) {
    emitDiagnostic(noteLocation, diag::required_by_decl_ref,
                   AffectedDecl->getDescriptiveKind(),
                   AffectedDecl->getFullName(),
                   reqDC->getSelfNominalTypeDecl()->getDeclaredType(),
                   req.getFirstType(), nonConformingType);
  } else {
    emitDiagnostic(noteLocation, diag::required_by_decl,
                   AffectedDecl->getDescriptiveKind(),
                   AffectedDecl->getFullName(), req.getFirstType(),
                   nonConformingType);
  }

  return true;
}

bool MissingConformanceFailure::diagnoseAsAmbiguousOperatorRef() {
  auto *anchor = getRawAnchor();
  auto *ODRE = dyn_cast<OverloadedDeclRefExpr>(anchor);
  if (!ODRE)
    return false;

  auto isStdlibType = [](Type type) {
    if (auto *NTD = type->getAnyNominal()) {
      auto *DC = NTD->getDeclContext();
      return DC->isModuleScopeContext() &&
             DC->getParentModule()->isStdlibModule();
    }

    return false;
  };

  auto name = ODRE->getDecls().front()->getBaseName();
  if (!(name.isOperator() && isStdlibType(getLHS()) && isStdlibType(getRHS())))
    return false;

  // If this is an operator reference and both types are from stdlib,
  // let's produce a generic diagnostic about invocation and a note
  // about missing conformance just in case.
  auto operatorID = name.getIdentifier();

  auto *applyExpr = cast<ApplyExpr>(findParentExpr(anchor));
  if (auto *binaryOp = dyn_cast<BinaryExpr>(applyExpr)) {
    auto lhsType = getType(binaryOp->getArg()->getElement(0));
    auto rhsType = getType(binaryOp->getArg()->getElement(1));

    if (lhsType->isEqual(rhsType)) {
      emitDiagnostic(anchor->getLoc(), diag::cannot_apply_binop_to_same_args,
                     operatorID.str(), lhsType);
    } else {
      emitDiagnostic(anchor->getLoc(), diag::cannot_apply_binop_to_args,
                     operatorID.str(), lhsType, rhsType);
    }
  } else {
    emitDiagnostic(anchor->getLoc(), diag::cannot_apply_unop_to_arg,
                   operatorID.str(), getType(applyExpr->getArg()));
  }

  diagnoseAsNote();
  return true;
}

Optional<Diag<Type, Type>> GenericArgumentsMismatchFailure::getDiagnosticFor(
    ContextualTypePurpose context) {
  switch (context) {
  case CTP_Initialization:
  case CTP_AssignSource:
    return diag::cannot_convert_assign;
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
    return diag::cannot_convert_to_return_type;
  case CTP_DefaultParameter:
    return diag::cannot_convert_default_arg_value;
  case CTP_YieldByValue:
    return diag::cannot_convert_yield_value;
  case CTP_CallArgument:
    return diag::cannot_convert_argument_value;
  case CTP_ClosureResult:
    return diag::cannot_convert_closure_result;
  case CTP_ArrayElement:
    return diag::cannot_convert_array_element;
  // TODO(diagnostics): Make dictionary related diagnostics take prescedence
  // over CSDiag. Currently these won't ever be produced.
  case CTP_DictionaryKey:
    return diag::cannot_convert_dict_key;
  case CTP_DictionaryValue:
    return diag::cannot_convert_dict_value;
  case CTP_CoerceOperand:
    return diag::cannot_convert_coerce;
  case CTP_SubscriptAssignSource:
    return diag::cannot_convert_subscript_assign;
  case CTP_Condition:
    return diag::cannot_convert_condition_value;

  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_Unused:
  case CTP_CannotFail:
  case CTP_YieldByReference:
  case CTP_CalleeResult:
  case CTP_EnumCaseRawValue:
    break;
  }
  return None;
}

void GenericArgumentsMismatchFailure::emitNoteForMismatch(int position) {
  auto *locator = getLocator();
  // Since there could be implicit conversions assoicated with argument
  // to parameter conversions, let's use parameter type as a source of
  // generic parameter information.
  auto paramSourceTy =
      locator->isLastElement<LocatorPathElt::ApplyArgToParam>() ? getRequired()
                                                                : getActual();

  auto genericTypeDecl = paramSourceTy->getAnyGeneric();
  auto param = genericTypeDecl->getGenericParams()->getParams()[position];

  auto lhs = getActual()->getGenericArgs()[position];
  auto rhs = getRequired()->getGenericArgs()[position];

  auto noteLocation = param->getLoc();

  if (!noteLocation.isValid()) {
    noteLocation = getAnchor()->getLoc();
  }

  emitDiagnostic(noteLocation, diag::generic_argument_mismatch,
                 param->getName(), lhs, rhs);
}

bool GenericArgumentsMismatchFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto path = getLocator()->getPath();

  auto fromType = getFromType();
  auto toType = getToType();

  Optional<Diag<Type, Type>> diagnostic;
  if (path.empty()) {
    if (isa<AssignExpr>(anchor)) {
      diagnostic = getDiagnosticFor(CTP_AssignSource);
    } else if (isa<CoerceExpr>(anchor)) {
      diagnostic = getDiagnosticFor(CTP_CoerceOperand);
    } else {
      return false;
    }
  } else {
    const auto &last = path.back();
    switch (last.getKind()) {
    case ConstraintLocator::ContextualType: {
      auto purpose = getContextualTypePurpose();
      assert(!(purpose == CTP_Unused && purpose == CTP_CannotFail));
      diagnostic = getDiagnosticFor(purpose);
      break;
    }

    case ConstraintLocator::AutoclosureResult:
    case ConstraintLocator::ApplyArgToParam:
    case ConstraintLocator::ApplyArgument: {
      diagnostic = diag::cannot_convert_argument_value;
      break;
    }

    case ConstraintLocator::ParentType: {
      diagnostic = diag::cannot_convert_parent_type;
      break;
    }

    case ConstraintLocator::ClosureResult: {
      diagnostic = diag::cannot_convert_closure_result;
      break;
    }

    case ConstraintLocator::GenericArgument: {
      // In cases like `[[Int]]` vs. `[[String]]`
      if (auto *assignExpr = dyn_cast<AssignExpr>(anchor)) {
        diagnostic = getDiagnosticFor(CTP_AssignSource);
        fromType = getType(assignExpr->getSrc());
        toType = getType(assignExpr->getDest());
      }
      break;
    }

    default:
      return false;
    }
  }

  if (!diagnostic)
    return false;

  emitDiagnostic(anchor->getLoc(), *diagnostic, fromType, toType);
  emitNotesForMismatches();
  return true;
}

bool LabelingFailure::diagnoseAsError() {
  auto *argExpr = getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  auto &cs = getConstraintSystem();
  auto *anchor = getRawAnchor();
  return diagnoseArgumentLabelError(cs.getASTContext(), argExpr, CorrectLabels,
                                    isa<SubscriptExpr>(anchor));
}

bool LabelingFailure::diagnoseAsNote() {
  auto *argExpr = getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  SmallVector<Identifier, 4> argLabels;
  if (auto *paren = dyn_cast<ParenExpr>(argExpr)) {
    argLabels.push_back(Identifier());
  } else if (auto *tuple = dyn_cast<TupleExpr>(argExpr)) {
    argLabels.append(tuple->getElementNames().begin(),
                     tuple->getElementNames().end());
  } else {
    return false;
  }

  auto stringifyLabels = [](ArrayRef<Identifier> labels) -> std::string {
    std::string str;
    for (auto label : labels) {
      str += label.empty() ? "_" : label.str();
      str += ':';
    }
    return "(" + str + ")";
  };

  auto selectedOverload = getChoiceFor(getLocator());
  if (!selectedOverload)
    return false;

  const auto &choice = selectedOverload->choice;
  if (auto *decl = choice.getDeclOrNull()) {
    emitDiagnostic(decl, diag::candidate_expected_different_labels,
                   stringifyLabels(argLabels), stringifyLabels(CorrectLabels));
    return true;
  }

  return false;
}

bool NoEscapeFuncToTypeConversionFailure::diagnoseAsError() {
  auto *anchor = getAnchor();

  if (diagnoseParameterUse())
    return true;

  if (ConvertTo) {
    emitDiagnostic(anchor->getLoc(), diag::converting_noescape_to_type,
                   ConvertTo);
    return true;
  }

  auto *loc = getLocator();
  if (auto gpElt = loc->getLastElementAs<LocatorPathElt::GenericParameter>()) {
    auto *paramTy = gpElt->getType();
    emitDiagnostic(anchor->getLoc(), diag::converting_noescape_to_type,
                  paramTy);
  } else {
    emitDiagnostic(anchor->getLoc(), diag::unknown_escaping_use_of_noescape);
  }
  return true;
}

bool NoEscapeFuncToTypeConversionFailure::diagnoseParameterUse() const {
  // If the other side is not a function, we have common case diagnostics
  // which handle function-to-type conversion diagnostics.
  if (!ConvertTo || !ConvertTo->is<FunctionType>())
    return false;

  auto *anchor = getAnchor();
  auto diagnostic = diag::general_noescape_to_escaping;

  ParamDecl *PD = nullptr;
  if (auto *DRE = dyn_cast<DeclRefExpr>(anchor)) {
    PD = dyn_cast<ParamDecl>(DRE->getDecl());

    // If anchor is not a parameter declaration there
    // is no need to dig up more information.
    if (!PD)
      return false;

    // Let's check whether this is a function parameter passed
    // as an argument to another function which accepts @escaping
    // function at that position.
    if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator())) {
      auto paramInterfaceTy = argApplyInfo->getParamInterfaceType();
      if (paramInterfaceTy->isTypeParameter()) {
        auto diagnoseGenericParamFailure = [&](GenericTypeParamDecl *decl) {
          emitDiagnostic(anchor->getLoc(),
                         diag::converting_noespace_param_to_generic_type,
                         PD->getName(), paramInterfaceTy);

          emitDiagnostic(decl, diag::generic_parameters_always_escaping);
        };

        // If this is a situation when non-escaping parameter is passed
        // to the argument which represents generic parameter, there is
        // a tailored diagnostic for that.

        if (auto *DMT = paramInterfaceTy->getAs<DependentMemberType>()) {
          diagnoseGenericParamFailure(DMT->getRootGenericParam()->getDecl());
          return true;
        }

        if (auto *GP = paramInterfaceTy->getAs<GenericTypeParamType>()) {
          diagnoseGenericParamFailure(GP->getDecl());
          return true;
        }
      }

      // If there are no generic parameters involved, this could
      // only mean that parameter is expecting @escaping function type.
      diagnostic = diag::passing_noescape_to_escaping;
    }
  } else if (auto *AE = dyn_cast<AssignExpr>(getRawAnchor())) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getSrc())) {
      PD = dyn_cast<ParamDecl>(DRE->getDecl());
      diagnostic = diag::assigning_noescape_to_escaping;
    }
  }

  if (!PD)
    return false;

  emitDiagnostic(anchor->getLoc(), diagnostic, PD->getName());

  // Give a note and fix-it
  auto note =
      emitDiagnostic(PD->getLoc(), diag::noescape_parameter, PD->getName());

  if (!PD->isAutoClosure()) {
    SourceLoc reprLoc;
    if (auto *repr = PD->getTypeRepr())
      reprLoc = repr->getStartLoc();
    note.fixItInsert(reprLoc, "@escaping ");
  } // TODO: add in a fixit for autoclosure

  return true;
}

bool MissingForcedDowncastFailure::diagnoseAsError() {
  if (hasComplexLocator())
    return false;

  auto *expr = getAnchor();
  if (auto *assignExpr = dyn_cast<AssignExpr>(expr))
    expr = assignExpr->getSrc();

  auto *coerceExpr = cast<CoerceExpr>(expr);

  auto fromType = getFromType();
  auto toType = getToType();

  emitDiagnostic(coerceExpr->getLoc(), diag::missing_forced_downcast, fromType,
                 toType)
      .highlight(coerceExpr->getSourceRange())
      .fixItReplace(coerceExpr->getLoc(), "as!");
  return true;
}

bool MissingAddressOfFailure::diagnoseAsError() {
  if (hasComplexLocator())
    return false;

  auto *anchor = getAnchor();
  auto argTy = getFromType();
  auto paramTy = getToType();

  if (paramTy->getAnyPointerElementType()) {
    emitDiagnostic(anchor->getLoc(), diag::cannot_convert_argument_value, argTy,
                   paramTy)
        .fixItInsert(anchor->getStartLoc(), "&");
  } else {
    emitDiagnostic(anchor->getLoc(), diag::missing_address_of, argTy)
        .fixItInsert(anchor->getStartLoc(), "&");
  }
  return true;
}

bool MissingExplicitConversionFailure::diagnoseAsError() {
  if (hasComplexLocator())
    return false;

  auto *DC = getDC();
  auto *anchor = getAnchor();
  if (auto *assign = dyn_cast<AssignExpr>(anchor))
    anchor = assign->getSrc();
  if (auto *paren = dyn_cast<ParenExpr>(anchor))
    anchor = paren->getSubExpr();

  auto fromType = getFromType();
  auto toType = getToType();

  if (!toType->hasTypeRepr())
    return false;

  bool useAs = TypeChecker::isExplicitlyConvertibleTo(fromType, toType, DC);
  if (!useAs && !TypeChecker::checkedCastMaySucceed(fromType, toType, DC))
    return false;

  auto *expr = findParentExpr(getAnchor());
  if (!expr)
    expr = getAnchor();

  // If we're performing pattern matching,
  // "as" means something completely different...
  if (auto binOpExpr = dyn_cast<BinaryExpr>(expr)) {
    auto overloadedFn = dyn_cast<OverloadedDeclRefExpr>(binOpExpr->getFn());
    if (overloadedFn && !overloadedFn->getDecls().empty()) {
      ValueDecl *decl0 = overloadedFn->getDecls()[0];
      if (decl0->getBaseName() == decl0->getASTContext().Id_MatchOperator)
        return false;
    }
  }

  bool needsParensInside = exprNeedsParensBeforeAddingAs(anchor);
  bool needsParensOutside = exprNeedsParensAfterAddingAs(anchor, expr);

  llvm::SmallString<2> insertBefore;
  llvm::SmallString<32> insertAfter;
  if (needsParensOutside) {
    insertBefore += "(";
  }
  if (needsParensInside) {
    insertBefore += "(";
    insertAfter += ")";
  }
  insertAfter += useAs ? " as " : " as! ";
  insertAfter += toType->getWithoutParens()->getString();
  if (needsParensOutside)
    insertAfter += ")";

  auto diagID =
      useAs ? diag::missing_explicit_conversion : diag::missing_forced_downcast;
  auto diag = emitDiagnostic(anchor->getLoc(), diagID, fromType, toType);
  if (!insertBefore.empty()) {
    diag.fixItInsert(anchor->getStartLoc(), insertBefore);
  }
  diag.fixItInsertAfter(anchor->getEndLoc(), insertAfter);
  return true;
}

bool MemberAccessOnOptionalBaseFailure::diagnoseAsError() {
  if (hasComplexLocator())
    return false;

  auto *anchor = getAnchor();
  auto baseType = getType(anchor);
  bool resultIsOptional = ResultTypeIsOptional;

  // If we've resolved the member overload to one that returns an optional
  // type, then the result of the expression is optional (and we want to offer
  // only a '?' fixit) even though the constraint system didn't need to add any
  // additional optionality.
  auto overload = getResolvedOverload(getLocator());
  if (overload && overload->ImpliedType->getOptionalObjectType())
    resultIsOptional = true;

  auto unwrappedBaseType = baseType->getOptionalObjectType();
  if (!unwrappedBaseType)
    return false;

  emitDiagnostic(anchor->getLoc(), diag::optional_base_not_unwrapped,
                 baseType, Member, unwrappedBaseType);

  // FIXME: It would be nice to immediately offer "base?.member ?? defaultValue"
  // for non-optional results where that would be appropriate. For the moment
  // always offering "?" means that if the user chooses chaining, we'll end up
  // in MissingOptionalUnwrapFailure:diagnose() to offer a default value during
  // the next compile.
  emitDiagnostic(anchor->getLoc(), diag::optional_base_chain, Member)
      .fixItInsertAfter(anchor->getEndLoc(), "?");

  if (!resultIsOptional) {
    emitDiagnostic(anchor->getLoc(), diag::unwrap_with_force_value)
      .fixItInsertAfter(anchor->getEndLoc(), "!");
  }

  return true;
}

void MissingOptionalUnwrapFailure::offerDefaultValueUnwrapFixIt(
    DeclContext *DC, Expr *expr) const {
  assert(expr);

  auto *anchor = getAnchor();
  // If anchor is n explicit address-of, or expression which produces
  // an l-value (e.g. first argument of `+=` operator), let's not
  // suggest default value here because that would produce r-value type.
  if (isa<InOutExpr>(anchor))
    return;

  if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator()))
    if (argApplyInfo->getParameterFlags().isInOut())
      return;

  auto diag = emitDiagnostic(expr->getLoc(), diag::unwrap_with_default_value);

  // Figure out what we need to parenthesize.
  bool needsParensInside =
      exprNeedsParensBeforeAddingNilCoalescing(DC, expr);
  auto parentExpr = findParentExpr(anchor);
  bool needsParensOutside =
      exprNeedsParensAfterAddingNilCoalescing(DC, expr, parentExpr);

  llvm::SmallString<2> insertBefore;
  llvm::SmallString<32> insertAfter;
  if (needsParensOutside) {
    insertBefore += "(";
  }
  if (needsParensInside) {
    insertBefore += "(";
    insertAfter += ")";
  }
  insertAfter += " ?? <" "#default value#" ">";
  if (needsParensOutside)
    insertAfter += ")";

  if (!insertBefore.empty()) {
    diag.fixItInsert(expr->getStartLoc(), insertBefore);
  }
  diag.fixItInsertAfter(expr->getEndLoc(), insertAfter);
}

// Suggest a force-unwrap.
void MissingOptionalUnwrapFailure::offerForceUnwrapFixIt(Expr *expr) const {
  auto diag = emitDiagnostic(expr->getLoc(), diag::unwrap_with_force_value);

  // If expr is optional as the result of an optional chain and this last
  // dot isn't a member returning optional, then offer to force the last
  // link in the chain, rather than an ugly parenthesized postfix force.
  if (auto optionalChain = dyn_cast<OptionalEvaluationExpr>(expr)) {
    if (auto dotExpr =
        dyn_cast<UnresolvedDotExpr>(optionalChain->getSubExpr())) {
      auto bind = dyn_cast<BindOptionalExpr>(dotExpr->getBase());
      if (bind && !getType(dotExpr)->getOptionalObjectType()) {
        diag.fixItReplace(SourceRange(bind->getLoc()), "!");
        return;
      }
    }
  }

  if (expr->canAppendPostfixExpression(true)) {
    diag.fixItInsertAfter(expr->getEndLoc(), "!");
  } else {
    diag.fixItInsert(expr->getStartLoc(), "(")
        .fixItInsertAfter(expr->getEndLoc(), ")!");
  }
}

class VarDeclMultipleReferencesChecker : public ASTWalker {
  VarDecl *varDecl;
  int count;

  std::pair<bool, Expr *> walkToExprPre(Expr *E) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (DRE->getDecl() == varDecl)
        count++;
    }
    return { true, E };
  }

public:
  VarDeclMultipleReferencesChecker(VarDecl *varDecl) : varDecl(varDecl),count(0) {}
  int referencesCount() { return count; }
};

bool MissingOptionalUnwrapFailure::diagnoseAsError() {
  if (hasComplexLocator())
    return false;

  if (!getUnwrappedType()->isBool()) {
    if (diagnoseConversionToBool())
      return true;
  }

  auto *anchor = getAnchor();

  // If this is an unresolved member expr e.g. `.foo` its
  // base type is going to be the same as result type minus
  // r-value adjustment because base could be an l-value type.
  // We want to fix both cases by only diagnose one of them,
  // otherwise this is just going to result in a duplcate diagnostic.
  if (getLocator()->isLastElement<LocatorPathElt::UnresolvedMember>())
    return false;

  if (auto assignExpr = dyn_cast<AssignExpr>(anchor))
    anchor = assignExpr->getSrc();

  auto *unwrappedExpr = anchor->getValueProvidingExpr();

  if (auto *tryExpr = dyn_cast<OptionalTryExpr>(unwrappedExpr)) {
    bool isSwift5OrGreater = getASTContext().isSwiftVersionAtLeast(5);
    auto subExprType = getType(tryExpr->getSubExpr());
    bool subExpressionIsOptional = (bool)subExprType->getOptionalObjectType();

    if (isSwift5OrGreater && subExpressionIsOptional) {
      // Using 'try!' won't change the type for a 'try?' with an optional
      // sub-expr under Swift 5+, so just report that a missing unwrap can't be
      // handled here.
      return false;
    }

    emitDiagnostic(tryExpr->getTryLoc(), diag::missing_unwrap_optional_try,
                   getType(anchor))
        .fixItReplace({tryExpr->getTryLoc(), tryExpr->getQuestionLoc()},
                      "try!");
    return true;
  }

  auto baseType = getBaseType();
  auto unwrappedType = getUnwrappedType();

  assert(!baseType->hasTypeVariable() &&
         "Base type must not be a type variable");
  assert(!unwrappedType->hasTypeVariable() &&
         "Unwrapped type must not be a type variable");

  if (!baseType->getOptionalObjectType())
    return false;

  emitDiagnostic(unwrappedExpr->getLoc(), diag::optional_not_unwrapped,
                 baseType, unwrappedType);

  // If the expression we're unwrapping is the only reference to a
  // local variable whose type isn't explicit in the source, then
  // offer unwrapping fixits on the initializer as well.
  if (auto declRef = dyn_cast<DeclRefExpr>(unwrappedExpr)) {
    if (auto varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
      bool singleUse = false;
      AbstractFunctionDecl *AFD = nullptr;
      if (auto contextDecl = varDecl->getDeclContext()->getAsDecl()) {
        if ((AFD = dyn_cast<AbstractFunctionDecl>(contextDecl))) {
          auto checker = VarDeclMultipleReferencesChecker(varDecl);
          AFD->getBody()->walk(checker);
          singleUse = checker.referencesCount() == 1;
        }
      }

      PatternBindingDecl *binding = varDecl->getParentPatternBinding();
      if (singleUse && binding && binding->getNumPatternEntries() == 1 &&
          varDecl->getTypeSourceRangeForDiagnostics().isInvalid()) {

        auto *initializer = varDecl->getParentInitializer();
        if (!initializer)
          return true;

        if (auto declRefExpr = dyn_cast<DeclRefExpr>(initializer)) {
          if (declRefExpr->getDecl()->isImplicitlyUnwrappedOptional()) {
            emitDiagnostic(declRefExpr->getLoc(), diag::unwrap_iuo_initializer,
                           baseType);
          }
        }

        auto fnTy = AFD->getInterfaceType()->castTo<AnyFunctionType>();
        bool voidReturn =
            fnTy->getResult()->isEqual(TupleType::getEmpty(getASTContext()));

        auto diag = emitDiagnostic(varDecl->getLoc(), diag::unwrap_with_guard);
        diag.fixItInsert(binding->getStartLoc(), "guard ");
        if (voidReturn) {
          diag.fixItInsertAfter(binding->getEndLoc(), " else { return }");
        } else {
          diag.fixItInsertAfter(binding->getEndLoc(), " else { return <"
                                                      "#default value#"
                                                      "> }");
        }
        diag.flush();

        offerDefaultValueUnwrapFixIt(varDecl->getDeclContext(), initializer);
        offerForceUnwrapFixIt(initializer);
      }
    }
  }

  offerDefaultValueUnwrapFixIt(getDC(), unwrappedExpr);
  offerForceUnwrapFixIt(unwrappedExpr);
  return true;
}

bool RValueTreatedAsLValueFailure::diagnoseAsError() {
  Diag<StringRef> subElementDiagID;
  Diag<Type> rvalueDiagID = diag::assignment_lhs_not_lvalue;
  Expr *diagExpr = getRawAnchor();
  SourceLoc loc = diagExpr->getLoc();

  auto &cs = getConstraintSystem();
  // Assignment is not allowed inside of a condition,
  // so let's not diagnose immutability, because
  // most likely the problem is related to use of `=` itself.
  if (cs.getContextualTypePurpose() == CTP_Condition)
    return false;

  if (auto assignExpr = dyn_cast<AssignExpr>(diagExpr)) {
    // Let's check whether this is an attempt to assign
    // variable or property to itself.
    if (TypeChecker::diagnoseSelfAssignment(assignExpr))
      return true;

    diagExpr = assignExpr->getDest();
  }

  if (auto callExpr = dyn_cast<ApplyExpr>(diagExpr)) {
    Expr *argExpr = callExpr->getArg();
    loc = callExpr->getFn()->getLoc();

    if (isa<PrefixUnaryExpr>(callExpr) || isa<PostfixUnaryExpr>(callExpr)) {
      subElementDiagID = diag::cannot_apply_lvalue_unop_to_subelement;
      rvalueDiagID = diag::cannot_apply_lvalue_unop_to_rvalue;
      diagExpr = argExpr;
    } else if (isa<BinaryExpr>(callExpr)) {
      subElementDiagID = diag::cannot_apply_lvalue_binop_to_subelement;
      rvalueDiagID = diag::cannot_apply_lvalue_binop_to_rvalue;
      auto argTuple = dyn_cast<TupleExpr>(argExpr);
      diagExpr = argTuple->getElement(0);
    } else if (getLocator()->getPath().size() > 0) {
      auto argElt =
          getLocator()->castLastElementTo<LocatorPathElt::ApplyArgToParam>();

      subElementDiagID = diag::cannot_pass_rvalue_inout_subelement;
      rvalueDiagID = diag::cannot_pass_rvalue_inout;
      if (auto argTuple = dyn_cast<TupleExpr>(argExpr))
        diagExpr = argTuple->getElement(argElt.getArgIdx());
      else if (auto parens = dyn_cast<ParenExpr>(argExpr))
        diagExpr = parens->getSubExpr();
    } else {
      subElementDiagID = diag::assignment_lhs_is_apply_expression;
    }
  } else if (auto inoutExpr = dyn_cast<InOutExpr>(diagExpr)) {
    if (auto restriction = getRestrictionForType(getType(inoutExpr))) {
      PointerTypeKind pointerKind;
      if (restriction->second == ConversionRestrictionKind::ArrayToPointer &&
          restriction->first->getAnyPointerElementType(pointerKind) &&
          (pointerKind == PTK_UnsafePointer ||
           pointerKind == PTK_UnsafeRawPointer)) {
        // If we're converting to an UnsafePointer, then the programmer
        // specified an & unnecessarily. Produce a fixit hint to remove it.
        emitDiagnostic(inoutExpr->getLoc(),
                       diag::extra_address_of_unsafepointer, restriction->first)
            .highlight(inoutExpr->getSourceRange())
            .fixItRemove(inoutExpr->getStartLoc());
        return true;
      }
    }

    subElementDiagID = diag::cannot_pass_rvalue_inout_subelement;
    rvalueDiagID = diag::cannot_pass_rvalue_inout;
    diagExpr = inoutExpr->getSubExpr();
  } else if (isa<DeclRefExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_variable;
  } else if (isa<ForceValueExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_bang_has_immutable_subcomponent;
  } else if (isa<MemberRefExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;
  } else if (auto member = dyn_cast<UnresolvedDotExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;

    if (auto *ctor = dyn_cast<ConstructorDecl>(getDC())) {
      if (auto *baseRef = dyn_cast<DeclRefExpr>(member->getBase())) {
        if (baseRef->getDecl() == ctor->getImplicitSelfDecl() &&
            ctor->getDelegatingOrChainedInitKind(nullptr) ==
            ConstructorDecl::BodyInitKind::Delegating) {
          emitDiagnostic(loc, diag::assignment_let_property_delegating_init,
                      member->getName());
          if (auto *ref = getResolvedMemberRef(member)) {
            emitDiagnostic(ref, diag::decl_declared_here, member->getName());
          }
          return true;
        }
      }
    }

    if (auto resolvedOverload = getResolvedOverload(getLocator())) {
      if (resolvedOverload->Choice.getKind() ==
          OverloadChoiceKind::DynamicMemberLookup)
        subElementDiagID = diag::assignment_dynamic_property_has_immutable_base;

      if (resolvedOverload->Choice.getKind() ==
          OverloadChoiceKind::KeyPathDynamicMemberLookup) {
        if (!getType(member->getBase(), /*wantRValue=*/false)->hasLValueType())
          subElementDiagID =
              diag::assignment_dynamic_property_has_immutable_base;
      }
    }
  } else if (auto sub = dyn_cast<SubscriptExpr>(diagExpr)) {
      subElementDiagID = diag::assignment_subscript_has_immutable_base;
  } else {
    subElementDiagID = diag::assignment_lhs_is_immutable_variable;
  }

  AssignmentFailure failure(diagExpr, getConstraintSystem(), loc,
                            subElementDiagID, rvalueDiagID);
  return failure.diagnose();
}

static Decl *findSimpleReferencedDecl(const Expr *E) {
  if (auto *LE = dyn_cast<LoadExpr>(E))
    E = LE->getSubExpr();

  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  return nullptr;
}

static std::pair<Decl *, Decl *> findReferencedDecl(const Expr *E) {
  E = E->getValueProvidingExpr();

  if (auto *LE = dyn_cast<LoadExpr>(E))
    return findReferencedDecl(LE->getSubExpr());

  if (auto *AE = dyn_cast<AssignExpr>(E))
    return findReferencedDecl(AE->getDest());

  if (auto *D = findSimpleReferencedDecl(E))
    return std::make_pair(nullptr, D);

  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    if (auto *BaseDecl = findSimpleReferencedDecl(MRE->getBase()))
      return std::make_pair(BaseDecl, MRE->getMember().getDecl());
  }

  return std::make_pair(nullptr, nullptr);
}

bool TypeChecker::diagnoseSelfAssignment(const Expr *expr) {
  auto *assignExpr = dyn_cast<AssignExpr>(expr);
  if (!assignExpr)
    return false;

  auto *dstExpr = assignExpr->getDest();
  auto *srcExpr = assignExpr->getSrc();

  auto dstDecl = findReferencedDecl(dstExpr);
  auto srcDecl = findReferencedDecl(srcExpr);

  if (dstDecl.second && dstDecl == srcDecl) {
    auto &DE = dstDecl.second->getASTContext().Diags;
    DE.diagnose(expr->getLoc(), dstDecl.first ? diag::self_assignment_prop
                                              : diag::self_assignment_var)
        .highlight(dstExpr->getSourceRange())
        .highlight(srcExpr->getSourceRange());
    return true;
  }

  return false;
}

bool TrailingClosureAmbiguityFailure::diagnoseAsNote() {
  const auto *expr = findParentExpr(getAnchor());
  auto *callExpr = dyn_cast_or_null<CallExpr>(expr);
  if (!callExpr)
    return false;
  if (!callExpr->hasTrailingClosure())
    return false;
  if (callExpr->getFn() != getAnchor())
    return false;

  llvm::SmallMapVector<Identifier, const ValueDecl *, 8> choicesByLabel;
  for (const auto &choice : Choices) {
    auto *callee = dyn_cast<AbstractFunctionDecl>(choice.getDecl());
    if (!callee)
      return false;

    const ParameterList *paramList = callee->getParameters();
    const ParamDecl *param = paramList->getArray().back();

    // Sanity-check that the trailing closure corresponds to this parameter.
    if (!param->hasInterfaceType() ||
        !param->getInterfaceType()->is<AnyFunctionType>())
      return false;

    Identifier trailingClosureLabel = param->getArgumentName();
    auto &choiceForLabel = choicesByLabel[trailingClosureLabel];

    // FIXME: Cargo-culted from diagnoseAmbiguity: apparently the same decl can
    // appear more than once?
    if (choiceForLabel == callee)
      continue;

    // If just providing the trailing closure label won't solve the ambiguity,
    // don't bother offering the fix-it.
    if (choiceForLabel != nullptr)
      return false;

    choiceForLabel = callee;
  }

  // If we got here, then all of the choices have unique labels. Offer them in
  // order.
  for (const auto &choicePair : choicesByLabel) {
    auto diag = emitDiagnostic(
        expr->getLoc(), diag::ambiguous_because_of_trailing_closure,
        choicePair.first.empty(), choicePair.second->getFullName());
    swift::fixItEncloseTrailingClosure(getASTContext(), diag, callExpr,
                                       choicePair.first);
  }

  return true;
}

AssignmentFailure::AssignmentFailure(Expr *destExpr, ConstraintSystem &cs,
                                     SourceLoc diagnosticLoc)
    : FailureDiagnostic(cs, cs.getConstraintLocator(destExpr)),
      DestExpr(destExpr),
      Loc(diagnosticLoc),
      DeclDiagnostic(findDeclDiagonstic(cs.getASTContext(), destExpr)),
      TypeDiagnostic(diag::assignment_lhs_not_lvalue) {}

bool AssignmentFailure::diagnoseAsError() {
  auto &cs = getConstraintSystem();
  auto *DC = getDC();

  // Walk through the destination expression, resolving what the problem is.  If
  // we find a node in the lvalue path that is problematic, this returns it.
  Expr *immutableExpr;
  Optional<OverloadChoice> choice;
  std::tie(immutableExpr, choice) = resolveImmutableBase(DestExpr);

  // Attempt diagnostics based on the overload choice.
  if (choice.hasValue()) {

    auto getKeyPathArgument = [](SubscriptExpr *expr) {
      auto *TE = dyn_cast<TupleExpr>(expr->getIndex());
      assert(TE->getNumElements() == 1);
      assert(TE->getElementName(0).str() == "keyPath");
      return TE->getElement(0);
    };

    if (!choice->isDecl()) {
      if (choice->getKind() == OverloadChoiceKind::KeyPathApplication &&
          !isa<ApplyExpr>(immutableExpr)) {
        std::string message = "key path is read-only";
        if (auto *SE = dyn_cast<SubscriptExpr>(immutableExpr)) {
          if (auto *DRE = dyn_cast<DeclRefExpr>(getKeyPathArgument(SE))) {
            auto identifier = DRE->getDecl()->getBaseName().getIdentifier();
            message =
                "'" + identifier.str().str() + "' is a read-only key path";
          }
        }
        emitDiagnostic(Loc, DeclDiagnostic, message)
            .highlight(immutableExpr->getSourceRange());
        return true;
      }
      return false;
    }

    // Otherwise, we cannot resolve this because the available setter candidates
    // are all mutating and the base must be mutating.  If we dug out a
    // problematic decl, we can produce a nice tailored diagnostic.
    if (auto *VD = dyn_cast<VarDecl>(choice->getDecl())) {
      std::string message = "'";
      message += VD->getName().str().str();
      message += "'";

      auto type = getType(immutableExpr);

      if (isKnownKeyPathType(type))
        message += " is read-only";
      else if (VD->isCaptureList())
        message += " is an immutable capture";
      else if (VD->isImplicit())
        message += " is immutable";
      else if (VD->isLet())
        message += " is a 'let' constant";
      else if (!VD->isSettable(DC))
        message += " is a get-only property";
      else if (!VD->isSetterAccessibleFrom(DC))
        message += " setter is inaccessible";
      else {
        message += " is immutable";
      }

      emitDiagnostic(Loc, DeclDiagnostic, message)
          .highlight(immutableExpr->getSourceRange());

      // If there is a masked property of the same type, emit a
      // note to fixit prepend a 'self.' or 'Type.'.
      if (auto typeContext = DC->getInnermostTypeContext()) {
        SmallVector<ValueDecl *, 2> results;
        DC->lookupQualified(typeContext->getSelfNominalTypeDecl(),
                            VD->getFullName(), NL_QualifiedDefault, results);

        auto foundProperty = llvm::find_if(results, [&](ValueDecl *decl) {
          // We're looking for a settable property that is the same type as the
          // var we found.
          auto *var = dyn_cast<VarDecl>(decl);
          if (!var || var == VD)
            return false;

          if (!var->isSettable(DC) || !var->isSetterAccessibleFrom(DC))
            return false;

          if (!var->getType()->isEqual(VD->getType()))
            return false;

          // Don't suggest a property if we're in one of its accessors.
          auto *methodDC = DC->getInnermostMethodContext();
          if (auto *AD = dyn_cast_or_null<AccessorDecl>(methodDC))
            if (AD->getStorage() == var)
              return false;

          return true;
        });

        if (foundProperty != results.end()) {
          auto startLoc = immutableExpr->getStartLoc();
          auto *property = *foundProperty;
          auto selfTy = typeContext->getSelfTypeInContext();

          // If we found an instance property, suggest inserting "self.",
          // otherwise suggest "Type." for a static property.
          std::string fixItText;
          if (property->isInstanceMember()) {
            fixItText = "self.";
          } else {
            fixItText = selfTy->getString() + ".";
          }
          emitDiagnostic(startLoc, diag::masked_mutable_property,
                         fixItText, property->getDescriptiveKind(), selfTy)
              .fixItInsert(startLoc, fixItText);
        }
      }

      // If this is a simple variable marked with a 'let', emit a note to fixit
      // hint it to 'var'.
      VD->emitLetToVarNoteIfSimple(DC);
      return true;
    }

    // If the underlying expression was a read-only subscript, diagnose that.
    if (auto *SD = dyn_cast_or_null<SubscriptDecl>(choice->getDecl())) {
      StringRef message;
      if (!SD->supportsMutation())
        message = "subscript is get-only";
      else if (!SD->isSetterAccessibleFrom(DC))
        message = "subscript setter is inaccessible";
      else
        message = "subscript is immutable";

      emitDiagnostic(Loc, DeclDiagnostic, message)
          .highlight(immutableExpr->getSourceRange());
      return true;
    }

    // If we're trying to set an unapplied method, say that.
    if (auto *VD = choice->getDecl()) {
      std::string message = "'";
      message += VD->getBaseName().getIdentifier().str();
      message += "'";

      auto diagID = DeclDiagnostic;
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD)) {
        if (AFD->hasImplicitSelfDecl()) {
          message += " is a method";
          diagID = diag::assignment_lhs_is_immutable_variable;
        } else {
          message += " is a function";
        }
      } else
        message += " is not settable";

      emitDiagnostic(Loc, diagID, message)
          .highlight(immutableExpr->getSourceRange());
      return true;
    }
  }

  // Fall back to producing diagnostics based on the expression since we
  // couldn't determine anything from the OverloadChoice.

  // If a keypath was the problem but wasn't resolved into a vardecl
  // it is ambiguous or unable to be used for setting.
  if (auto *KPE = dyn_cast_or_null<KeyPathExpr>(immutableExpr)) {
    emitDiagnostic(Loc, DeclDiagnostic, "immutable key path")
        .highlight(KPE->getSourceRange());
    return true;
  }

  if (auto LE = dyn_cast<LiteralExpr>(immutableExpr)) {
    emitDiagnostic(Loc, DeclDiagnostic, "literals are not mutable")
        .highlight(LE->getSourceRange());
    return true;
  }

  // If the expression is the result of a call, it is an rvalue, not a mutable
  // lvalue.
  if (auto *AE = dyn_cast<ApplyExpr>(immutableExpr)) {
    // Handle literals, which are a call to the conversion function.
    auto argsTuple =
        dyn_cast<TupleExpr>(AE->getArg()->getSemanticsProvidingExpr());
    if (isa<CallExpr>(AE) && AE->isImplicit() && argsTuple &&
        argsTuple->getNumElements() == 1) {
      if (auto LE = dyn_cast<LiteralExpr>(
              argsTuple->getElement(0)->getSemanticsProvidingExpr())) {
        emitDiagnostic(Loc, DeclDiagnostic, "literals are not mutable")
            .highlight(LE->getSourceRange());
        return true;
      }
    }

    std::string name = "call";
    if (isa<PrefixUnaryExpr>(AE) || isa<PostfixUnaryExpr>(AE))
      name = "unary operator";
    else if (isa<BinaryExpr>(AE))
      name = "binary operator";
    else if (isa<CallExpr>(AE))
      name = "function call";
    else if (isa<DotSyntaxCallExpr>(AE) || isa<DotSyntaxBaseIgnoredExpr>(AE))
      name = "method call";

    if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getFn()->getValueProvidingExpr()))
      name = std::string("'") +
             DRE->getDecl()->getBaseName().getIdentifier().str().str() + "'";

    emitDiagnostic(Loc, DeclDiagnostic, name + " returns immutable value")
        .highlight(AE->getSourceRange());
    return true;
  }

  if (auto contextualType = cs.getContextualType(immutableExpr)) {
    Type neededType = contextualType->getInOutObjectType();
    Type actualType = getType(immutableExpr)->getInOutObjectType();
    if (!neededType->isEqual(actualType)) {
      if (DeclDiagnostic.ID != diag::cannot_pass_rvalue_inout_subelement.ID) {
        emitDiagnostic(Loc, DeclDiagnostic,
                       "implicit conversion from '" + actualType->getString() +
                           "' to '" + neededType->getString() +
                           "' requires a temporary")
            .highlight(immutableExpr->getSourceRange());
      }
      return true;
    }
  }

  if (auto IE = dyn_cast<IfExpr>(immutableExpr)) {
    emitDiagnostic(Loc, DeclDiagnostic,
                   "result of conditional operator '? :' is never mutable")
        .highlight(IE->getQuestionLoc())
        .highlight(IE->getColonLoc());
    return true;
  }

  emitDiagnostic(Loc, TypeDiagnostic, getType(DestExpr))
      .highlight(immutableExpr->getSourceRange());
  return true;
}

std::pair<Expr *, Optional<OverloadChoice>>
AssignmentFailure::resolveImmutableBase(Expr *expr) const {
  auto &cs = getConstraintSystem();
  auto *DC = getDC();
  expr = expr->getValueProvidingExpr();

  auto isImmutable = [&DC](ValueDecl *decl) {
    if (auto *storage = dyn_cast<AbstractStorageDecl>(decl))
      return !storage->isSettable(nullptr) ||
             !storage->isSetterAccessibleFrom(DC);

    // If this is not something which could possibly be mutable,
    // then it's immutable.
    return true;
  };

  // Provide specific diagnostics for assignment to subscripts whose base expr
  // is known to be an rvalue.
  if (auto *SE = dyn_cast<SubscriptExpr>(expr)) {
    // If we found a decl for the subscript, check to see if it is a set-only
    // subscript decl.
    if (SE->hasDecl()) {
      const auto &declRef = SE->getDecl();
      if (auto *subscript =
              dyn_cast_or_null<SubscriptDecl>(declRef.getDecl())) {
        if (isImmutable(subscript))
          return {expr, OverloadChoice(getType(SE->getBase()), subscript,
                                       FunctionRefKind::DoubleApply)};
      }
    }

    Optional<OverloadChoice> member = getMemberRef(
        cs.getConstraintLocator(SE, ConstraintLocator::SubscriptMember));

    // If it isn't settable, return it.
    if (member) {
      if (member->isDecl() && isImmutable(member->getDecl()))
        return {expr, member};

      // We still have a choice, the choice is not a decl
      if (!member->isDecl()) {
        // This must be a keypath application
        assert(member->getKind() == OverloadChoiceKind::KeyPathApplication);

        auto *argType = getType(SE->getIndex())->castTo<TupleType>();
        assert(argType->getNumElements() == 1);

        auto indexType = resolveType(argType->getElementType(0));

        if (auto bgt = indexType->getAs<BoundGenericType>()) {
          // In Swift versions lower than 5, this check will fail as read only
          // key paths can masquerade as writable for compatibilty reasons.
          // This is fine as in this case we just fall back on old diagnostics.
          if (bgt->getDecl() == getASTContext().getKeyPathDecl()) {
            return {expr, member};
          }
        }
      }
    }

    // If it is settable, then the base must be the problem, recurse.
    return resolveImmutableBase(SE->getBase());
  }

  // Look through property references.
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
    // If we found a decl for the UDE, check it.
    auto loc = cs.getConstraintLocator(UDE, ConstraintLocator::Member);

    auto member = getMemberRef(loc);

    // If we can resolve a member, we can determine whether it is settable in
    // this context.
    if (member && member->isDecl() && isImmutable(member->getDecl()))
      return {expr, member};

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(UDE->getBase());
  }

  if (auto *MRE = dyn_cast<MemberRefExpr>(expr)) {
    // If the member isn't settable, then it is the problem: return it.
    if (auto member = dyn_cast<AbstractStorageDecl>(MRE->getMember().getDecl()))
      if (isImmutable(member))
        return {expr, OverloadChoice(getType(MRE->getBase()), member,
                                     FunctionRefKind::SingleApply)};

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(MRE->getBase());
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(expr))
    return {expr,
            OverloadChoice(Type(), DRE->getDecl(), FunctionRefKind::Unapplied)};

  // Look through x!
  if (auto *FVE = dyn_cast<ForceValueExpr>(expr))
    return resolveImmutableBase(FVE->getSubExpr());

  // Look through x?
  if (auto *BOE = dyn_cast<BindOptionalExpr>(expr))
    return resolveImmutableBase(BOE->getSubExpr());

  // Look through implicit conversions
  if (auto *ICE = dyn_cast<ImplicitConversionExpr>(expr))
    if (!isa<LoadExpr>(ICE->getSubExpr()))
      return resolveImmutableBase(ICE->getSubExpr());

  if (auto *SAE = dyn_cast<SelfApplyExpr>(expr))
    return resolveImmutableBase(SAE->getFn());

  return {expr, None};
}

Optional<OverloadChoice>
AssignmentFailure::getMemberRef(ConstraintLocator *locator) const {
  auto member = getOverloadChoiceIfAvailable(locator);
  if (!member)
    return None;

  if (!member->choice.isDecl())
    return member->choice;

  auto *DC = getDC();
  auto *decl = member->choice.getDecl();
  if (isa<SubscriptDecl>(decl) &&
      isValidDynamicMemberLookupSubscript(cast<SubscriptDecl>(decl), DC)) {
    auto *subscript = cast<SubscriptDecl>(decl);
    // If this is a keypath dynamic member lookup, we have to
    // adjust the locator to find member referred by it.
    if (isValidKeyPathDynamicMemberLookup(subscript)) {
      auto &cs = getConstraintSystem();
      // Type has a following format:
      // `(Self) -> (dynamicMember: {Writable}KeyPath<T, U>) -> U`
      auto *fullType = member->openedFullType->castTo<FunctionType>();
      auto *fnType = fullType->getResult()->castTo<FunctionType>();

      auto paramTy = fnType->getParams()[0].getPlainType();
      auto keyPath = paramTy->getAnyNominal();
      auto memberLoc = cs.getConstraintLocator(
          locator, LocatorPathElt::KeyPathDynamicMember(keyPath));

      auto memberRef = getOverloadChoiceIfAvailable(memberLoc);
      return memberRef ? Optional<OverloadChoice>(memberRef->choice) : None;
    }

    // If this is a string based dynamic lookup, there is no member declaration.
    return None;
  }

  return member->choice;
}

Diag<StringRef> AssignmentFailure::findDeclDiagonstic(ASTContext &ctx,
                                                      Expr *destExpr) {
  if (isa<ApplyExpr>(destExpr) || isa<SelfApplyExpr>(destExpr))
    return diag::assignment_lhs_is_apply_expression;

  if (isa<UnresolvedDotExpr>(destExpr) || isa<MemberRefExpr>(destExpr))
    return diag::assignment_lhs_is_immutable_property;

  if (auto *subscript = dyn_cast<SubscriptExpr>(destExpr)) {
    auto diagID = diag::assignment_subscript_has_immutable_base;
    // If the destination is a subscript with a 'dynamicLookup:' label and if
    // the tuple is implicit, then this was actually a @dynamicMemberLookup
    // access. Emit a more specific diagnostic.
    if (subscript->getIndex()->isImplicit() &&
        subscript->getArgumentLabels().size() == 1 &&
        subscript->getArgumentLabels().front() == ctx.Id_dynamicMember)
      diagID = diag::assignment_dynamic_property_has_immutable_base;

    return diagID;
  }

  return diag::assignment_lhs_is_immutable_variable;
}

bool ContextualFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto path = getLocator()->getPath();

  if (CTP == CTP_ReturnSingleExpr || CTP == CTP_ReturnStmt) {
    // Special case the "conversion to void".
    if (getToType()->isVoid()) {
      emitDiagnostic(anchor->getLoc(), diag::cannot_return_value_from_void_func)
          .highlight(anchor->getSourceRange());
      return true;
    }
  }

  if (diagnoseConversionToNil())
    return true;

  if (path.empty()) {
    if (auto *KPE = dyn_cast<KeyPathExpr>(anchor)) {
      emitDiagnostic(KPE->getLoc(),
                     diag::expr_smart_keypath_value_covert_to_contextual_type,
                     getFromType(), getToType());
      return true;
    }

    return false;
  }

  if (diagnoseMissingFunctionCall())
    return true;

  if (diagnoseConversionToDictionary())
    return true;

  // Special case of some common conversions involving Swift.String
  // indexes, catching cases where people attempt to index them with an integer.
  if (isIntegerToStringIndexConversion()) {
    emitDiagnostic(anchor->getLoc(), diag::string_index_not_integer,
                   getFromType())
        .highlight(anchor->getSourceRange());
    emitDiagnostic(anchor->getLoc(), diag::string_index_not_integer_note);
    return true;
  }

  Diag<Type, Type> diagnostic;
  switch (path.back().getKind()) {
  case ConstraintLocator::ClosureResult: {
    auto *closure = cast<ClosureExpr>(getRawAnchor());
    if (closure->hasExplicitResultType() &&
        closure->getExplicitResultTypeLoc().getTypeRepr()) {
      auto resultRepr = closure->getExplicitResultTypeLoc().getTypeRepr();
      emitDiagnostic(resultRepr->getStartLoc(),
                     diag::incorrect_explicit_closure_result, getFromType(),
                     getToType())
          .fixItReplace(resultRepr->getSourceRange(), getToType().getString());
      return true;
    }

    diagnostic = diag::cannot_convert_closure_result;
    break;
  }

  case ConstraintLocator::Condition: {
    // Tailored diagnostics for optional or assignment use
    // in condition expression.
    if (diagnoseConversionToBool())
      return true;

    diagnostic = diag::cannot_convert_condition_value;
    break;
  }

  case ConstraintLocator::ContextualType: {
    if (diagnoseConversionToBool())
      return true;

    if (diagnoseThrowsTypeMismatch())
      return true;

    if (diagnoseYieldByReferenceMismatch())
      return true;

    if (CTP == CTP_ForEachStmt) {
      if (FromType->isAnyExistentialType()) {
        emitDiagnostic(anchor->getLoc(), diag::type_cannot_conform,
                       /*isExistentialType=*/true, FromType, ToType);
        return true;
      }

      emitDiagnostic(
          anchor->getLoc(),
          diag::foreach_sequence_does_not_conform_to_expected_protocol,
          FromType, ToType, bool(FromType->getOptionalObjectType()))
          .highlight(anchor->getSourceRange());
      return true;
    }

    auto contextualType = getToType();
    if (auto msg = getDiagnosticFor(CTP, contextualType->isExistentialType())) {
      diagnostic = *msg;
      break;
    }
    return false;
  }

  case ConstraintLocator::RValueAdjustment: {
    auto &cs = getConstraintSystem();

    auto overload = getChoiceFor(
        cs.getConstraintLocator(anchor, ConstraintLocator::UnresolvedMember));
    if (!(overload && overload->choice.isDecl()))
      return false;

    auto *choice = overload->choice.getDecl();
    auto fnType = FromType->getAs<FunctionType>();
    if (!fnType) {
      emitDiagnostic(anchor->getLoc(),
                     diag::expected_result_in_contextual_member,
                     choice->getFullName(), FromType, ToType);
      return true;
    }

    // If member type is a function and contextual type matches
    // its result type, most likely problem is related to a
    // missing call e.g.:
    //
    // struct S {
    //   static func foo() -> S {}
    // }
    //
    // let _: S = .foo

    auto params = fnType->getParams();

    ParameterListInfo info(params, choice,
                           hasAppliedSelf(cs, overload->choice));
    auto numMissingArgs = llvm::count_if(
        indices(params), [&info](const unsigned paramIdx) -> bool {
          return !info.hasDefaultArgument(paramIdx);
        });

    if (numMissingArgs == 0 || numMissingArgs > 1) {
      auto diagnostic = emitDiagnostic(
          anchor->getLoc(), diag::expected_parens_in_contextual_member,
          choice->getFullName());

      // If there are no parameters we can suggest a fix-it
      // to form an explicit call.
      if (numMissingArgs == 0)
        diagnostic.fixItInsertAfter(anchor->getEndLoc(), "()");
    } else {
      emitDiagnostic(anchor->getLoc(),
                     diag::expected_argument_in_contextual_member,
                     choice->getFullName(), params.front().getPlainType());
    }

    return true;
  }

  default:
    return false;
  }

  auto diag = emitDiagnostic(anchor->getLoc(), diagnostic, FromType, ToType);
  diag.highlight(anchor->getSourceRange());

  (void)tryFixIts(diag);
  return true;
}

static Optional<Diag<Type>>
getContextualNilDiagnostic(ContextualTypePurpose CTP) {
  switch (CTP) {
  case CTP_Unused:
  case CTP_CannotFail:
    llvm_unreachable("These contextual type purposes cannot fail with a "
                     "conversion type specified!");
  case CTP_CalleeResult:
    llvm_unreachable("CTP_CalleeResult does not actually install a "
                     "contextual type");
  case CTP_Initialization:
    return diag::cannot_convert_initializer_value_nil;

  case CTP_ReturnSingleExpr:
  case CTP_ReturnStmt:
    return diag::cannot_convert_to_return_type_nil;

  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_YieldByReference:
    return None;

  case CTP_EnumCaseRawValue:
    return diag::cannot_convert_raw_initializer_value_nil;
  case CTP_DefaultParameter:
    return diag::cannot_convert_default_arg_value_nil;
  case CTP_YieldByValue:
    return diag::cannot_convert_yield_value_nil;
  case CTP_CallArgument:
    return diag::cannot_convert_argument_value_nil;
  case CTP_ClosureResult:
    return diag::cannot_convert_closure_result_nil;
  case CTP_ArrayElement:
    return diag::cannot_convert_array_element_nil;
  case CTP_DictionaryKey:
    return diag::cannot_convert_dict_key_nil;
  case CTP_DictionaryValue:
    return diag::cannot_convert_dict_value_nil;
  case CTP_CoerceOperand:
    return diag::cannot_convert_coerce_nil;
  case CTP_AssignSource:
    return diag::cannot_convert_assign_nil;
  case CTP_SubscriptAssignSource:
    return diag::cannot_convert_subscript_assign_nil;
  case CTP_Condition:
    return diag::cannot_convert_condition_value_nil;
  }
  llvm_unreachable("Unhandled ContextualTypePurpose in switch");
}

bool ContextualFailure::diagnoseConversionToNil() const {
  auto *anchor = getAnchor();

  if (!isa<NilLiteralExpr>(anchor))
    return false;

  auto &cs = getConstraintSystem();
  auto *locator = getLocator();

  Optional<ContextualTypePurpose> CTP;
  // Easy case were failure has been identified as contextual already.
  if (locator->isLastElement<LocatorPathElt::ContextualType>()) {
    CTP = getContextualTypePurpose();
  } else {
    // Here we need to figure out where where `nil` is located.
    // It could be e.g. an argument to a subscript/call, assignment
    // source like `s[0] = nil` or an array element like `[nil]` or
    // `[nil: 42]` as a sub-expression to a larger one.
    auto *parentExpr = findParentExpr(anchor);

    // Looks like it's something similar to `let _ = nil`.
    if (!parentExpr) {
      emitDiagnostic(anchor->getLoc(), diag::unresolved_nil_literal);
      return true;
    }

    // Two choices here - whether it's a regular assignment
    // e.g. `let _: S = nil` or a subscript one e.g. `s[0] = nil`.
    if (auto *AE = dyn_cast<AssignExpr>(parentExpr)) {
      CTP = isa<SubscriptExpr>(AE->getDest()) ? CTP_SubscriptAssignSource
                                              : CTP_AssignSource;
    } else if (isa<ArrayExpr>(parentExpr)) {
      CTP = CTP_ArrayElement;
    } else if (isa<ClosureExpr>(parentExpr)) {
      CTP = CTP_ClosureResult;
    } else if (isa<ParenExpr>(parentExpr) || isa<TupleExpr>(parentExpr)) {
      auto *enclosingExpr = findParentExpr(parentExpr);

      if (!enclosingExpr) {
        // If there is no enclosing expression it's something like
        // `(nil)` or `(a: nil)` which can't be inferred without a
        // contextual type.
        emitDiagnostic(anchor->getLoc(), diag::unresolved_nil_literal);
        return true;
      }

      if (auto *TE = dyn_cast<TupleExpr>(parentExpr)) {
        // In case of dictionary e.g. `[42: nil]` we need to figure
        // out whether nil is a "key" or a "value".
        if (auto *DE = dyn_cast<DictionaryExpr>(enclosingExpr)) {
          assert(TE->getNumElements() == 2);
          CTP = TE->getElement(0) == anchor ? CTP_DictionaryKey
                                            : CTP_DictionaryValue;
        } else {
          // Can't initialize one of the tuple elements with `nil`.
          CTP = CTP_Initialization;
        }
      }

      // `nil` is passed as an argument to a parameter which doesn't
      // expect it e.g. `foo(a: nil)`, `s[x: nil]` or `\S.[x: nil]`.
      // FIXME: Find a more robust way of checking this.
      if (isa<ApplyExpr>(enclosingExpr) || isa<SubscriptExpr>(enclosingExpr) ||
          isa<KeyPathExpr>(enclosingExpr))
        CTP = CTP_CallArgument;
    } else if (auto *CE = dyn_cast<CoerceExpr>(parentExpr)) {
      // `nil` is passed as a left-hand side of the coercion
      // operator e.g. `nil as Foo`
      CTP = CTP_CoerceOperand;
    } else {
      // Otherwise let's produce a generic `nil` conversion diagnostic.
      emitDiagnostic(anchor->getLoc(), diag::cannot_use_nil_with_this_type,
                     getToType());
      return true;
    }
  }

  if (!CTP)
    return false;

  if (CTP == CTP_ThrowStmt) {
    emitDiagnostic(anchor->getLoc(), diag::cannot_throw_nil);
    return true;
  }

  auto diagnostic = getContextualNilDiagnostic(*CTP);
  if (!diagnostic)
    return false;

  emitDiagnostic(anchor->getLoc(), *diagnostic, getToType());

  if (CTP == CTP_Initialization) {
    auto *patternTR = cs.getContextualTypeLoc().getTypeRepr();
    if (!patternTR)
      return true;

    auto diag = emitDiagnostic(patternTR->getLoc(), diag::note_make_optional,
                               OptionalType::get(getToType()));
    if (patternTR->isSimple()) {
      diag.fixItInsertAfter(patternTR->getEndLoc(), "?");
    } else {
      diag.fixItInsert(patternTR->getStartLoc(), "(");
      diag.fixItInsertAfter(patternTR->getEndLoc(), ")?");
    }
  }

  return true;
}

void ContextualFailure::tryFixIts(InFlightDiagnostic &diagnostic) const {
  auto *locator = getLocator();
  // Can't apply any of the fix-its below if this failure
  // is related to `inout` argument.
  if (locator->isLastElement<LocatorPathElt::LValueConversion>())
    return;

  if (trySequenceSubsequenceFixIts(diagnostic))
    return;

  if (tryRawRepresentableFixIts(
          diagnostic, KnownProtocolKind::ExpressibleByIntegerLiteral) ||
      tryRawRepresentableFixIts(diagnostic,
                                KnownProtocolKind::ExpressibleByStringLiteral))
    return;

  if (tryIntegerCastFixIts(diagnostic))
    return;

  if (tryProtocolConformanceFixIt(diagnostic))
    return;

  if (tryTypeCoercionFixIt(diagnostic))
    return;
}

bool ContextualFailure::diagnoseMissingFunctionCall() const {
  if (getLocator()->isLastElement<LocatorPathElt::RValueAdjustment>())
    return false;

  auto *srcFT = FromType->getAs<FunctionType>();
  if (!srcFT || !srcFT->getParams().empty())
    return false;

  if (ToType->is<AnyFunctionType>() ||
      !TypeChecker::isConvertibleTo(srcFT->getResult(), ToType, getDC()))
    return false;

  auto *anchor = getAnchor();
  emitDiagnostic(anchor->getLoc(), diag::missing_nullary_call,
                 srcFT->getResult())
      .highlight(anchor->getSourceRange())
      .fixItInsertAfter(anchor->getEndLoc(), "()");

  tryComputedPropertyFixIts(anchor);

  return true;
}

bool ContextualFailure::diagnoseConversionToBool() const {
  auto toType = getToType();
  if (!toType->isBool())
    return false;

  auto *expr = getAnchor();
  // Check for "=" converting to Bool.  The user probably meant ==.
  if (auto *AE = dyn_cast<AssignExpr>(expr->getValueProvidingExpr())) {
    emitDiagnostic(AE->getEqualLoc(), diag::use_of_equal_instead_of_equality)
        .fixItReplace(AE->getEqualLoc(), "==")
        .highlight(AE->getDest()->getLoc())
        .highlight(AE->getSrc()->getLoc());
    return true;
  }

  // If we're trying to convert something from optional type to Bool, then a
  // comparison against nil was probably expected.
  // TODO: It would be nice to handle "!x" --> x == false, but we have no way
  // to get to the parent expr at present.
  auto fromType = getFromType();
  if (fromType->getOptionalObjectType()) {
    StringRef prefix = "((";
    StringRef suffix = ") != nil)";

    // Check if we need the inner parentheses.
    // Technically we only need them if there's something in 'expr' with
    // lower precedence than '!=', but the code actually comes out nicer
    // in most cases with parens on anything non-trivial.
    if (expr->canAppendPostfixExpression()) {
      prefix = prefix.drop_back();
      suffix = suffix.drop_front();
    }
    // FIXME: The outer parentheses may be superfluous too.

    emitDiagnostic(expr->getLoc(), diag::optional_used_as_boolean, fromType)
        .fixItInsert(expr->getStartLoc(), prefix)
        .fixItInsertAfter(expr->getEndLoc(), suffix);
    return true;
  }

  return false;
}

bool ContextualFailure::isInvalidDictionaryConversion(
    ConstraintSystem &cs, Expr *anchor, Type contextualType) {
  auto *arrayExpr = dyn_cast<ArrayExpr>(anchor);
  if (!arrayExpr)
    return false;

  auto type = contextualType->lookThroughAllOptionalTypes();
  if (!conformsToKnownProtocol(
        cs, type, KnownProtocolKind::ExpressibleByDictionaryLiteral))
    return false;

  return (arrayExpr->getNumElements() & 1) == 0;
}

bool ContextualFailure::diagnoseConversionToDictionary() const {
  auto &cs = getConstraintSystem();
  auto toType = getToType()->lookThroughAllOptionalTypes();

  if (!isInvalidDictionaryConversion(cs, getAnchor(), toType))
    return false;

  auto *arrayExpr = cast<ArrayExpr>(getAnchor());

  // If the contextual type conforms to ExpressibleByDictionaryLiteral and
  // this is an empty array, then they meant "[:]".
  auto numElements = arrayExpr->getNumElements();
  if (numElements == 0) {
    emitDiagnostic(arrayExpr->getStartLoc(),
                   diag::should_use_empty_dictionary_literal)
        .fixItInsert(arrayExpr->getEndLoc(), ":");
    return true;
  }

  // If the contextual type conforms to ExpressibleByDictionaryLiteral, then
  // they wrote "x = [1,2]" but probably meant "x = [1:2]".
  bool isIniting = getContextualTypePurpose() == CTP_Initialization;
  emitDiagnostic(arrayExpr->getStartLoc(), diag::should_use_dictionary_literal,
                 toType, isIniting);

  auto diagnostic =
      emitDiagnostic(arrayExpr->getStartLoc(), diag::meant_dictionary_lit);

  // Change every other comma into a colon, only if the number
  // of commas present matches the number of elements, because
  // otherwise it might a structural problem with the expression
  // e.g. ["a""b": 1].
  const auto commaLocs = arrayExpr->getCommaLocs();
  if (commaLocs.size() == numElements - 1) {
    for (unsigned i = 0, e = numElements / 2; i != e; ++i)
      diagnostic.fixItReplace(commaLocs[i * 2], ":");
  }

  return true;
}

bool ContextualFailure::diagnoseThrowsTypeMismatch() const {
  // If this is conversion failure due to a return statement with an argument
  // that cannot be coerced to the result type of the function, emit a
  // specific error.
  if (CTP != CTP_ThrowStmt)
    return false;

  auto *anchor = getAnchor();

  // If we tried to throw the error code of an error type, suggest object
  // construction.
  auto &Ctx = getASTContext();
  if (auto errorCodeProtocol =
          Ctx.getProtocol(KnownProtocolKind::ErrorCodeProtocol)) {
    Type errorCodeType = getFromType();
    auto conformance = TypeChecker::conformsToProtocol(
        errorCodeType, errorCodeProtocol, getDC(),
        ConformanceCheckFlags::InExpression);
    if (conformance) {
      Type errorType =
          conformance
              .getTypeWitnessByName(errorCodeType, getASTContext().Id_ErrorType)
              ->getCanonicalType();
      if (errorType) {
        auto diagnostic =
            emitDiagnostic(anchor->getLoc(), diag::cannot_throw_error_code,
                           errorCodeType, errorType);
        if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor)) {
          diagnostic.fixItInsert(UDE->getDotLoc(), "(");
          diagnostic.fixItInsertAfter(UDE->getEndLoc(), ")");
        }
        return true;
      }
    }
  }

  // The conversion destination of throw is always ErrorType (at the moment)
  // if this ever expands, this should be a specific form like () is for
  // return.
  emitDiagnostic(anchor->getLoc(), diag::cannot_convert_thrown_type,
                 getFromType())
      .highlight(anchor->getSourceRange());
  return true;
}

bool ContextualFailure::diagnoseYieldByReferenceMismatch() const {
  if (CTP != CTP_YieldByReference)
    return false;

  auto *anchor = getAnchor();
  auto exprType = getType(anchor, /*wantRValue=*/false);
  auto contextualType = getToType();

  if (auto exprLV = exprType->getAs<LValueType>()) {
    emitDiagnostic(anchor->getLoc(), diag::cannot_yield_wrong_type_by_reference,
                   exprLV->getObjectType(), contextualType);
  } else if (exprType->isEqual(contextualType)) {
    emitDiagnostic(anchor->getLoc(),
                   diag::cannot_yield_rvalue_by_reference_same_type, exprType);
  } else {
    emitDiagnostic(anchor->getLoc(), diag::cannot_yield_rvalue_by_reference,
                   exprType, contextualType);
  }
  return true;
}

bool ContextualFailure::tryRawRepresentableFixIts(
    InFlightDiagnostic &diagnostic,
    KnownProtocolKind rawRepresentableProtocol) const {
  auto &CS = getConstraintSystem();
  auto *expr = getAnchor();
  auto fromType = getFromType();
  auto toType = getToType();

  // The following fixes apply for optional destination types as well.
  bool toTypeIsOptional = !toType->getOptionalObjectType().isNull();
  toType = toType->lookThroughAllOptionalTypes();

  Type fromTypeUnwrapped = fromType->getOptionalObjectType();
  bool fromTypeIsOptional = !fromTypeUnwrapped.isNull();
  if (fromTypeIsOptional)
    fromType = fromTypeUnwrapped;

  auto fixIt = [&](StringRef convWrapBefore, StringRef convWrapAfter) {
    SourceRange exprRange = expr->getSourceRange();
    if (fromTypeIsOptional && toTypeIsOptional) {
      // Use optional's map function to convert conditionally, like so:
      //   expr.map{ T(rawValue: $0) }
      bool needsParens = !expr->canAppendPostfixExpression();
      std::string mapCodeFix;
      if (needsParens) {
        diagnostic.fixItInsert(exprRange.Start, "(");
        mapCodeFix += ")";
      }
      mapCodeFix += ".map { ";
      mapCodeFix += convWrapBefore;
      mapCodeFix += "$0";
      mapCodeFix += convWrapAfter;
      mapCodeFix += " }";
      diagnostic.fixItInsertAfter(exprRange.End, mapCodeFix);
    } else if (!fromTypeIsOptional) {
      diagnostic.fixItInsert(exprRange.Start, convWrapBefore);
      diagnostic.fixItInsertAfter(exprRange.End, convWrapAfter);
    } else {
      SmallString<16> fixItBefore(convWrapBefore);
      SmallString<16> fixItAfter;

      if (!expr->canAppendPostfixExpression(true)) {
        fixItBefore += "(";
        fixItAfter = ")";
      }

      fixItAfter += "!" + convWrapAfter.str();

      diagnostic.flush();
      emitDiagnostic(expr->getLoc(),
                     diag::construct_raw_representable_from_unwrapped_value,
                     toType, fromType)
          .highlight(exprRange)
          .fixItInsert(exprRange.Start, fixItBefore)
          .fixItInsertAfter(exprRange.End, fixItAfter);
    }
  };

  if (conformsToKnownProtocol(CS, fromType, rawRepresentableProtocol)) {
    if (conformsToKnownProtocol(CS, fromType, KnownProtocolKind::OptionSet) &&
        isa<IntegerLiteralExpr>(expr) &&
        cast<IntegerLiteralExpr>(expr)->getDigitsText() == "0") {
      diagnostic.fixItReplace(expr->getSourceRange(), "[]");
      return true;
    }
    if (auto rawTy = isRawRepresentable(CS, toType, rawRepresentableProtocol)) {
      // Produce before/after strings like 'Result(rawValue: RawType(<expr>))'
      // or just 'Result(rawValue: <expr>)'.
      std::string convWrapBefore = toType.getString();
      convWrapBefore += "(rawValue: ";
      std::string convWrapAfter = ")";
      if (!isa<LiteralExpr>(expr) &&
          !TypeChecker::isConvertibleTo(fromType, rawTy, getDC())) {
        // Only try to insert a converting construction if the protocol is a
        // literal protocol and not some other known protocol.
        switch (rawRepresentableProtocol) {
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(name, _, __, ___)            \
  case KnownProtocolKind::name:                                                \
    break;
#define PROTOCOL_WITH_NAME(name, _)                                            \
  case KnownProtocolKind::name:                                                \
    return false;
#include "swift/AST/KnownProtocols.def"
        }
        convWrapBefore += rawTy->getString();
        convWrapBefore += "(";
        convWrapAfter += ")";
      }
      fixIt(convWrapBefore, convWrapAfter);
      return true;
    }
  }

  if (auto rawTy = isRawRepresentable(CS, fromType, rawRepresentableProtocol)) {
    if (conformsToKnownProtocol(CS, toType, rawRepresentableProtocol)) {
      std::string convWrapBefore;
      std::string convWrapAfter = ".rawValue";
      if (!TypeChecker::isConvertibleTo(rawTy, toType, getDC())) {
        // Only try to insert a converting construction if the protocol is a
        // literal protocol and not some other known protocol.
        switch (rawRepresentableProtocol) {
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(name, _, __, ___)            \
  case KnownProtocolKind::name:                                                \
    break;
#define PROTOCOL_WITH_NAME(name, _)                                            \
  case KnownProtocolKind::name:                                                \
    return false;
#include "swift/AST/KnownProtocols.def"
        }
        convWrapBefore += toType->getString();
        convWrapBefore += "(";
        convWrapAfter += ")";
      }
      fixIt(convWrapBefore, convWrapAfter);
      return true;
    }
  }

  return false;
}

bool ContextualFailure::tryIntegerCastFixIts(
    InFlightDiagnostic &diagnostic) const {
  if (!isIntegerType(FromType) || !isIntegerType(ToType))
    return false;

  auto getInnerCastedExpr = [&](Expr *expr) -> Expr * {
    if (auto *CE = dyn_cast<CoerceExpr>(expr))
      return CE->getSubExpr();

    auto *CE = dyn_cast<CallExpr>(expr);
    if (!CE)
      return nullptr;
    if (!isa<ConstructorRefCallExpr>(CE->getFn()))
      return nullptr;
    auto *parenE = dyn_cast<ParenExpr>(CE->getArg());
    if (!parenE)
      return nullptr;
    return parenE->getSubExpr();
  };

  auto *anchor = getAnchor();
  if (Expr *innerE = getInnerCastedExpr(anchor)) {
    Type innerTy = getType(innerE);
    if (TypeChecker::isConvertibleTo(innerTy, ToType, getDC())) {
      // Remove the unnecessary cast.
      diagnostic.fixItRemoveChars(anchor->getLoc(), innerE->getStartLoc())
          .fixItRemove(anchor->getEndLoc());
      return true;
    }
  }

  // Add a wrapping integer cast.
  std::string convWrapBefore = ToType.getString();
  convWrapBefore += "(";
  std::string convWrapAfter = ")";
  SourceRange exprRange = anchor->getSourceRange();
  diagnostic.fixItInsert(exprRange.Start, convWrapBefore);
  diagnostic.fixItInsertAfter(exprRange.End, convWrapAfter);
  return true;
}

bool ContextualFailure::trySequenceSubsequenceFixIts(
    InFlightDiagnostic &diagnostic) const {
  if (!getASTContext().getStdlibModule())
    return false;

  auto String = TypeChecker::getStringType(getASTContext());
  auto Substring = TypeChecker::getSubstringType(getASTContext());

  if (!String || !Substring)
    return false;

  // Substring -> String conversion
  // Wrap in String.init
  if (FromType->isEqual(Substring)) {
    if (ToType->isEqual(String)) {
      auto *anchor = getAnchor()->getSemanticsProvidingExpr();
      auto range = anchor->getSourceRange();
      diagnostic.fixItInsert(range.Start, "String(");
      diagnostic.fixItInsertAfter(range.End, ")");
      return true;
    }
  }

  return false;
}

bool ContextualFailure::tryTypeCoercionFixIt(
    InFlightDiagnostic &diagnostic) const {
  auto fromType = getFromType();
  auto toType = getToType();

  // Look through optional types; casts can add them, but can't remove extra
  // ones.
  bool bothOptional =
      fromType->getOptionalObjectType() && toType->getOptionalObjectType();
  if (bothOptional)
    fromType = fromType->getOptionalObjectType();
  toType = toType->lookThroughAllOptionalTypes();

  if (!toType->hasTypeRepr())
    return false;

  CheckedCastKind Kind =
      TypeChecker::typeCheckCheckedCast(fromType, toType,
                                        CheckedCastContextKind::None, getDC(),
                                        SourceLoc(), nullptr, SourceRange());

  if (Kind != CheckedCastKind::Unresolved) {
    auto *anchor = getAnchor();

    bool canUseAs = Kind == CheckedCastKind::Coercion ||
                    Kind == CheckedCastKind::BridgingCoercion;
    if (bothOptional && canUseAs)
      toType = OptionalType::get(toType);
    diagnostic.fixItInsert(Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                      anchor->getEndLoc()),
                           diag::insert_type_coercion, canUseAs, toType);
    return true;
  }

  return false;
}

bool ContextualFailure::tryProtocolConformanceFixIt(
    InFlightDiagnostic &diagnostic) const {
  auto innermostTyCtx = getDC()->getInnermostTypeContext();
  if (!innermostTyCtx)
    return false;

  auto nominal = innermostTyCtx->getSelfNominalTypeDecl();
  if (!nominal)
    return false;

  // We need to get rid of optionals and parens as it's not relevant when
  // printing the diagnostic and the fix-it.
  auto unwrappedToType =
      ToType->lookThroughAllOptionalTypes()->getWithoutParens();

  // If the protocol requires a class & we don't have one (maybe the context
  // is a struct), then bail out instead of offering a broken fix-it later on.
  auto requiresClass = false;
  ExistentialLayout layout;
  if (unwrappedToType->isExistentialType()) {
    layout = unwrappedToType->getExistentialLayout();
    requiresClass = layout.requiresClass();
  }

  if (requiresClass && !FromType->is<ClassType>()) {
    return false;
  }

  // We can only offer a fix-it if we're assigning to a protocol type and
  // the type we're assigning is the same as the innermost type context.
  bool shouldOfferFixIt = nominal->getSelfTypeInContext()->isEqual(FromType) &&
                          unwrappedToType->isExistentialType();
  if (!shouldOfferFixIt)
    return false;

  diagnostic.flush();

  // Let's build a list of protocols that the context does not conform to.
  SmallVector<std::string, 8> missingProtoTypeStrings;
  for (auto protocol : layout.getProtocols()) {
    if (!TypeChecker::conformsToProtocol(FromType, protocol->getDecl(), getDC(),
                                         ConformanceCheckFlags::InExpression)) {
      missingProtoTypeStrings.push_back(protocol->getString());
    }
  }

  // If we have a protocol composition type and we don't conform to all
  // the protocols of the composition, then store the composition directly.
  // This is because we need to append 'Foo & Bar' instead of 'Foo, Bar' in
  // order to match the written type.
  if (auto compositionTy = unwrappedToType->getAs<ProtocolCompositionType>()) {
    if (compositionTy->getMembers().size() == missingProtoTypeStrings.size()) {
      missingProtoTypeStrings = {compositionTy->getString()};
    }
  }

  assert(!missingProtoTypeStrings.empty() &&
         "type already conforms to all the protocols?");

  // Combine all protocol names together, separated by commas.
  std::string protoString = llvm::join(missingProtoTypeStrings, ", ");

  // Emit a diagnostic to inform the user that they need to conform to the
  // missing protocols.
  //
  // TODO: Maybe also insert the requirement stubs?
  auto conformanceDiag = emitDiagnostic(
      getAnchor()->getLoc(), diag::assign_protocol_conformance_fix_it,
      unwrappedToType, nominal->getDescriptiveKind(), FromType);
  if (nominal->getInherited().size() > 0) {
    auto lastInherited = nominal->getInherited().back().getLoc();
    auto lastInheritedEndLoc =
        Lexer::getLocForEndOfToken(getASTContext().SourceMgr, lastInherited);
    conformanceDiag.fixItInsert(lastInheritedEndLoc, ", " + protoString);
  } else {
    auto nameEndLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                 nominal->getNameLoc());
    conformanceDiag.fixItInsert(nameEndLoc, ": " + protoString);
  }

  return true;
}

void ContextualFailure::tryComputedPropertyFixIts(Expr *expr) const {
  if (!isa<ClosureExpr>(expr))
    return;

  // It is possible that we're looking at a stored property being
  // initialized with a closure. Something like:
  //
  // var foo: Int = { return 0 }
  //
  // Let's offer another fix-it to remove the '=' to turn the stored
  // property into a computed property. If the variable is immutable, then
  // replace the 'let' with a 'var'.

  PatternBindingDecl *PBD = nullptr;

  if (auto TLCD = dyn_cast<TopLevelCodeDecl>(getDC())) {
    if (TLCD->getBody()->isImplicit()) {
      if (auto decl = TLCD->getBody()->getFirstElement().dyn_cast<Decl *>()) {
        if (auto binding = dyn_cast<PatternBindingDecl>(decl)) {
          PBD = binding;
        }
      }
    }
  } else if (auto PBI = dyn_cast<PatternBindingInitializer>(getDC())) {
    PBD = PBI->getBinding();
  }

  if (PBD) {
    if (auto VD = PBD->getSingleVar()) {
      const auto i = PBD->getPatternEntryIndexForVarDecl(VD);
      auto *initExpr = PBD->getInit(i);
      if (!VD->isStatic() &&
          !VD->getAttrs().getAttribute<DynamicReplacementAttr>() &&
          initExpr && isa<ClosureExpr>(initExpr)) {
        auto diag = emitDiagnostic(expr->getLoc(),
                                   diag::extension_stored_property_fixit,
                                   VD->getName());
        diag.fixItRemove(PBD->getEqualLoc(i));

        if (VD->isLet()) {
          diag.fixItReplace(PBD->getStartLoc(), getTokenText(tok::kw_var));
        }

        if (auto lazyAttr = VD->getAttrs().getAttribute<LazyAttr>()) {
          diag.fixItRemove(lazyAttr->getRange());
        }
      }
    }
  }
}

bool ContextualFailure::isIntegerToStringIndexConversion() const {
  auto &cs = getConstraintSystem();
  auto kind = KnownProtocolKind::ExpressibleByIntegerLiteral;

  auto fromType = getFromType();
  auto toType = getToType()->getCanonicalType();
  return (conformsToKnownProtocol(cs, fromType, kind) &&
          toType.getString() == "String.CharacterView.Index");
}

Optional<Diag<Type, Type>>
ContextualFailure::getDiagnosticFor(ContextualTypePurpose context,
                                    bool forProtocol) {
  switch (context) {
  case CTP_Initialization:
    return forProtocol ? diag::cannot_convert_initializer_value_protocol
                       : diag::cannot_convert_initializer_value;
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
    return forProtocol ? diag::cannot_convert_to_return_type_protocol
                       : diag::cannot_convert_to_return_type;
  case CTP_EnumCaseRawValue:
    return diag::cannot_convert_raw_initializer_value;
  case CTP_DefaultParameter:
    return forProtocol ? diag::cannot_convert_default_arg_value_protocol
                       : diag::cannot_convert_default_arg_value;
  case CTP_YieldByValue:
    return forProtocol ? diag::cannot_convert_yield_value_protocol
                       : diag::cannot_convert_yield_value;
  case CTP_CallArgument:
    return forProtocol ? diag::cannot_convert_argument_value_protocol
                       : diag::cannot_convert_argument_value;
  case CTP_ClosureResult:
    return forProtocol ? diag::cannot_convert_closure_result_protocol
                       : diag::cannot_convert_closure_result;
  case CTP_ArrayElement:
    return forProtocol ? diag::cannot_convert_array_element_protocol
                       : diag::cannot_convert_array_element;
  case CTP_DictionaryKey:
    return forProtocol ? diag::cannot_convert_dict_key_protocol
                       : diag::cannot_convert_dict_key;
  case CTP_DictionaryValue:
    return forProtocol ? diag::cannot_convert_dict_value_protocol
                       : diag::cannot_convert_dict_value;
  case CTP_CoerceOperand:
    return forProtocol ? diag::cannot_convert_coerce_protocol
                       : diag::cannot_convert_coerce;
  case CTP_AssignSource:
    return forProtocol ? diag::cannot_convert_assign_protocol
                       : diag::cannot_convert_assign;
  case CTP_SubscriptAssignSource:
    return forProtocol ? diag::cannot_convert_subscript_assign_protocol
                       : diag::cannot_convert_subscript_assign;
  case CTP_Condition:
    return diag::cannot_convert_condition_value;

  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_Unused:
  case CTP_CannotFail:
  case CTP_YieldByReference:
  case CTP_CalleeResult:
    break;
  }
  return None;
}

bool TupleContextualFailure::diagnoseAsError() {
  auto diagnostic = isNumElementsMismatch()
                        ? diag::tuple_types_not_convertible_nelts
                        : diag::tuple_types_not_convertible;
  emitDiagnostic(getAnchor()->getLoc(), diagnostic, getFromType(), getToType());
  return true;
}

bool AutoClosureForwardingFailure::diagnoseAsError() {
  auto *loc = getLocator();
  auto last = loc->castLastElementTo<LocatorPathElt::ApplyArgToParam>();

  // We need a raw anchor here because `getAnchor()` is simplified
  // to the argument expression.
  auto *argExpr = getArgumentExpr(getRawAnchor(), last.getArgIdx());
  emitDiagnostic(argExpr->getLoc(), diag::invalid_autoclosure_forwarding)
      .highlight(argExpr->getSourceRange())
      .fixItInsertAfter(argExpr->getEndLoc(), "()");
  return true;
}

bool AutoClosurePointerConversionFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto diagnostic = diag::invalid_autoclosure_pointer_conversion;
  emitDiagnostic(anchor->getLoc(), diagnostic, getFromType(), getToType())
      .highlight(anchor->getSourceRange());
  return true;
}

bool NonOptionalUnwrapFailure::diagnoseAsError() {
  auto *anchor = getAnchor();

  auto diagnostic = diag::invalid_optional_chain;
  if (isa<ForceValueExpr>(anchor))
    diagnostic = diag::invalid_force_unwrap;

  emitDiagnostic(anchor->getLoc(), diagnostic, BaseType)
      .highlight(anchor->getSourceRange())
      .fixItRemove(anchor->getEndLoc());

  return true;
}

bool MissingCallFailure::diagnoseAsError() {
  auto *baseExpr = getAnchor();
  SourceLoc insertLoc = baseExpr->getEndLoc();

  if (auto *FVE = dyn_cast<ForceValueExpr>(baseExpr))
    baseExpr = FVE->getSubExpr();

  // Calls are not yet supported by key path, but it
  // is useful to record this fix to diagnose chaining
  // where one of the key path components is a method
  // reference.
  if (isa<KeyPathExpr>(baseExpr))
    return false;

  auto path = getLocator()->getPath();
  if (!path.empty()) {
    const auto &last = path.back();

    switch (last.getKind()) {
    case ConstraintLocator::ContextualType:
    case ConstraintLocator::ApplyArgToParam: {
      auto fnType = getType(baseExpr)->castTo<FunctionType>();
      emitDiagnostic(baseExpr->getLoc(), diag::missing_nullary_call,
                     fnType->getResult())
          .fixItInsertAfter(baseExpr->getEndLoc(), "()");
      return true;
    }

    case ConstraintLocator::FunctionResult: {
      path = path.drop_back();
      if (path.back().getKind() != ConstraintLocator::AutoclosureResult)
        break;

      LLVM_FALLTHROUGH;
    }

    case ConstraintLocator::AutoclosureResult: {
      auto &cs = getConstraintSystem();
      auto loc = getConstraintLocator(getRawAnchor(), path.drop_back());
      AutoClosureForwardingFailure failure(cs, loc);
      return failure.diagnoseAsError();
    }
    default:
      break;
    }
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(baseExpr)) {
    emitDiagnostic(baseExpr->getLoc(), diag::did_not_call_function,
                   DRE->getDecl()->getBaseName().getIdentifier())
        .fixItInsertAfter(insertLoc, "()");
    return true;
  }

  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(baseExpr)) {
    emitDiagnostic(baseExpr->getLoc(), diag::did_not_call_method,
                   UDE->getName().getBaseIdentifier())
        .fixItInsertAfter(insertLoc, "()");
    return true;
  }

  if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(baseExpr)) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(DSCE->getFn())) {
      emitDiagnostic(baseExpr->getLoc(), diag::did_not_call_method,
                     DRE->getDecl()->getBaseName().getIdentifier())
          .fixItInsertAfter(insertLoc, "()");
      return true;
    }
  }

  if (auto *AE = dyn_cast<AssignExpr>(baseExpr)) {
    auto *srcExpr = AE->getSrc();
    if (auto *fnType = getType(srcExpr)->getAs<FunctionType>()) {
      emitDiagnostic(srcExpr->getLoc(), diag::missing_nullary_call,
                     fnType->getResult())
          .highlight(srcExpr->getSourceRange())
          .fixItInsertAfter(srcExpr->getEndLoc(), "()");
      return true;
    }
  }

  emitDiagnostic(baseExpr->getLoc(), diag::did_not_call_function_value)
      .fixItInsertAfter(insertLoc, "()");
  return true;
}

bool ExtraneousPropertyWrapperUnwrapFailure::diagnoseAsError() {
  auto loc = getAnchor()->getLoc();
  auto newPrefix = usingStorageWrapper() ? "$" : "_";

  if (auto *member = getReferencedMember()) {
    emitDiagnostic(loc, diag::incorrect_property_wrapper_reference_member,
                   member->getDescriptiveKind(), member->getFullName(), false,
                   getToType())
        .fixItInsert(loc, newPrefix);
    return true;
  }

  emitDiagnostic(loc, diag::incorrect_property_wrapper_reference,
                 getPropertyName(), getFromType(), getToType(), false)
      .fixItInsert(loc, newPrefix);
  return true;
}

bool MissingPropertyWrapperUnwrapFailure::diagnoseAsError() {
  auto loc = getAnchor()->getLoc();
  auto endLoc = getAnchor()->getLoc().getAdvancedLoc(1);

  if (auto *member = getReferencedMember()) {
    emitDiagnostic(loc, diag::incorrect_property_wrapper_reference_member,
                   member->getDescriptiveKind(), member->getFullName(), true,
                   getToType())
        .fixItRemoveChars(loc, endLoc);
    return true;
  }

  emitDiagnostic(loc, diag::incorrect_property_wrapper_reference,
                 getPropertyName(), getFromType(), getToType(), true)
      .fixItRemoveChars(loc, endLoc);
  return true;
}

bool SubscriptMisuseFailure::diagnoseAsError() {
  auto &sourceMgr = getASTContext().SourceMgr;

  auto *memberExpr = cast<UnresolvedDotExpr>(getRawAnchor());
  auto *baseExpr = getAnchor();

  auto memberRange = baseExpr->getSourceRange();
  (void)simplifyLocator(getConstraintSystem(), getLocator(), memberRange);

  auto nameLoc = DeclNameLoc(memberRange.Start);

  auto diag = emitDiagnostic(baseExpr->getLoc(),
                             diag::could_not_find_subscript_member_did_you_mean,
                             getType(baseExpr));

  diag.highlight(memberRange).highlight(nameLoc.getSourceRange());

  if (auto *parentExpr = dyn_cast_or_null<ApplyExpr>(findParentExpr(memberExpr))) {
    auto *argExpr = parentExpr->getArg();

    auto toCharSourceRange = Lexer::getCharSourceRangeFromSourceRange;
    auto lastArgSymbol = toCharSourceRange(sourceMgr, argExpr->getEndLoc());

    diag.fixItReplace(SourceRange(argExpr->getStartLoc()),
                      getTokenText(tok::l_square));
    diag.fixItRemove(nameLoc.getSourceRange());
    diag.fixItRemove(SourceRange(memberExpr->getDotLoc()));

    if (sourceMgr.extractText(lastArgSymbol) == getTokenText(tok::r_paren))
      diag.fixItReplace(SourceRange(argExpr->getEndLoc()),
                        getTokenText(tok::r_square));
    else
      diag.fixItInsertAfter(argExpr->getEndLoc(), getTokenText(tok::r_square));
  } else {
    diag.fixItReplace(SourceRange(memberExpr->getDotLoc(), memberExpr->getLoc()), "[<#index#>]");
  }
  diag.flush();

  if (auto overload = getOverloadChoiceIfAvailable(getLocator())) {
    emitDiagnostic(overload->choice.getDecl(), diag::kind_declared_here,
                   DescriptiveDeclKind::Subscript);
  }

  return true;
}

bool SubscriptMisuseFailure::diagnoseAsNote() {
  if (auto overload = getOverloadChoiceIfAvailable(getLocator())) {
    emitDiagnostic(overload->choice.getDecl(), diag::found_candidate);
    return true;
  }
  return false;
}

/// When a user refers a enum case with a wrong member name, we try to find a
/// enum element whose name differs from the wrong name only in convention;
/// meaning their lower case counterparts are identical.
///   - DeclName is valid when such a correct case is found; invalid otherwise.
DeclName MissingMemberFailure::findCorrectEnumCaseName(
    Type Ty, TypoCorrectionResults &corrections, DeclName memberName) {
  if (memberName.isSpecial() || !memberName.isSimpleName())
    return DeclName();
  if (!Ty->getEnumOrBoundGenericEnum())
    return DeclName();
  auto candidate =
      corrections.getUniqueCandidateMatching([&](ValueDecl *candidate) {
        return (isa<EnumElementDecl>(candidate) &&
                candidate->getFullName().getBaseIdentifier().str().equals_lower(
                    memberName.getBaseIdentifier().str()));
      });
  return (candidate ? candidate->getFullName() : DeclName());
}

bool MissingMemberFailure::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  auto *baseExpr = getAnchor();

  if (!anchor || !baseExpr)
    return false;

  auto baseType = resolveType(getBaseType())->getWithoutSpecifierType();

  DeclNameLoc nameLoc(anchor->getStartLoc());
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor)) {
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = dyn_cast<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  }

  auto emitBasicError = [&](Type baseType) {
    auto diagnostic = diag::could_not_find_value_member;

    if (auto *metatype = baseType->getAs<MetatypeType>()) {
      baseType = metatype->getInstanceType();
      diagnostic = diag::could_not_find_type_member;
    }

    if (baseType->is<TupleType>())
      diagnostic = diag::could_not_find_tuple_member;

    bool hasUnresolvedPattern = false;
    anchor->forEachChildExpr([&](Expr *expr) {
      hasUnresolvedPattern |= isa<UnresolvedPatternExpr>(expr);
      return hasUnresolvedPattern ? nullptr : expr;
    });
    if (hasUnresolvedPattern && !baseType->getAs<EnumType>()) {
      emitDiagnostic(anchor->getLoc(),
          diag::cannot_match_unresolved_expr_pattern_with_value, baseType);
      return;
    }

    emitDiagnostic(anchor->getLoc(), diagnostic, baseType, getName())
        .highlight(baseExpr->getSourceRange())
        .highlight(nameLoc.getSourceRange());
  };

  TypoCorrectionResults corrections(getName(), nameLoc);
  auto tryTypoCorrection = [&] (Type type) {
    TypeChecker::performTypoCorrection(getDC(), DeclRefKind::Ordinary, type,
                                       defaultMemberLookupOptions, corrections);
  };

  if (getName().getBaseName().getKind() == DeclBaseName::Kind::Subscript) {
    auto loc = anchor->getLoc();
    if (auto *metatype = baseType->getAs<MetatypeType>()) {
      emitDiagnostic(loc, diag::could_not_find_type_member,
                     metatype->getInstanceType(), getName())
        .highlight(baseExpr->getSourceRange());
    } else {
      emitDiagnostic(loc, diag::could_not_find_value_subscript, baseType)
        .highlight(baseExpr->getSourceRange());
    }
  } else if (getName().getBaseName() == "deinit") {
    // Specialised diagnostic if trying to access deinitialisers
    emitDiagnostic(anchor->getLoc(), diag::destructor_not_accessible)
        .highlight(baseExpr->getSourceRange());
  } else if (auto metatypeTy = baseType->getAs<MetatypeType>()) {
    auto instanceTy = metatypeTy->getInstanceType();
    tryTypoCorrection(baseType);

    if (DeclName rightName =
            findCorrectEnumCaseName(instanceTy, corrections, getName())) {
      emitDiagnostic(anchor->getLoc(), diag::could_not_find_enum_case,
                     instanceTy, getName(), rightName)
          .fixItReplace(nameLoc.getBaseNameLoc(),
                        rightName.getBaseIdentifier().str());
      return true;
    }

    if (auto correction = corrections.claimUniqueCorrection()) {
      auto diagnostic = emitDiagnostic(
          anchor->getLoc(), diag::could_not_find_type_member_corrected,
          instanceTy, getName(), correction->CorrectedName);
      diagnostic.highlight(baseExpr->getSourceRange())
          .highlight(nameLoc.getSourceRange());
      correction->addFixits(diagnostic);
    } else if (instanceTy->getAnyNominal() &&
               getName().getBaseName() == DeclBaseName::createConstructor()) {
      auto &cs = getConstraintSystem();

      auto memberName = getName().getBaseName();
      auto result = cs.performMemberLookup(
          ConstraintKind::ValueMember, memberName, metatypeTy,
          FunctionRefKind::DoubleApply, getLocator(),
          /*includeInaccessibleMembers=*/true);

      // If there are no `init` members at all produce a tailored
      // diagnostic for that, otherwise fallback to generic
      // "no such member" one.
      if (result.ViableCandidates.empty() &&
          result.UnviableCandidates.empty()) {
        emitDiagnostic(anchor->getLoc(), diag::no_accessible_initializers,
                       instanceTy)
            .highlight(baseExpr->getSourceRange());
      } else {
        emitBasicError(baseType);
      }
    } else {
      emitBasicError(baseType);
    }
  } else if (auto moduleTy = baseType->getAs<ModuleType>()) {
    emitDiagnostic(baseExpr->getLoc(), diag::no_member_of_module,
                   moduleTy->getModule()->getName(), getName())
        .highlight(baseExpr->getSourceRange())
        .highlight(nameLoc.getSourceRange());
    return true;
  } else {
    // Check for a few common cases that can cause missing members.
    auto *ED = baseType->getEnumOrBoundGenericEnum();
    if (ED && getName().isSimpleName("rawValue")) {
      auto loc = ED->getNameLoc();
      if (loc.isValid()) {
        emitBasicError(baseType);
        emitDiagnostic(loc, diag::did_you_mean_raw_type);
        return true;
      }
    } else if (baseType->isAny()) {
      emitBasicError(baseType);
      emitDiagnostic(anchor->getLoc(), diag::any_as_anyobject_fixit)
          .fixItInsert(baseExpr->getStartLoc(), "(")
          .fixItInsertAfter(baseExpr->getEndLoc(), " as AnyObject)");
      return true;
    }
    
    tryTypoCorrection(baseType);
    
    // If locator points to the member found via key path dynamic member lookup,
    // we provide a custom diagnostic and emit typo corrections for the wrapper type too.
    if (getLocator()->isForKeyPathDynamicMemberLookup()) {
      auto baseExprType = getType(baseExpr)->getWithoutSpecifierType();
      
      tryTypoCorrection(baseExprType);
      
      if (auto correction = corrections.claimUniqueCorrection()) {
        auto diagnostic = emitDiagnostic(
            anchor->getLoc(),
            diag::could_not_find_value_dynamic_member_corrected,
            baseExprType, baseType, getName(),
            correction->CorrectedName);
        diagnostic.highlight(baseExpr->getSourceRange())
            .highlight(nameLoc.getSourceRange());
        correction->addFixits(diagnostic);
      } else {
        auto diagnostic = emitDiagnostic(
            anchor->getLoc(),
            diag::could_not_find_value_dynamic_member,
            baseExprType, baseType, getName());
        diagnostic.highlight(baseExpr->getSourceRange())
            .highlight(nameLoc.getSourceRange());
      }
    } else {
      if (auto correction = corrections.claimUniqueCorrection()) {
        auto diagnostic = emitDiagnostic(
            anchor->getLoc(),
            diag::could_not_find_value_member_corrected,
            baseType, getName(),
            correction->CorrectedName);
        diagnostic.highlight(baseExpr->getSourceRange())
            .highlight(nameLoc.getSourceRange());
        correction->addFixits(diagnostic);
      } else {
        emitBasicError(baseType);
      }
    }
  }

  // Note all the correction candidates.
  corrections.noteAllCandidates();
  return true;
}

bool InvalidMemberRefOnExistential::diagnoseAsError() {
  auto *anchor = getRawAnchor();

  Expr *baseExpr = getAnchor();
  DeclNameLoc nameLoc;
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor)) {
    baseExpr = UDE->getBase();
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = dyn_cast<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  } else if (auto *SE = dyn_cast<SubscriptExpr>(anchor)) {
    baseExpr = SE->getBase();
  } else if (auto *call = dyn_cast<CallExpr>(anchor)) {
    baseExpr = call->getFn();
  }

  emitDiagnostic(getAnchor()->getLoc(),
                 diag::could_not_use_member_on_existential, getBaseType(),
                 getName())
      .highlight(nameLoc.getSourceRange())
      .highlight(baseExpr->getSourceRange());
  return true;
}

bool AllowTypeOrInstanceMemberFailure::diagnoseAsError() {
  auto loc = getAnchor()->getLoc();
  auto &cs = getConstraintSystem();
  auto *DC = getDC();
  auto locator = getLocator();

  if (loc.isInvalid()) {
    return true;
  }

  Expr *expr = findParentExpr(getAnchor());
  SourceRange baseRange = expr ? expr->getSourceRange() : SourceRange();

  // If the base is an implicit self type reference, and we're in a
  // an initializer, then the user wrote something like:
  //
  //   class Foo { let x = 1, y = x }
  //
  // which runs in type context, not instance context, or
  //
  //   class Bar {
  //     let otherwise = 1              // instance member
  //     var x: Int
  //     func init(x: Int =otherwise) { // default parameter
  //       self.x = x
  //     }
  //   }
  //
  // in which an instance member is used as a default value for a
  // parameter.
  //
  // Produce a tailored diagnostic for these cases since this
  // comes up and is otherwise non-obvious what is going on.

  if (Name.isSimpleName(DeclBaseName::createConstructor()) &&
      !BaseType->is<AnyMetatypeType>()) {
    if (auto ctorRef = dyn_cast<UnresolvedDotExpr>(getRawAnchor())) {
      if (isa<SuperRefExpr>(ctorRef->getBase())) {
        emitDiagnostic(loc, diag::super_initializer_not_in_initializer);
        return true;
      }

      auto isCallArgument = [this](Expr *expr) {
        auto &cs = getConstraintSystem();
        auto argExpr = cs.getParentExpr(expr);
        if (!argExpr)
          return false;
        auto possibleApplyExpr = cs.getParentExpr(expr);
        return possibleApplyExpr && isa<ApplyExpr>(possibleApplyExpr);
      };

      auto *initCall = cs.getParentExpr(cs.getParentExpr(ctorRef));

      auto isMutable = [&DC](ValueDecl *decl) {
        if (auto *storage = dyn_cast<AbstractStorageDecl>(decl))
          return storage->isSettable(DC) && storage->isSetterAccessibleFrom(DC);

        return true;
      };

      auto *baseLoc = cs.getConstraintLocator(ctorRef->getBase());
      if (auto selection = getChoiceFor(baseLoc)) {
        OverloadChoice choice = selection->choice;
        if (choice.isDecl() && isMutable(choice.getDecl()) &&
            !isCallArgument(initCall) &&
            cs.getContextualTypePurpose() == CTP_Unused) {
          auto fixItLoc = ctorRef->getBase()->getSourceRange().End;
          emitDiagnostic(loc, diag::init_not_instance_member_use_assignment)
              .fixItInsertAfter(fixItLoc, " = ");
          return true;
        }

        SourceRange fixItRng = ctorRef->getBase()->getSourceRange();
        emitDiagnostic(loc, diag::init_not_instance_member)
            .fixItInsert(fixItRng.Start, "type(of: ")
            .fixItInsertAfter(fixItRng.End, ")");
        return true;
      }
    }
  }

  if (BaseType->is<AnyMetatypeType>() && !Member->isStatic()) {
    auto instanceTy = BaseType;

    if (auto *AMT = instanceTy->getAs<AnyMetatypeType>()) {
      instanceTy = AMT->getInstanceType();
    }

    if (getRawAnchor() &&
        cs.DC->getContextKind() == DeclContextKind::Initializer) {
      auto *TypeDC = cs.DC->getParent();
      bool propertyInitializer = true;
      // If the parent context is not a type context, we expect it
      // to be a defaulted parameter in a function declaration.
      if (!TypeDC->isTypeContext()) {
        assert(TypeDC->getContextKind() ==
               DeclContextKind::AbstractFunctionDecl &&
               "Expected function decl context for initializer!");
        TypeDC = TypeDC->getParent();
        propertyInitializer = false;
      }
      
      assert(TypeDC->isTypeContext() && "Expected type decl context!");
      
      if (TypeDC->getSelfNominalTypeDecl() == instanceTy->getAnyNominal()) {
        if (propertyInitializer) {
          emitDiagnostic(loc, diag::instance_member_in_initializer, Name);
          return true;
        } else {
          emitDiagnostic(loc, diag::instance_member_in_default_parameter, Name);
          return true;
        }
      }
    }

    auto maybeCallExpr = getRawAnchor();

    if (auto UDE = dyn_cast<UnresolvedDotExpr>(maybeCallExpr)) {
      maybeCallExpr = UDE->getBase();
    }

    if (auto callExpr = dyn_cast<ApplyExpr>(maybeCallExpr)) {
      auto fnExpr = callExpr->getFn();
      auto fnType = cs.getType(fnExpr)->getRValueType();
      auto arg = callExpr->getArg();

      if (fnType->is<ExistentialMetatypeType>()) {
        emitDiagnostic(arg->getStartLoc(),
                       diag::missing_init_on_metatype_initialization)
            .highlight(fnExpr->getSourceRange());
        return true;
      }
    }

    // Check whether the instance member is declared on parent context and if so
    // provide more specialized message.
    auto memberTypeContext =
        Member->getDeclContext()->getInnermostTypeContext();
    auto currentTypeContext = cs.DC->getInnermostTypeContext();
    
    if (memberTypeContext && currentTypeContext &&
        memberTypeContext->getSemanticDepth() <
        currentTypeContext->getSemanticDepth()) {
      emitDiagnostic(loc, diag::could_not_use_instance_member_on_type,
                     currentTypeContext->getDeclaredInterfaceType(), Name,
                     memberTypeContext->getDeclaredInterfaceType(), true)
          .highlight(baseRange)
          .highlight(Member->getSourceRange());
      return true;
    }

    if (auto *UDE = dyn_cast<UnresolvedDotExpr>(getRawAnchor())) {
      auto *baseExpr = UDE->getBase();
      if (isa<TypeExpr>(baseExpr)) {
        emitDiagnostic(loc, diag::instance_member_use_on_type, instanceTy, Name)
            .highlight(baseExpr->getSourceRange());
        return true;
      }
    }

    // Just emit a generic "instance member cannot be used" error
    emitDiagnostic(loc, diag::could_not_use_instance_member_on_type, instanceTy,
                   Name, instanceTy, false)
        .highlight(getAnchor()->getSourceRange());
    return true;
  } else {
    // If the base of the lookup is a protocol metatype, suggest
    // to replace the metatype with 'Self'
    // error saying the lookup cannot be on a protocol metatype
    Optional<InFlightDiagnostic> Diag;
    auto baseTy = BaseType;

    if (auto metatypeTy = baseTy->getAs<AnyMetatypeType>()) {
      auto instanceTy = metatypeTy->getInstanceType();

      // This will only happen if we have an unresolved dot expression
      // (.foo) where foo is a protocol member and the contextual type is
      // an optional protocol metatype.
      if (auto objectTy = instanceTy->getOptionalObjectType()) {
        instanceTy = objectTy;
        baseTy = MetatypeType::get(objectTy);
      }

      if (instanceTy->isExistentialType()) {
        // Give a customized message if we're accessing a member type
        // of a protocol -- otherwise a diagnostic talking about
        // static members doesn't make a whole lot of sense
        if (auto TAD = dyn_cast<TypeAliasDecl>(Member)) {
          Diag.emplace(emitDiagnostic(loc, diag::typealias_outside_of_protocol,
                                      TAD->getName()));
        } else if (auto ATD = dyn_cast<AssociatedTypeDecl>(Member)) {
          Diag.emplace(emitDiagnostic(loc, diag::assoc_type_outside_of_protocol,
                                      ATD->getName()));
        } else if (isa<ConstructorDecl>(Member)) {
          Diag.emplace(emitDiagnostic(loc, diag::construct_protocol_by_name,
                                      instanceTy));
        } else {
          Diag.emplace(emitDiagnostic(
              loc, diag::could_not_use_type_member_on_protocol_metatype, baseTy,
              Name));
        }

        Diag->highlight(baseRange).highlight(getAnchor()->getSourceRange());

        // See through function decl context
        if (auto parent = cs.DC->getInnermostTypeContext()) {
          // If we are in a protocol extension of 'Proto' and we see
          // 'Proto.static', suggest 'Self.static'
          if (auto extensionContext = parent->getExtendedProtocolDecl()) {
            if (extensionContext->getDeclaredType()->isEqual(instanceTy)) {
              Diag->fixItReplace(getAnchor()->getSourceRange(), "Self");
            }
          }
        }

        return true;
      }
    }

    // If this is a reference to a static member by one of the key path
    // components, let's provide a tailored diagnostic and return because
    // that is unsupported so there is no fix-it.
    if (locator->isForKeyPathComponent()) {
      InvalidStaticMemberRefInKeyPath failure(cs, Member, locator);
      return failure.diagnoseAsError();
    }

    if (isa<EnumElementDecl>(Member)) {
      Diag.emplace(emitDiagnostic(
          loc, diag::could_not_use_enum_element_on_instance, Name));
    } else {
      Diag.emplace(emitDiagnostic(
          loc, diag::could_not_use_type_member_on_instance, baseTy, Name));
    }

    Diag->highlight(getAnchor()->getSourceRange());

    if (Name.isSimpleName(DeclBaseName::createConstructor()) &&
        !baseTy->is<AnyMetatypeType>()) {
      if (auto ctorRef = dyn_cast<UnresolvedDotExpr>(getRawAnchor())) {
        SourceRange fixItRng = ctorRef->getNameLoc().getSourceRange();
        Diag->fixItInsert(fixItRng.Start, "type(of: ");
        Diag->fixItInsertAfter(fixItRng.End, ")");
        return true;
      }
    }

    // Determine the contextual type of the expression
    Type contextualType;
    for (auto iterateCS = &cs; contextualType.isNull() && iterateCS;
         iterateCS = iterateCS->baseCS) {
      contextualType = iterateCS->getContextualType();
    }
    
    // Try to provide a fix-it that only contains a '.'
    if (contextualType && baseTy->isEqual(contextualType)) {
      Diag->fixItInsert(loc, ".");
      return true;
    }

    // Check if the expression is the matching operator ~=, most often used in
    // case statements. If so, try to provide a single dot fix-it
    const Expr *contextualTypeNode = nullptr;
    ConstraintSystem *lastCS = nullptr;
    for (auto iterateCS = &cs; iterateCS; iterateCS = iterateCS->baseCS) {
      lastCS = iterateCS;
      contextualTypeNode = iterateCS->getContextualTypeNode();
    }
    
    // The '~=' operator is an overloaded decl ref inside a binaryExpr
    if (auto binaryExpr = dyn_cast<BinaryExpr>(contextualTypeNode)) {
      if (auto overloadedFn
          = dyn_cast<OverloadedDeclRefExpr>(binaryExpr->getFn())) {
        if (!overloadedFn->getDecls().empty()) {
          // Fetch any declaration to check if the name is '~='
          ValueDecl *decl0 = overloadedFn->getDecls()[0];
          
          if (decl0->getBaseName() == decl0->getASTContext().Id_MatchOperator) {
            assert(binaryExpr->getArg()->getElements().size() == 2);
            
            // If the rhs of '~=' is the enum type, a single dot suffixes
            // since the type can be inferred
            Type secondArgType =
            lastCS->getType(binaryExpr->getArg()->getElement(1));
            if (secondArgType->isEqual(baseTy)) {
              Diag->fixItInsert(loc, ".");
              return true;
            }
          }
        }
      }
    }

    // Fall back to a fix-it with a full type qualifier
    if (auto *NTD = Member->getDeclContext()->getSelfNominalTypeDecl()) {
      auto type = NTD->getSelfInterfaceType();
      if (auto *SE = dyn_cast<SubscriptExpr>(getRawAnchor())) {
        auto *baseExpr = SE->getBase();
        Diag->fixItReplace(baseExpr->getSourceRange(), diag::replace_with_type,
                           type);
      } else {
        Diag->fixItInsert(loc, diag::insert_type_qualification, type);
      }
    }

    return true;
  }

  return false;
}

bool PartialApplicationFailure::diagnoseAsError() {
  auto &cs = getConstraintSystem();
  auto *anchor = cast<UnresolvedDotExpr>(getRawAnchor());

  RefKind kind = RefKind::MutatingMethod;

  // If this is initializer delegation chain, we have a tailored message.
  if (getOverloadChoiceIfAvailable(cs.getConstraintLocator(
          anchor, ConstraintLocator::ConstructorMember))) {
    kind = anchor->getBase()->isSuperExpr() ? RefKind::SuperInit
                                            : RefKind::SelfInit;
  }

  auto diagnostic = CompatibilityWarning
                        ? diag::partial_application_of_function_invalid_swift4
                        : diag::partial_application_of_function_invalid;

  emitDiagnostic(anchor->getNameLoc(), diagnostic, kind);
  return true;
}

bool InvalidDynamicInitOnMetatypeFailure::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  emitDiagnostic(anchor->getLoc(), diag::dynamic_construct_class,
                 BaseType->getMetatypeInstanceType())
      .highlight(BaseRange);
  emitDiagnostic(Init, diag::note_nonrequired_initializer, Init->isImplicit(),
                 Init->getFullName());
  return true;
}

bool InitOnProtocolMetatypeFailure::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  if (IsStaticallyDerived) {
    emitDiagnostic(anchor->getLoc(), diag::construct_protocol_by_name,
                   BaseType->getMetatypeInstanceType())
        .highlight(BaseRange);
  } else {
    emitDiagnostic(anchor->getLoc(), diag::construct_protocol_value, BaseType)
        .highlight(BaseRange);
  }

  return true;
}

bool ImplicitInitOnNonConstMetatypeFailure::diagnoseAsError() {
  auto *apply = cast<ApplyExpr>(getRawAnchor());
  auto loc = apply->getArg()->getStartLoc();
  emitDiagnostic(loc, diag::missing_init_on_metatype_initialization)
      .fixItInsert(loc, ".init");
  return true;
}

bool MissingArgumentsFailure::diagnoseAsError() {
  auto &cs = getConstraintSystem();
  auto *locator = getLocator();
  auto path = locator->getPath();

  if (path.empty() ||
      !(path.back().getKind() == ConstraintLocator::ApplyArgToParam ||
        path.back().getKind() == ConstraintLocator::ContextualType ||
        path.back().getKind() == ConstraintLocator::ApplyArgument))
    return false;

  // If this is a misplaced `missng argument` situation, it would be
  // diagnosed by invalid conversion fix.
  if (isMisplacedMissingArgument(cs, locator))
    return false;

  auto *anchor = getAnchor();
  if (auto *captureList = dyn_cast<CaptureListExpr>(anchor))
    anchor = captureList->getClosureBody();

  if (auto *closure = dyn_cast<ClosureExpr>(anchor))
    return diagnoseClosure(closure);

  // This is a situation where function type is passed as an argument
  // to a function type parameter and their argument arity is different.
  //
  // ```
  // func foo(_: (Int) -> Void) {}
  // func bar() {}
  //
  // foo(bar) // `() -> Void` vs. `(Int) -> Void`
  // ```
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    auto info = *getFunctionArgApplyInfo(locator);

    auto *argExpr = info.getArgExpr();
    emitDiagnostic(argExpr->getLoc(), diag::cannot_convert_argument_value,
                   info.getArgType(), info.getParamType());
    // TODO: It would be great so somehow point out which arguments are missing.
    return true;
  }

  // Function type has fewer arguments than expected by context:
  //
  // ```
  // func foo() {}
  // let _: (Int) -> Void = foo
  // ```
  if (locator->isLastElement<LocatorPathElt::ContextualType>()) {
    auto &cs = getConstraintSystem();
    emitDiagnostic(anchor->getLoc(), diag::cannot_convert_initializer_value,
                   getType(anchor), resolveType(cs.getContextualType()));
    // TODO: It would be great so somehow point out which arguments are missing.
    return true;
  }

  if (diagnoseInvalidTupleDestructuring())
    return true;

  if (SynthesizedArgs.size() == 1)
    return diagnoseSingleMissingArgument();

  // At this point we know that this is a situation when
  // there are multiple arguments missing, so let's produce
  // a diagnostic which lists all of them and a fix-it
  // to add arguments at appropriate positions.

  SmallString<32> diagnostic;
  llvm::raw_svector_ostream arguments(diagnostic);

  interleave(
      SynthesizedArgs,
      [&](const AnyFunctionType::Param &arg) {
        if (arg.hasLabel()) {
          arguments << "'" << arg.getLabel().str() << "'";
        } else {
          auto *typeVar = arg.getPlainType()->castTo<TypeVariableType>();
          auto *locator = typeVar->getImpl().getLocator();
          auto paramIdx = locator->findLast<LocatorPathElt::ApplyArgToParam>()
                              ->getParamIdx();

          arguments << "#" << (paramIdx + 1);
        }
      },
      [&] { arguments << ", "; });

  auto diag = emitDiagnostic(anchor->getLoc(), diag::missing_arguments_in_call,
                             arguments.str());

  Expr *fnExpr = nullptr;
  Expr *argExpr = nullptr;
  unsigned numArguments = 0;
  bool hasTrailingClosure = false;

  std::tie(fnExpr, argExpr, numArguments, hasTrailingClosure) =
      getCallInfo(getRawAnchor());

  // TODO(diagnostics): We should be able to suggest this fix-it
  // unconditionally.
  if (argExpr && numArguments == 0) {
    SmallString<32> scratch;
    llvm::raw_svector_ostream fixIt(scratch);
    interleave(
        SynthesizedArgs,
        [&](const AnyFunctionType::Param &arg) { forFixIt(fixIt, arg); },
        [&] { fixIt << ", "; });

    auto *tuple = cast<TupleExpr>(argExpr);
    diag.fixItInsertAfter(tuple->getLParenLoc(), fixIt.str());
  }

  diag.flush();

  if (auto selectedOverload = getChoiceFor(locator)) {
    if (auto *decl = selectedOverload->choice.getDeclOrNull()) {
      emitDiagnostic(decl, diag::decl_declared_here, decl->getFullName());
    }
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseSingleMissingArgument() const {
  auto &ctx = getASTContext();

  auto *anchor = getRawAnchor();
  if (!(isa<CallExpr>(anchor) || isa<SubscriptExpr>(anchor) ||
        isa<UnresolvedMemberExpr>(anchor)))
    return false;

  if (SynthesizedArgs.size() != 1)
    return false;

  const auto &argument = SynthesizedArgs.front();
  auto *argType = argument.getPlainType()->castTo<TypeVariableType>();
  auto *argLocator = argType->getImpl().getLocator();
  auto position =
      argLocator->findLast<LocatorPathElt::ApplyArgToParam>()->getParamIdx();
  auto label = argument.getLabel();

  SmallString<32> insertBuf;
  llvm::raw_svector_ostream insertText(insertBuf);

  if (position != 0)
    insertText << ", ";

  forFixIt(insertText, argument);

  Expr *fnExpr = nullptr;
  Expr *argExpr = nullptr;
  unsigned insertableEndIdx = 0;
  bool hasTrailingClosure = false;

  std::tie(fnExpr, argExpr, insertableEndIdx, hasTrailingClosure) =
      getCallInfo(anchor);

  if (!argExpr)
    return false;

  if (hasTrailingClosure)
    insertableEndIdx -= 1;

  if (position == 0 && insertableEndIdx != 0)
    insertText << ", ";

  SourceLoc insertLoc;
  if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    // fn():
    //   fn([argMissing])
    // fn(argX, argY):
    //   fn([argMissing, ]argX, argY)
    //   fn(argX[, argMissing], argY)
    //   fn(argX, argY[, argMissing])
    // fn(argX) { closure }:
    //   fn([argMissing, ]argX) { closure }
    //   fn(argX[, argMissing]) { closure }
    //   fn(argX[, closureLabel: ]{closure}[, argMissing)] // Not impl.
    if (insertableEndIdx == 0)
      insertLoc = TE->getRParenLoc();
    else if (position != 0) {
      auto argPos = std::min(TE->getNumElements(), position) - 1;
      insertLoc = Lexer::getLocForEndOfToken(
          ctx.SourceMgr, TE->getElement(argPos)->getEndLoc());
    } else {
      insertLoc = TE->getElementNameLoc(0);
      if (insertLoc.isInvalid())
        insertLoc = TE->getElement(0)->getStartLoc();
    }
  } else {
    auto *PE = cast<ParenExpr>(argExpr);
    if (PE->getRParenLoc().isValid()) {
      // fn(argX):
      //   fn([argMissing, ]argX)
      //   fn(argX[, argMissing])
      // fn() { closure }:
      //   fn([argMissing]) {closure}
      //   fn([closureLabel: ]{closure}[, argMissing]) // Not impl.
      if (insertableEndIdx == 0)
        insertLoc = PE->getRParenLoc();
      else if (position == 0)
        insertLoc = PE->getSubExpr()->getStartLoc();
      else
        insertLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr,
                                               PE->getSubExpr()->getEndLoc());
    } else {
      // fn { closure }:
      //   fn[(argMissing)] { closure }
      //   fn[(closureLabel:] { closure }[, missingArg)]  // Not impl.
      assert(!isa<SubscriptExpr>(anchor) && "bracket less subscript");
      assert(PE->hasTrailingClosure() &&
             "paren less ParenExpr without trailing closure");
      insertBuf.insert(insertBuf.begin(), '(');
      insertBuf.insert(insertBuf.end(), ')');
      insertLoc =
          Lexer::getLocForEndOfToken(ctx.SourceMgr, fnExpr->getEndLoc());
    }
  }

  if (insertLoc.isInvalid())
    return false;

  if (label.empty()) {
    emitDiagnostic(insertLoc, diag::missing_argument_positional, position + 1)
        .fixItInsert(insertLoc, insertText.str());
  } else if (isPropertyWrapperInitialization()) {
    auto *TE = cast<TypeExpr>(fnExpr);
    emitDiagnostic(TE->getLoc(), diag::property_wrapper_missing_arg_init, label,
                   resolveType(TE->getInstanceType())->getString());
  } else {
    emitDiagnostic(insertLoc, diag::missing_argument_named, label)
        .fixItInsert(insertLoc, insertText.str());
  }

  if (auto selectedOverload = getChoiceFor(getLocator())) {
    if (auto *decl = selectedOverload->choice.getDeclOrNull()) {
      emitDiagnostic(decl, diag::decl_declared_here, decl->getFullName());
    }
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseClosure(ClosureExpr *closure) {
  auto &cs = getConstraintSystem();
  FunctionType *funcType = nullptr;

  auto *locator = getLocator();
  if (locator->isForContextualType()) {
    funcType = cs.getContextualType()->getAs<FunctionType>();
  } else if (auto info = getFunctionArgApplyInfo(locator)) {
    funcType = info->getParamType()->getAs<FunctionType>();
  }

  if (!funcType)
    return false;

  unsigned numSynthesized = SynthesizedArgs.size();
  auto diff = funcType->getNumParams() - numSynthesized;

  // If the closure didn't specify any arguments and it is in a context that
  // needs some, produce a fixit to turn "{...}" into "{ _,_ in ...}".
  if (diff == 0) {
    auto diag =
        emitDiagnostic(closure->getStartLoc(),
                       diag::closure_argument_list_missing, numSynthesized);

    std::string fixText; // Let's provide fixits for up to 10 args.
    if (funcType->getNumParams() <= 10) {
      fixText += " ";
      interleave(
          funcType->getParams(),
          [&fixText](const AnyFunctionType::Param &param) { fixText += '_'; },
          [&fixText] { fixText += ','; });
      fixText += " in ";
    }

    if (!fixText.empty()) {
      // Determine if there is already a space after the { in the closure to
      // make sure we introduce the right whitespace.
      auto afterBrace = closure->getStartLoc().getAdvancedLoc(1);
      auto text = getASTContext().SourceMgr.extractText({afterBrace, 1});
      if (text.size() == 1 && text == " ")
        fixText = fixText.erase(fixText.size() - 1);
      else
        fixText = fixText.erase(0, 1);
      diag.fixItInsertAfter(closure->getStartLoc(), fixText);
    }

    return true;
  }

  auto params = closure->getParameters();
  bool onlyAnonymousParams =
      std::all_of(params->begin(), params->end(),
                  [](ParamDecl *param) { return !param->hasName(); });

  auto diag = emitDiagnostic(
      params->getStartLoc(), diag::closure_argument_list_tuple,
      resolveType(funcType), funcType->getNumParams(), diff, diff == 1);

  // If the number of parameters is less than number of inferred
  // let's try to suggest a fix-it with the rest of the missing parameters.
  if (!closure->hasExplicitResultType() &&
      closure->getInLoc().isValid()) {
    SmallString<32> fixIt;
    llvm::raw_svector_ostream OS(fixIt);

    OS << ",";
    for (unsigned i = 0; i != numSynthesized; ++i) {
      OS << ((onlyAnonymousParams) ? "_" : "<#arg#>");
      OS << ((i == numSynthesized - 1) ? " " : ",");
    }

    diag.fixItInsertAfter(params->getEndLoc(), OS.str());
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseInvalidTupleDestructuring() const {
  auto *locator = getLocator();
  if (!locator->isLastElement<LocatorPathElt::ApplyArgument>())
    return false;

  if (SynthesizedArgs.size() < 2)
    return false;

  auto *anchor = getAnchor();

  Expr *argExpr = nullptr;
  // Something like `foo(x: (1, 2))`
  if (auto *TE = dyn_cast<TupleExpr>(anchor)) {
    if (TE->getNumElements() == 1)
      argExpr = TE->getElement(0);
  } else { // or `foo((1, 2))`
    argExpr = cast<ParenExpr>(anchor)->getSubExpr();
  }

  if (!(argExpr && getType(argExpr)->getRValueType()->is<TupleType>()))
    return false;

  auto selectedOverload = getChoiceFor(locator);
  if (!selectedOverload)
    return false;

  auto *decl = selectedOverload->choice.getDeclOrNull();
  if (!decl)
    return false;

  auto name = decl->getBaseName();
  auto diagnostic =
      emitDiagnostic(anchor->getLoc(),
                     diag::cannot_convert_single_tuple_into_multiple_arguments,
                     decl->getDescriptiveKind(), name, name.isSpecial(),
                     SynthesizedArgs.size(), isa<TupleExpr>(argExpr));

  // If argument is a literal tuple, let's suggest removal of parentheses.
  if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    diagnostic.fixItRemove(TE->getLParenLoc()).fixItRemove(TE->getRParenLoc());
  }

  diagnostic.flush();

  // Add a note which points to the overload choice location.
  emitDiagnostic(decl, diag::decl_declared_here, decl->getFullName());
  return true;
}

bool MissingArgumentsFailure::isPropertyWrapperInitialization() const {
  auto *call = dyn_cast<CallExpr>(getRawAnchor());
  if (!(call && call->isImplicit()))
    return false;

  auto TE = dyn_cast<TypeExpr>(call->getFn());
  if (!TE)
    return false;

  auto instanceTy = TE->getInstanceType();
  if (!instanceTy)
    return false;

  auto *NTD = resolveType(instanceTy)->getAnyNominal();
  return NTD && NTD->getAttrs().hasAttribute<PropertyWrapperAttr>();
}

bool MissingArgumentsFailure::isMisplacedMissingArgument(
    ConstraintSystem &cs, ConstraintLocator *locator) {
  auto *calleeLocator = cs.getCalleeLocator(locator);
  auto overloadChoice = cs.findSelectedOverloadFor(calleeLocator);
  if (!overloadChoice)
    return false;

  auto *fnType =
      cs.simplifyType(overloadChoice->ImpliedType)->getAs<FunctionType>();
  if (!(fnType && fnType->getNumParams() == 2))
    return false;

  auto *anchor = locator->getAnchor();

  auto hasFixFor = [&](FixKind kind, ConstraintLocator *locator) -> bool {
    auto fix = llvm::find_if(cs.getFixes(), [&](const ConstraintFix *fix) {
      return fix->getLocator() == locator;
    });

    if (fix == cs.getFixes().end())
      return false;

    return (*fix)->getKind() == kind;
  };

  auto *callLocator =
      cs.getConstraintLocator(anchor, ConstraintLocator::ApplyArgument);

  auto argFlags = fnType->getParams()[0].getParameterFlags();
  auto *argLoc = cs.getConstraintLocator(
      callLocator, LocatorPathElt::ApplyArgToParam(0, 0, argFlags));

  if (!(hasFixFor(FixKind::AllowArgumentTypeMismatch, argLoc) &&
        hasFixFor(FixKind::AddMissingArguments, callLocator)))
    return false;

  Expr *argExpr = nullptr;
  if (auto *call = dyn_cast<CallExpr>(anchor)) {
    argExpr = call->getArg();
  } else if (auto *subscript = dyn_cast<SubscriptExpr>(anchor)) {
    argExpr = subscript->getIndex();
  } else {
    return false;
  }

  Expr *argument = nullptr;
  if (auto *PE = dyn_cast<ParenExpr>(argExpr)) {
    argument = PE->getSubExpr();
  } else {
    auto *tuple = cast<TupleExpr>(argExpr);
    if (tuple->getNumElements() != 1)
      return false;
    argument = tuple->getElement(0);
  }

  auto argType = cs.simplifyType(cs.getType(argument));
  auto paramType = fnType->getParams()[1].getPlainType();

  return TypeChecker::isConvertibleTo(argType, paramType, cs.DC);
}

std::tuple<Expr *, Expr *, unsigned, bool>
MissingArgumentsFailure::getCallInfo(Expr *anchor) const {
  if (auto *call = dyn_cast<CallExpr>(anchor)) {
    return std::make_tuple(call->getFn(), call->getArg(),
                           call->getNumArguments(), call->hasTrailingClosure());
  } else if (auto *UME = dyn_cast<UnresolvedMemberExpr>(anchor)) {
    return std::make_tuple(UME, UME->getArgument(), UME->getNumArguments(),
                           UME->hasTrailingClosure());
  } else if (auto *SE = dyn_cast<SubscriptExpr>(anchor)) {
    return std::make_tuple(SE, SE->getIndex(), SE->getNumArguments(),
                           SE->hasTrailingClosure());
  }

  return std::make_tuple(nullptr, nullptr, 0, false);
}

void MissingArgumentsFailure::forFixIt(
    llvm::raw_svector_ostream &out,
    const AnyFunctionType::Param &argument) const {
  if (argument.hasLabel())
    out << argument.getLabel().str() << ": ";

  // Explode inout type.
  if (argument.isInOut())
    out << "&";

  auto resolvedType = resolveType(argument.getPlainType());
  // @autoclosure; the type should be the result type.
  if (argument.isAutoClosure())
    resolvedType = resolvedType->castTo<FunctionType>()->getResult();

  out << "<#" << resolvedType << "#>";
}

bool ClosureParamDestructuringFailure::diagnoseAsError() {
  auto *closure = cast<ClosureExpr>(getAnchor());
  auto params = closure->getParameters();

  // In case of implicit parameters e.g. $0, $1 we
  // can't really provide good fix-it because
  // structure of parameter type itself is unclear.
  for (auto *param : params->getArray()) {
    if (param->isImplicit()) {
      emitDiagnostic(params->getStartLoc(),
                     diag::closure_tuple_parameter_destructuring_implicit,
                     getParameterType());
      return true;
    }
  }

  auto diag = emitDiagnostic(params->getStartLoc(),
                             diag::closure_tuple_parameter_destructuring,
                             getParameterType());

  auto *closureBody = closure->getBody();
  if (!closureBody)
    return true;

  auto &sourceMgr = getASTContext().SourceMgr;
  auto bodyStmts = closureBody->getElements();

  SourceLoc bodyLoc;
  SourceLoc inLoc = closure->getInLoc();
  // If location for `in` is unknown we can't proceed
  // since we'll not be able to figure out source line
  // to place the fix-it on.
  if (inLoc.isInvalid())
    return true;

  // If the body is empty let's put the cursor
  // right after "in", otherwise make it start
  // location of the first statement in the body.
  if (bodyStmts.empty())
    bodyLoc = Lexer::getLocForEndOfToken(sourceMgr, inLoc);
  else
    bodyLoc = bodyStmts.front().getStartLoc();

  if (bodyLoc.isInvalid())
    return true;

  SmallString<64> fixIt;
  llvm::raw_svector_ostream OS(fixIt);

  // If this is multi-line closure we'd have to insert new lines
  // in the suggested 'let' to keep the structure of the code intact,
  // otherwise just use ';' to keep everything on the same line.
  auto inLine = sourceMgr.getLineNumber(inLoc);
  auto bodyLine = sourceMgr.getLineNumber(bodyLoc);
  auto isMultiLineClosure = bodyLine > inLine;
  auto indent =
      bodyStmts.empty() ? "" : Lexer::getIndentationForLine(sourceMgr, bodyLoc);

  SmallString<16> parameter;
  llvm::raw_svector_ostream parameterOS(parameter);

  parameterOS << "(";
  interleave(
      params->getArray(),
      [&](const ParamDecl *param) { parameterOS << param->getNameStr(); },
      [&] { parameterOS << ", "; });
  parameterOS << ")";

  // Check if there are any explicit types associated
  // with parameters, if there are, we'll have to add
  // type information to the replacement argument.
  bool explicitTypes =
      llvm::any_of(params->getArray(),
                   [](const ParamDecl *param) { return param->getTypeRepr(); });

  if (isMultiLineClosure)
    OS << '\n' << indent;

  // Let's form 'let <name> : [<type>]? = arg' expression.
  OS << "let " << parameterOS.str() << " = arg"
     << (isMultiLineClosure ? "\n" + indent : "; ");

  SmallString<64> argName;
  llvm::raw_svector_ostream nameOS(argName);
  if (explicitTypes) {
    nameOS << "(arg: " << getParameterType()->getString() << ")";
  } else {
    nameOS << "(arg)";
  }

  if (closure->hasSingleExpressionBody()) {
    // Let's see if we need to add result type to the argument/fix-it:
    //  - if the there is a result type associated with the closure;
    //  - and it's not a void type;
    //  - and it hasn't been explicitly written.
    auto resultType = resolveType(ContextualType->getResult());
    auto hasResult = [](Type resultType) -> bool {
      return resultType && !resultType->isVoid();
    };

    auto isValidType = [](Type resultType) -> bool {
      return resultType && !resultType->hasUnresolvedType() &&
             !resultType->hasTypeVariable();
    };

    // If there an expected result type but it hasn't been explicitly
    // provided, let's add it to the argument.
    if (hasResult(resultType) && !closure->hasExplicitResultType()) {
      nameOS << " -> ";
      if (isValidType(resultType))
        nameOS << resultType->getString();
      else
        nameOS << "<#Result#>";
    }

    if (auto stmt = bodyStmts.front().get<Stmt *>()) {
      // If the body is a single expression with implicit return.
      if (isa<ReturnStmt>(stmt) && stmt->isImplicit()) {
        // And there is non-void expected result type,
        // because we add 'let' expression to the body
        // we need to make such 'return' explicit.
        if (hasResult(resultType))
          OS << "return ";
      }
    }
  }

  diag.fixItReplace(params->getSourceRange(), nameOS.str())
      .fixItInsert(bodyLoc, OS.str());
  return true;
}

bool OutOfOrderArgumentFailure::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  auto *argExpr = isa<TupleExpr>(anchor) ? anchor
                                         : getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  auto *tuple = cast<TupleExpr>(argExpr);

  Identifier first = tuple->getElementName(ArgIdx);
  Identifier second = tuple->getElementName(PrevArgIdx);

  // Build a mapping from arguments to parameters.
  SmallVector<unsigned, 4> argBindings(tuple->getNumElements());
  for (unsigned paramIdx = 0; paramIdx != Bindings.size(); ++paramIdx) {
    for (auto argIdx : Bindings[paramIdx])
      argBindings[argIdx] = paramIdx;
  }

  auto argRange = [&](unsigned argIdx, Identifier label) -> SourceRange {
    auto range = tuple->getElement(argIdx)->getSourceRange();
    if (!label.empty())
      range.Start = tuple->getElementNameLoc(argIdx);

    unsigned paramIdx = argBindings[argIdx];
    if (Bindings[paramIdx].size() > 1)
      range.End = tuple->getElement(Bindings[paramIdx].back())->getEndLoc();

    return range;
  };

  auto firstRange = argRange(ArgIdx, first);
  auto secondRange = argRange(PrevArgIdx, second);

  SourceLoc diagLoc = firstRange.Start;

  auto addFixIts = [&](InFlightDiagnostic diag) {
    // Don't add Fix-Its if one of the ranges is outside of the argument
    // list, which can happen when we're splicing together an argument list
    // from multiple sources.
    auto &SM = getASTContext().SourceMgr;
    auto argsRange = tuple->getSourceRange();
    if (!SM.rangeContains(argsRange, firstRange) ||
        !SM.rangeContains(argsRange, secondRange))
      return;

    diag.highlight(firstRange).highlight(secondRange);

    // Move the misplaced argument by removing it from one location and
    // inserting it in another location. To maintain argument comma
    // separation, since the argument is always moving to an earlier index
    // the preceding comma and whitespace is removed and a new trailing
    // comma and space is inserted with the moved argument.
    auto text = SM.extractText(
        Lexer::getCharSourceRangeFromSourceRange(SM, firstRange));

    auto removalRange =
        SourceRange(Lexer::getLocForEndOfToken(
                        SM, tuple->getElement(ArgIdx - 1)->getEndLoc()),
                    firstRange.End);
    diag.fixItRemove(removalRange);
    diag.fixItInsert(secondRange.Start, text.str() + ", ");
  };

  // There are 4 diagnostic messages variations depending on
  // labeled/unlabeled arguments.
  if (first.empty() && second.empty()) {
    addFixIts(emitDiagnostic(diagLoc,
                             diag::argument_out_of_order_unnamed_unnamed,
                             ArgIdx + 1, PrevArgIdx + 1));
  } else if (first.empty() && !second.empty()) {
    addFixIts(emitDiagnostic(diagLoc, diag::argument_out_of_order_unnamed_named,
                             ArgIdx + 1, second));
  } else if (!first.empty() && second.empty()) {
    addFixIts(emitDiagnostic(diagLoc, diag::argument_out_of_order_named_unnamed,
                             first, PrevArgIdx + 1));
  } else {
    addFixIts(emitDiagnostic(diagLoc, diag::argument_out_of_order_named_named,
                             first, second));
  }
  return true;
}

bool ExtraneousArgumentsFailure::diagnoseAsError() {
  // Simplified anchor would point directly to the
  // argument in case of contextual mismatch.
  auto *anchor = getAnchor();
  if (auto *closure = dyn_cast<ClosureExpr>(anchor)) {
    auto fnType = ContextualType;
    auto params = closure->getParameters();

    auto diag = emitDiagnostic(
        params->getStartLoc(), diag::closure_argument_list_tuple, fnType,
        fnType->getNumParams(), params->size(), (params->size() == 1));

    bool onlyAnonymousParams =
        std::all_of(params->begin(), params->end(),
                    [](ParamDecl *param) { return !param->hasName(); });

    // If closure expects no parameters but N was given,
    // and all of them are anonymous let's suggest removing them.
    if (fnType->getNumParams() == 0 && onlyAnonymousParams) {
      auto inLoc = closure->getInLoc();
      auto &sourceMgr = getASTContext().SourceMgr;

      if (inLoc.isValid())
        diag.fixItRemoveChars(params->getStartLoc(),
                              Lexer::getLocForEndOfToken(sourceMgr, inLoc));
    }
    return true;
  }

  if (isContextualMismatch()) {
    auto *locator = getLocator();
    emitDiagnostic(anchor->getLoc(),
                   locator->isLastElement<LocatorPathElt::ContextualType>()
                       ? diag::cannot_convert_initializer_value
                       : diag::cannot_convert_argument_value,
                   getType(anchor), ContextualType);
    return true;
  }

  if (ExtraArgs.size() == 1) {
    return diagnoseSingleExtraArgument();
  }

  if (ContextualType->getNumParams() == 0) {
    if (auto argExpr = getArgumentListExprFor(getLocator())) {
      emitDiagnostic(anchor->getLoc(), diag::extra_argument_to_nullary_call)
          .highlight(argExpr->getSourceRange())
          .fixItRemove(argExpr->getSourceRange());
      return true;
    }
  }

  if (ExtraArgs.size() < 2)
    return false;

  llvm::SmallString<64> positions;
  llvm::raw_svector_ostream OS(positions);

  interleave(
      ExtraArgs,
      [&](const std::pair<unsigned, AnyFunctionType::Param> &arg) {
        OS << "#" << (arg.first + 1);
      },
      [&] { OS << ", "; });

  emitDiagnostic(anchor->getLoc(), diag::extra_arguments_in_call, OS.str());

  if (auto overload = getChoiceFor(getLocator())) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnostic(decl, diag::decl_declared_here, decl->getFullName());
    }
  }

  return true;
}

bool ExtraneousArgumentsFailure::diagnoseAsNote() {
  auto overload = getChoiceFor(getLocator());
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *decl = overload->choice.getDecl();
  auto *anchor = getAnchor();
  auto numArgs = getTotalNumArguments();
  emitDiagnostic(decl, diag::candidate_with_extraneous_args, ContextualType,
                 ContextualType->getNumParams(), numArgs, (numArgs == 1),
                 isa<ClosureExpr>(anchor));
  return true;
}

bool ExtraneousArgumentsFailure::diagnoseSingleExtraArgument() const {
  auto *arguments = getArgumentListExprFor(getLocator());
  if (!arguments)
    return false;

  const auto &e = ExtraArgs.front();
  auto index = e.first;
  auto argument = e.second;

  auto tuple = dyn_cast<TupleExpr>(arguments);
  auto argExpr = tuple ? tuple->getElement(index)
                       : cast<ParenExpr>(arguments)->getSubExpr();

  auto loc = argExpr->getLoc();
  if (tuple && index == tuple->getNumElements() - 1 &&
      tuple->hasTrailingClosure()) {
    emitDiagnostic(loc, diag::extra_trailing_closure_in_call)
        .highlight(argExpr->getSourceRange());
  } else if (ContextualType->getNumParams() == 0) {
    auto *PE = dyn_cast<ParenExpr>(arguments);
    Expr *subExpr = nullptr;
    if (PE)
      subExpr = PE->getSubExpr();

    if (subExpr && argument.getPlainType()->isVoid()) {
      emitDiagnostic(loc, diag::extra_argument_to_nullary_call)
          .fixItRemove(subExpr->getSourceRange());
    } else {
      emitDiagnostic(loc, diag::extra_argument_to_nullary_call)
          .highlight(argExpr->getSourceRange());
    }
  } else if (argument.hasLabel()) {
    emitDiagnostic(loc, diag::extra_argument_named, argument.getLabel())
        .highlight(argExpr->getSourceRange());
  } else {
    emitDiagnostic(loc, diag::extra_argument_positional)
        .highlight(argExpr->getSourceRange());
  }
  return true;
}

bool InaccessibleMemberFailure::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  // Let's try to avoid over-diagnosing chains of inaccessible
  // members e.g.:
  //
  // struct A {
  //   struct B {
  //     struct C {}
  //   }
  // }
  //
  // _ = A.B.C()
  //
  // We'll have a fix for each `B', `C` and `C.init` but it makes
  // sense to diagnose only `B` and consider the rest hidden.
  Expr *baseExpr = nullptr;
  DeclNameLoc nameLoc;
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor)) {
    baseExpr = UDE->getBase();
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = dyn_cast<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  } else if (auto *SE = dyn_cast<SubscriptExpr>(anchor)) {
    baseExpr = SE->getBase();
  } else if (auto *call = dyn_cast<CallExpr>(anchor)) {
    baseExpr = call->getFn();
  }

  if (baseExpr) {
    auto &cs = getConstraintSystem();
    auto *locator =
        cs.getConstraintLocator(baseExpr, ConstraintLocator::Member);
    if (llvm::any_of(cs.getFixes(), [&](const ConstraintFix *fix) {
          return fix->getLocator() == locator;
        }))
      return false;
  }

  auto loc = nameLoc.isValid() ? nameLoc.getStartLoc() : anchor->getLoc();
  auto accessLevel = Member->getFormalAccessScope().accessLevelForDiagnostics();
  if (auto *CD = dyn_cast<ConstructorDecl>(Member)) {
    emitDiagnostic(loc, diag::init_candidate_inaccessible,
                   CD->getResultInterfaceType(), accessLevel)
        .highlight(nameLoc.getSourceRange());
  } else {
    emitDiagnostic(loc, diag::candidate_inaccessible, Member->getBaseName(),
                   accessLevel)
        .highlight(nameLoc.getSourceRange());
  }

  emitDiagnostic(Member, diag::decl_declared_here, Member->getFullName());
  return true;
}

bool AnyObjectKeyPathRootFailure::diagnoseAsError() {
  // Diagnose use of AnyObject as root for a keypath

  auto anchor = getAnchor();
  auto loc = anchor->getLoc();
  auto range = anchor->getSourceRange();

  if (auto KPE = dyn_cast<KeyPathExpr>(anchor)) {
    if (auto rootTyRepr = KPE->getRootType()) {
      loc = rootTyRepr->getLoc();
      range = rootTyRepr->getSourceRange();
    }
  }

  emitDiagnostic(loc, diag::expr_swift_keypath_anyobject_root).highlight(range);
  return true;
}

bool KeyPathSubscriptIndexHashableFailure::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  auto *locator = getLocator();

  auto loc = anchor->getLoc();
  if (locator->isKeyPathSubscriptComponent()) {
    auto *KPE = cast<KeyPathExpr>(anchor);
    if (auto kpElt = locator->findFirst<LocatorPathElt::KeyPathComponent>())
      loc = KPE->getComponents()[kpElt->getIndex()].getLoc();
  }

  emitDiagnostic(loc, diag::expr_keypath_subscript_index_not_hashable,
                 resolveType(NonConformingType));
  return true;
}

SourceLoc InvalidMemberRefInKeyPath::getLoc() const {
  auto *anchor = getRawAnchor();

  if (auto *KPE = dyn_cast<KeyPathExpr>(anchor)) {
    auto *locator = getLocator();
    auto component = locator->findFirst<LocatorPathElt::KeyPathComponent>();
    assert(component);
    return KPE->getComponents()[component->getIndex()].getLoc();
  }

  return anchor->getLoc();
}

bool InvalidStaticMemberRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(getLoc(), diag::expr_keypath_static_member, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidMemberWithMutatingGetterInKeyPath::diagnoseAsError() {
  emitDiagnostic(getLoc(), diag::expr_keypath_mutating_getter, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidMethodRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(getLoc(), diag::expr_keypath_not_property, getKind(),
                 getName(), isForKeyPathDynamicMemberLookup());
  return true;
}

SourceLoc InvalidUseOfAddressOf::getLoc() const {
  auto *anchor = getAnchor();

  if (auto *assign = dyn_cast<AssignExpr>(anchor))
    anchor = assign->getSrc();

  return anchor->getLoc();
}

bool InvalidUseOfAddressOf::diagnoseAsError() {
  if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator())) {
    if (!argApplyInfo->getParameterFlags().isInOut()) {
      auto anchor = getAnchor();
      emitDiagnostic(anchor->getLoc(), diag::extra_address_of, getToType())
          .highlight(anchor->getSourceRange())
          .fixItRemove(anchor->getStartLoc());
      return true;
    }
  }

  emitDiagnostic(getLoc(), diag::extraneous_address_of);
  return true;
}

bool ExtraneousReturnFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  emitDiagnostic(anchor->getLoc(), diag::cannot_return_value_from_void_func);
  return true;
}

bool CollectionElementContextualFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto *locator = getLocator();

  // Check whether this is situation like `let _: [String: Int] = ["A", 0]`
  // which attempts to convert an array into a dictionary. We have a tailored
  // contextual diagnostic for that, so no need to diagnose element mismatches
  // as well.
  auto &cs = getConstraintSystem();
  auto *rawAnchor = getRawAnchor();
  if (llvm::any_of(cs.getFixes(), [&](const ConstraintFix *fix) -> bool {
        auto *locator = fix->getLocator();
        if (!(fix->getKind() == FixKind::ContextualMismatch &&
              locator->getAnchor() == rawAnchor))
          return false;

        auto *mismatch = static_cast<const ContextualMismatch *>(fix);
        return isInvalidDictionaryConversion(cs, rawAnchor,
                                             mismatch->getToType());
      }))
    return false;

  auto eltType = getFromType();
  auto contextualType = getToType();

  Optional<InFlightDiagnostic> diagnostic;
  if (isa<ArrayExpr>(getRawAnchor())) {
    diagnostic.emplace(emitDiagnostic(anchor->getLoc(),
                                      diag::cannot_convert_array_element,
                                      eltType, contextualType));
  }

  if (isa<DictionaryExpr>(getRawAnchor())) {
    auto eltLoc = locator->castLastElementTo<LocatorPathElt::TupleElement>();
    switch (eltLoc.getIndex()) {
    case 0: // key
      diagnostic.emplace(emitDiagnostic(anchor->getLoc(),
                                        diag::cannot_convert_dict_key, eltType,
                                        contextualType));
      break;

    case 1: // value
      diagnostic.emplace(emitDiagnostic(anchor->getLoc(),
                                        diag::cannot_convert_dict_value,
                                        eltType, contextualType));
      break;

    default:
      break;
    }
  }

  if (locator->isForSequenceElementType()) {
    diagnostic.emplace(
        emitDiagnostic(anchor->getLoc(),
                       contextualType->isExistentialType()
                           ? diag::cannot_convert_sequence_element_protocol
                           : diag::cannot_convert_sequence_element_value,
                       eltType, contextualType));
  }

  if (!diagnostic)
    return false;

  (void)trySequenceSubsequenceFixIts(*diagnostic);
  return true;
}

bool MissingContextualConformanceFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto path = getLocator()->getPath();

  Optional<Diag<Type, Type>> diagnostic;
  if (path.empty()) {
    assert(isa<AssignExpr>(anchor));
    if (isa<SubscriptExpr>(cast<AssignExpr>(anchor)->getDest())) {
      diagnostic =
          getDiagnosticFor(CTP_SubscriptAssignSource, /*forProtocol=*/true);
    } else {
      diagnostic = getDiagnosticFor(CTP_AssignSource, /*forProtocol=*/true);
    }
  } else {
    const auto &last = path.back();
    switch (last.getKind()) {
    case ConstraintLocator::ContextualType:
      assert(Context != CTP_Unused);
      diagnostic = getDiagnosticFor(Context, /*forProtocol=*/true);
      break;

    case ConstraintLocator::SequenceElementType: {
      diagnostic = diag::cannot_convert_sequence_element_protocol;
      break;
    }

    default:
      break;
    }
  }

  if (!diagnostic)
    return false;

  auto srcType = getFromType();
  auto dstType = getToType();

  emitDiagnostic(anchor->getLoc(), *diagnostic, srcType, dstType);

  if (isa<InOutExpr>(anchor))
    return true;

  if (srcType->isAny() && dstType->isAnyObject()) {
    emitDiagnostic(anchor->getLoc(), diag::any_as_anyobject_fixit)
        .fixItInsertAfter(anchor->getEndLoc(), " as AnyObject");
  }

  return true;
}

bool MissingGenericArgumentsFailure::hasLoc(GenericTypeParamType *GP) const {
  return GP->getDecl()->getStartLoc().isValid();
}

bool MissingGenericArgumentsFailure::diagnoseAsError() {
  llvm::SmallDenseMap<TypeRepr *, SmallVector<GenericTypeParamType *, 4>>
      scopedParameters;

  auto isScoped =
      findArgumentLocations([&](TypeRepr *base, GenericTypeParamType *GP) {
        scopedParameters[base].push_back(GP);
      });

  if (!isScoped)
    return diagnoseForAnchor(getAnchor(), Parameters);

  bool diagnosed = false;
  for (const auto &scope : scopedParameters)
    diagnosed |= diagnoseForAnchor(scope.first, scope.second);
  return diagnosed;
}

bool MissingGenericArgumentsFailure::diagnoseForAnchor(
    Anchor anchor, ArrayRef<GenericTypeParamType *> params) const {
  bool diagnosed = false;
  for (auto *GP : params)
    diagnosed |= diagnoseParameter(anchor, GP);

  if (!diagnosed)
    return false;

  auto *DC = getDeclContext();
  if (!DC)
    return true;

  if (auto *SD = dyn_cast<SubscriptDecl>(DC)) {
    emitDiagnostic(SD, diag::note_call_to_subscript, SD->getFullName());
    return true;
  }

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
    if (isa<ConstructorDecl>(AFD)) {
      emitDiagnostic(AFD, diag::note_call_to_initializer);
    } else {
      emitDiagnostic(AFD,
                     AFD->isOperator() ? diag::note_call_to_operator
                                       : diag::note_call_to_func,
                     AFD->getFullName());
    }
    return true;
  }

  emitGenericSignatureNote(anchor);
  return true;
}

bool MissingGenericArgumentsFailure::diagnoseParameter(
    Anchor anchor, GenericTypeParamType *GP) const {
  auto &cs = getConstraintSystem();

  auto loc = anchor.is<Expr *>() ? anchor.get<Expr *>()->getLoc()
                                 : anchor.get<TypeRepr *>()->getLoc();

  auto *locator = getLocator();
  // Type variables associated with missing generic parameters are
  // going to be completely cut off from the rest of constraint system,
  // that's why we'd get two fixes in this case which is not ideal.
  if (locator->isForContextualType() &&
      llvm::count_if(cs.DefaultedConstraints,
                     [&GP](const ConstraintLocator *locator) {
                       return locator->getGenericParameter() == GP;
                     }) > 1) {
    return false;
  }

  if (auto *CE = dyn_cast<ExplicitCastExpr>(getRawAnchor())) {
    auto castTo = getType(CE->getCastTypeLoc());
    auto *NTD = castTo->getAnyNominal();
    emitDiagnostic(loc, diag::unbound_generic_parameter_cast, GP,
                   NTD ? NTD->getDeclaredType() : castTo);
  } else {
    emitDiagnostic(loc, diag::unbound_generic_parameter, GP);
  }

  Type baseTyForNote;
  auto *DC = getDeclContext();
  if (!DC)
    return true;

  if (!hasLoc(GP))
    return true;

  if (auto *NTD =
          dyn_cast_or_null<NominalTypeDecl>(DC->getSelfNominalTypeDecl())) {
    baseTyForNote = NTD->getDeclaredType();
  } else if (auto *TAD = dyn_cast<TypeAliasDecl>(DC)) {
    baseTyForNote = TAD->getUnboundGenericType();
  } else {
    return true;
  }

  emitDiagnostic(GP->getDecl(), diag::archetype_declared_in_type, GP,
                 baseTyForNote);
  return true;
}

void MissingGenericArgumentsFailure::emitGenericSignatureNote(
    Anchor anchor) const {
  auto &cs = getConstraintSystem();
  auto *paramDC = getDeclContext();

  if (!paramDC)
    return;

  auto *GTD = dyn_cast<GenericTypeDecl>(paramDC);
  if (!GTD || anchor.is<Expr *>())
    return;

  auto getParamDecl =
      [](const ConstraintLocator *locator) -> GenericTypeParamDecl * {
    return locator->isForGenericParameter()
               ? locator->getGenericParameter()->getDecl()
               : nullptr;
  };

  llvm::SmallDenseMap<GenericTypeParamDecl *, Type> params;
  for (auto *typeVar : cs.getTypeVariables()) {
    auto *GP = typeVar->getImpl().getGenericParameter();
    if (!GP)
      continue;

    auto type = resolveType(typeVar);
    // This could happen if the diagnostic is used by CSDiag.
    if (type->is<TypeVariableType>())
      continue;

    // If this is one of the defaulted parameter types, attempt
    // to emit placeholder for it instead of `Any`.
    if (llvm::any_of(cs.DefaultedConstraints,
                     [&](const ConstraintLocator *locator) {
                       return GP->getDecl() == getParamDecl(locator);
                     }))
      continue;

    params[GP->getDecl()] = type;
  }

  auto getPreferredType = [&](const GenericTypeParamDecl *GP) -> Type {
    auto type = params.find(GP);
    return (type == params.end()) ? Type() : type->second;
  };

  SmallString<64> paramsAsString;
  auto baseType = anchor.get<TypeRepr *>();
  if (TypeChecker::getDefaultGenericArgumentsString(paramsAsString, GTD,
                                                    getPreferredType)) {
    auto diagnostic = emitDiagnostic(
        baseType->getLoc(), diag::unbound_generic_parameter_explicit_fix);

    if (auto *genericTy = dyn_cast<GenericIdentTypeRepr>(baseType)) {
      // If some of the eneric arguments have been specified, we need to
      // replace existing signature with a new one.
      diagnostic.fixItReplace(genericTy->getAngleBrackets(), paramsAsString);
    } else {
      // Otherwise we can simply insert new generic signature.
      diagnostic.fixItInsertAfter(baseType->getEndLoc(), paramsAsString);
    }
  }
}

bool MissingGenericArgumentsFailure::findArgumentLocations(
    llvm::function_ref<void(TypeRepr *, GenericTypeParamType *)> callback) {
  using Callback = llvm::function_ref<void(TypeRepr *, GenericTypeParamType *)>;

  auto *anchor = getRawAnchor();

  TypeLoc typeLoc;
  if (auto *TE = dyn_cast<TypeExpr>(anchor))
    typeLoc = TE->getTypeLoc();
  else if (auto *ECE = dyn_cast<ExplicitCastExpr>(anchor))
    typeLoc = ECE->getCastTypeLoc();

  if (!typeLoc.hasLocation())
    return false;

  struct AssociateMissingParams : public ASTWalker {
    llvm::SmallVector<GenericTypeParamType *, 4> Params;
    Callback Fn;

    AssociateMissingParams(ArrayRef<GenericTypeParamType *> params,
                           Callback callback)
        : Params(params.begin(), params.end()), Fn(callback) {}

    bool walkToTypeReprPre(TypeRepr *T) override {
      if (Params.empty())
        return false;

      auto *ident = dyn_cast<ComponentIdentTypeRepr>(T);
      if (!ident)
        return true;

      auto *decl = dyn_cast_or_null<GenericTypeDecl>(ident->getBoundDecl());
      if (!decl)
        return true;

      auto *paramList = decl->getGenericParams();
      if (!paramList)
        return true;

      // There could a situation like `S<S>()`, so we need to be
      // careful not to point at first `S` because it has all of
      // its generic parameters specified.
      if (auto *generic = dyn_cast<GenericIdentTypeRepr>(ident)) {
        if (paramList->size() == generic->getNumGenericArgs())
          return true;
      }

      for (auto *candidate : paramList->getParams()) {
        auto result =
            llvm::find_if(Params, [&](const GenericTypeParamType *param) {
              return candidate == param->getDecl();
            });

        if (result != Params.end()) {
          Fn(ident, *result);
          Params.erase(result);
        }
      }

      // Keep walking.
      return true;
    }

    bool allParamsAssigned() const { return Params.empty(); }

  } associator(Parameters, callback);

  typeLoc.getTypeRepr()->walk(associator);
  return associator.allParamsAssigned();
}

void SkipUnhandledConstructInFunctionBuilderFailure::diagnosePrimary(
    bool asNote) {
  if (auto stmt = unhandled.dyn_cast<Stmt *>()) {
    emitDiagnostic(stmt->getStartLoc(),
                   asNote? diag::note_function_builder_control_flow
                         : diag::function_builder_control_flow,
                   builder->getFullName());
  } else {
    auto decl = unhandled.get<Decl *>();
    emitDiagnostic(decl,
                   asNote ? diag::note_function_builder_decl
                          : diag::function_builder_decl,
                   builder->getFullName());
  }
}

bool SkipUnhandledConstructInFunctionBuilderFailure::diagnoseAsError() {
  diagnosePrimary(/*asNote=*/false);
  emitDiagnostic(builder,
                 diag::kind_declname_declared_here,
                 builder->getDescriptiveKind(),
                 builder->getFullName());
  return true;
}

bool SkipUnhandledConstructInFunctionBuilderFailure::diagnoseAsNote() {
  diagnosePrimary(/*asNote=*/true);
  return true;
}

bool MutatingMemberRefOnImmutableBase::diagnoseAsError() {
  auto *anchor = getRawAnchor();
  auto baseExpr = getBaseExprFor(anchor);
  if (!baseExpr)
    return false;

  auto diagIDsubelt = diag::cannot_pass_rvalue_mutating_subelement;
  auto diagIDmember = diag::cannot_pass_rvalue_mutating;

  if (auto *storage = dyn_cast<AbstractStorageDecl>(Member)) {
    if (storage->isGetterMutating()) {
      diagIDsubelt = diag::cannot_pass_rvalue_mutating_getter_subelement;
      diagIDmember = diag::cannot_pass_rvalue_mutating_getter;
    }
  }

  auto &cs = getConstraintSystem();
  AssignmentFailure failure(baseExpr, cs, anchor->getLoc(), diagIDsubelt,
                            diagIDmember);
  return failure.diagnoseAsError();
}

bool InvalidTupleSplatWithSingleParameterFailure::diagnoseAsError() {
  auto selectedOverload = getChoiceFor(getLocator());
  if (!selectedOverload || !selectedOverload->choice.isDecl())
    return false;

  auto *choice = selectedOverload->choice.getDecl();

  auto *argExpr = getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  using Substitution = std::pair<GenericTypeParamType *, Type>;
  llvm::SmallVector<Substitution, 8> substitutions;

  auto paramTy = restoreGenericParameters(
      ParamType, [&](GenericTypeParamType *GP, Type resolvedType) {
        substitutions.push_back(std::make_pair(GP, resolvedType));
      });

  DeclBaseName name = choice->getBaseName();

  std::string subsStr;
  if (!substitutions.empty()) {
    llvm::array_pod_sort(
        substitutions.begin(), substitutions.end(),
        [](const std::pair<GenericTypeParamType *, Type> *lhs,
           const std::pair<GenericTypeParamType *, Type> *rhs) -> int {
          GenericParamKey key1(lhs->first);
          GenericParamKey key2(rhs->first);
          return key1 < key2 ? -1 : (key1 == key2) ? 0 : 1;
        });

    subsStr += " [with ";
    interleave(
        substitutions,
        [&subsStr](const Substitution &substitution) {
          subsStr += substitution.first->getString();
          subsStr += " = ";
          subsStr += substitution.second->getString();
        },
        [&subsStr] { subsStr += ", "; });
    subsStr += ']';
  }

  auto diagnostic =
      name.isSpecial()
          ? emitDiagnostic(argExpr->getLoc(),
                           diag::single_tuple_parameter_mismatch_special,
                           choice->getDescriptiveKind(), paramTy, subsStr)
          : emitDiagnostic(argExpr->getLoc(),
                           diag::single_tuple_parameter_mismatch_normal,
                           choice->getDescriptiveKind(), name, paramTy, subsStr);


  auto newLeftParenLoc = argExpr->getStartLoc();
  if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    auto firstArgLabel = TE->getElementName(0);
    // Cover situations like:
    //
    // func foo(x: (Int, Int)) {}
    // foo(x: 0, 1)
    //
    // Where left paren should be suggested after the label,
    // since the label belongs to the parameter itself.
    if (!firstArgLabel.empty()) {
      auto paramTuple = resolveType(ParamType)->castTo<TupleType>();
      // If the label of the first argument matches the one required
      // by the parameter it would be omitted from the fixed parameter type.
      if (!paramTuple->getElement(0).hasName())
        newLeftParenLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                     TE->getElementNameLoc(0));
    }
  }

  diagnostic.highlight(argExpr->getSourceRange())
      .fixItInsertAfter(newLeftParenLoc, "(")
      .fixItInsert(argExpr->getEndLoc(), ")");

  return true;
}

bool ThrowingFunctionConversionFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  emitDiagnostic(anchor->getLoc(), diag::throws_functiontype_mismatch,
                 getFromType(), getToType());
  return true;
}

bool InOutConversionFailure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto *locator = getLocator();
  auto path = locator->getPath();

  if (!path.empty() &&
      path.back().getKind() == ConstraintLocator::FunctionArgument) {
    if (auto argApplyInfo = getFunctionArgApplyInfo(locator)) {
      emitDiagnostic(anchor->getLoc(), diag::cannot_convert_argument_value,
          argApplyInfo->getArgType(), argApplyInfo->getParamType());
    } else {
      assert(locator->findLast<LocatorPathElt::ContextualType>());
      auto contextualType = getConstraintSystem().getContextualType();
      auto purpose = getContextualTypePurpose();
      auto diagnostic = getDiagnosticFor(purpose, /*forProtocol=*/false);

      if (!diagnostic)
        return false;

      emitDiagnostic(anchor->getLoc(), *diagnostic, getType(anchor),
                     contextualType);
    }

    return true;
  }

  emitDiagnostic(anchor->getLoc(), diag::cannot_pass_rvalue_inout_converted,
                 getFromType(), getToType());
  fixItChangeArgumentType();
  return true;
}

void InOutConversionFailure::fixItChangeArgumentType() const {
  auto *argExpr = getAnchor();
  auto *DC = getDC();

  if (auto *IOE = dyn_cast<InOutExpr>(argExpr))
    argExpr = IOE->getSubExpr();

  auto *DRE = dyn_cast<DeclRefExpr>(argExpr);
  if (!DRE)
    return;

  auto *VD = dyn_cast_or_null<VarDecl>(DRE->getDecl());
  if (!VD)
    return;

  // Don't emit for non-local variables.
  // (But in script-mode files, we consider module-scoped
  // variables in the same file to be local variables.)
  auto VDC = VD->getDeclContext();
  bool isLocalVar = VDC->isLocalContext();
  if (!isLocalVar && VDC->isModuleScopeContext()) {
    auto argFile = DC->getParentSourceFile();
    auto varFile = VDC->getParentSourceFile();
    isLocalVar = (argFile == varFile && argFile->isScriptMode());
  }
  if (!isLocalVar)
    return;

  auto actualType = getFromType();
  auto neededType = getToType();

  SmallString<32> scratch;
  SourceLoc endLoc;   // Filled in if we decide to diagnose this
  SourceLoc startLoc; // Left invalid if we're inserting

  auto isSimpleTypelessPattern = [](Pattern *P) -> bool {
    if (auto VP = dyn_cast_or_null<VarPattern>(P))
      P = VP->getSubPattern();
    return P && isa<NamedPattern>(P);
  };

  auto typeRange = VD->getTypeSourceRangeForDiagnostics();
  if (typeRange.isValid()) {
    startLoc = typeRange.Start;
    endLoc = typeRange.End;
  } else if (isSimpleTypelessPattern(VD->getParentPattern())) {
    endLoc = VD->getNameLoc();
    scratch += ": ";
  }

  if (endLoc.isInvalid())
    return;

  scratch += neededType.getString();

  // Adjust into the location where we actually want to insert
  endLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr, endLoc);

  // Since we already adjusted endLoc, this will turn an insertion
  // into a zero-character replacement.
  if (!startLoc.isValid())
    startLoc = endLoc;

  emitDiagnostic(VD->getLoc(), diag::inout_change_var_type_if_possible,
                 actualType, neededType)
      .fixItReplaceChars(startLoc, endLoc, scratch);
}

bool ArgumentMismatchFailure::diagnoseAsError() {
  if (diagnoseMisplacedMissingArgument())
    return true;

  if (diagnoseConversionToBool())
    return true;

  if (diagnoseArchetypeMismatch())
    return true;

  if (diagnosePatternMatchingMismatch())
    return true;

  if (diagnoseUseOfReferenceEqualityOperator())
    return true;

  if (diagnosePropertyWrapperMismatch())
    return true;

  auto argType = getFromType();
  auto paramType = getToType();

  Diag<Type, Type> diagnostic = diag::cannot_convert_argument_value;

  // If parameter type is a protocol value, let's says that
  // argument doesn't conform to a give protocol.
  if (paramType->isExistentialType())
    diagnostic = diag::cannot_convert_argument_value_protocol;

  auto diag = emitDiagnostic(getLoc(), diagnostic, argType, paramType);

  // If argument is an l-value type and parameter is a pointer type,
  // let's match up its element type to the argument to see whether
  // it would be appropriate to suggest adding `&`.
  auto *argExpr = getAnchor();
  if (getType(argExpr, /*wantRValue=*/false)->is<LValueType>()) {
    auto elementTy = paramType->getAnyPointerElementType();
    if (elementTy && argType->isEqual(elementTy)) {
      diag.fixItInsert(argExpr->getStartLoc(), "&");
      return true;
    }
  }

  tryFixIts(diag);
  return true;
}

bool ArgumentMismatchFailure::diagnoseAsNote() {
  auto *locator = getLocator();
  if (auto *callee = getCallee()) {
    emitDiagnostic(callee, diag::candidate_has_invalid_argument_at_position,
                   getToType(), getParamPosition(),
                   locator->isLastElement<LocatorPathElt::LValueConversion>());
    return true;
  }

  return false;
}

bool ArgumentMismatchFailure::diagnoseUseOfReferenceEqualityOperator() const {
  auto *locator = getLocator();

  if (!isArgumentOfReferenceEqualityOperator(locator))
    return false;

  auto *binaryOp = cast<BinaryExpr>(getRawAnchor());
  auto *lhs = binaryOp->getArg()->getElement(0);
  auto *rhs = binaryOp->getArg()->getElement(1);

  auto name = *getOperatorName(binaryOp->getFn());

  auto lhsType = getType(lhs);
  auto rhsType = getType(rhs);

  // If both arguments where incorrect e.g. both are function types,
  // let's avoid producing a diagnostic second time, because first
  // one would cover both arguments.
  if (getAnchor() == rhs && rhsType->is<FunctionType>()) {
    auto &cs = getConstraintSystem();
    if (cs.hasFixFor(cs.getConstraintLocator(
            binaryOp, {ConstraintLocator::ApplyArgument,
                       LocatorPathElt::ApplyArgToParam(
                           0, 0, getParameterFlagsAtIndex(0))})))
      return true;
  }

  // Regardless of whether the type has reference or value semantics,
  // comparison with nil is illegal, albeit for different reasons spelled
  // out by the diagnosis.
  if (isa<NilLiteralExpr>(lhs) || isa<NilLiteralExpr>(rhs)) {
    std::string revisedName = name.str();
    revisedName.pop_back();

    auto loc = binaryOp->getLoc();
    auto nonNilType = isa<NilLiteralExpr>(lhs) ? rhsType : lhsType;
    auto nonNilExpr = isa<NilLiteralExpr>(lhs) ? rhs : lhs;

    // If we made it here, then we're trying to perform a comparison with
    // reference semantics rather than value semantics. The fixit will
    // lop off the extra '=' in the operator.
    if (nonNilType->getOptionalObjectType()) {
      emitDiagnostic(loc,
                     diag::value_type_comparison_with_nil_illegal_did_you_mean,
                     nonNilType)
          .fixItReplace(loc, revisedName);
    } else {
      emitDiagnostic(loc, diag::value_type_comparison_with_nil_illegal,
                     nonNilType)
          .highlight(nonNilExpr->getSourceRange());
    }

    return true;
  }

  if (lhsType->is<FunctionType>() || rhsType->is<FunctionType>()) {
    emitDiagnostic(binaryOp->getLoc(), diag::cannot_reference_compare_types,
                   name.str(), lhsType, rhsType)
        .highlight(lhs->getSourceRange())
        .highlight(rhs->getSourceRange());
    return true;
  }

  return false;
}

bool ArgumentMismatchFailure::diagnosePatternMatchingMismatch() const {
  if (!isArgumentOfPatternMatchingOperator(getLocator()))
    return false;

  auto *op = cast<BinaryExpr>(getRawAnchor());
  auto *lhsExpr = op->getArg()->getElement(0);
  auto *rhsExpr = op->getArg()->getElement(1);

  auto lhsType = getType(lhsExpr);
  auto rhsType = getType(rhsExpr);

  auto diagnostic =
      lhsType->is<UnresolvedType>()
          ? emitDiagnostic(
                getLoc(), diag::cannot_match_unresolved_expr_pattern_with_value,
                rhsType)
          : emitDiagnostic(getLoc(), diag::cannot_match_expr_pattern_with_value,
                           lhsType, rhsType);

  diagnostic.highlight(lhsExpr->getSourceRange());
  diagnostic.highlight(rhsExpr->getSourceRange());

  if (auto optUnwrappedType = rhsType->getOptionalObjectType()) {
    if (lhsType->isEqual(optUnwrappedType)) {
      diagnostic.fixItInsertAfter(lhsExpr->getEndLoc(), "?");
    }
  }

  return true;
}

bool ArgumentMismatchFailure::diagnoseArchetypeMismatch() const {
  auto *argTy = getFromType()->getAs<ArchetypeType>();
  auto *paramTy = getToType()->getAs<ArchetypeType>();

  if (!(argTy && paramTy))
    return false;

  // Produce this diagnostic only if the names
  // of the generic parameters are the same.
  if (argTy->getName() != paramTy->getName())
    return false;

  auto getGenericTypeDecl = [&](ArchetypeType *archetype) -> ValueDecl * {
    auto paramType = archetype->getInterfaceType();

    if (auto *GTPT = paramType->getAs<GenericTypeParamType>())
      return GTPT->getDecl();

    if (auto *DMT = paramType->getAs<DependentMemberType>())
      return DMT->getAssocType();

    return nullptr;
  };

  auto *argDecl = getGenericTypeDecl(argTy);
  auto *paramDecl = getGenericTypeDecl(paramTy);

  if (!(paramDecl && argDecl))
    return false;

  auto describeGenericType = [&](ValueDecl *genericParam,
                                 bool includeName = false) -> std::string {
    if (!genericParam)
      return "";

    Decl *parent = nullptr;
    if (auto *AT = dyn_cast<AssociatedTypeDecl>(genericParam)) {
      parent = AT->getProtocol();
    } else {
      auto *dc = genericParam->getDeclContext();
      parent = dc->getInnermostDeclarationDeclContext();
    }

    if (!parent)
      return "";

    llvm::SmallString<64> result;
    llvm::raw_svector_ostream OS(result);

    OS << Decl::getDescriptiveKindName(genericParam->getDescriptiveKind());

    if (includeName && genericParam->hasName())
      OS << " '" << genericParam->getBaseName() << "'";

    OS << " of ";
    OS << Decl::getDescriptiveKindName(parent->getDescriptiveKind());
    if (auto *decl = dyn_cast<ValueDecl>(parent)) {
      if (decl->hasName())
        OS << " '" << decl->getFullName() << "'";
    }

    return OS.str();
  };

  emitDiagnostic(
      getAnchor()->getLoc(), diag::cannot_convert_argument_value_generic, argTy,
      describeGenericType(argDecl), paramTy, describeGenericType(paramDecl));

  emitDiagnostic(argDecl, diag::descriptive_generic_type_declared_here,
                 describeGenericType(argDecl, true));

  emitDiagnostic(paramDecl, diag::descriptive_generic_type_declared_here,
                 describeGenericType(paramDecl, true));

  return true;
}

bool ArgumentMismatchFailure::diagnoseMisplacedMissingArgument() const {
  auto &cs = getConstraintSystem();
  auto *locator = getLocator();

  if (!MissingArgumentsFailure::isMisplacedMissingArgument(cs, locator))
    return false;

  auto *argType = cs.createTypeVariable(
      cs.getConstraintLocator(locator, LocatorPathElt::SynthesizedArgument(1)),
      /*flags=*/0);

  // Assign new type variable to a type of a parameter.
  auto *fnType = getFnType();
  const auto &param = fnType->getParams()[0];
  cs.assignFixedType(argType, param.getOldType());

  auto *anchor = getRawAnchor();

  MissingArgumentsFailure failure(
      cs, {param.withType(argType)},
      cs.getConstraintLocator(anchor, ConstraintLocator::ApplyArgument));

  return failure.diagnoseSingleMissingArgument();
}

bool ArgumentMismatchFailure::diagnosePropertyWrapperMismatch() const {
  auto argType = getFromType();
  auto paramType = getToType();

  // Verify that this is an implicit call to a property wrapper initializer
  // in a form of `init(wrappedValue:)` or deprecated `init(initialValue:)`.
  auto *call = dyn_cast<CallExpr>(getRawAnchor());
  if (!(call && call->isImplicit() && isa<TypeExpr>(call->getFn()) &&
        call->getNumArguments() == 1 &&
        (call->getArgumentLabels().front() == getASTContext().Id_wrappedValue ||
         call->getArgumentLabels().front() == getASTContext().Id_initialValue)))
    return false;

  auto argExpr = cast<TupleExpr>(call->getArg())->getElement(0);
  // If this is an attempt to initialize property wrapper with opaque value
  // of error type, let's just ignore that problem since original mismatch
  // has been diagnosed already.
  if (argExpr->isImplicit() && isa<OpaqueValueExpr>(argExpr) &&
      argType->is<ErrorType>())
    return true;

  emitDiagnostic(getLoc(), diag::cannot_convert_initializer_value, argType,
                 paramType);
  return true;
}

void ExpandArrayIntoVarargsFailure::tryDropArrayBracketsFixIt(
    Expr *anchor) const {
  // If this is an array literal, offer to remove the brackets and pass the
  // elements directly as variadic arguments.
  if (auto *arrayExpr = dyn_cast<ArrayExpr>(anchor)) {
    auto diag = emitDiagnostic(arrayExpr->getLoc(),
                               diag::suggest_pass_elements_directly);
    diag.fixItRemove(arrayExpr->getLBracketLoc())
        .fixItRemove(arrayExpr->getRBracketLoc());
    // Handle the case where the array literal has a trailing comma.
    if (arrayExpr->getNumCommas() == arrayExpr->getNumElements())
      diag.fixItRemove(arrayExpr->getCommaLocs().back());
  }
}

bool ExpandArrayIntoVarargsFailure::diagnoseAsError() {
  if (auto anchor = getAnchor()) {
    emitDiagnostic(anchor->getLoc(), diag::cannot_convert_array_to_variadic,
                   getFromType(), getToType());
    tryDropArrayBracketsFixIt(anchor);
    // TODO: Array splat fix-it once that's supported.
    return true;
  }
  return false;
}

bool ExpandArrayIntoVarargsFailure::diagnoseAsNote() {
  auto overload = getChoiceFor(getLocator());
  auto anchor = getAnchor();
  if (!overload || !anchor)
    return false;

  if (auto chosenDecl = overload->choice.getDeclOrNull()) {
    emitDiagnostic(chosenDecl, diag::candidate_would_match_array_to_variadic,
                   getToType());
    tryDropArrayBracketsFixIt(anchor);
    return true;
  }
  return false;
}

bool ExtraneousCallFailure::diagnoseAsError() {
  auto &cs = getConstraintSystem();

  auto *anchor = getAnchor();
  auto *locator = getLocator();

  // If this is something like `foo()` where `foo` is a variable
  // or a property, let's suggest dropping `()`.
  auto removeParensFixIt = [&](InFlightDiagnostic &diagnostic) {
    auto *argLoc = cs.getConstraintLocator(getRawAnchor(),
                                           ConstraintLocator::ApplyArgument);

    if (auto *TE =
            dyn_cast_or_null<TupleExpr>(simplifyLocatorToAnchor(argLoc))) {
      if (TE->getNumElements() == 0) {
        diagnostic.fixItRemove(TE->getSourceRange());
      }
    }
  };

  if (auto overload = getChoiceFor(cs.getCalleeLocator(locator))) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      if (auto *enumCase = dyn_cast<EnumElementDecl>(decl)) {
        auto diagnostic = emitDiagnostic(
            anchor->getLoc(), diag::unexpected_arguments_in_enum_case,
            enumCase->getName());
        removeParensFixIt(diagnostic);
        return true;
      }
    }
  }

  auto diagnostic = emitDiagnostic(
      anchor->getLoc(), diag::cannot_call_non_function_value, getType(anchor));
  removeParensFixIt(diagnostic);
  return true;
}

bool InvalidUseOfTrailingClosure::diagnoseAsError() {
  auto *anchor = getAnchor();
  auto &cs = getConstraintSystem();

  emitDiagnostic(anchor->getLoc(), diag::trailing_closure_bad_param,
                 getToType())
      .highlight(anchor->getSourceRange());

  if (auto overload = getChoiceFor(cs.getCalleeLocator(getLocator()))) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnostic(decl, diag::decl_declared_here, decl->getFullName());
    }
  }

  return true;
}

void NonEphemeralConversionFailure::emitSuggestionNotes() const {
  auto getPointerKind = [](Type ty) -> PointerTypeKind {
    PointerTypeKind pointerKind;
    auto pointeeType = ty->lookThroughSingleOptionalType()
                         ->getAnyPointerElementType(pointerKind);
    assert(pointeeType && "Expected a pointer!");
    (void)pointeeType;

    return pointerKind;
  };

  // This must stay in sync with diag::ephemeral_use_array_with_unsafe_buffer
  // and diag::ephemeral_use_with_unsafe_pointer.
  enum AlternativeKind {
    AK_Raw = 0,
    AK_MutableRaw,
    AK_Typed,
    AK_MutableTyped,
  };

  auto getAlternativeKind = [&]() -> Optional<AlternativeKind> {
    switch (getPointerKind(getParamType())) {
    case PTK_UnsafeRawPointer:
      return AK_Raw;
    case PTK_UnsafeMutableRawPointer:
      return AK_MutableRaw;
    case PTK_UnsafePointer:
      return AK_Typed;
    case PTK_UnsafeMutablePointer:
      return AK_MutableTyped;
    case PTK_AutoreleasingUnsafeMutablePointer:
      return None;
    }
  };

  // First emit a note about the implicit conversion only lasting for the
  // duration of the call.
  auto *argExpr = getArgExpr();
  emitDiagnostic(argExpr->getLoc(),
                 diag::ephemeral_pointer_argument_conversion_note,
                 getArgType(), getParamType(), getCallee(), getCalleeFullName())
      .highlight(argExpr->getSourceRange());

  // Then try to find a suitable alternative.
  switch (ConversionKind) {
  case ConversionRestrictionKind::ArrayToPointer: {
    // Don't suggest anything for optional arrays, as there's currently no
    // direct alternative.
    if (getArgType()->getOptionalObjectType())
      break;

    // We can suggest using withUnsafe[Mutable][Bytes/BufferPointer].
    if (auto alternative = getAlternativeKind())
      emitDiagnostic(argExpr->getLoc(),
                     diag::ephemeral_use_array_with_unsafe_buffer,
                     *alternative);
    break;
  }
  case ConversionRestrictionKind::StringToPointer: {
    // Don't suggest anything for optional strings, as there's currently no
    // direct alternative.
    if (getArgType()->getOptionalObjectType())
      break;

    // We can suggest withCString as long as the resulting pointer is
    // immutable.
    switch (getPointerKind(getParamType())) {
    case PTK_UnsafePointer:
    case PTK_UnsafeRawPointer:
      emitDiagnostic(argExpr->getLoc(),
                     diag::ephemeral_use_string_with_c_string);
      break;
    case PTK_UnsafeMutableRawPointer:
    case PTK_UnsafeMutablePointer:
    case PTK_AutoreleasingUnsafeMutablePointer:
      // There's nothing really sensible we can suggest for a mutable pointer.
      break;
    }
    break;
  }
  case ConversionRestrictionKind::InoutToPointer:
    // For an arbitrary inout-to-pointer, we can suggest
    // withUnsafe[Mutable][Bytes/Pointer].
    if (auto alternative = getAlternativeKind())
      emitDiagnostic(argExpr->getLoc(), diag::ephemeral_use_with_unsafe_pointer,
                     *alternative);
    break;
  case ConversionRestrictionKind::DeepEquality:
  case ConversionRestrictionKind::Superclass:
  case ConversionRestrictionKind::Existential:
  case ConversionRestrictionKind::MetatypeToExistentialMetatype:
  case ConversionRestrictionKind::ExistentialMetatypeToMetatype:
  case ConversionRestrictionKind::ValueToOptional:
  case ConversionRestrictionKind::OptionalToOptional:
  case ConversionRestrictionKind::ClassMetatypeToAnyObject:
  case ConversionRestrictionKind::ExistentialMetatypeToAnyObject:
  case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass:
  case ConversionRestrictionKind::PointerToPointer:
  case ConversionRestrictionKind::ArrayUpcast:
  case ConversionRestrictionKind::DictionaryUpcast:
  case ConversionRestrictionKind::SetUpcast:
  case ConversionRestrictionKind::HashableToAnyHashable:
  case ConversionRestrictionKind::CFTollFreeBridgeToObjC:
  case ConversionRestrictionKind::ObjCTollFreeBridgeToCF:
    llvm_unreachable("Expected an ephemeral conversion!");
  }
}

bool NonEphemeralConversionFailure::diagnosePointerInit() const {
  auto *constructor = dyn_cast_or_null<ConstructorDecl>(getCallee());
  if (!constructor)
    return false;

  auto constructedTy = getFnType()->getResult();

  // Strip off a level of optionality if we have a failable initializer.
  if (constructor->isFailable())
    constructedTy = constructedTy->getOptionalObjectType();

  // This must stay in sync with diag::cannot_construct_dangling_pointer.
  enum ConstructorKind {
    CK_Pointer = 0,
    CK_BufferPointer,
  };

  // Consider OpaquePointer as well as the other kinds of pointers.
  auto isConstructingPointer =
      constructedTy->getAnyPointerElementType() ||
      constructedTy->getAnyNominal() == getASTContext().getOpaquePointerDecl();

  ConstructorKind constructorKind;
  auto parameterCount = constructor->getParameters()->size();
  if (isConstructingPointer && parameterCount == 1) {
    constructorKind = CK_Pointer;
  } else if (constructedTy->getAnyBufferPointerElementType() &&
             parameterCount == 2) {
    constructorKind = CK_BufferPointer;
  } else {
    return false;
  }

  auto diagID = DowngradeToWarning
                    ? diag::cannot_construct_dangling_pointer_warning
                    : diag::cannot_construct_dangling_pointer;

  auto *anchor = getRawAnchor();
  emitDiagnostic(anchor->getLoc(), diagID, constructedTy, constructorKind)
      .highlight(anchor->getSourceRange());

  emitSuggestionNotes();
  return true;
}

bool NonEphemeralConversionFailure::diagnoseAsNote() {
  // We can only emit a useful note if we have a callee.
  if (auto *callee = getCallee()) {
    emitDiagnostic(callee, diag::candidate_performs_illegal_ephemeral_conv,
                   getParamPosition());
    return true;
  }
  return false;
}

bool NonEphemeralConversionFailure::diagnoseAsError() {
  // Emit a specialized diagnostic for
  // Unsafe[Mutable][Raw]Pointer.init([mutating]:) &
  // Unsafe[Mutable][Raw]BufferPointer.init(start:count:).
  if (diagnosePointerInit())
    return true;

  // Otherwise, emit a more general diagnostic.
  SmallString<8> scratch;
  auto argDesc = getArgDescription(scratch);

  auto *argExpr = getArgExpr();
  if (isa<InOutExpr>(argExpr)) {
    auto diagID = DowngradeToWarning
                      ? diag::cannot_use_inout_non_ephemeral_warning
                      : diag::cannot_use_inout_non_ephemeral;

    emitDiagnostic(argExpr->getLoc(), diagID, argDesc, getCallee(),
                   getCalleeFullName())
        .highlight(argExpr->getSourceRange());
  } else {
    auto diagID = DowngradeToWarning
                      ? diag::cannot_pass_type_to_non_ephemeral_warning
                      : diag::cannot_pass_type_to_non_ephemeral;

    emitDiagnostic(argExpr->getLoc(), diagID, getArgType(), argDesc,
                   getCallee(), getCalleeFullName())
        .highlight(argExpr->getSourceRange());
  }
  emitSuggestionNotes();
  return true;
}

bool AssignmentTypeMismatchFailure::diagnoseMissingConformance() const {
  auto srcType = getFromType();
  auto dstType = getToType()->lookThroughAllOptionalTypes();

  llvm::SmallPtrSet<ProtocolDecl *, 4> srcMembers;
  llvm::SmallPtrSet<ProtocolDecl *, 4> dstMembers;

  auto retrieveProtocols = [](Type type,
                              llvm::SmallPtrSetImpl<ProtocolDecl *> &members) {
    if (auto *protocol = type->getAs<ProtocolType>())
      members.insert(protocol->getDecl());

    if (auto *composition = type->getAs<ProtocolCompositionType>()) {
      for (auto member : composition->getMembers()) {
        if (auto *protocol = member->getAs<ProtocolType>())
          members.insert(protocol->getDecl());
      }
    }
  };

  retrieveProtocols(srcType, srcMembers);
  retrieveProtocols(dstType, dstMembers);

  if (srcMembers.empty() || dstMembers.empty())
    return false;

  // Let's check whether there is an overlap between source and destination.
  for (auto *member : srcMembers)
    dstMembers.erase(member);

  if (dstMembers.size() == 1)
    dstType = (*dstMembers.begin())->getDeclaredType();

  auto *anchor = getAnchor();
  emitDiagnostic(anchor->getLoc(), diag::cannot_convert_assign_protocol,
                 srcType, dstType);
  return true;
}

bool AssignmentTypeMismatchFailure::diagnoseAsError() {
  if (diagnoseMissingConformance())
    return true;

  return ContextualFailure::diagnoseAsError();
}

bool AssignmentTypeMismatchFailure::diagnoseAsNote() {
  auto *anchor = getAnchor();
  auto &cs = getConstraintSystem();

  if (auto overload = getChoiceFor(cs.getConstraintLocator(anchor))) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnostic(decl,
                     diag::cannot_convert_candidate_result_to_contextual_type,
                     decl->getFullName(), getFromType(), getToType());
      return true;
    }
  }

  return false;
}

