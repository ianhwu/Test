//===--- TypeCheckPattern.cpp - Type Checking for Patterns ----------------===//
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
// This file implements semantic analysis for patterns, analyzing a
// pattern tree in both bottom-up and top-down ways.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckType.h"
#include "swift/Basic/StringExtras.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "llvm/Support/SaveAndRestore.h"
#include <utility>
using namespace swift;

/// If the given VarDecl is a computed property whose getter always returns a
/// particular enum element, return that element.
///
/// This requires the getter's body to have a certain syntactic form. It should
/// be kept in sync with importEnumCaseAlias in the ClangImporter library.
static EnumElementDecl *
extractEnumElement(DeclContext *DC, SourceLoc UseLoc,
                   const VarDecl *constant) {
  diagnoseExplicitUnavailability(constant, UseLoc, DC, nullptr);

  const FuncDecl *getter = constant->getAccessor(AccessorKind::Get);
  if (!getter)
    return nullptr;

  const BraceStmt *body = getter->getBody();
  if (!body || body->getNumElements() != 1)
    return nullptr;

  auto *retStmtRaw = body->getFirstElement().dyn_cast<Stmt *>();
  auto *retStmt = dyn_cast_or_null<ReturnStmt>(retStmtRaw);
  if (!retStmt)
    return nullptr;

  auto *resultExpr = dyn_cast_or_null<ApplyExpr>(retStmt->getResult());
  if (!resultExpr)
    return nullptr;

  auto *ctorExpr = dyn_cast<DeclRefExpr>(resultExpr->getFn());
  if (!ctorExpr)
    return nullptr;

  // If the declaration we found isn't in the same nominal type as the
  // constant, ignore it.
  if (ctorExpr->getDecl()->getDeclContext()->getSelfNominalTypeDecl() !=
        constant->getDeclContext()->getSelfNominalTypeDecl())
    return nullptr;

  return dyn_cast<EnumElementDecl>(ctorExpr->getDecl());
}

/// Find the first enum element in \p foundElements.
///
/// If there are no enum elements but there are properties, attempts to map
/// an arbitrary property to an enum element using extractEnumElement.
static EnumElementDecl *
filterForEnumElement(DeclContext *DC, SourceLoc UseLoc,
                     bool unqualifiedLookup, LookupResult foundElements) {
  EnumElementDecl *foundElement = nullptr;
  VarDecl *foundConstant = nullptr;

  for (LookupResultEntry result : foundElements) {
    ValueDecl *e = result.getValueDecl();
    assert(e);
    if (e->isInvalid()) {
      continue;
    }
    // Skip if the enum element was referenced as an instance member
    if (unqualifiedLookup) {
      if (!result.getBaseDecl() ||
          !result.getBaseDecl()->getInterfaceType()->is<MetatypeType>()) {
        continue;
      }
    }

    if (auto *oe = dyn_cast<EnumElementDecl>(e)) {
      // Ambiguities should be ruled out by parsing.
      assert(!foundElement && "ambiguity in enum case name lookup?!");
      foundElement = oe;
      continue;
    }

    if (auto *var = dyn_cast<VarDecl>(e)) {
      foundConstant = var;
      continue;
    }
  }

  if (!foundElement && foundConstant && foundConstant->hasClangNode())
    foundElement = extractEnumElement(DC, UseLoc, foundConstant);

  return foundElement;
}

/// Find an unqualified enum element.
static EnumElementDecl *
lookupUnqualifiedEnumMemberElement(DeclContext *DC,
                                   Identifier name, SourceLoc UseLoc) {
  auto lookupOptions = defaultUnqualifiedLookupOptions;
  lookupOptions |= NameLookupFlags::KnownPrivate;
  auto lookup =
      TypeChecker::lookupUnqualified(DC, name, SourceLoc(), lookupOptions);
  return filterForEnumElement(DC, UseLoc,
                              /*unqualifiedLookup=*/true, lookup);
}

/// Find an enum element in an enum type.
static EnumElementDecl *
lookupEnumMemberElement(DeclContext *DC, Type ty,
                        Identifier name, SourceLoc UseLoc) {
  if (!ty->mayHaveMembers())
    return nullptr;

  // Look up the case inside the enum.
  // FIXME: We should be able to tell if this is a private lookup.
  NameLookupOptions lookupOptions = defaultMemberLookupOptions;
  LookupResult foundElements =
      TypeChecker::lookupMember(DC, ty, name, lookupOptions);
  return filterForEnumElement(DC, UseLoc,
                              /*unqualifiedLookup=*/false, foundElements);
}

namespace {
// Build up an IdentTypeRepr and see what it resolves to.
struct ExprToIdentTypeRepr : public ASTVisitor<ExprToIdentTypeRepr, bool>
{
  SmallVectorImpl<ComponentIdentTypeRepr *> &components;
  ASTContext &C;

  ExprToIdentTypeRepr(decltype(components) &components, ASTContext &C)
    : components(components), C(C) {}
  
  bool visitExpr(Expr *e) {
    return false;
  }
  
  bool visitTypeExpr(TypeExpr *te) {
    if (auto *TR = te->getTypeRepr())
      if (auto *CITR = dyn_cast<ComponentIdentTypeRepr>(TR)) {
        components.push_back(CITR);
        return true;
      }
    return false;
  }

  bool visitDeclRefExpr(DeclRefExpr *dre) {
    assert(components.empty() && "decl ref should be root element of expr");
    
    // Get the declared type.
    if (auto *td = dyn_cast<TypeDecl>(dre->getDecl())) {
      components.push_back(
        new (C) SimpleIdentTypeRepr(dre->getLoc(), td->getName()));
      components.back()->setValue(td, nullptr);
      return true;
    }
    return false;
  }
  
  bool visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *udre) {
    assert(components.empty() && "decl ref should be root element of expr");
    // Track the AST location of the component.
    components.push_back(
      new (C) SimpleIdentTypeRepr(udre->getLoc(),
                                  udre->getName().getBaseIdentifier()));
    return true;
  }
  
  bool visitUnresolvedDotExpr(UnresolvedDotExpr *ude) {
    if (!visit(ude->getBase()))
      return false;
    
    assert(!components.empty() && "no components before dot expr?!");

    // Track the AST location of the new component.
    components.push_back(
      new (C) SimpleIdentTypeRepr(ude->getLoc(),
                                  ude->getName().getBaseIdentifier()));
    return true;
  }
  
  bool visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *use) {
    if (!visit(use->getSubExpr()))
      return false;
    
    assert(!components.empty() && "no components before generic args?!");
    
    // Track the AST location of the generic arguments.
    SmallVector<TypeRepr*, 4> argTypeReprs;
    for (auto &arg : use->getUnresolvedParams())
      argTypeReprs.push_back(arg.getTypeRepr());
    auto origComponent = components.back();
    components.back() =
      GenericIdentTypeRepr::create(C, origComponent->getIdLoc(),
                                   origComponent->getIdentifier(), argTypeReprs,
                                   SourceRange(use->getLAngleLoc(),
                                               use->getRAngleLoc()));

    return true;
  }
};
} // end anonymous namespace


namespace {
  class UnresolvedPatternFinder : public ASTWalker {
    bool &HadUnresolvedPattern;
  public:
    
    UnresolvedPatternFinder(bool &HadUnresolvedPattern)
      : HadUnresolvedPattern(HadUnresolvedPattern) {}
    
    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      // If we find an UnresolvedPatternExpr, return true.
      if (isa<UnresolvedPatternExpr>(E)) {
        HadUnresolvedPattern = true;
        return { false, E };
      }
      
      return { true, E };
    }
    
    static bool hasAny(Expr *E) {
      bool HasUnresolvedPattern = false;
      E->walk(UnresolvedPatternFinder(HasUnresolvedPattern));
      return HasUnresolvedPattern;
    }
  };
} // end anonymous namespace

namespace {
  
class ResolvePattern : public ASTVisitor<ResolvePattern,
                                         /*ExprRetTy=*/Pattern*,
                                         /*StmtRetTy=*/void,
                                         /*DeclRetTy=*/void,
                                         /*PatternRetTy=*/Pattern*>
{
public:
  ASTContext &Context;
  DeclContext *DC;

  ResolvePattern(DeclContext *DC) : Context(DC->getASTContext()), DC(DC) {}

  // Convert a subexpression to a pattern if possible, or wrap it in an
  // ExprPattern.
  Pattern *getSubExprPattern(Expr *E) {
    if (Pattern *p = visit(E))
      return p;

    return new (Context) ExprPattern(E, nullptr, nullptr);
  }
  
  // Handle productions that are always leaf patterns or are already resolved.
#define ALWAYS_RESOLVED_PATTERN(Id) \
  Pattern *visit##Id##Pattern(Id##Pattern *P) { return P; }
  ALWAYS_RESOLVED_PATTERN(Named)
  ALWAYS_RESOLVED_PATTERN(Any)
  ALWAYS_RESOLVED_PATTERN(Is)
  ALWAYS_RESOLVED_PATTERN(Paren)
  ALWAYS_RESOLVED_PATTERN(Tuple)
  ALWAYS_RESOLVED_PATTERN(EnumElement)
  ALWAYS_RESOLVED_PATTERN(Bool)
#undef ALWAYS_RESOLVED_PATTERN

  Pattern *visitVarPattern(VarPattern *P) {
    // Keep track of the fact that we're inside of a var/let pattern.  This
    // affects how unqualified identifiers are processed.
    P->setSubPattern(visit(P->getSubPattern()));
    
    // If the var pattern has no variables bound underneath it, then emit a
    // warning that the var/let is pointless.
    if (!P->isImplicit()) {
      bool HasVariable = false;
      P->forEachVariable([&](VarDecl *VD) { HasVariable = true; });
      
      if (!HasVariable) {
        Context.Diags
            .diagnose(P->getLoc(), diag::var_pattern_didnt_bind_variables,
                      P->isLet() ? "let" : "var")
            .highlight(P->getSubPattern()->getSourceRange())
            .fixItRemove(P->getLoc());
      }
    }
    
    return P;
  }

  Pattern *visitOptionalSomePattern(OptionalSomePattern *P) {
    P->setSubPattern(visit(P->getSubPattern()));
    return P;
  }

  Pattern *visitTypedPattern(TypedPattern *P) {
    P->setSubPattern(visit(P->getSubPattern()));
    return P;
  }
  
  Pattern *visitExprPattern(ExprPattern *P) {
    if (P->isResolved())
      return P;

    // Try to convert to a pattern.
    Pattern *exprAsPattern = visit(P->getSubExpr());
    // If we failed, keep the ExprPattern as is.
    if (!exprAsPattern) {
      P->setResolved(true);
      return P;
    }
    return exprAsPattern;
  }
  
  // Most exprs remain exprs and should be wrapped in ExprPatterns.
  Pattern *visitExpr(Expr *E) {
    return nullptr;
  }
  
  // Unwrap UnresolvedPatternExprs.
  Pattern *visitUnresolvedPatternExpr(UnresolvedPatternExpr *E) {
    return visit(E->getSubPattern());
  }
  
  // Convert a '_' expression to an AnyPattern.
  Pattern *visitDiscardAssignmentExpr(DiscardAssignmentExpr *E) {
    return new (Context) AnyPattern(E->getLoc(), E->isImplicit());
  }
  
  // Cast expressions 'x as T' get resolved to checked cast patterns.
  // Pattern resolution occurs before sequence resolution, so the cast will
  // appear as a SequenceExpr.
  Pattern *visitSequenceExpr(SequenceExpr *E) {
    if (E->getElements().size() != 3)
      return nullptr;
    auto cast = dyn_cast<CoerceExpr>(E->getElement(1));
    if (!cast)
      return nullptr;
    
    Pattern *subPattern = getSubExprPattern(E->getElement(0));
    return new (Context) IsPattern(cast->getLoc(), cast->getCastTypeLoc(),
                                   subPattern, CheckedCastKind::Unresolved);
  }
  
  // Convert a paren expr to a pattern if it contains a pattern.
  Pattern *visitParenExpr(ParenExpr *E) {
    Pattern *subPattern = getSubExprPattern(E->getSubExpr());
    return new (Context)
        ParenPattern(E->getLParenLoc(), subPattern, E->getRParenLoc());
  }
  
  // Convert all tuples to patterns.
  Pattern *visitTupleExpr(TupleExpr *E) {
    // Construct a TuplePattern.
    SmallVector<TuplePatternElt, 4> patternElts;

    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      Pattern *pattern = getSubExprPattern(E->getElement(i));
      patternElts.push_back(TuplePatternElt(E->getElementName(i),
                                            E->getElementNameLoc(i),
                                            pattern));
    }

    return TuplePattern::create(Context, E->getLoc(), patternElts,
                                E->getRParenLoc());
  }

  Pattern *convertBindingsToOptionalSome(Expr *E) {
    auto *expr = E->getSemanticsProvidingExpr();
    auto *bindExpr = dyn_cast<BindOptionalExpr>(expr);
    if (!bindExpr) {
      // Let's see if this expression prefixed with any number of '?'
      // has any other disjoint 'BindOptionalExpr' inside of it, if so,
      // we need to wrap such sub-expression into `OptionalEvaluationExpr`.
      bool hasDisjointChaining = false;
      expr->forEachChildExpr([&](Expr *subExpr) -> Expr * {
        // If there is `OptionalEvaluationExpr` in the AST
        // it means that all of possible `BindOptionalExpr`
        // which follow are covered by it.
        if (isa<OptionalEvaluationExpr>(subExpr))
          return nullptr;

        if (isa<BindOptionalExpr>(subExpr)) {
          hasDisjointChaining = true;
          return nullptr;
        }

        return subExpr;
      });

      if (hasDisjointChaining)
        E = new (Context) OptionalEvaluationExpr(E);

      return getSubExprPattern(E);
    }

    auto *subExpr = convertBindingsToOptionalSome(bindExpr->getSubExpr());
    return new (Context)
        OptionalSomePattern(subExpr, bindExpr->getQuestionLoc());
  }

  // Convert a x? to OptionalSome pattern.  In the AST form, this will look like
  // an OptionalEvaluationExpr with an immediate BindOptionalExpr inside of it.
  Pattern *visitOptionalEvaluationExpr(OptionalEvaluationExpr *E) {
    auto *subExpr = E->getSubExpr();
    // We only handle the case where one or more bind expressions are subexprs
    // of the optional evaluation.  Other cases are not simple postfix ?'s.
    if (!isa<BindOptionalExpr>(subExpr->getSemanticsProvidingExpr()))
      return nullptr;

    return convertBindingsToOptionalSome(subExpr);
  }


  // Unresolved member syntax '.Element' forms an EnumElement pattern. The
  // element will be resolved when we type-check the pattern.
  Pattern *visitUnresolvedMemberExpr(UnresolvedMemberExpr *ume) {
    // If the unresolved member has an argument, turn it into a subpattern.
    Pattern *subPattern = nullptr;
    if (auto arg = ume->getArgument()) {
      subPattern = getSubExprPattern(arg);
    }
    
    if (ume->getName().getBaseName().isSpecial())
      return nullptr;

    // FIXME: Compound names.
    return new (Context)
        EnumElementPattern(ume->getDotLoc(), ume->getNameLoc().getBaseNameLoc(),
                           ume->getName().getBaseIdentifier(), subPattern, ume);
  }
  
  // Member syntax 'T.Element' forms a pattern if 'T' is an enum and the
  // member name is a member of the enum.
  Pattern *visitUnresolvedDotExpr(UnresolvedDotExpr *ude) {
    SmallVector<ComponentIdentTypeRepr *, 2> components;
    if (!ExprToIdentTypeRepr(components, Context).visit(ude->getBase()))
      return nullptr;

    TypeResolutionOptions options = None;
    options |= TypeResolutionFlags::AllowUnboundGenerics;
    options |= TypeResolutionFlags::SilenceErrors;

    auto *repr = IdentTypeRepr::create(Context, components);

    // See if the repr resolves to a type.
    Type ty = TypeChecker::resolveIdentifierType(
        TypeResolution::forContextual(DC), repr, options);

    auto *enumDecl = dyn_cast_or_null<EnumDecl>(ty->getAnyNominal());
    if (!enumDecl)
      return nullptr;

    // FIXME: Argument labels?
    EnumElementDecl *referencedElement
      = lookupEnumMemberElement(DC, ty, ude->getName().getBaseIdentifier(),
                                ude->getLoc());
    if (!referencedElement)
      return nullptr;
    
    // Build a TypeRepr from the head of the full path.
    // FIXME: Compound names.
    TypeLoc loc(repr);
    loc.setType(ty);
    return new (Context) EnumElementPattern(
        loc, ude->getDotLoc(), ude->getNameLoc().getBaseNameLoc(),
        ude->getName().getBaseIdentifier(), referencedElement, nullptr);
  }
  
  // A DeclRef 'E' that refers to an enum element forms an EnumElementPattern.
  Pattern *visitDeclRefExpr(DeclRefExpr *de) {
    auto *elt = dyn_cast<EnumElementDecl>(de->getDecl());
    if (!elt)
      return nullptr;
    
    // Use the type of the enum from context.
    TypeLoc loc = TypeLoc::withoutLoc(
                            elt->getParentEnum()->getDeclaredTypeInContext());
    return new (Context) EnumElementPattern(loc, SourceLoc(), de->getLoc(),
                                            elt->getName(), elt, nullptr);
  }
  Pattern *visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *ude) {
    // FIXME: This shouldn't be needed.  It is only necessary because of the
    // poor representation of clang enum aliases and should be removed when
    // rdar://20879992 is addressed.
    //
    // Try looking up an enum element in context.
    if (EnumElementDecl *referencedElement
        = lookupUnqualifiedEnumMemberElement(DC,
                                             ude->getName().getBaseIdentifier(),
                                             ude->getLoc())) {
      auto *enumDecl = referencedElement->getParentEnum();
      auto enumTy = enumDecl->getDeclaredTypeInContext();
      TypeLoc loc = TypeLoc::withoutLoc(enumTy);

      return new (Context) EnumElementPattern(
          loc, SourceLoc(), ude->getLoc(), ude->getName().getBaseIdentifier(),
          referencedElement, nullptr);
    }
      
    
    // Perform unqualified name lookup to find out what the UDRE is.
    return getSubExprPattern(TypeChecker::resolveDeclRefExpr(ude, DC));
  }
  
  // Call syntax forms a pattern if:
  // - the callee in 'Element(x...)' or '.Element(x...)'
  //   references an enum element. The arguments then form a tuple
  //   pattern matching the element's data.
  // - the callee in 'T(...)' is a struct or class type. The argument tuple is
  //   then required to have keywords for every argument that name properties
  //   of the type.
  Pattern *visitCallExpr(CallExpr *ce) {
    // Specialized call are not allowed anyway.
    // Let it be diagnosed as an expression.
    if (isa<UnresolvedSpecializeExpr>(ce->getFn()))
      return nullptr;

    SmallVector<ComponentIdentTypeRepr *, 2> components;
    if (!ExprToIdentTypeRepr(components, Context).visit(ce->getFn()))
      return nullptr;
    
    if (components.empty())
      return nullptr;

    auto tailComponent = components.pop_back_val();
    EnumElementDecl *referencedElement = nullptr;
    TypeLoc loc;

    if (components.empty()) {
      // Only one component. Try looking up an enum element in context.
      referencedElement
        = lookupUnqualifiedEnumMemberElement(DC,
                                             tailComponent->getIdentifier(),
                                             tailComponent->getLoc());
      if (!referencedElement)
        return nullptr;

      auto *enumDecl = referencedElement->getParentEnum();
      loc = TypeLoc::withoutLoc(enumDecl->getDeclaredTypeInContext());
    } else {
      TypeResolutionOptions options = None;
      options |= TypeResolutionFlags::AllowUnboundGenerics;
      options |= TypeResolutionFlags::SilenceErrors;

      // Otherwise, see whether we had an enum type as the penultimate
      // component, and look up an element inside it.
      auto *prefixRepr = IdentTypeRepr::create(Context, components);

      // See first if the entire repr resolves to a type.
      Type enumTy = TypeChecker::resolveIdentifierType(TypeResolution::forContextual(DC),
                                                       prefixRepr, options);
      if (!dyn_cast_or_null<EnumDecl>(enumTy->getAnyNominal()))
        return nullptr;

      referencedElement
        = lookupEnumMemberElement(DC, enumTy,
                                  tailComponent->getIdentifier(),
                                  tailComponent->getLoc());
      if (!referencedElement)
        return nullptr;

      loc = TypeLoc(prefixRepr);
      loc.setType(enumTy);
    }

    assert(!isa<GenericIdentTypeRepr>(tailComponent) &&
           "should be handled above");

    auto *subPattern = getSubExprPattern(ce->getArg());
    return new (Context) EnumElementPattern(
        loc, SourceLoc(), tailComponent->getIdLoc(),
        tailComponent->getIdentifier(), referencedElement, subPattern);
  }
};

} // end anonymous namespace


/// Perform top-down syntactic disambiguation of a pattern. Where ambiguous
/// expr/pattern productions occur (tuples, function calls, etc.), favor the
/// pattern interpretation if it forms a valid pattern; otherwise, leave it as
/// an expression. This does no type-checking except for the bare minimum to
/// disambiguate semantics-dependent pattern forms.
Pattern *TypeChecker::resolvePattern(Pattern *P, DeclContext *DC,
                                     bool isStmtCondition) {
  P = ResolvePattern(DC).visit(P);

  // If the entire pattern is "(pattern_expr (type_expr SomeType))", then this
  // is an invalid pattern.  If it were actually a value comparison (with ~=)
  // then the metatype would have had to be spelled with "SomeType.self".  What
  // they actually meant is to write "is SomeType", so we rewrite it to that
  // pattern for good QoI.
  auto &Context = DC->getASTContext();
  if (auto *EP = dyn_cast<ExprPattern>(P))
    if (auto *TE = dyn_cast<TypeExpr>(EP->getSubExpr())) {
      Context.Diags.diagnose(TE->getStartLoc(), diag::type_pattern_missing_is)
        .fixItInsert(TE->getStartLoc(), "is ");
      
      P = new (Context) IsPattern(TE->getStartLoc(), TE->getTypeLoc(),
                                  /*subpattern*/nullptr,
                                  CheckedCastKind::Unresolved);
    }
  
  // Look through a TypedPattern if present.
  auto *InnerP = P;
  if (auto *TP = dyn_cast<TypedPattern>(P))
    InnerP = TP->getSubPattern();

  // If the pattern was valid, check for an implicit VarPattern on the outer
  // level.  If so, we have an "if let" condition and we want to enforce some
  // more structure on it.
  if (isStmtCondition && isa<VarPattern>(InnerP) && InnerP->isImplicit()) {
    auto *Body = cast<VarPattern>(InnerP)->getSubPattern();

    // If they wrote a "x?" pattern, they probably meant "if let x".
    // Check for this and recover nicely if they wrote that.
    if (auto *OSP = dyn_cast<OptionalSomePattern>(Body)) {
      if (!OSP->getSubPattern()->isRefutablePattern()) {
        Context.Diags.diagnose(OSP->getStartLoc(),
                               diag::iflet_implicitly_unwraps)
          .highlight(OSP->getSourceRange())
          .fixItRemove(OSP->getQuestionLoc());
        return P;
      }
    }

    // If the pattern bound is some other refutable pattern, then they
    // probably meant:
    //   if case let <pattern> =
    if (Body->isRefutablePattern()) {
      Context.Diags.diagnose(P->getLoc(), diag::iflet_pattern_matching)
        .fixItInsert(P->getLoc(), "case ");
      return P;
    }

    // "if let" implicitly looks inside of an optional, so wrap it in an
    // OptionalSome pattern.
    InnerP = new (Context) OptionalSomePattern(InnerP, InnerP->getEndLoc(),
                                               true);
    if (auto *TP = dyn_cast<TypedPattern>(P))
      TP->setSubPattern(InnerP);
    else
      P = InnerP;
  }

  return P;
}

static bool validateTypedPattern(TypeResolution resolution,
                                 TypedPattern *TP,
                                 TypeResolutionOptions options) {
  if (TP->hasType())
    return TP->getType()->hasError();

  TypeLoc TL = TP->getTypeLoc();
  
  bool hadError;
  
  // If the pattern declares an opaque type, and applies to a single
  // variable binding, then we can bind the opaque return type from the
  // property definition.
  auto &Context = resolution.getASTContext();
  if (auto opaqueRepr = dyn_cast<OpaqueReturnTypeRepr>(TL.getTypeRepr())) {
    auto named = dyn_cast<NamedPattern>(
                           TP->getSubPattern()->getSemanticsProvidingPattern());
    if (named) {
      auto *var = named->getDecl();
      auto opaqueDecl = var->getOpaqueResultTypeDecl();
      auto opaqueTy = (opaqueDecl
                       ? opaqueDecl->getDeclaredInterfaceType()
                       : ErrorType::get(Context));
      TL.setType(named->getDecl()->getDeclContext()
                                 ->mapTypeIntoContext(opaqueTy));
      hadError = opaqueTy->hasError();
    } else {
      Context.Diags.diagnose(TP->getLoc(), diag::opaque_type_unsupported_pattern);
      hadError = true;
    }
  } else {
    hadError = TypeChecker::validateType(Context, TL, resolution, options);
  }

  if (hadError) {
    TP->setType(ErrorType::get(Context));
    return hadError;
  }

  TP->setType(TL.getType());

  assert(!dyn_cast_or_null<SpecifierTypeRepr>(TL.getTypeRepr()));

  return hadError;
}

bool TypeChecker::typeCheckPattern(Pattern *P, DeclContext *dc,
                                   TypeResolutionOptions options) {
  auto &Context = dc->getASTContext();
  switch (P->getKind()) {
  // Type-check paren patterns by checking the sub-pattern and
  // propagating that type out.
  case PatternKind::Paren:
  case PatternKind::Var: {
    Pattern *SP;
    if (auto *PP = dyn_cast<ParenPattern>(P))
      SP = PP->getSubPattern();
    else
      SP = cast<VarPattern>(P)->getSubPattern();
    if (TypeChecker::typeCheckPattern(SP, dc, options)) {
      P->setType(ErrorType::get(Context));
      return true;
    }
    if (SP->hasType()) {
      auto type = SP->getType();
      if (P->getKind() == PatternKind::Paren)
        type = ParenType::get(Context, type);
      P->setType(type);
    }
    return false;
  }

  // If we see an explicit type annotation, coerce the sub-pattern to
  // that type.
  case PatternKind::Typed: {
    auto resolution = TypeResolution::forContextual(dc);
    TypedPattern *TP = cast<TypedPattern>(P);
    bool hadError = validateTypedPattern(resolution, TP, options);

    // If we have unbound generic types, don't apply them below; instead,
    // the caller will call typeCheckBinding() later.
    if (P->getType()->hasUnboundGenericType())
      return hadError;

    Pattern *subPattern = TP->getSubPattern();
    if (TypeChecker::coercePatternToType(subPattern, resolution, P->getType(),
                                         options|TypeResolutionFlags::FromNonInferredPattern,
                                         TP->getTypeLoc()))
      hadError = true;
    else {
      TP->setSubPattern(subPattern);
      TP->setType(subPattern->getType());
    }
    return hadError;
  }

  // A wildcard or name pattern cannot appear by itself in a context
  // which requires an explicit type.
  case PatternKind::Any:
  case PatternKind::Named:
    // If we're type checking this pattern in a context that can provide type
    // information, then the lack of type information is not an error.
    if (options & TypeResolutionFlags::AllowUnspecifiedTypes)
      return false;

    Context.Diags.diagnose(P->getLoc(), diag::cannot_infer_type_for_pattern);
    P->setType(ErrorType::get(Context));
    if (auto named = dyn_cast<NamedPattern>(P)) {
      if (auto var = named->getDecl()) {
        var->setInvalid();
      }
    }
    return true;

  // A tuple pattern propagates its tuple-ness out.
  case PatternKind::Tuple: {
    auto tuplePat = cast<TuplePattern>(P);
    bool hadError = false;
    SmallVector<TupleTypeElt, 8> typeElts;

    const auto elementOptions = options.withoutContext();
    bool missingType = false;
    for (unsigned i = 0, e = tuplePat->getNumElements(); i != e; ++i) {
      TuplePatternElt &elt = tuplePat->getElement(i);
      Pattern *pattern = elt.getPattern();
      if (TypeChecker::typeCheckPattern(pattern, dc, elementOptions)) {
        hadError = true;
        continue;
      }
      if (!pattern->hasType()) {
        missingType = true;
        continue;
      }

      typeElts.push_back(TupleTypeElt(pattern->getType(), elt.getLabel()));
    }

    if (hadError) {
      P->setType(ErrorType::get(Context));
      return true;
    }
    if (!missingType && !(options &
                          TypeResolutionFlags::AllowUnspecifiedTypes)) {
      P->setType(TupleType::get(typeElts, Context));
    }
    return false;
  }
      
  //--- Refutable patterns.
  //
  // Refutable patterns occur when checking the PatternBindingDecls in if/let,
  // while/let, and let/else conditions.
  case PatternKind::Is:
  case PatternKind::EnumElement:
  case PatternKind::OptionalSome:
  case PatternKind::Bool:
  case PatternKind::Expr:
    // In a let/else, these always require an initial value to match against.
    if (!(options & TypeResolutionFlags::AllowUnspecifiedTypes)) {
      Context.Diags.diagnose(P->getLoc(),
                             diag::refutable_pattern_requires_initializer);
      P->setType(ErrorType::get(Context));
      return true;
    }

    return false;
  }
  llvm_unreachable("bad pattern kind!");
}

namespace {

/// We need to allow particular matches for backwards compatibility, so we
/// "repair" the pattern if needed, so that the exhaustiveness checker receives
/// well-formed input. Also emit diagnostics warning the user to fix their code.
///
/// See SR-11160 and SR-11212 for more discussion.
//
// type ~ (T1, ..., Tn) (n >= 2)
//   1a. pat ~ ((P1, ..., Pm)) (m >= 2)
//   1b. pat
// type ~ ((T1, ..., Tn)) (n >= 2)
//   2. pat ~ (P1, ..., Pm) (m >= 2)
void implicitlyUntuplePatternIfApplicable(DiagnosticEngine &DE,
                                          Pattern *&enumElementInnerPat,
                                          Type enumPayloadType) {
  if (auto *tupleType = dyn_cast<TupleType>(enumPayloadType.getPointer())) {
    if (tupleType->getNumElements() >= 2
        && enumElementInnerPat->getKind() == PatternKind::Paren) {
      auto *semantic = enumElementInnerPat->getSemanticsProvidingPattern();
      if (auto *tuplePattern = dyn_cast<TuplePattern>(semantic)) {
        if (tuplePattern->getNumElements() >= 2) {
          DE.diagnose(tuplePattern->getLoc(),
                      diag::matching_tuple_pattern_with_many_assoc_values);
          enumElementInnerPat = semantic;
        }
      } else {
        DE.diagnose(enumElementInnerPat->getLoc(),
                    diag::matching_pattern_with_many_assoc_values);
      }
    }
  } else if (auto *tupleType = enumPayloadType->getAs<TupleType>()) {
    if (tupleType->getNumElements() >= 2
        && enumElementInnerPat->getKind() == PatternKind::Tuple)
      DE.diagnose(enumElementInnerPat->getLoc(),
                  diag::matching_many_patterns_with_tupled_assoc_value);
  }
}
}

/// Perform top-down type coercion on the given pattern.
bool TypeChecker::coercePatternToType(Pattern *&P, TypeResolution resolution,
                                      Type type,
                                      TypeResolutionOptions options,
                                      TypeLoc tyLoc) {
recur:
  if (tyLoc.isNull()) {
    tyLoc = TypeLoc::withoutLoc(type);
  }

  auto dc = resolution.getDeclContext();
  auto &Context = dc->getASTContext();
  auto &diags = Context.Diags;
  auto subOptions = options;
  subOptions.setContext(None);
  switch (P->getKind()) {
  // For parens and vars, just set the type annotation and propagate inwards.
  case PatternKind::Paren: {
    auto PP = cast<ParenPattern>(P);
    auto sub = PP->getSubPattern();
    auto semantic = P->getSemanticsProvidingPattern();
    // If this is the payload of an enum, and the type is a single-element
    // labeled tuple, treat this as a tuple pattern. It's unlikely that the
    // user is interested in binding a variable of type (foo: Int).
    if ((options.getContext() == TypeResolverContext::EnumPatternPayload)
        && !isa<TuplePattern>(semantic)) {
      if (auto tupleType = type->getAs<TupleType>()) {
        if (tupleType->getNumElements() == 1 &&
            !tupleType->getElement(0).isVararg()) {
          auto elementTy = tupleType->getElementType(0);
          if (coercePatternToType(sub, resolution, elementTy, subOptions))
            return true;
          TuplePatternElt elt(sub);
          P = TuplePattern::create(Context, PP->getLParenLoc(), elt,
                                   PP->getRParenLoc());
          if (PP->isImplicit())
            P->setImplicit();
          P->setType(type);
          return false;
        }
      }
    }
  
    if (coercePatternToType(sub, resolution, type, subOptions))
      return true;
    PP->setSubPattern(sub);
    PP->setType(sub->getType());
    return false;
  }
  case PatternKind::Var: {
    auto VP = cast<VarPattern>(P);
    
    Pattern *sub = VP->getSubPattern();
    if (coercePatternToType(sub, resolution, type, subOptions))
      return true;
    VP->setSubPattern(sub);
    if (sub->hasType())
      VP->setType(sub->getType());
    return false;
  }

  // If we see an explicit type annotation, coerce the sub-pattern to
  // that type.
  case PatternKind::Typed: {
    TypedPattern *TP = cast<TypedPattern>(P);
    bool hadError = validateTypedPattern(resolution, TP, options);
    if (!hadError) {
      if (!type->isEqual(TP->getType()) && !type->hasError()) {
        if (options & TypeResolutionFlags::OverrideType) {
          TP->setType(type);
        } else {
          diags.diagnose(P->getLoc(), diag::pattern_type_mismatch_context,
                         type);
          hadError = true;
        }
      }
    }

    Pattern *sub = TP->getSubPattern();
    hadError |= coercePatternToType(sub, resolution, TP->getType(),
                    subOptions | TypeResolutionFlags::FromNonInferredPattern);
    if (!hadError) {
      TP->setSubPattern(sub);
      TP->setType(sub->getType());
    }
    return hadError;
  }

  // For wildcard and name patterns, set the type.
  case PatternKind::Named: {
    NamedPattern *NP = cast<NamedPattern>(P);
    VarDecl *var = NP->getDecl();
    if (var->hasInterfaceType() && var->isInvalid())
      type = ErrorType::get(Context);

    // In SIL mode, VarDecls are written as having reference storage types.
    type = type->getReferenceStorageReferent();

    // Note that the pattern's type does not include the reference storage type.
    P->setType(type);
    var->setNamingPattern(NP);

    // FIXME: This call can be removed once pattern binding validation is
    // sufficiently requestified.
    TypeChecker::checkForForbiddenPrefix(Context, var->getBaseName());

    // If we are inferring a variable to have type AnyObject.Type,
    // "()", "[()]", an uninhabited type, or optional thereof, emit a diagnostic.
    // They are probably missing a cast or didn't mean to bind to a variable.
    // We always tell them that they can silence the warning with an
    // explicit type annotation (and provide a fixit) as a note.
    Type diagTy = type->lookThroughAllOptionalTypes();
    bool isOptional = !type->getOptionalObjectType().isNull();
    if (!diagTy) diagTy = type;
    
    auto diag = diag::type_inferred_to_undesirable_type;
    bool shouldRequireType = false;
    if (NP->isImplicit()) {
      // If the whole pattern is implicit, the user didn't write it.
      // Assume the compiler knows what it's doing.
    } else if (diagTy->isEqual(Context.TheEmptyTupleType)) {
      shouldRequireType = true;
    } else if (auto MTT = diagTy->getAs<AnyMetatypeType>()) {
      if (MTT->getInstanceType()->isAnyObject())
        shouldRequireType = true;
    } else if (diagTy->isStructurallyUninhabited()) {
      shouldRequireType = true;
      diag = isOptional ? diag::type_inferred_to_undesirable_type
                        : diag::type_inferred_to_uninhabited_type;

      if (diagTy->is<TupleType>()) {
        diag = isOptional ? diag::type_inferred_to_undesirable_type
                          : diag::type_inferred_to_uninhabited_tuple_type;
      } else {
        assert((diagTy->is<EnumType>() || diagTy->is<BoundGenericEnumType>()) &&
          "unknown structurally uninhabited type");
      }
    } else if (auto *BST = diagTy->getAs<BoundGenericStructType>()) {
      if (BST->getDecl() == Context.getArrayDecl())
          shouldRequireType = BST->getGenericArgs()[0]->isEqual(Context.TheEmptyTupleType);
    }
    
    if (shouldRequireType &&
        !options.is(TypeResolverContext::ForEachStmt) &&
        !options.is(TypeResolverContext::EditorPlaceholderExpr) &&
        !(options & TypeResolutionFlags::FromNonInferredPattern)) {
      diags.diagnose(NP->getLoc(), diag, NP->getDecl()->getName(), type,
                     NP->getDecl()->isLet());
      diags.diagnose(NP->getLoc(), diag::add_explicit_type_annotation_to_silence)
          .fixItInsertAfter(var->getNameLoc(), ": " + type->getWithoutParens()->getString());
    }

    return false;
  }
  case PatternKind::Any:
    P->setType(type);
    return false;

  // We can match a tuple pattern with a tuple type.
  // TODO: permit implicit conversions?
  case PatternKind::Tuple: {
    TuplePattern *TP = cast<TuplePattern>(P);
    bool hadError = type->hasError();
    
    // Sometimes a paren is just a paren. If the tuple pattern has a single
    // element, we can reduce it to a paren pattern.
    bool canDecayToParen = TP->getNumElements() == 1;
    auto decayToParen = [&]() -> bool {
      assert(canDecayToParen);
      Pattern *sub = TP->getElement(0).getPattern();
      if (TypeChecker::coercePatternToType(sub, resolution, type, subOptions))
        return true;
      
      if (TP->getLParenLoc().isValid()) {
        P = new (Context) ParenPattern(TP->getLParenLoc(), sub,
                                       TP->getRParenLoc(),
                                       /*implicit*/ TP->isImplicit());
        P->setType(sub->getType());
      } else {
        P = sub;
      }
      return false;
    };

    // The context type must be a tuple.
    TupleType *tupleTy = type->getAs<TupleType>();
    if (!tupleTy && !hadError) {
      if (canDecayToParen)
        return decayToParen();
      diags.diagnose(TP->getStartLoc(),
                     diag::tuple_pattern_in_non_tuple_context, type);
      hadError = true;
    }

    // The number of elements must match exactly.
    if (!hadError && tupleTy->getNumElements() != TP->getNumElements()) {
      if (canDecayToParen)
        return decayToParen();
      
      diags.diagnose(TP->getStartLoc(), diag::tuple_pattern_length_mismatch,
                     type);
      hadError = true;
    }

    // Coerce each tuple element to the respective type.
    P->setType(type);

    for (unsigned i = 0, e = TP->getNumElements(); i != e; ++i) {
      TuplePatternElt &elt = TP->getElement(i);
      Pattern *pattern = elt.getPattern();

      Type CoercionType;
      if (hadError)
        CoercionType = ErrorType::get(Context);
      else
        CoercionType = tupleTy->getElement(i).getType();
      
      // If the tuple pattern had a label for the tuple element, it must match
      // the label for the tuple type being matched.
      if (!hadError && !elt.getLabel().empty() &&
          elt.getLabel() != tupleTy->getElement(i).getName()) {
        diags.diagnose(elt.getLabelLoc(), diag::tuple_pattern_label_mismatch,
                       elt.getLabel(), tupleTy->getElement(i).getName());
        hadError = true;
      }
      
      hadError |= coercePatternToType(pattern, resolution, CoercionType,
                                      options);
      if (!hadError)
        elt.setPattern(pattern);
    }

    return hadError;
  }

  // Coerce expressions by finding a '~=' operator that can compare the
  // expression to a value of the coerced type.
  case PatternKind::Expr: {
    assert(cast<ExprPattern>(P)->isResolved()
           && "coercing unresolved expr pattern!");
    if (type->getAnyNominal() == Context.getBoolDecl()) {
      // The type is Bool.
      // Check if the pattern is a Bool literal
      auto EP = cast<ExprPattern>(P);
      if (auto *BLE = dyn_cast<BooleanLiteralExpr>(
              EP->getSubExpr()->getSemanticsProvidingExpr())) {
        P = new (Context) BoolPattern(BLE->getLoc(), BLE->getValue());
        P->setType(type);
        return false;
      }
    }

    // case nil is equivalent to .none when switching on Optionals.
    if (type->getOptionalObjectType()) {
      auto EP = cast<ExprPattern>(P);
      if (auto *NLE = dyn_cast<NilLiteralExpr>(EP->getSubExpr())) {
        auto *NoneEnumElement = Context.getOptionalNoneDecl();
        P = new (Context) EnumElementPattern(TypeLoc::withoutLoc(type),
                                             NLE->getLoc(), NLE->getLoc(),
                                             NoneEnumElement->getName(),
                                             NoneEnumElement, nullptr, false);
        return TypeChecker::coercePatternToType(P, resolution, type, options);
      }
    }
    return TypeChecker::typeCheckExprPattern(cast<ExprPattern>(P), dc, type);
  }
      
  // Coerce an 'is' pattern by determining the cast kind.
  case PatternKind::Is: {
    auto IP = cast<IsPattern>(P);

    // Type-check the type parameter.
    TypeResolutionOptions paramOptions(TypeResolverContext::InExpression); 
    if (validateType(Context, IP->getCastTypeLoc(), resolution, paramOptions))
      return true;

    auto castType = IP->getCastTypeLoc().getType();

    // Determine whether we have an imbalance in the number of optionals.
    SmallVector<Type, 2> inputTypeOptionals;
    type->lookThroughAllOptionalTypes(inputTypeOptionals);
    SmallVector<Type, 2> castTypeOptionals;
    castType->lookThroughAllOptionalTypes(castTypeOptionals);

    // If we have extra optionals on the input type. Create ".Some" patterns
    // wrapping the isa pattern to balance out the optionals.
    int numExtraOptionals = inputTypeOptionals.size()-castTypeOptionals.size();
    if (numExtraOptionals > 0) {
      Pattern *sub = IP;
      for (int i = 0; i < numExtraOptionals; ++i) {
        auto some = Context.getOptionalDecl()->getUniqueElement(/*hasVal*/true);
        sub = new (Context) EnumElementPattern(TypeLoc(),
                                               IP->getStartLoc(),
                                               IP->getEndLoc(),
                                               some->getName(),
                                               nullptr, sub,
                                               /*Implicit=*/true);
      }

      P = sub;
      return coercePatternToType(P, resolution, type, options);
    }


    CheckedCastKind castKind
      = TypeChecker::typeCheckCheckedCast(type, IP->getCastTypeLoc().getType(),
                                          type->hasError()
                                            ? CheckedCastContextKind::None
                                            : CheckedCastContextKind::IsPattern,
                                          dc,
                                          IP->getLoc(),
                                          nullptr,
                                          IP->getCastTypeLoc().getSourceRange());
    switch (castKind) {
    case CheckedCastKind::Unresolved:
      return true;
    case CheckedCastKind::Coercion:
    case CheckedCastKind::BridgingCoercion:
      // If this is an 'as' pattern coercing between two different types, then
      // it is "useful" because it is providing a different type to the
      // sub-pattern.  If this is an 'is' pattern or an 'as' pattern where the
      // types are the same, then produce a warning.
      if (!IP->getSubPattern() ||
          type->isEqual(IP->getCastTypeLoc().getType())) {
        diags.diagnose(IP->getLoc(), diag::isa_is_always_true,
                       IP->getSubPattern() ? "as" : "is");
      }
      IP->setCastKind(castKind);
      break;

    // Valid checks.
    case CheckedCastKind::ArrayDowncast:
    case CheckedCastKind::DictionaryDowncast:
    case CheckedCastKind::SetDowncast: {
      diags.diagnose(IP->getLoc(),
                     diag::isa_collection_downcast_pattern_value_unimplemented,
                     IP->getCastTypeLoc().getType());
      return false;
    }

    case CheckedCastKind::ValueCast:
      IP->setCastKind(castKind);
      break;
    }
    IP->setType(type);
    
    // Coerce the subpattern to the destination type.
    if (Pattern *sub = IP->getSubPattern()) {
      if (coercePatternToType(sub, resolution, IP->getCastTypeLoc().getType(),
                        subOptions|TypeResolutionFlags::FromNonInferredPattern))
        return true;
      IP->setSubPattern(sub);
    }
    
    return false;
  }
      
  case PatternKind::EnumElement: {
    auto *EEP = cast<EnumElementPattern>(P);
    
    // If the element decl was not resolved (because it was spelled without a
    // type as `.Foo`), resolve it now that we have a type.
    Optional<CheckedCastKind> castKind;
    
    EnumElementDecl *elt = EEP->getElementDecl();
    
    Type enumTy;
    if (!elt) {
      elt = lookupEnumMemberElement(dc, type, EEP->getName(),
                                    EEP->getLoc());
      if (!elt) {
        if (!type->hasError()) {
          // Lowercasing of Swift.Optional's cases is handled in the
          // standard library itself, not through the clang importer,
          // so we have to do this check here. Additionally, .Some
          // isn't a static VarDecl, so the existing mechanics in
          // extractEnumElement won't work.
          if (type->getAnyNominal() == Context.getOptionalDecl()) {
            if (EEP->getName().str() == "None" ||
                EEP->getName().str() == "Some") {
              SmallString<4> Rename;
              camel_case::toLowercaseWord(EEP->getName().str(), Rename);
              diags.diagnose(
                  EEP->getLoc(), diag::availability_decl_unavailable_rename,
                  /*"getter" prefix*/ 2, EEP->getName(), /*replaced*/ false,
                  /*special kind*/ 0, Rename.str(), /*message*/ StringRef())
                  .fixItReplace(EEP->getLoc(), Rename.str());

              return true;
            }
          
          // If we have the original expression parse tree, try reinterpreting
          // it as an expr-pattern if enum element lookup failed, since `.foo`
          // could also refer to a static member of the context type.
          } else if (EEP->hasUnresolvedOriginalExpr()) {
            P = new (Context) ExprPattern(EEP->getUnresolvedOriginalExpr(),
                                          nullptr, nullptr);
            goto recur;
          }

          // If we have an optional type, let's try to see if the case
          // exists in its base type and if it does then synthesize an
          // OptionalSomePattern that wraps the case. This uses recursion
          // to add multiple levels of OptionalSomePattern if the optional
          // is nested.
          if (auto baseType = type->getOptionalObjectType()) {
            if (lookupEnumMemberElement(dc,
                                        baseType->lookThroughAllOptionalTypes(),
                                        EEP->getName(), EEP->getLoc())) {
              P = new (Context)
                  OptionalSomePattern(EEP, EEP->getEndLoc(), /*implicit*/true);
              return coercePatternToType(P, resolution, type, options);
            } else {
              diags.diagnose(EEP->getLoc(),
                             diag::enum_element_pattern_member_not_found,
                             EEP->getName().str(), type);
              return true;
            }
          }
        }
      }

      if (!elt)
        return true;

      enumTy = type;
    } else {
      // Check if the explicitly-written enum type matches the type we're
      // coercing to.
      assert(!EEP->getParentType().isNull()
             && "enum with resolved element doesn't specify parent type?!");
      auto parentTy = EEP->getParentType().getType();
      // If the type matches exactly, use it.
      if (parentTy->isEqual(type)) {
        enumTy = type;
      }
      // Otherwise, if the type is an unbound generic of the context type, use
      // the context type to resolve the parameters.
      else if (parentTy->hasUnboundGenericType()) {
        if (parentTy->is<UnboundGenericType>() &&
            parentTy->getAnyNominal() == type->getAnyNominal()) {
          enumTy = type;
        } else {
          diags.diagnose(EEP->getLoc(), diag::ambiguous_enum_pattern_type,
                         parentTy, type);
          return true;
        }
      }
      // Otherwise, see if we can introduce a cast pattern to get from an
      // existential pattern type to the enum type.
      else if (type->isAnyExistentialType()) {
        auto foundCastKind =
          typeCheckCheckedCast(type, parentTy,
                               CheckedCastContextKind::EnumElementPattern,
                               dc,
                               EEP->getLoc(),
                               nullptr, SourceRange());
        // If the cast failed, we can't resolve the pattern.
        if (foundCastKind < CheckedCastKind::First_Resolved)
          return true;
        
        // Otherwise, we can type-check as the enum type, and insert a cast
        // from the outer pattern type.
        castKind = foundCastKind;
        enumTy = parentTy;
      } else {
        diags.diagnose(EEP->getLoc(),
                       diag::enum_element_pattern_not_member_of_enum,
                       EEP->getName().str(), type);
        return true;
      }
    }

    // If there is a subpattern, push the enum element type down onto it.
    auto argType = elt->getArgumentInterfaceType();
    if (EEP->hasSubPattern()) {
      Pattern *sub = EEP->getSubPattern();
      if (!elt->hasAssociatedValues()) {
        diags.diagnose(EEP->getLoc(),
                       diag::enum_element_pattern_assoc_values_mismatch,
                       EEP->getName());
        diags.diagnose(EEP->getLoc(),
                       diag::enum_element_pattern_assoc_values_remove)
          .fixItRemove(sub->getSourceRange());
        return true;
      }
      
      Type elementType;
      if (argType)
        elementType = enumTy->getTypeOfMember(elt->getModuleContext(),
                                              elt, argType);
      else
        elementType = TupleType::getEmpty(Context);
      auto newSubOptions = subOptions;
      newSubOptions.setContext(TypeResolverContext::EnumPatternPayload);
      newSubOptions |= TypeResolutionFlags::FromNonInferredPattern;

      ::implicitlyUntuplePatternIfApplicable(Context.Diags, sub, elementType);

      if (coercePatternToType(sub, resolution, elementType, newSubOptions))
        return true;
      EEP->setSubPattern(sub);
    } else if (argType) {
      // Else if the element pattern has no sub-pattern but the element type has
      // associated values, expand it to be semantically equivalent to an
      // element pattern of wildcards.
      Type elementType = enumTy->getTypeOfMember(elt->getModuleContext(),
                                                 elt, argType);
      SmallVector<TuplePatternElt, 8> elements;
      if (auto *TTy = dyn_cast<TupleType>(elementType.getPointer())) {
        for (auto &elt : TTy->getElements()) {
          auto *subPattern = new (Context) AnyPattern(SourceLoc());
          elements.push_back(TuplePatternElt(elt.getName(), SourceLoc(),
                                             subPattern));
        }
      } else {
        auto parenTy = dyn_cast<ParenType>(elementType.getPointer());
        assert(parenTy && "Associated value type is neither paren nor tuple?");
        (void)parenTy;
        
        auto *subPattern = new (Context) AnyPattern(SourceLoc());
        elements.push_back(TuplePatternElt(Identifier(), SourceLoc(),
                                           subPattern));
      }
      Pattern *sub = TuplePattern::createSimple(Context, SourceLoc(),
                                                elements, SourceLoc(),
                                                /*implicit*/true);
      auto newSubOptions = subOptions;
      newSubOptions.setContext(TypeResolverContext::EnumPatternPayload);
      newSubOptions |= TypeResolutionFlags::FromNonInferredPattern;
      if (coercePatternToType(sub, resolution, elementType, newSubOptions))
        return true;
      EEP->setSubPattern(sub);
    }

    EEP->setElementDecl(elt);
    EEP->setType(enumTy);
    
    // Ensure that the type of our TypeLoc is fully resolved. If an unbound
    // generic type was spelled in the source (e.g. `case Optional.None:`) this
    // will fill in the generic parameters.
    EEP->getParentType().setType(enumTy);
    
    // If we needed a cast, wrap the pattern in a cast pattern.
    if (castKind) {
      auto isPattern = new (Context) IsPattern(SourceLoc(),
                                               TypeLoc::withoutLoc(enumTy),
                                               EEP, *castKind,
                                               /*implicit*/true);
      isPattern->setType(type);
      P = isPattern;
    }
    
    return false;
  }

  case PatternKind::OptionalSome: {
    auto *OP = cast<OptionalSomePattern>(P);
    Type elementType = type->getOptionalObjectType();

    if (elementType.isNull()) {
      auto diagID = diag::optional_element_pattern_not_valid_type;
      SourceLoc loc = OP->getQuestionLoc();
      // Produce tailored diagnostic for if/let and other conditions.
      if (OP->isImplicit()) {
        diagID = diag::condition_optional_element_pattern_not_valid_type;
        loc = OP->getLoc();
      }

      diags.diagnose(loc, diagID, type);
      return true;
    }

    EnumElementDecl *elementDecl = Context.getOptionalSomeDecl();
    if (!elementDecl)
      return true;

    OP->setElementDecl(elementDecl);

    Pattern *sub = OP->getSubPattern();
    auto newSubOptions = subOptions;
    newSubOptions.setContext(TypeResolverContext::EnumPatternPayload);
    newSubOptions |= TypeResolutionFlags::FromNonInferredPattern;
    if (coercePatternToType(sub, resolution, elementType, newSubOptions))
      return true;
    OP->setSubPattern(sub);
    OP->setType(type);
    return false;
  }

  case PatternKind::Bool:
    P->setType(type);
    return false;
  }
  llvm_unreachable("bad pattern kind!");
}


/// Coerce the specified parameter list of a ClosureExpr to the specified
/// contextual type.
void TypeChecker::coerceParameterListToType(ParameterList *P, ClosureExpr *CE,
                                            AnyFunctionType *FN) {

  // Local function to check if the given type is valid e.g. doesn't have
  // errors, type variables or unresolved types related to it.
  auto isValidType = [](Type type) -> bool {
    return !(type->hasError() || type->hasUnresolvedType());
  };

  // Local function to check whether type of given parameter
  // should be coerced to a given contextual type or not.
  auto shouldOverwriteParam = [&](ParamDecl *param) -> bool {
    if (param->isInvalid())
      return true;

    return !isValidType(param->getType());
  };

  auto handleParameter = [&](ParamDecl *param, Type ty, bool forceMutable) {
    if (forceMutable)
      param->setSpecifier(ParamDecl::Specifier::InOut);

    // If contextual type is invalid and we have a valid argument type
    // trying to coerce argument to contextual type would mean erasing
    // valuable diagnostic information.
    if (isValidType(ty) || shouldOverwriteParam(param)) {
      param->setInterfaceType(ty->mapTypeOutOfContext());
    }
  };

  // Coerce each parameter to the respective type.
  ArrayRef<AnyFunctionType::Param> params = FN->getParams();
  for (unsigned i = 0, e = P->size(); i != e; ++i) {
    auto &param = P->get(i);
    assert(param->getArgumentName().empty() &&
           "Closures cannot have API names");
    
    handleParameter(param,
                    params[i].getParameterType(),
                    params[i].isInOut());
    assert(!param->isDefaultArgument() && "Closures cannot have default args");
  }
}
