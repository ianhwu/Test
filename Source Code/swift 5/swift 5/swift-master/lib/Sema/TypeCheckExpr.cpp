//===--- TypeCheckExpr.cpp - Type Checking for Expressions ----------------===//
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
//
// This file implements semantic analysis for expressions, analyzing an
// expression tree in post-order, bottom-up, from leaves up to the root.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Parse/Lexer.h"
using namespace swift;

//===----------------------------------------------------------------------===//
// Expression Semantic Analysis Routines
//===----------------------------------------------------------------------===//

static void substituteInputSugarArgumentType(Type argTy, CanType resultTy,
                                             Type &resultSugarTy,
                                             bool &uniqueSugarTy) {
  // If we already failed finding a unique sugar, bail out.
  if (!uniqueSugarTy)
    return;
    
  if (TupleType *argTupleTy = argTy->getAs<TupleType>()) {
    // Recursively walk tuple arguments.
    for (auto &field : argTupleTy->getElements()) {
      substituteInputSugarArgumentType(field.getType(), resultTy,
                                       resultSugarTy, uniqueSugarTy);
      if (!uniqueSugarTy)
        return;
    }
    return;
  }
  
  if (argTy->getCanonicalType() != resultTy) {
    // If the argument is a metatype of what we're looking for, propagate that.
    if (auto MTT = argTy->getAs<MetatypeType>())
      argTy = MTT->getInstanceType();

    if (argTy->getCanonicalType() != resultTy)
      return;
  }

  // If this type is parenthesized, remove the parens.  We don't want to
  // propagate parens from arguments to the result type.
  argTy = argTy->getWithoutParens();
  
  // If this is the first match against the sugar type we found, use it.
  if (!resultSugarTy) {
    resultSugarTy = argTy;
    return;
  }
  
  // Make sure this argument's sugar is consistent with the sugar we
  // already found.
  if (argTy.getPointer() == resultSugarTy.getPointer())
    return;
  uniqueSugarTy = false;
}

/// If we can propagate type sugar from input arguments types to the result of
/// an apply, do so.
///
Expr *TypeChecker::substituteInputSugarTypeForResult(ApplyExpr *E) {
  if (!E->getType() || E->getType()->hasError())
    return E;
  
  Type resultTy = E->getFn()->getType()->castTo<FunctionType>()->getResult();

  /// Check to see if you have "x+y" (where x and y are type aliases) that match
  // the canonical result type.  If so, propagate the sugar.
  Type resultSugarTy; // null if no sugar found, set when sugar found
  bool uniqueSugarTy = true; // true if a unique sugar mapping found
  substituteInputSugarArgumentType(E->getArg()->getType(),
                                   resultTy->getCanonicalType(),
                                   resultSugarTy, uniqueSugarTy);
  
  if (resultSugarTy && uniqueSugarTy && E->getType()->isCanonical()) {
    E->setType(resultSugarTy);
    return E;
  }

  // Otherwise check to see if this is a ConstructorRefExpr on a TypeExpr with
  // sugar on it.  If so, propagate the sugar to the curried result function
  // type.
  if (isa<ConstructorRefCallExpr>(E) && isa<TypeExpr>(E->getArg())) {
    auto resultSugar = cast<TypeExpr>(E->getArg())->getInstanceType();

    // The result of this apply is "(args) -> T" where T is the type being
    // constructed.  Apply the sugar onto it.
    if (auto FT = E->getType()->getAs<FunctionType>())
      if (FT->getResult()->isEqual(resultSugar) && !resultSugar->isCanonical()){
        auto NFT = FunctionType::get(FT->getParams(), resultSugar,
                                     FT->getExtInfo());
        E->setType(NFT);
        return E;
      }
  }

  // Otherwise, if the callee function had sugar on the result type, but it got
  // dropped, make sure to propagate it along.
  if (!resultTy->isCanonical() && E->getType()->isCanonical() &&
      resultTy->isEqual(E->getType())) {
    E->setType(resultTy);
    return E;
  }


  return E;
}

static PrecedenceGroupDecl *lookupPrecedenceGroupForOperator(DeclContext *DC,
                                                             Identifier name,
                                                             SourceLoc loc) {
  SourceFile *SF = DC->getParentSourceFile();
  bool isCascading = DC->isCascadingContextForLookup(true);
  if (auto op = SF->lookupInfixOperator(name, isCascading, loc)) {
    return op->getPrecedenceGroup();
  } else {
    DC->getASTContext().Diags.diagnose(loc, diag::unknown_binop);
  }
  return nullptr;
}

PrecedenceGroupDecl *
TypeChecker::lookupPrecedenceGroupForInfixOperator(DeclContext *DC, Expr *E) {
  /// Look up the builtin precedence group with the given name.

  auto getBuiltinPrecedenceGroup = [](DeclContext *DC, Identifier name,
                                      SourceLoc loc) {
    auto group = TypeChecker::lookupPrecedenceGroup(DC, name, loc);
    if (!group) {
      DC->getASTContext().Diags.diagnose(
          loc, diag::missing_builtin_precedence_group, name);
    }
    return group;
  };
  
  auto &Context = DC->getASTContext();
  if (auto ifExpr = dyn_cast<IfExpr>(E)) {
    // Ternary has fixed precedence.
    return getBuiltinPrecedenceGroup(DC, Context.Id_TernaryPrecedence,
                                     ifExpr->getQuestionLoc());
  }

  if (auto assignExpr = dyn_cast<AssignExpr>(E)) {
    // Assignment has fixed precedence.
    return getBuiltinPrecedenceGroup(DC, Context.Id_AssignmentPrecedence,
                                     assignExpr->getEqualLoc());
  }

  if (auto castExpr = dyn_cast<ExplicitCastExpr>(E)) {
    // 'as' and 'is' casts have fixed precedence.
    return getBuiltinPrecedenceGroup(DC, Context.Id_CastingPrecedence,
                                     castExpr->getAsLoc());
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    Identifier name = DRE->getDecl()->getBaseName().getIdentifier();
    return lookupPrecedenceGroupForOperator(DC, name, DRE->getLoc());
  }

  if (auto *OO = dyn_cast<OverloadedDeclRefExpr>(E)) {
    Identifier name = OO->getDecls()[0]->getBaseName().getIdentifier();
    return lookupPrecedenceGroupForOperator(DC, name, OO->getLoc());
  }

  if (auto arrowExpr = dyn_cast<ArrowExpr>(E)) {
    return getBuiltinPrecedenceGroup(DC,
                                     Context.Id_FunctionArrowPrecedence,
                                     arrowExpr->getArrowLoc());
  }

  // An already-folded binary operator comes up for non-primary use cases
  // of this function.
  if (auto binaryExpr = dyn_cast<BinaryExpr>(E)) {
    return lookupPrecedenceGroupForInfixOperator(DC, binaryExpr->getFn());
  }

  if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(E)) {
    return lookupPrecedenceGroupForInfixOperator(DC, DSCE->getFn());
  }

  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    Identifier name = MRE->getDecl().getDecl()->getBaseName().getIdentifier();
    return lookupPrecedenceGroupForOperator(DC, name, MRE->getLoc());
  }

  // If E is already an ErrorExpr, then we've diagnosed it as invalid already,
  // otherwise emit an error.
  if (!isa<ErrorExpr>(E))
    Context.Diags.diagnose(E->getLoc(), diag::unknown_binop);

  return nullptr;
}

/// Find LHS as if we append binary operator to existing pre-folded expresion.
/// Returns found expression, or \c nullptr if the operator is not applicable.
///
/// For example, given '(== R (* A B))':
/// 'findLHS(DC, expr, "+")' returns '(* A B)'.
/// 'findLHS(DC, expr, "<<")' returns 'B'.
/// 'findLHS(DC, expr, '==')' returns nullptr.
Expr *TypeChecker::findLHS(DeclContext *DC, Expr *E, Identifier name) {
  auto right = lookupPrecedenceGroupForOperator(DC, name, E->getEndLoc());
  if (!right)
    return nullptr;

  while (true) {

    // Look through implicit conversions.
    if (auto ICE = dyn_cast<ImplicitConversionExpr>(E)) {
      E = ICE->getSyntacticSubExpr();
      continue;
    }
    if (auto ACE = dyn_cast<AutoClosureExpr>(E)) {
      E = ACE->getSingleExpressionBody();
      continue;
    }

    auto left = lookupPrecedenceGroupForInfixOperator(DC, E);
    if (!left)
      // LHS is not binary expression.
      return E;
    switch (DC->getASTContext().associateInfixOperators(left, right)) {
      case swift::Associativity::None:
        return nullptr;
      case swift::Associativity::Left:
        return E;
      case swift::Associativity::Right:
        break;
    }
    // Find the RHS of the current binary expr.
    if (auto *assignExpr = dyn_cast<AssignExpr>(E)) {
      E = assignExpr->getSrc();
    } else if (auto *ifExpr = dyn_cast<IfExpr>(E)) {
      E = ifExpr->getElseExpr();
    } else if (auto *binaryExpr = dyn_cast<BinaryExpr>(E)) {
      auto *Args = dyn_cast<TupleExpr>(binaryExpr->getArg());
      if (!Args || Args->getNumElements() != 2)
        return nullptr;
      E = Args->getElement(1);
    } else {
      // E.g. 'fn() as Int << 2'.
      // In this case '<<' has higher precedence than 'as', but the LHS should
      // be 'fn() as Int' instead of 'Int'.
      return E;
    }
  }
}

// The way we compute isEndOfSequence relies on the assumption that
// the sequence-folding algorithm never recurses with a prefix of the
// entire sequence.
static Expr *makeBinOp(ASTContext &Ctx, Expr *Op, Expr *LHS, Expr *RHS,
                       PrecedenceGroupDecl *opPrecedence,
                       bool isEndOfSequence) {
  if (!LHS || !RHS)
    return nullptr;

  // If the left-hand-side is a 'try', hoist it up.
  auto *tryEval = dyn_cast<AnyTryExpr>(LHS);
  if (tryEval) {
    LHS = tryEval->getSubExpr();
  }
  
  // If this is an assignment operator, and the left operand is an optional
  // evaluation, pull the operator into the chain.
  OptionalEvaluationExpr *optEval = nullptr;
  if (opPrecedence && opPrecedence->isAssignment()) {
    if ((optEval = dyn_cast<OptionalEvaluationExpr>(LHS))) {
      LHS = optEval->getSubExpr();
    }
  }

  // If the right operand is a try, it's an error unless the operator
  // is an assignment or conditional operator and there's nothing to
  // the right that didn't parse as part of the right operand.
  //
  // Generally, nothing to the right will fail to parse as part of the
  // right operand because there are no standard operators that have
  // lower precedence than assignment operators or the conditional
  // operator.
  //
  // We allow the right operand of the conditional operator to begin
  // with 'try' for consistency with the middle operand.  This allows:
  //   x ? try foo() : try bar()
  // but not:
  //   x ? try foo() : try bar() $#! 1
  // assuming $#! is some crazy operator with lower precedence
  // than the conditional operator.
  if (isa<AnyTryExpr>(RHS)) {
    // If you change this, also change TRY_KIND_SELECT in diagnostics.
    enum class TryKindForDiagnostics : unsigned {
      Try,
      ForceTry,
      OptionalTry
    };
    TryKindForDiagnostics tryKind;
    switch (RHS->getKind()) {
    case ExprKind::Try:
      tryKind = TryKindForDiagnostics::Try;
      break;
    case ExprKind::ForceTry:
      tryKind = TryKindForDiagnostics::ForceTry;
      break;
    case ExprKind::OptionalTry:
      tryKind = TryKindForDiagnostics::OptionalTry;
      break;
    default:
      llvm_unreachable("unknown try-like expression");
    }

    if (isa<IfExpr>(Op) ||
        (opPrecedence && opPrecedence->isAssignment())) {
      if (!isEndOfSequence) {
        if (isa<IfExpr>(Op)) {
          Ctx.Diags.diagnose(RHS->getStartLoc(), diag::try_if_rhs_noncovering,
                             static_cast<unsigned>(tryKind));
        } else {
          Ctx.Diags.diagnose(RHS->getStartLoc(),
                             diag::try_assign_rhs_noncovering,
                             static_cast<unsigned>(tryKind));
        }
      }
    } else {
      Ctx.Diags.diagnose(RHS->getStartLoc(), diag::try_rhs,
                         static_cast<unsigned>(tryKind));
    }
  }

  // Fold the result into the optional evaluation or try.
  auto makeResultExpr = [&](Expr *result) -> Expr * {
    if (optEval) {
      optEval->setSubExpr(result);
      result = optEval;
    }
    if (tryEval) {
      tryEval->setSubExpr(result);
      result = tryEval;
    }
    return result;
  };
  
  if (auto *ifExpr = dyn_cast<IfExpr>(Op)) {
    // Resolve the ternary expression.
    assert(!ifExpr->isFolded() && "already folded if expr in sequence?!");
    ifExpr->setCondExpr(LHS);
    ifExpr->setElseExpr(RHS);
    return makeResultExpr(ifExpr);
  }

  if (auto *assign = dyn_cast<AssignExpr>(Op)) {
    // Resolve the assignment expression.
    assert(!assign->isFolded() && "already folded assign expr in sequence?!");
    assign->setDest(LHS);
    assign->setSrc(RHS);
    return makeResultExpr(assign);
  }
  
  if (auto *as = dyn_cast<ExplicitCastExpr>(Op)) {
    // Resolve the 'as' or 'is' expression.
    assert(!as->isFolded() && "already folded 'as' expr in sequence?!");
    assert(RHS == as && "'as' with non-type RHS?!");
    as->setSubExpr(LHS);    
    return makeResultExpr(as);
  }

  if (auto *arrow = dyn_cast<ArrowExpr>(Op)) {
    // Resolve the '->' expression.
    assert(!arrow->isFolded() && "already folded '->' expr in sequence?!");
    arrow->setArgsExpr(LHS);
    arrow->setResultExpr(RHS);
    return makeResultExpr(arrow);
  }
  
  // Build the argument to the operation.
  Expr *ArgElts[] = { LHS, RHS };
  auto ArgElts2 = Ctx.AllocateCopy(MutableArrayRef<Expr*>(ArgElts));
  TupleExpr *Arg = TupleExpr::create(Ctx,
                                     SourceLoc(),
                                     ArgElts2, { }, { }, SourceLoc(),
                                     /*HasTrailingClosure=*/false,
                                     /*Implicit=*/true);

  
  
  // Build the operation.
  return makeResultExpr(new (Ctx) BinaryExpr(Op, Arg, Op->isImplicit()));
}

namespace {
  class PrecedenceBound {
    llvm::PointerIntPair<PrecedenceGroupDecl*,1,bool> GroupAndIsStrict;
  public:
    PrecedenceBound() {}
    PrecedenceBound(PrecedenceGroupDecl *decl, bool isStrict)
      : GroupAndIsStrict(decl, isStrict) {}

    bool shouldConsider(PrecedenceGroupDecl *group) {
      auto storedGroup = GroupAndIsStrict.getPointer();
      if (!storedGroup) return true;
      if (!group) return false;
      if (storedGroup == group) return !GroupAndIsStrict.getInt();
      return group->getASTContext().associateInfixOperators(group, storedGroup)
               != Associativity::Right;
    }
  };
} // end anonymous namespace

/// foldSequence - Take a sequence of expressions and fold a prefix of
/// it into a tree of BinaryExprs using precedence parsing.
static Expr *foldSequence(DeclContext *DC,
                          Expr *LHS,
                          ArrayRef<Expr*> &S,
                          PrecedenceBound precedenceBound) {
  // Invariant: S is even-sized.
  // Invariant: All elements at even indices are operator references.
  assert(!S.empty());
  assert((S.size() & 1) == 0);
  
  struct Op {
    Expr *op;
    PrecedenceGroupDecl *precedence;
    
    explicit operator bool() const { return op != nullptr; }
  };
  
  /// Get the operator, if appropriate to this pass.
  auto getNextOperator = [&]() -> Op {
    Expr *op = S[0];

    // If the operator's precedence is lower than the minimum, stop here.
    auto opPrecedence = TypeChecker::lookupPrecedenceGroupForInfixOperator(DC, op);
    if (!precedenceBound.shouldConsider(opPrecedence))
      return {nullptr, nullptr};
    return {op, opPrecedence};
  };

  // Extract out the first operator.
  Op op1 = getNextOperator();
  if (!op1) return LHS;
  
  // We will definitely be consuming at least one operator.
  // Pull out the prospective RHS and slice off the first two elements.
  Expr *RHS = S[1];
  S = S.slice(2);

  auto &Ctx = DC->getASTContext();
  while (!S.empty()) {
    assert((S.size() & 1) == 0);
    assert(precedenceBound.shouldConsider(op1.precedence));

    // If the operator is a cast operator, the RHS can't extend past the type
    // that's part of the cast production.
    if (isa<ExplicitCastExpr>(op1.op)) {
      LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
      op1 = getNextOperator();
      if (!op1) return LHS;
      RHS = S[1];
      S = S.slice(2);
      continue;
    }
    
    // Pull out the next binary operator.
    Op op2{ S[0], TypeChecker::lookupPrecedenceGroupForInfixOperator(DC, S[0]) };

    // If the second operator's precedence is lower than the
    // precedence bound, break out of the loop.
    if (!precedenceBound.shouldConsider(op2.precedence)) break;

    // If we're missing precedence info for either operator, treat them
    // as non-associative.
    Associativity associativity;
    if (!op1.precedence || !op2.precedence) {
      associativity = Associativity::None;
    } else {
      associativity =
        Ctx.associateInfixOperators(op1.precedence, op2.precedence);
    }

    // Apply left-associativity immediately by folding the first two
    // operands.
    if (associativity == Associativity::Left) {
      LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
      op1 = op2;
      RHS = S[1];
      S = S.slice(2);
      continue;
    }

    // If the first operator's precedence is lower than the second
    // operator's precedence, recursively fold all such
    // higher-precedence operators starting from this point, then
    // repeat.
    if (associativity == Associativity::Right &&
        op1.precedence != op2.precedence) {
      RHS = foldSequence(DC, RHS, S,
                         PrecedenceBound(op1.precedence, /*strict*/ true));
      continue;
    }

    // Apply right-associativity by recursively folding operators
    // starting from this point, then immediately folding the LHS and RHS.
    if (associativity == Associativity::Right) {
      RHS = foldSequence(DC, RHS, S,
                         PrecedenceBound(op1.precedence, /*strict*/ false));
      LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());

      // If we've drained the entire sequence, we're done.
      if (S.empty()) return LHS;

      // Otherwise, start all over with our new LHS.
      return foldSequence(DC, LHS, S, precedenceBound);
    }

    // If we ended up here, it's because we're either:
    //   - missing precedence groups,
    //   - have unordered precedence groups, or
    //   - have the same precedence group with no associativity.
    assert(associativity == Associativity::None);

    // Don't diagnose if we're missing a precedence group; this is
    // an invalid-code situation.
    if (!op1.precedence || !op2.precedence) {
      // do nothing
    } else if (op1.precedence == op2.precedence) {
      assert(op1.precedence->isNonAssociative());
      // FIXME: QoI ranges
      Ctx.Diags.diagnose(op1.op->getLoc(),
                         diag::non_associative_adjacent_operators,
                         op1.precedence->getName())
        .highlight(SourceRange(op2.op->getLoc(), op2.op->getLoc()));

    } else {
      Ctx.Diags.diagnose(op1.op->getLoc(),
                         diag::unordered_adjacent_operators,
                         op1.precedence->getName(), op2.precedence->getName())
        .highlight(SourceRange(op2.op->getLoc(), op2.op->getLoc()));      
    }
    
    // Recover by arbitrarily binding the first two.
    LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
    return foldSequence(DC, LHS, S, precedenceBound);
  }

  // Fold LHS and RHS together and declare completion.
  return makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
}

bool TypeChecker::requireOptionalIntrinsics(ASTContext &ctx, SourceLoc loc) {
  if (ctx.hasOptionalIntrinsics())
    return false;

  ctx.Diags.diagnose(loc, diag::optional_intrinsics_not_found);
  return true;
}

bool TypeChecker::requirePointerArgumentIntrinsics(ASTContext &ctx,
                                                   SourceLoc loc) {
  if (ctx.hasPointerArgumentIntrinsics())
    return false;

  ctx.Diags.diagnose(loc, diag::pointer_argument_intrinsics_not_found);
  return true;
}

bool TypeChecker::requireArrayLiteralIntrinsics(ASTContext &ctx,
                                                SourceLoc loc) {
  if (ctx.hasArrayLiteralIntrinsics())
    return false;

  ctx.Diags.diagnose(loc, diag::array_literal_intrinsics_not_found);
  return true;
}

Expr *TypeChecker::buildCheckedRefExpr(VarDecl *value, DeclContext *UseDC,
                                       DeclNameLoc loc, bool Implicit) {
  auto type = TypeChecker::getUnopenedTypeOfReference(value, Type(), UseDC);
  auto semantics = value->getAccessSemanticsFromContext(UseDC,
                                                       /*isAccessOnSelf*/false);
  return new (value->getASTContext())
      DeclRefExpr(value, loc, Implicit, semantics, type);
}

Expr *TypeChecker::buildRefExpr(ArrayRef<ValueDecl *> Decls,
                                DeclContext *UseDC, DeclNameLoc NameLoc,
                                bool Implicit, FunctionRefKind functionRefKind) {
  assert(!Decls.empty() && "Must have at least one declaration");

  auto &Context = UseDC->getASTContext();
  if (Decls.size() == 1 && !isa<ProtocolDecl>(Decls[0]->getDeclContext())) {
    auto semantics = Decls[0]->getAccessSemanticsFromContext(UseDC,
                                                       /*isAccessOnSelf*/false);
    return new (Context) DeclRefExpr(Decls[0], NameLoc, Implicit, semantics);
  }

  Decls = Context.AllocateCopy(Decls);
  auto result = new (Context) OverloadedDeclRefExpr(Decls, NameLoc, 
                                                    functionRefKind,
                                                    Implicit);
  return result;
}

static Type lookupDefaultLiteralType(const DeclContext *dc,
                                     StringRef name) {
  auto &ctx = dc->getASTContext();
  auto lookupOptions = defaultUnqualifiedLookupOptions;
  if (isa<AbstractFunctionDecl>(dc))
    lookupOptions |= NameLookupFlags::KnownPrivate;
  auto lookup = TypeChecker::lookupUnqualified(dc->getModuleScopeContext(),
                                               ctx.getIdentifier(name),
                                               SourceLoc(),
                                               lookupOptions);
  TypeDecl *TD = lookup.getSingleTypeResult();
  if (!TD)
    return Type();
  
  if (TD->isInvalid())
    return Type();

  if (auto *NTD = dyn_cast<NominalTypeDecl>(TD))
    return NTD->getDeclaredType();
  return cast<TypeAliasDecl>(TD)->getDeclaredInterfaceType();
}

static Optional<KnownProtocolKind>
getKnownProtocolKindIfAny(const ProtocolDecl *protocol) {
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, _, __, ___)              \
  if (protocol == TypeChecker::getProtocol(protocol->getASTContext(),          \
                                           SourceLoc(),                        \
                                           KnownProtocolKind::Id))             \
    return KnownProtocolKind::Id;
#include "swift/AST/KnownProtocols.def"
#undef EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME

  return None;
}

Type TypeChecker::getDefaultType(ProtocolDecl *protocol, DeclContext *dc) {
  if (auto knownProtocolKindIfAny = getKnownProtocolKindIfAny(protocol)) {
    return evaluateOrDefault(
        protocol->getASTContext().evaluator,
        DefaultTypeRequest{knownProtocolKindIfAny.getValue(), dc}, nullptr);
  }
  return Type();
}

static std::pair<const char *, bool> lookupDefaultTypeInfoForKnownProtocol(
    const KnownProtocolKind knownProtocolKind) {
  switch (knownProtocolKind) {
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, Name, typeName,          \
                                                  performLocalLookup)          \
  case KnownProtocolKind::Id:                                                  \
    return {typeName, performLocalLookup};
#include "swift/AST/KnownProtocols.def"
#undef EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME
  default:
    return {nullptr, false};
  }
}

llvm::Expected<Type>
swift::DefaultTypeRequest::evaluate(Evaluator &evaluator,
                                    KnownProtocolKind knownProtocolKind,
                                    const DeclContext *dc) const {
  const char *name;
  bool performLocalLookup;
  std::tie(name, performLocalLookup) =
      lookupDefaultTypeInfoForKnownProtocol(knownProtocolKind);
  if (!name)
    return nullptr;

  Type type;
  if (performLocalLookup)
    type = lookupDefaultLiteralType(dc, name);

  if (!type)
    type = lookupDefaultLiteralType(TypeChecker::getStdlibModule(dc), name);

  // Strip off one level of sugar; we don't actually want to print
  // the name of the typealias itself anywhere.
  if (type) {
    if (auto boundTypeAlias = dyn_cast<TypeAliasType>(type.getPointer()))
      type = boundTypeAlias->getSinglyDesugaredType();
  }
  return type;
}

Expr *TypeChecker::foldSequence(SequenceExpr *expr, DeclContext *dc) {
  ArrayRef<Expr*> Elts = expr->getElements();
  assert(Elts.size() > 1 && "inadequate number of elements in sequence");
  assert((Elts.size() & 1) == 1 && "even number of elements in sequence");

  Expr *LHS = Elts[0];
  Elts = Elts.slice(1);

  Expr *Result = ::foldSequence(dc, LHS, Elts, PrecedenceBound());
  assert(Elts.empty());

  return Result;
}
