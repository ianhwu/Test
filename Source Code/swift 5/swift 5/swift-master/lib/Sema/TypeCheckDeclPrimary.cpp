//===--- TypeCheckDeclPrimary.cpp - Type Checking for Primary Files -------===//
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
// This file implements type checking for primary files, that is, files whose
// declarations we're planning to emit. This exhaustively triggers diagnostics
// and type checking of all delayed bodies in those files.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "ConstraintSystem.h"
#include "DerivedConformances.h"
#include "TypeChecker.h"
#include "TypeCheckAccess.h"
#include "TypeCheckDecl.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckObjC.h"
#include "TypeCheckType.h"
#include "MiscDiagnostics.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ReferencedNameTracker.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Statistic.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Strings.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DJB.h"

using namespace swift;

#define DEBUG_TYPE "TypeCheckDeclPrimary"

/// Check the inheritance clause of a type declaration or extension thereof.
///
/// This routine performs detailed checking of the inheritance clause of the
/// given type or extension. It need only be called within the primary source
/// file.
static void checkInheritanceClause(
                    llvm::PointerUnion<TypeDecl *, ExtensionDecl *> declUnion) {
  DeclContext *DC;
  MutableArrayRef<TypeLoc> inheritedClause;
  ExtensionDecl *ext = nullptr;
  TypeDecl *typeDecl = nullptr;
  Decl *decl;
  if ((ext = declUnion.dyn_cast<ExtensionDecl *>())) {
    decl = ext;
    DC = ext;

    inheritedClause = ext->getInherited();

    // Protocol extensions cannot have inheritance clauses.
    if (auto proto = ext->getExtendedProtocolDecl()) {
      if (!inheritedClause.empty()) {
        ext->diagnose(diag::extension_protocol_inheritance,
                 proto->getName())
          .highlight(SourceRange(inheritedClause.front().getSourceRange().Start,
                                 inheritedClause.back().getSourceRange().End));
        return;
      }
    }
  } else {
    typeDecl = declUnion.get<TypeDecl *>();
    decl = typeDecl;
    if (auto nominal = dyn_cast<NominalTypeDecl>(typeDecl)) {
      DC = nominal;
    } else {
      DC = typeDecl->getDeclContext();
    }

    inheritedClause = typeDecl->getInherited();
  }

  // Can this declaration's inheritance clause contain a class or
  // subclass existential?
  bool canHaveSuperclass = (isa<ClassDecl>(decl) ||
                            (isa<ProtocolDecl>(decl) &&
                             !cast<ProtocolDecl>(decl)->isObjC()));

  ASTContext &ctx = decl->getASTContext();
  auto &diags = ctx.Diags;

  // Retrieve the location of the start of the inheritance clause.
  auto getStartLocOfInheritanceClause = [&] {
    if (ext)
      return ext->getSourceRange().End;

    return typeDecl->getNameLoc();
  };

  // Compute the source range to be used when removing something from an
  // inheritance clause.
  auto getRemovalRange = [&](unsigned i) {
    // If there is just one entry, remove the entire inheritance clause.
    if (inheritedClause.size() == 1) {
      SourceLoc start = getStartLocOfInheritanceClause();
      SourceLoc end = inheritedClause[i].getSourceRange().End;
      return SourceRange(Lexer::getLocForEndOfToken(ctx.SourceMgr, start),
                         Lexer::getLocForEndOfToken(ctx.SourceMgr, end));
    }

    // If we're at the first entry, remove from the start of this entry to the
    // start of the next entry.
    if (i == 0) {
      return SourceRange(inheritedClause[i].getSourceRange().Start,
                         inheritedClause[i+1].getSourceRange().Start);
    }

    // Otherwise, remove from the end of the previous entry to the end of this
    // entry.
    SourceLoc afterPriorLoc =
      Lexer::getLocForEndOfToken(ctx.SourceMgr,
                                 inheritedClause[i-1].getSourceRange().End);

    SourceLoc afterMyEndLoc =
      Lexer::getLocForEndOfToken(ctx.SourceMgr,
                                 inheritedClause[i].getSourceRange().End);

    return SourceRange(afterPriorLoc, afterMyEndLoc);
  };

  // Check all of the types listed in the inheritance clause.
  Type superclassTy;
  SourceRange superclassRange;
  Optional<std::pair<unsigned, SourceRange>> inheritedAnyObject;
  for (unsigned i = 0, n = inheritedClause.size(); i != n; ++i) {
    auto &inherited = inheritedClause[i];

    // Validate the type.
    InheritedTypeRequest request{declUnion, i, TypeResolutionStage::Interface};
    Type inheritedTy = evaluateOrDefault(ctx.evaluator, request, Type());

    // If we couldn't resolve an the inherited type, or it contains an error,
    // ignore it.
    if (!inheritedTy || inheritedTy->hasError())
      continue;

    // For generic parameters and associated types, the GSB checks constraints;
    // however, we still want to fire off the requests to produce diagnostics
    // in some circular validation cases.
    if (isa<AbstractTypeParamDecl>(decl))
      continue;

    // Check whether we inherited from 'AnyObject' twice.
    // Other redundant-inheritance scenarios are checked below, the
    // GenericSignatureBuilder (for protocol inheritance) or the
    // ConformanceLookupTable (for protocol conformance).
    if (inheritedTy->isAnyObject()) {
      if (inheritedAnyObject) {
        // If the first occurrence was written as 'class', downgrade the error
        // to a warning in such case for backward compatibility with
        // Swift <= 4.
        auto knownIndex = inheritedAnyObject->first;
        auto knownRange = inheritedAnyObject->second;
        SourceRange removeRange = getRemovalRange(knownIndex);
        if (!ctx.LangOpts.isSwiftVersionAtLeast(5) &&
            (isa<ProtocolDecl>(decl) || isa<AbstractTypeParamDecl>(decl)) &&
            Lexer::getTokenAtLocation(ctx.SourceMgr, knownRange.Start)
              .is(tok::kw_class)) {
          SourceLoc classLoc = knownRange.Start;

          diags.diagnose(classLoc, diag::duplicate_anyobject_class_inheritance)
            .fixItRemoveChars(removeRange.Start, removeRange.End);
        } else {
          diags.diagnose(inherited.getSourceRange().Start,
                 diag::duplicate_inheritance, inheritedTy)
            .fixItRemoveChars(removeRange.Start, removeRange.End);
        }
        continue;
      }

      // Note that we saw inheritance from 'AnyObject'.
      inheritedAnyObject = { i, inherited.getSourceRange() };
    }

    if (inheritedTy->isExistentialType()) {
      auto layout = inheritedTy->getExistentialLayout();

      // Subclass existentials are not allowed except on classes and
      // non-@objc protocols.
      if (layout.explicitSuperclass &&
          !canHaveSuperclass) {
        decl->diagnose(diag::inheritance_from_protocol_with_superclass,
                       inheritedTy);
        continue;
      }

      // AnyObject is not allowed except on protocols.
      if (layout.hasExplicitAnyObject &&
          !isa<ProtocolDecl>(decl)) {
        decl->diagnose(canHaveSuperclass
                       ? diag::inheritance_from_non_protocol_or_class
                       : diag::inheritance_from_non_protocol,
                       inheritedTy);
        continue;
      }

      // If the existential did not have a class constraint, we're done.
      if (!layout.explicitSuperclass)
        continue;

      // Classes and protocols can inherit from subclass existentials.
      // For classes, we check for a duplicate superclass below.
      // For protocols, the GSB emits its own warning instead.
      if (isa<ProtocolDecl>(decl))
        continue;

      assert(isa<ClassDecl>(decl));
      assert(canHaveSuperclass);
      inheritedTy = layout.explicitSuperclass;
    }

    // If this is an enum inheritance clause, check for a raw type.
    if (isa<EnumDecl>(decl)) {
      // Check if we already had a raw type.
      if (superclassTy) {
        if (superclassTy->isEqual(inheritedTy)) {
          auto removeRange = getRemovalRange(i);
          diags.diagnose(inherited.getSourceRange().Start,
                         diag::duplicate_inheritance, inheritedTy)
            .fixItRemoveChars(removeRange.Start, removeRange.End);
        } else {
          diags.diagnose(inherited.getSourceRange().Start,
                         diag::multiple_enum_raw_types, superclassTy,
                         inheritedTy)
            .highlight(superclassRange);
        }
        continue;
      }
      
      // If this is not the first entry in the inheritance clause, complain.
      if (i > 0) {
        auto removeRange = getRemovalRange(i);

        diags.diagnose(inherited.getSourceRange().Start,
                       diag::raw_type_not_first, inheritedTy)
          .fixItRemoveChars(removeRange.Start, removeRange.End)
          .fixItInsert(inheritedClause[0].getSourceRange().Start,
                       inheritedTy.getString() + ", ");

        // Fall through to record the raw type.
      }

      // Record the raw type.
      superclassTy = inheritedTy;
      superclassRange = inherited.getSourceRange();
      continue;
    }

    // If this is a class type, it may be the superclass. We end up here when
    // the inherited type is either itself a class, or when it is a subclass
    // existential via the existential type path above.
    if (inheritedTy->getClassOrBoundGenericClass()) {
      // First, check if we already had a superclass.
      if (superclassTy) {
        // FIXME: Check for shadowed protocol names, i.e., NSObject?

        if (superclassTy->isEqual(inheritedTy)) {
          // Duplicate superclass.
          auto removeRange = getRemovalRange(i);
          diags.diagnose(inherited.getSourceRange().Start,
                         diag::duplicate_inheritance, inheritedTy)
            .fixItRemoveChars(removeRange.Start, removeRange.End);
        } else {
          // Complain about multiple inheritance.
          // Don't emit a Fix-It here. The user has to think harder about this.
          diags.diagnose(inherited.getSourceRange().Start,
                         diag::multiple_inheritance, superclassTy, inheritedTy)
            .highlight(superclassRange);
        }
        continue;
      }

      // If this is not the first entry in the inheritance clause, complain.
      if (isa<ClassDecl>(decl) && i > 0) {
        auto removeRange = getRemovalRange(i);
        diags.diagnose(inherited.getSourceRange().Start,
                       diag::superclass_not_first, inheritedTy)
          .fixItRemoveChars(removeRange.Start, removeRange.End)
          .fixItInsert(inheritedClause[0].getSourceRange().Start,
                       inheritedTy.getString() + ", ");

        // Fall through to record the superclass.
      }

      if (canHaveSuperclass) {
        // Record the superclass.
        superclassTy = inheritedTy;
        superclassRange = inherited.getSourceRange();
        continue;
      }
    }

    // We can't inherit from a non-class, non-protocol type.
    decl->diagnose(canHaveSuperclass
                   ? diag::inheritance_from_non_protocol_or_class
                   : diag::inheritance_from_non_protocol,
                   inheritedTy);
    // FIXME: Note pointing to the declaration 'inheritedTy' references?
  }
}

// Check for static properties that produce empty option sets
// using a rawValue initializer with a value of '0'
static void checkForEmptyOptionSet(const VarDecl *VD) {
  // Check if property is a 'static let'
  if (!VD->isStatic() || !VD->isLet())
    return;
  
  auto DC = VD->getDeclContext();
  
  // Make sure property is of same type as the type it is declared in
  if (!VD->getType()->isEqual(DC->getSelfTypeInContext()))
    return;
  
  // Make sure this type conforms to OptionSet
  auto *optionSetProto = VD->getASTContext().getProtocol(KnownProtocolKind::OptionSet);
  bool conformsToOptionSet = (bool)TypeChecker::containsProtocol(
                                                  DC->getSelfTypeInContext(),
                                                  optionSetProto,
                                                  DC,
                                                  /*Flags*/None);
  
  if (!conformsToOptionSet)
    return;
  
  auto PBD = VD->getParentPatternBinding();
  if (!PBD)
    return;
  
  auto initIndex = PBD->getPatternEntryIndexForVarDecl(VD);
  auto init = PBD->getInit(initIndex);

  // Make sure property is being set with a constructor
  auto ctor = dyn_cast_or_null<CallExpr>(init);
  if (!ctor)
    return;
  auto ctorCalledVal = ctor->getCalledValue();
  if (!ctorCalledVal)
    return;
  if (!isa<ConstructorDecl>(ctorCalledVal))
    return;
  
  // Make sure it is calling the rawValue constructor
  if (ctor->getNumArguments() != 1)
    return;
  if (ctor->getArgumentLabels().front() != VD->getASTContext().Id_rawValue)
    return;
  
  // Make sure the rawValue parameter is a '0' integer literal
  auto *args = cast<TupleExpr>(ctor->getArg());
  auto intArg = dyn_cast<IntegerLiteralExpr>(args->getElement(0));
  if (!intArg)
    return;
  if (intArg->getValue() != 0)
    return;
  
  VD->diagnose(diag::option_set_zero_constant, VD->getName());
  VD->diagnose(diag::option_set_empty_set_init)
    .fixItReplace(args->getSourceRange(), "([])");
}


/// Check the inheritance clauses generic parameters along with any
/// requirements stored within the generic parameter list.
static void checkGenericParams(GenericContext *ownerCtx) {
  const auto genericParams = ownerCtx->getGenericParams();
  if (!genericParams)
    return;

  for (auto gp : *genericParams) {
    TypeChecker::checkDeclAttributes(gp);
    checkInheritanceClause(gp);
  }

  // Force visitation of each of the requirements here.
  WhereClauseOwner(ownerCtx)
      .visitRequirements(TypeResolutionStage::Interface,
                         [](Requirement, RequirementRepr *) { return false; });
}

/// Check whether \c current is a redeclaration.
static void checkRedeclaration(ASTContext &ctx, ValueDecl *current) {
  // If we've already checked this declaration, don't do it again.
  if (current->alreadyCheckedRedeclaration())
    return;

  // Make sure we don't do this checking again.
  current->setCheckedRedeclaration(true);

  // Ignore invalid and anonymous declarations.
  if (current->isInvalid() || !current->hasName())
    return;

  // If this declaration isn't from a source file, don't check it.
  // FIXME: Should restrict this to the source file we care about.
  DeclContext *currentDC = current->getDeclContext();
  SourceFile *currentFile = currentDC->getParentSourceFile();
  if (!currentFile || currentDC->isLocalContext())
    return;

  ReferencedNameTracker *tracker = currentFile->getReferencedNameTracker();
  bool isCascading = (current->getFormalAccess() > AccessLevel::FilePrivate);

  // Find other potential definitions.
  SmallVector<ValueDecl *, 4> otherDefinitions;
  if (currentDC->isTypeContext()) {
    // Look within a type context.
    if (auto nominal = currentDC->getSelfNominalTypeDecl()) {
      auto found = nominal->lookupDirect(current->getBaseName());
      otherDefinitions.append(found.begin(), found.end());
      if (tracker)
        tracker->addUsedMember({nominal, current->getBaseName()}, isCascading);
    }
  } else {
    // Look within a module context.
    currentFile->getParentModule()->lookupValue(current->getBaseName(),
                                                NLKind::QualifiedLookup,
                                                otherDefinitions);
    if (tracker)
      tracker->addTopLevelName(current->getBaseName(), isCascading);
  }

  // Compare this signature against the signature of other
  // declarations with the same name.
  OverloadSignature currentSig = current->getOverloadSignature();
  CanType currentSigType = current->getOverloadSignatureType();
  ModuleDecl *currentModule = current->getModuleContext();
  for (auto other : otherDefinitions) {
    // Skip invalid declarations and ourselves.
    //
    // FIXME: Breaking a cycle here with hasInterfaceType() is bogus.
    if (current == other || (other->hasInterfaceType() && other->isInvalid()))
      continue;

    // Skip declarations in other modules.
    if (currentModule != other->getModuleContext())
      continue;

    // If both declarations are in the same file, only diagnose the second one.
    if (currentFile == other->getDeclContext()->getParentSourceFile())
      if (current->getLoc().isValid() &&
          ctx.SourceMgr.isBeforeInBuffer(
            current->getLoc(), other->getLoc()))
        continue;

    // Don't compare methods vs. non-methods (which only happens with
    // operators).
    if (currentDC->isTypeContext() != other->getDeclContext()->isTypeContext())
      continue;

    // Check whether the overload signatures conflict (ignoring the type for
    // now).
    auto otherSig = other->getOverloadSignature();
    if (!conflicting(currentSig, otherSig))
      continue;

    // Skip declarations in other files.
    // In practice, this means we will warn on a private declaration that
    // shadows a non-private one, but only in the file where the shadowing
    // happens. We will warn on conflicting non-private declarations in both
    // files.
    if (!other->isAccessibleFrom(currentDC))
      continue;

    // Skip invalid declarations.
    if (other->isInvalid())
      continue;

    // Thwart attempts to override the same declaration more than once.
    const auto *currentOverride = current->getOverriddenDecl();
    const auto *otherOverride = other->getOverriddenDecl();
    if (currentOverride && currentOverride == otherOverride) {
      current->diagnose(diag::multiple_override, current->getFullName());
      other->diagnose(diag::multiple_override_prev, other->getFullName());
      current->setInvalid();
      break;
    }

    // Get the overload signature type.
    CanType otherSigType = other->getOverloadSignatureType();

    bool wouldBeSwift5Redeclaration = false;
    auto isRedeclaration = conflicting(ctx, currentSig, currentSigType,
                                       otherSig, otherSigType,
                                       &wouldBeSwift5Redeclaration);
    // If there is another conflict, complain.
    if (isRedeclaration || wouldBeSwift5Redeclaration) {
      // If the two declarations occur in the same source file, make sure
      // we get the diagnostic ordering to be sensible.
      if (auto otherFile = other->getDeclContext()->getParentSourceFile()) {
        if (currentFile == otherFile &&
            current->getLoc().isValid() &&
            other->getLoc().isValid() &&
            ctx.SourceMgr.isBeforeInBuffer(current->getLoc(),
                                           other->getLoc())) {
          std::swap(current, other);
        }
      }

      // If we're currently looking at a .sil and the conflicting declaration
      // comes from a .sib, don't error since we won't be considering the sil
      // from the .sib. So it's fine for the .sil to shadow it, since that's the
      // one we want.
      if (currentFile->Kind == SourceFileKind::SIL) {
        auto *otherFile = dyn_cast<SerializedASTFile>(
            other->getDeclContext()->getModuleScopeContext());
        if (otherFile && otherFile->isSIB())
          continue;
      }

      // If the conflicting declarations have non-overlapping availability and,
      // we allow the redeclaration to proceed if...
      //
      // - they are initializers with different failability,
      bool isAcceptableVersionBasedChange = false;
      {
        const auto *currentInit = dyn_cast<ConstructorDecl>(current);
        const auto *otherInit = dyn_cast<ConstructorDecl>(other);
        if (currentInit && otherInit &&
            (currentInit->isFailable() !=
             otherInit->isFailable())) {
          isAcceptableVersionBasedChange = true;
        }
      }
      // - one throws and the other does not,
      {
        const auto *currentAFD = dyn_cast<AbstractFunctionDecl>(current);
        const auto *otherAFD = dyn_cast<AbstractFunctionDecl>(other);
        if (currentAFD && otherAFD &&
            currentAFD->hasThrows() != otherAFD->hasThrows()) {
          isAcceptableVersionBasedChange = true;
        }
      }
      // - or they are computed properties of different types,
      {
        const auto *currentVD = dyn_cast<VarDecl>(current);
        const auto *otherVD = dyn_cast<VarDecl>(other);
        if (currentVD && otherVD &&
            !currentVD->hasStorage() &&
            !otherVD->hasStorage() &&
            !currentVD->getInterfaceType()->isEqual(
              otherVD->getInterfaceType())) {
          isAcceptableVersionBasedChange = true;
        }
      }

      if (isAcceptableVersionBasedChange) {
        class AvailabilityRange {
          Optional<llvm::VersionTuple> introduced;
          Optional<llvm::VersionTuple> obsoleted;

        public:
          static AvailabilityRange from(const ValueDecl *VD) {
            AvailabilityRange result;
            for (auto *attr : VD->getAttrs().getAttributes<AvailableAttr>()) {
              if (attr->PlatformAgnostic ==
                    PlatformAgnosticAvailabilityKind::SwiftVersionSpecific) {
                if (attr->Introduced)
                  result.introduced = attr->Introduced;
                if (attr->Obsoleted)
                  result.obsoleted = attr->Obsoleted;
              }
            }
            return result;
          }

          bool fullyPrecedes(const AvailabilityRange &other) const {
            if (!obsoleted.hasValue())
              return false;
            if (!other.introduced.hasValue())
              return false;
            return *obsoleted <= *other.introduced;
          }

          bool overlaps(const AvailabilityRange &other) const {
            return !fullyPrecedes(other) && !other.fullyPrecedes(*this);
          }
        };

        auto currentAvail = AvailabilityRange::from(current);
        auto otherAvail = AvailabilityRange::from(other);
        if (!currentAvail.overlaps(otherAvail))
          continue;
      }

      // If both are VarDecls, and both have exactly the same type, then
      // matching the Swift 4 behaviour (i.e. just emitting the future-compat
      // warning) will result in SILGen crashes due to both properties mangling
      // the same, so it's better to just follow the Swift 5 behaviour and emit
      // the actual error.
      if (wouldBeSwift5Redeclaration && isa<VarDecl>(current) &&
          isa<VarDecl>(other) &&
          current->getInterfaceType()->isEqual(other->getInterfaceType())) {
        wouldBeSwift5Redeclaration = false;
      }

      // If this isn't a redeclaration in the current version of Swift, but
      // would be in Swift 5 mode, emit a warning instead of an error.
      if (wouldBeSwift5Redeclaration) {
        current->diagnose(diag::invalid_redecl_swift5_warning,
                          current->getFullName());
        other->diagnose(diag::invalid_redecl_prev, other->getFullName());
      } else {
        const auto *otherInit = dyn_cast<ConstructorDecl>(other);
        // Provide a better description for implicit initializers.
        if (otherInit && otherInit->isImplicit()) {
          // Skip conflicts with inherited initializers, which only happen
          // when the current declaration is within an extension. The override
          // checker should have already taken care of emitting a more
          // productive diagnostic.
          if (!other->getOverriddenDecl())
            current->diagnose(diag::invalid_redecl_init,
                              current->getFullName(),
                              otherInit->isMemberwiseInitializer());
        } else {
          ctx.Diags.diagnoseWithNotes(
            current->diagnose(diag::invalid_redecl,
                              current->getFullName()), [&]() {
            other->diagnose(diag::invalid_redecl_prev, other->getFullName());
          });
        }
        current->setInvalid();
      }

      // Make sure we don't do this checking again for the same decl. We also
      // set this at the beginning of the function, but we might have swapped
      // the decls for diagnostics; so ensure we also set this for the actual
      // decl we diagnosed on.
      current->setCheckedRedeclaration(true);
      break;
    }
  }
}

static Optional<unsigned>
getParamIndex(const ParameterList *paramList, const ParamDecl *decl) {
  ArrayRef<ParamDecl *> params = paramList->getArray();
  for (unsigned i = 0; i < params.size(); ++i) {
    if (params[i] == decl) return i;
  }
  return None;
}

static void checkInheritedDefaultValueRestrictions(ParamDecl *PD) {
  assert(PD->getDefaultArgumentKind() == DefaultArgumentKind::Inherited);

  auto *DC = PD->getInnermostDeclContext();
  const SourceFile *SF = DC->getParentSourceFile();
  assert((SF && SF->Kind == SourceFileKind::Interface || PD->isImplicit()) &&
         "explicit inherited default argument outside of a module interface?");

  // The containing decl should be a designated initializer.
  auto ctor = dyn_cast<ConstructorDecl>(DC);
  if (!ctor || ctor->isConvenienceInit()) {
    PD->diagnose(diag::inherited_default_value_not_in_designated_constructor);
    return;
  }

  // The decl it overrides should also be a designated initializer.
  auto overridden = ctor->getOverriddenDecl();
  if (!overridden || overridden->isConvenienceInit()) {
    PD->diagnose(
        diag::inherited_default_value_used_in_non_overriding_constructor);
    if (overridden)
      overridden->diagnose(diag::overridden_here);
    return;
  }

  // The corresponding parameter should have a default value.
  Optional<unsigned> idx = getParamIndex(ctor->getParameters(), PD);
  assert(idx && "containing decl does not contain param?");
  ParamDecl *equivalentParam = overridden->getParameters()->get(*idx);
  if (equivalentParam->getDefaultArgumentKind() == DefaultArgumentKind::None) {
    PD->diagnose(diag::corresponding_param_not_defaulted);
    equivalentParam->diagnose(diag::inherited_default_param_here);
  }
}

/// Check the default arguments that occur within this pattern.
static void checkDefaultArguments(ParameterList *params) {
  // Force the default values in case they produce diagnostics.
  for (auto *param : *params)
    (void)param->getTypeCheckedDefaultExpr();
}

llvm::Expected<Expr *>
DefaultArgumentExprRequest::evaluate(Evaluator &evaluator,
                                     ParamDecl *param) const {
  if (param->getDefaultArgumentKind() == DefaultArgumentKind::Inherited) {
    // Inherited default arguments don't have expressions, but we need to
    // perform a couple of semantic checks to make sure they're valid.
    checkInheritedDefaultValueRestrictions(param);
    return nullptr;
  }

  auto &ctx = param->getASTContext();
  auto paramTy = param->getType();
  auto *initExpr = param->getStructuralDefaultExpr();
  assert(initExpr);

  if (paramTy->hasError())
    return new (ctx) ErrorExpr(initExpr->getSourceRange(), ErrorType::get(ctx));

  auto *dc = param->getDefaultArgumentInitContext();
  assert(dc);

  if (!TypeChecker::typeCheckParameterDefault(initExpr, dc, paramTy,
                                              param->isAutoClosure())) {
    return new (ctx) ErrorExpr(initExpr->getSourceRange(), ErrorType::get(ctx));
  }

  TypeChecker::checkInitializerErrorHandling(dc, initExpr);

  // Walk the checked initializer and contextualize any closures
  // we saw there.
  (void)TypeChecker::contextualizeInitializer(dc, initExpr);
  return initExpr;
}

llvm::Expected<Initializer *>
DefaultArgumentInitContextRequest::evaluate(Evaluator &eval,
                                            ParamDecl *param) const {
  auto &ctx = param->getASTContext();
  auto *parentDC = param->getDeclContext();
  auto *paramList = getParameterList(cast<ValueDecl>(parentDC->getAsDecl()));

  // In order to compute the initializer context for this parameter, we need to
  // know its index in the parameter list. Therefore iterate over the parameters
  // looking for it and fill in the other parameter's contexts while we're here.
  Initializer *result = nullptr;
  for (auto idx : indices(*paramList)) {
    auto *otherParam = paramList->get(idx);

    // If this param doesn't need a context, we're done.
    if (!otherParam->hasDefaultExpr() && !otherParam->getStoredProperty())
      continue;

    // If this param already has a context, continue using it.
    if (otherParam->getCachedDefaultArgumentInitContext())
      continue;

    // Create a new initializer context. If this is for the parameter that
    // kicked off the request, make a note of it for when we return. Otherwise
    // cache the result ourselves.
    auto *initDC = new (ctx) DefaultArgumentInitializer(parentDC, idx);
    if (param == otherParam) {
      result = initDC;
    } else {
      eval.cacheOutput(DefaultArgumentInitContextRequest{otherParam},
                       std::move(initDC));
    }
  }
  assert(result && "Didn't create init context?");
  return result;
}

/// Check the requirements in the where clause of the given \c atd
/// to ensure that they don't introduce additional 'Self' requirements.
static void checkProtocolSelfRequirements(ProtocolDecl *proto,
                                          AssociatedTypeDecl *atd) {
  WhereClauseOwner(atd).visitRequirements(
      TypeResolutionStage::Interface,
      [proto](const Requirement &req, RequirementRepr *reqRepr) {
        switch (req.getKind()) {
        case RequirementKind::Conformance:
        case RequirementKind::Layout:
        case RequirementKind::Superclass:
          if (reqRepr &&
              req.getFirstType()->isEqual(proto->getSelfInterfaceType())) {
            auto &diags = proto->getASTContext().Diags;
            diags.diagnose(reqRepr->getSubjectLoc().getLoc(),
                           diag::protocol_where_clause_self_requirement);
          }

          return false;

        case RequirementKind::SameType:
          return false;
        }
        llvm_unreachable("unhandled kind");
      });
}

/// For now, DynamicSelfType can only appear at the top level of a
/// function result type, possibly wrapped in an optional type.
///
/// In the future, we could generalize it to allow it in any
/// covariant position, so that for example a class method could
/// return '() -> Self'.
static void checkDynamicSelfType(ValueDecl *decl, Type type) {
  if (!type->hasDynamicSelfType())
    return;

  if (auto objectTy = type->getOptionalObjectType())
    type = objectTy;

  if (type->is<DynamicSelfType>())
    return;

  if (isa<FuncDecl>(decl))
    decl->diagnose(diag::dynamic_self_invalid_method);
  else if (isa<VarDecl>(decl))
    decl->diagnose(diag::dynamic_self_invalid_property);
  else {
    assert(isa<SubscriptDecl>(decl));
    decl->diagnose(diag::dynamic_self_invalid_subscript);
  }
}

/// Build a default initializer string for the given pattern.
///
/// This string is suitable for display in diagnostics.
static Optional<std::string> buildDefaultInitializerString(DeclContext *dc,
                                                           Pattern *pattern) {
  switch (pattern->getKind()) {
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#define PATTERN(Id, Parent)
#include "swift/AST/PatternNodes.def"
    return None;
  case PatternKind::Any:
    return None;

  case PatternKind::Named: {
    if (!pattern->hasType())
      return None;

    // Special-case the various types we might see here.
    auto type = pattern->getType();

    // For literal-convertible types, form the corresponding literal.
#define CHECK_LITERAL_PROTOCOL(Kind, String)                                   \
  if (auto proto = TypeChecker::getProtocol(                                   \
          type->getASTContext(), SourceLoc(), KnownProtocolKind::Kind)) {      \
    if (TypeChecker::conformsToProtocol(type, proto, dc,                       \
                                        ConformanceCheckFlags::InExpression))  \
      return std::string(String);                                              \
  }
    CHECK_LITERAL_PROTOCOL(ExpressibleByArrayLiteral, "[]")
    CHECK_LITERAL_PROTOCOL(ExpressibleByDictionaryLiteral, "[:]")
    CHECK_LITERAL_PROTOCOL(ExpressibleByUnicodeScalarLiteral, "\"\"")
    CHECK_LITERAL_PROTOCOL(ExpressibleByExtendedGraphemeClusterLiteral, "\"\"")
    CHECK_LITERAL_PROTOCOL(ExpressibleByFloatLiteral, "0.0")
    CHECK_LITERAL_PROTOCOL(ExpressibleByIntegerLiteral, "0")
    CHECK_LITERAL_PROTOCOL(ExpressibleByStringLiteral, "\"\"")
#undef CHECK_LITERAL_PROTOCOL

    // For optional types, use 'nil'.
    if (type->getOptionalObjectType())
      return std::string("nil");

    return None;
  }

  case PatternKind::Paren: {
    if (auto sub = buildDefaultInitializerString(
            dc, cast<ParenPattern>(pattern)->getSubPattern())) {
      return "(" + *sub + ")";
    }

    return None;
  }

  case PatternKind::Tuple: {
    std::string result = "(";
    bool first = true;
    for (auto elt : cast<TuplePattern>(pattern)->getElements()) {
      if (auto sub = buildDefaultInitializerString(dc, elt.getPattern())) {
        if (first) {
          first = false;
        } else {
          result += ", ";
        }

        result += *sub;
      } else {
        return None;
      }
    }
    result += ")";
    return result;
  }

  case PatternKind::Typed:
    return buildDefaultInitializerString(
        dc, cast<TypedPattern>(pattern)->getSubPattern());

  case PatternKind::Var:
    return buildDefaultInitializerString(
        dc, cast<VarPattern>(pattern)->getSubPattern());
  }

  llvm_unreachable("Unhandled PatternKind in switch.");
}

/// Diagnose a class that does not have any initializers.
static void diagnoseClassWithoutInitializers(ClassDecl *classDecl) {
  ASTContext &C = classDecl->getASTContext();
  C.Diags.diagnose(classDecl, diag::class_without_init,
                   classDecl->getDeclaredType());

  // HACK: We've got a special case to look out for and diagnose specifically to
  // improve the experience of seeing this, and mitigate some confusion.
  //
  // For a class A which inherits from Decodable class B, class A may have
  // additional members which prevent default initializer synthesis (and
  // inheritance of other initializers). The user may have assumed that this
  // case would synthesize Encodable/Decodable conformance for class A the same
  // way it may have for class B, or other classes.
  //
  // It is helpful to suggest here that the user may have forgotten to override
  // init(from:) (and encode(to:), if applicable) in a note, before we start
  // listing the members that prevented initializer synthesis.
  // TODO: Add a fixit along with this suggestion.
  if (auto *superclassDecl = classDecl->getSuperclassDecl()) {
    auto *decodableProto = C.getProtocol(KnownProtocolKind::Decodable);
    auto superclassType = superclassDecl->getDeclaredInterfaceType();
    auto ref = TypeChecker::conformsToProtocol(
        superclassType, decodableProto, superclassDecl,
        ConformanceCheckOptions(), SourceLoc());
    if (ref) {
      // super conforms to Decodable, so we've failed to inherit init(from:).
      // Let's suggest overriding it here.
      //
      // We're going to diagnose on the concrete init(from:) decl if it exists
      // and isn't implicit; otherwise, on the subclass itself.
      ValueDecl *diagDest = classDecl;
      auto initFrom = DeclName(C, DeclBaseName::createConstructor(), C.Id_from);
      auto result =
          TypeChecker::lookupMember(superclassDecl, superclassType, initFrom,
                                    NameLookupFlags::ProtocolMembers |
                                        NameLookupFlags::IgnoreAccessControl);

      if (!result.empty() && !result.front().getValueDecl()->isImplicit())
        diagDest = result.front().getValueDecl();

      auto diagName = diag::decodable_suggest_overriding_init_here;

      // This is also a bit of a hack, but the best place we've got at the
      // moment to suggest this.
      //
      // If the superclass also conforms to Encodable, it's quite
      // likely that the user forgot to override its encode(to:). In this case,
      // we can produce a slightly different diagnostic to suggest doing so.
      auto *encodableProto = C.getProtocol(KnownProtocolKind::Encodable);
      auto ref = TypeChecker::conformsToProtocol(
          superclassType, encodableProto, superclassDecl,
          ConformanceCheckOptions(), SourceLoc());
      if (ref) {
        // We only want to produce this version of the diagnostic if the
        // subclass doesn't directly implement encode(to:).
        // The direct lookup here won't see an encode(to:) if it is inherited
        // from the superclass.
        auto encodeTo = DeclName(C, C.Id_encode, C.Id_to);
        if (classDecl->lookupDirect(encodeTo).empty())
          diagName = diag::codable_suggest_overriding_init_here;
      }

      C.Diags.diagnose(diagDest, diagName);
    }
  }

  // Lazily construct a mapping from backing storage properties to the
  // declared properties.
  bool computedBackingToOriginalVars = false;
  llvm::SmallDenseMap<VarDecl *, VarDecl *> backingToOriginalVars;
  auto getOriginalVar = [&](VarDecl *var) -> VarDecl * {
    // If we haven't computed the mapping yet, do so now.
    if (!computedBackingToOriginalVars) {
      for (auto member : classDecl->getMembers()) {
        if (auto var = dyn_cast<VarDecl>(member)) {
          if (auto backingVar = var->getPropertyWrapperBackingProperty()) {
            backingToOriginalVars[backingVar] = var;
          }
        }
      }

      computedBackingToOriginalVars = true;
    }

    auto known = backingToOriginalVars.find(var);
    if (known == backingToOriginalVars.end())
      return nullptr;

    return known->second;
  };

  for (auto member : classDecl->getMembers()) {
    auto pbd = dyn_cast<PatternBindingDecl>(member);
    if (!pbd)
      continue;

    if (pbd->isStatic() || !pbd->hasStorage() ||
        pbd->isDefaultInitializable() || pbd->isInvalid())
      continue;
   
    for (auto idx : range(pbd->getNumPatternEntries())) {
      if (pbd->isInitialized(idx)) continue;

      auto *pattern = pbd->getPattern(idx);
      SmallVector<VarDecl *, 4> vars;
      pattern->collectVariables(vars);
      if (vars.empty()) continue;

      // Replace the variables we found with the originals for diagnostic
      // purposes.
      for (auto &var : vars) {
        if (auto originalVar = getOriginalVar(var))
          var = originalVar;
      }

      auto varLoc = vars[0]->getLoc();
      
      Optional<InFlightDiagnostic> diag;
      switch (vars.size()) {
      case 1:
        diag.emplace(C.Diags.diagnose(varLoc, diag::note_no_in_class_init_1,
                                      vars[0]->getName()));
        break;
      case 2:
        diag.emplace(C.Diags.diagnose(varLoc, diag::note_no_in_class_init_2,
                                      vars[0]->getName(), vars[1]->getName()));
        break;
      case 3:
        diag.emplace(C.Diags.diagnose(varLoc, diag::note_no_in_class_init_3plus,
                                      vars[0]->getName(), vars[1]->getName(),
                                      vars[2]->getName(), false));
        break;
      default:
        diag.emplace(C.Diags.diagnose(varLoc, diag::note_no_in_class_init_3plus,
                                      vars[0]->getName(), vars[1]->getName(),
                                      vars[2]->getName(), true));
        break;
      }

      if (auto defaultValueSuggestion =
              buildDefaultInitializerString(classDecl, pattern))
        diag->fixItInsertAfter(pattern->getEndLoc(),
                               " = " + *defaultValueSuggestion);
    }
  }
}

static void maybeDiagnoseClassWithoutInitializers(ClassDecl *classDecl) {
  if (auto *SF = classDecl->getParentSourceFile()) {
    // Allow classes without initializers in SIL and module interface files.
    switch (SF->Kind) {
    case SourceFileKind::SIL:
    case SourceFileKind::Interface:
      return;
    case SourceFileKind::Library:
    case SourceFileKind::Main:
    case SourceFileKind::REPL:
      break;
    }
  }

  // Some heuristics to skip emitting a diagnostic if the class is already
  // irreperably busted.
  if (classDecl->isInvalid() ||
      classDecl->inheritsSuperclassInitializers())
    return;

  auto *superclassDecl = classDecl->getSuperclassDecl();
  if (superclassDecl &&
      superclassDecl->hasMissingDesignatedInitializers())
    return;

  for (auto member : classDecl->lookupDirect(DeclBaseName::createConstructor())) {
    auto ctor = dyn_cast<ConstructorDecl>(member);
    if (ctor && ctor->isDesignatedInit())
      return;
  }

  diagnoseClassWithoutInitializers(classDecl);
}

namespace {
class DeclChecker : public DeclVisitor<DeclChecker> {
public:
  ASTContext &Ctx;

  explicit DeclChecker(ASTContext &ctx) : Ctx(ctx) {}

  ASTContext &getASTContext() const { return Ctx; }

  void visit(Decl *decl) {
    if (getASTContext().Stats)
      getASTContext().Stats->getFrontendCounters().NumDeclsTypechecked++;

    FrontendStatsTracer StatsTracer(getASTContext().Stats, "typecheck-decl",
                                    decl);
    PrettyStackTraceDecl StackTrace("type-checking", decl);
    
    DeclVisitor<DeclChecker>::visit(decl);

    TypeChecker::checkUnsupportedProtocolType(decl);

    if (auto VD = dyn_cast<ValueDecl>(decl)) {
      auto &Context = getASTContext();
      TypeChecker::checkForForbiddenPrefix(Context, VD->getBaseName());
      
      checkRedeclaration(Context, VD);

      // Force some requests, which can produce diagnostics.

      // Compute access level.
      (void) VD->getFormalAccess();

      // Compute overrides.
      (void) VD->getOverriddenDecls();

      // Check whether the member is @objc or dynamic.
      (void) VD->isObjC();
      (void) VD->isDynamic();

      // If this is a member of a nominal type, don't allow it to have a name of
      // "Type" or "Protocol" since we reserve the X.Type and X.Protocol
      // expressions to mean something builtin to the language.  We *do* allow
      // these if they are escaped with backticks though.
      if (VD->getDeclContext()->isTypeContext() &&
          (VD->getFullName().isSimpleName(Context.Id_Type) ||
           VD->getFullName().isSimpleName(Context.Id_Protocol)) &&
          VD->getNameLoc().isValid() &&
          Context.SourceMgr.extractText({VD->getNameLoc(), 1}) != "`") {
        auto &DE = getASTContext().Diags;
        DE.diagnose(VD->getNameLoc(), diag::reserved_member_name,
                    VD->getFullName(), VD->getBaseName().getIdentifier().str());
        DE.diagnose(VD->getNameLoc(), diag::backticks_to_escape)
            .fixItReplace(VD->getNameLoc(),
                          "`" + VD->getBaseName().userFacingName().str() + "`");
      }
    }
  }


  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  void visitGenericTypeParamDecl(GenericTypeParamDecl *D) {
    llvm_unreachable("cannot reach here");
  }
  
  void visitImportDecl(ImportDecl *ID) {
    TypeChecker::checkDeclAttributes(ID);
  }

  void visitOperatorDecl(OperatorDecl *OD) {
    TypeChecker::checkDeclAttributes(OD);
    auto &Ctx = OD->getASTContext();
    if (auto *IOD = dyn_cast<InfixOperatorDecl>(OD)) {
      (void)IOD->getPrecedenceGroup();
    } else {
      auto nominalTypes = OD->getDesignatedNominalTypes();
      const auto wantsDesignatedTypes =
          Ctx.TypeCheckerOpts.EnableOperatorDesignatedTypes;
      if (nominalTypes.empty() && wantsDesignatedTypes) {
        auto identifiers = OD->getIdentifiers();
        auto identifierLocs = OD->getIdentifierLocs();
        if (checkDesignatedTypes(OD, identifiers, identifierLocs, Ctx))
          OD->setInvalid();
      }
      return;
    }
    checkAccessControl(OD);
  }

  void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD) {
    TypeChecker::checkDeclAttributes(PGD);
    validatePrecedenceGroup(PGD);
    checkAccessControl(PGD);
  }

  void visitMissingMemberDecl(MissingMemberDecl *MMD) {
    llvm_unreachable("should always be type-checked already");
  }

  void visitBoundVariable(VarDecl *VD) {
    // WARNING: Anything you put in this function will only be run when the
    // VarDecl is fully type-checked within its own file. It will NOT be run
    // when the VarDecl is merely used from another file.

    // Compute these requests in case they emit diagnostics.
    (void) VD->getInterfaceType();
    (void) VD->isGetterMutating();
    (void) VD->isSetterMutating();
    (void) VD->getPropertyWrapperBackingProperty();
    (void) VD->getImplInfo();

    // Add the '@_hasStorage' attribute if this property is stored.
    if (VD->hasStorage() && !VD->getAttrs().hasAttribute<HasStorageAttr>())
      VD->getAttrs().add(new (getASTContext())
                             HasStorageAttr(/*isImplicit=*/true));

    // Reject cases where this is a variable that has storage but it isn't
    // allowed.
    if (VD->hasStorage()) {
      // Note: Stored properties in protocols, enums, etc are diagnosed in
      // finishStorageImplInfo().

      // We haven't implemented type-level storage in some contexts.
      if (VD->isStatic()) {
        auto PBD = VD->getParentPatternBinding();
        // Selector for unimplemented_static_var message.
        enum : unsigned {
          Misc,
          GenericTypes,
          Classes,
          ProtocolExtensions
        };
        auto unimplementedStatic = [&](unsigned diagSel) {
          auto staticLoc = PBD->getStaticLoc();
          VD->diagnose(diag::unimplemented_static_var, diagSel,
                       PBD->getStaticSpelling(), diagSel == Classes)
              .highlight(staticLoc);
        };

        auto DC = VD->getDeclContext();

        // Stored type variables in a generic context need to logically
        // occur once per instantiation, which we don't yet handle.
        if (DC->getExtendedProtocolDecl()) {
          unimplementedStatic(ProtocolExtensions);
        } else if (DC->isGenericContext()
               && !DC->getGenericSignatureOfContext()->areAllParamsConcrete()) {
          unimplementedStatic(GenericTypes);
        } else if (DC->getSelfClassDecl()) {
          auto StaticSpelling = PBD->getStaticSpelling();
          if (StaticSpelling != StaticSpellingKind::KeywordStatic)
            unimplementedStatic(Classes);
        }
      }
    }

    TypeChecker::checkDeclAttributes(VD);

    if (!checkOverrides(VD)) {
      // If a property has an override attribute but does not override
      // anything, complain.
      auto overridden = VD->getOverriddenDecl();
      if (auto *OA = VD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!overridden) {
          VD->diagnose(diag::property_does_not_override)
              .highlight(OA->getLocation());
          OA->setInvalid();
        }
      }
    }

    if (VD->getDeclContext()->getSelfClassDecl()) {
      checkDynamicSelfType(VD, VD->getValueInterfaceType());

      if (VD->getValueInterfaceType()->hasDynamicSelfType()) {
        if (VD->hasStorage())
          VD->diagnose(diag::dynamic_self_in_stored_property);
        else if (VD->isSettable(nullptr))
          VD->diagnose(diag::dynamic_self_in_mutable_property);
      }
    }
    
    checkForEmptyOptionSet(VD);

    // Under the Swift 3 inference rules, if we have @IBInspectable or
    // @GKInspectable but did not infer @objc, warn that the attribute is
    auto &DE = getASTContext().Diags;
    if (!VD->isObjC() &&
        VD->getASTContext().LangOpts.EnableSwift3ObjCInference) {
      if (auto attr = VD->getAttrs().getAttribute<IBInspectableAttr>()) {
        DE.diagnose(attr->getLocation(),
                    diag::attribute_meaningless_when_nonobjc,
                    attr->getAttrName())
            .fixItRemove(attr->getRange());
      }

      if (auto attr = VD->getAttrs().getAttribute<GKInspectableAttr>()) {
        DE.diagnose(attr->getLocation(),
                    diag::attribute_meaningless_when_nonobjc,
                    attr->getAttrName())
            .fixItRemove(attr->getRange());
      }
    }

    // Now check all the accessors.
    VD->visitEmittedAccessors([&](AccessorDecl *accessor) {
      visit(accessor);
    });
  }


  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    DeclContext *DC = PBD->getDeclContext();

    TypeChecker::checkDeclAttributes(PBD);

    bool isInSILMode = false;
    if (auto sourceFile = DC->getParentSourceFile())
      isInSILMode = sourceFile->Kind == SourceFileKind::SIL;
    bool isTypeContext = DC->isTypeContext();

    auto &Ctx = getASTContext();
    for (auto i : range(PBD->getNumPatternEntries())) {
      const auto *entry = evaluateOrDefault(
          Ctx.evaluator, PatternBindingEntryRequest{PBD, i}, nullptr);
      assert(entry && "No pattern binding entry?");

      PBD->getPattern(i)->forEachVariable([&](VarDecl *var) {
        this->visitBoundVariable(var);

        if (PBD->isInitialized(i)) {
          // Add the attribute that preserves the "has an initializer" value
          // across module generation, as required for TBDGen.
          if (var->hasStorage() &&
              !var->getAttrs().hasAttribute<HasInitialValueAttr>()) {
            var->getAttrs().add(new (Ctx)
                                    HasInitialValueAttr(/*IsImplicit=*/true));
          }
          return;
        }

        // If this is a declaration without an initializer, reject code if
        // uninitialized vars are not allowed.
        if (isInSILMode) return;

        // If the variable has no storage, it never needs an initializer.
        if (!var->hasStorage())
          return;

        if (var->isInvalid() || PBD->isInvalid())
          return;

        auto markVarAndPBDInvalid = [PBD, var] {
          PBD->setInvalid();
          var->setInvalid();
        };
        
        // Properties with an opaque return type need an initializer to
        // determine their underlying type.
        if (var->getOpaqueResultTypeDecl()) {
          var->diagnose(diag::opaque_type_var_no_init);
        }

        // Non-member observing properties need an initializer.
        if (var->getWriteImpl() == WriteImplKind::StoredWithObservers &&
            !isTypeContext) {
          var->diagnose(diag::observingprop_requires_initializer);
          markVarAndPBDInvalid();
          return;
        }

        // Static/class declarations require an initializer unless in a
        // protocol.
        if (var->isStatic() && !isa<ProtocolDecl>(DC)) {
          // ...but don't enforce this for SIL or module interface files.
          switch (DC->getParentSourceFile()->Kind) {
          case SourceFileKind::Interface:
          case SourceFileKind::SIL:
            return;
          case SourceFileKind::Main:
          case SourceFileKind::REPL:
          case SourceFileKind::Library:
            break;
          }

          var->diagnose(diag::static_requires_initializer,
                        var->getCorrectStaticSpelling());
          markVarAndPBDInvalid();
          return;
        }

        // Global variables require an initializer in normal source files.
        if (DC->isModuleScopeContext()) {
          switch (DC->getParentSourceFile()->Kind) {
          case SourceFileKind::Main:
          case SourceFileKind::REPL:
          case SourceFileKind::Interface:
          case SourceFileKind::SIL:
            return;
          case SourceFileKind::Library:
            break;
          }

          var->diagnose(diag::global_requires_initializer, var->isLet());
          markVarAndPBDInvalid();
          return;
        }
      });
    }

    TypeChecker::checkDeclAttributes(PBD);

    checkAccessControl(PBD);

    // If the initializers in the PBD aren't checked yet, do so now.
    for (auto i : range(PBD->getNumPatternEntries())) {
      if (!PBD->isInitialized(i))
        continue;

      if (!PBD->isInitializerChecked(i)) {
        TypeChecker::typeCheckPatternBinding(PBD, i);
      }

      if (!PBD->isInvalid()) {
        auto *init = PBD->getInit(i);

        // If we're performing an binding to a weak or unowned variable from a
        // constructor call, emit a warning that the instance will be immediately
        // deallocated.
        diagnoseUnownedImmediateDeallocation(Ctx, PBD->getPattern(i),
                                             PBD->getEqualLoc(i),
                                             init);

        // If we entered an initializer context, contextualize any
        // auto-closures we might have created.
        // Note that we don't contextualize the initializer for a property
        // with a wrapper, because the initializer will have been subsumed
        // by the backing storage property.
        if (!DC->isLocalContext() &&
            !(PBD->getSingleVar() &&
              PBD->getSingleVar()->hasAttachedPropertyWrapper())) {
          auto *initContext = cast_or_null<PatternBindingInitializer>(
              PBD->getInitContext(i));
          if (initContext) {
            // Check safety of error-handling in the declaration, too.
            TypeChecker::checkInitializerErrorHandling(initContext, init);
            (void)TypeChecker::contextualizeInitializer(initContext, init);
          }
        }
      }
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    // Force requests that can emit diagnostics.
    (void) SD->getInterfaceType();
    (void) SD->getGenericSignature();

    if (!SD->isInvalid()) {
      TypeChecker::checkReferencedGenericParams(SD);
      checkGenericParams(SD);
      TypeChecker::checkProtocolSelfRequirements(SD);
    }

    TypeChecker::checkDeclAttributes(SD);

    checkAccessControl(SD);

    if (!checkOverrides(SD)) {
      // If a subscript has an override attribute but does not override
      // anything, complain.
      if (auto *OA = SD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!SD->getOverriddenDecl()) {
          SD->diagnose(diag::subscript_does_not_override)
              .highlight(OA->getLocation());
          OA->setInvalid();
        }
      }
    }

    // Compute these requests in case they emit diagnostics.
    (void) SD->isGetterMutating();
    (void) SD->isSetterMutating();
    (void) SD->getImplInfo();

    TypeChecker::checkParameterAttributes(SD->getIndices());

    checkDefaultArguments(SD->getIndices());

    if (SD->getDeclContext()->getSelfClassDecl()) {
      checkDynamicSelfType(SD, SD->getValueInterfaceType());

      if (SD->getValueInterfaceType()->hasDynamicSelfType() &&
          SD->supportsMutation()) {
        SD->diagnose(diag::dynamic_self_in_mutable_subscript);
      }
    }

    // Now check all the accessors.
    SD->visitEmittedAccessors([&](AccessorDecl *accessor) {
      visit(accessor);
    });
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    // Force requests that can emit diagnostics.
    (void) TAD->getGenericSignature();
    (void) TAD->getUnderlyingType();

    TypeChecker::checkDeclAttributes(TAD);
    checkAccessControl(TAD);
  }
  
  void visitOpaqueTypeDecl(OpaqueTypeDecl *OTD) {
    // Force requests that can emit diagnostics.
    (void) OTD->getGenericSignature();

    TypeChecker::checkDeclAttributes(OTD);
    checkAccessControl(OTD);
  }
  
  void visitAssociatedTypeDecl(AssociatedTypeDecl *AT) {
    TypeChecker::checkDeclAttributes(AT);

    checkInheritanceClause(AT);
    auto *proto = AT->getProtocol();

    checkProtocolSelfRequirements(proto, AT);

    if (proto->isObjC()) {
      AT->diagnose(diag::associated_type_objc, AT->getName(), proto->getName());
    }

    checkAccessControl(AT);

    // Trigger the checking for overridden declarations.
    (void) AT->getOverriddenDecls();

    auto defaultType = AT->getDefaultDefinitionType();
    if (defaultType && !defaultType->hasError()) {
      // associatedtype X = X is invalid
      auto mentionsItself =
        defaultType.findIf([&](Type defaultType) {
          if (auto DMT = defaultType->getAs<DependentMemberType>()) {
            return DMT->getAssocType() == AT;
          }
          return false;
        });

      if (mentionsItself) {
        auto &DE = getASTContext().Diags;
        DE.diagnose(AT->getDefaultDefinitionTypeRepr()->getLoc(),
                    diag::recursive_decl_reference, AT->getDescriptiveKind(),
                    AT->getName());
        AT->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);
      }
    }
  }

  void checkUnsupportedNestedType(NominalTypeDecl *NTD) {
    auto *DC = NTD->getDeclContext();
    if (DC->getResilienceExpansion() == ResilienceExpansion::Minimal) {
      auto kind = TypeChecker::getFragileFunctionKind(DC);
      NTD->diagnose(diag::local_type_in_inlinable_function, NTD->getFullName(),
                    static_cast<unsigned>(kind.first));
    }

    // We don't support protocols outside the top level of a file.
    if (isa<ProtocolDecl>(NTD) &&
        !NTD->getParent()->isModuleScopeContext()) {
      NTD->diagnose(diag::unsupported_nested_protocol, NTD->getName());
      NTD->setInvalid();
      return;
    }

    // We don't support nested types in generics yet.
    if (NTD->isGenericContext()) {
      auto DC = NTD->getDeclContext();
      if (auto proto = DC->getSelfProtocolDecl()) {
        if (DC->getExtendedProtocolDecl()) {
          NTD->diagnose(diag::unsupported_type_nested_in_protocol_extension,
                        NTD->getName(), proto->getName());
        } else {
          NTD->diagnose(diag::unsupported_type_nested_in_protocol,
                        NTD->getName(), proto->getName());
        }
      }

      if (DC->isLocalContext() && DC->isGenericContext()) {
        // A local generic context is a generic function.
        if (auto AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
          NTD->diagnose(diag::unsupported_type_nested_in_generic_function,
                        NTD->getName(), AFD->getFullName());
        } else {
          NTD->diagnose(diag::unsupported_type_nested_in_generic_closure,
                        NTD->getName());
        }
      }
    }
  }

  void visitEnumDecl(EnumDecl *ED) {
    checkUnsupportedNestedType(ED);

    // FIXME: Remove this once we clean up the mess involving raw values.
    (void) ED->getInterfaceType();

    checkGenericParams(ED);

    // Check for circular inheritance of the raw type.
    (void)ED->hasCircularRawValue();

    for (Decl *member : ED->getMembers())
      visit(member);

    TypeChecker::checkDeclAttributes(ED);

    checkInheritanceClause(ED);

    checkAccessControl(ED);

    TypeChecker::checkPatternBindingCaptures(ED);

    auto &DE = getASTContext().Diags;
    if (auto rawTy = ED->getRawType()) {
      // The raw type must be one of the blessed literal convertible types.
      if (!computeAutomaticEnumValueKind(ED)) {
        DE.diagnose(ED->getInherited().front().getSourceRange().Start,
                    diag::raw_type_not_literal_convertible, rawTy);
        ED->getInherited().front().setInvalidType(getASTContext());
      }
      
      // We need at least one case to have a raw value.
      if (ED->getAllElements().empty()) {
        DE.diagnose(ED->getInherited().front().getSourceRange().Start,
                    diag::empty_enum_raw_type);
      }
    }

    checkExplicitAvailability(ED);

    TypeChecker::checkDeclCircularity(ED);

    TypeChecker::checkConformancesInContext(ED, ED);
  }

  void visitStructDecl(StructDecl *SD) {
    checkUnsupportedNestedType(SD);

    checkGenericParams(SD);

    // Force lowering of stored properties.
    (void) SD->getStoredProperties();

    TypeChecker::addImplicitConstructors(SD);

    for (Decl *Member : SD->getMembers())
      visit(Member);

    TypeChecker::checkPatternBindingCaptures(SD);

    TypeChecker::checkDeclAttributes(SD);

    checkInheritanceClause(SD);

    checkAccessControl(SD);

    checkExplicitAvailability(SD);

    TypeChecker::checkDeclCircularity(SD);

    TypeChecker::checkConformancesInContext(SD, SD);
  }

  /// Check whether the given properties can be @NSManaged in this class.
  static bool propertiesCanBeNSManaged(ClassDecl *classDecl,
                                       ArrayRef<VarDecl *> vars) {
    // Check whether we have an Objective-C-defined class in our
    // inheritance chain.
    if (!classDecl->checkAncestry(AncestryFlags::ClangImported))
      return false;

    // If all of the variables are @objc, we can use @NSManaged.
    for (auto var : vars) {
      if (!var->isObjC())
        return false;
    }

    // Okay, we can use @NSManaged.
    return true;
  }

  /// Check that all stored properties have in-class initializers.
  void checkRequiredInClassInits(ClassDecl *cd) {
    ClassDecl *source = nullptr;
    for (auto member : cd->getMembers()) {
      auto pbd = dyn_cast<PatternBindingDecl>(member);
      if (!pbd)
        continue;

      if (pbd->isStatic() || !pbd->hasStorage() || 
          pbd->isDefaultInitializable() || pbd->isInvalid())
        continue;

      // The variables in this pattern have not been
      // initialized. Diagnose the lack of initial value.
      pbd->setInvalid();
      SmallVector<VarDecl *, 4> vars;
      for (auto idx : range(pbd->getNumPatternEntries()))
        pbd->getPattern(idx)->collectVariables(vars);
      bool suggestNSManaged = propertiesCanBeNSManaged(cd, vars);
      switch (vars.size()) {
      case 0:
        llvm_unreachable("should have been marked invalid");

      case 1:
        pbd->diagnose(diag::missing_in_class_init_1, vars[0]->getName(),
                      suggestNSManaged);
        break;

      case 2:
        pbd->diagnose(diag::missing_in_class_init_2, vars[0]->getName(),
                      vars[1]->getName(), suggestNSManaged);
        break;

      case 3:
        pbd->diagnose(diag::missing_in_class_init_3plus, vars[0]->getName(),
                      vars[1]->getName(), vars[2]->getName(), false,
                      suggestNSManaged);
        break;

      default:
        pbd->diagnose(diag::missing_in_class_init_3plus, vars[0]->getName(),
                      vars[1]->getName(), vars[2]->getName(), true,
                      suggestNSManaged);
        break;
      }

      // Figure out where this requirement came from.
      if (!source) {
        source = cd;
        while (true) {
          // If this class had the 'requires_stored_property_inits'
          // attribute, diagnose here.
          if (source->getAttrs().
                hasAttribute<RequiresStoredPropertyInitsAttr>())
            break;

          // If the superclass doesn't require in-class initial
          // values, the requirement was introduced at this point, so
          // stop here.
          auto superclass = source->getSuperclassDecl();
          if (!superclass->requiresStoredPropertyInits())
            break;

          // Keep looking.
          source = superclass;
        }
      }

      // Add a note describing why we need an initializer.
      source->diagnose(diag::requires_stored_property_inits_here,
                       source->getDeclaredType(), cd == source,
                       suggestNSManaged);
    }
  }


  void visitClassDecl(ClassDecl *CD) {
    checkUnsupportedNestedType(CD);

    // Force creation of the generic signature.
    (void) CD->getGenericSignature();

    checkGenericParams(CD);

    // Check for circular inheritance.
    (void)CD->hasCircularInheritance();

    // Force lowering of stored properties.
    (void) CD->getStoredProperties();

    // Force creation of an implicit destructor, if any.
    (void) CD->getDestructor();

    for (Decl *Member : CD->getEmittedMembers())
      visit(Member);

    TypeChecker::checkPatternBindingCaptures(CD);

    // If this class requires all of its stored properties to have
    // in-class initializers, diagnose this now.
    if (CD->requiresStoredPropertyInits())
      checkRequiredInClassInits(CD);

    // Compute @objc for each superclass member, to catch selector
    // conflicts resulting from unintended overrides.
    //
    // FIXME: This should be a request so we can measure how much work
    // we're doing here.
    CD->walkSuperclasses(
      [&](ClassDecl *superclass) {
        if (!superclass->getParentSourceFile())
          return TypeWalker::Action::Stop;

        for (auto *member : superclass->getMembers()) {
          if (auto *vd = dyn_cast<ValueDecl>(member)) {
            if (vd->isPotentiallyOverridable()) {
              (void) vd->isObjC();
            }
          }
        }

        return TypeWalker::Action::Continue;
      });

    if (auto superclassTy = CD->getSuperclass()) {
      ClassDecl *Super = superclassTy->getClassOrBoundGenericClass();

      if (auto *SF = CD->getParentSourceFile()) {
        if (auto *tracker = SF->getReferencedNameTracker()) {
          bool isPrivate =
              CD->getFormalAccess() <= AccessLevel::FilePrivate;
          tracker->addUsedMember({Super, Identifier()}, !isPrivate);
        }
      }

      bool isInvalidSuperclass = false;

      if (Super->isFinal()) {
        CD->diagnose(diag::inheritance_from_final_class, Super->getName());
        // FIXME: should this really be skipping the rest of decl-checking?
        return;
      }

      if (Super->hasClangNode() && Super->getGenericParams()
          && superclassTy->hasTypeParameter()) {
        CD->diagnose(diag::inheritance_from_unspecialized_objc_generic_class,
                     Super->getName());
      }

      switch (Super->getForeignClassKind()) {
      case ClassDecl::ForeignKind::Normal:
        break;
      case ClassDecl::ForeignKind::CFType:
        CD->diagnose(diag::inheritance_from_cf_class, Super->getName());
        isInvalidSuperclass = true;
        break;
      case ClassDecl::ForeignKind::RuntimeOnly:
        CD->diagnose(diag::inheritance_from_objc_runtime_visible_class,
                     Super->getName());
        isInvalidSuperclass = true;
        break;
      }

      if (!isInvalidSuperclass && Super->hasMissingVTableEntries() &&
          !Super->isResilient(CD->getParentModule(),
                              ResilienceExpansion::Minimal)) {
        auto *superFile = Super->getModuleScopeContext();
        if (auto *serialized = dyn_cast<SerializedASTFile>(superFile)) {
          const auto effVersion =
              CD->getASTContext().LangOpts.EffectiveLanguageVersion;
          if (serialized->getLanguageVersionBuiltWith() != effVersion) {
            CD->diagnose(
                diag::
                    inheritance_from_class_with_missing_vtable_entries_versioned,
                Super->getName(), serialized->getLanguageVersionBuiltWith(),
                effVersion);
            isInvalidSuperclass = true;
          }
        }
        if (!isInvalidSuperclass) {
          CD->diagnose(diag::inheritance_from_class_with_missing_vtable_entries,
                       Super->getName());
          isInvalidSuperclass = true;
        }
      }

      if (!getASTContext().isAccessControlDisabled()) {
        // Require the superclass to be open if this is outside its
        // defining module.  But don't emit another diagnostic if we
        // already complained about the class being inherently
        // un-subclassable.
        if (!isInvalidSuperclass &&
            !Super->hasOpenAccess(CD->getDeclContext()) &&
            Super->getModuleContext() != CD->getModuleContext()) {
          CD->diagnose(diag::superclass_not_open, superclassTy);
          isInvalidSuperclass = true;
        }

        // Require superclasses to be open if the subclass is open.
        // This is a restriction we can consider lifting in the future,
        // e.g. to enable a "sealed" superclass whose subclasses are all
        // of one of several alternatives.
        if (!isInvalidSuperclass &&
            CD->getFormalAccess() == AccessLevel::Open &&
            Super->getFormalAccess() != AccessLevel::Open) {
          CD->diagnose(diag::superclass_of_open_not_open, superclassTy);
          Super->diagnose(diag::superclass_here);
        }
      }
    }

    TypeChecker::checkDeclAttributes(CD);

    checkInheritanceClause(CD);

    checkAccessControl(CD);

    checkExplicitAvailability(CD);

    TypeChecker::checkDeclCircularity(CD);

    TypeChecker::checkConformancesInContext(CD, CD);

    maybeDiagnoseClassWithoutInitializers(CD);
  }

  void visitProtocolDecl(ProtocolDecl *PD) {
    checkUnsupportedNestedType(PD);

    // Check for circular inheritance within the protocol.
    (void)PD->hasCircularInheritedProtocols();

    auto *SF = PD->getParentSourceFile();
    if (SF) {
      if (auto *tracker = SF->getReferencedNameTracker()) {
        bool isNonPrivate = (PD->getFormalAccess() > AccessLevel::FilePrivate);
        for (auto *parentProto : PD->getInheritedProtocols())
          tracker->addUsedMember({parentProto, Identifier()}, isNonPrivate);
      }
    }

    // Check the members.
    for (auto Member : PD->getMembers())
      visit(Member);

    TypeChecker::checkDeclAttributes(PD);

    checkAccessControl(PD);

    checkInheritanceClause(PD);

    TypeChecker::checkDeclCircularity(PD);
    if (PD->isResilient())
      if (!SF || SF->Kind != SourceFileKind::Interface)
        TypeChecker::inferDefaultWitnesses(PD);

    if (PD->getASTContext().TypeCheckerOpts.DebugGenericSignatures) {
      auto requirementsSig =
        GenericSignature::get({PD->getProtocolSelfType()},
                              PD->getRequirementSignature());

      llvm::errs() << "Protocol requirement signature:\n";
      PD->dumpRef(llvm::errs());
      llvm::errs() << "\n";
      llvm::errs() << "Requirement signature: ";
      requirementsSig->print(llvm::errs());
      llvm::errs() << "\n";

      // Note: One cannot canonicalize a requirement signature, because
      // requirement signatures are necessarily missing requirements.
      llvm::errs() << "Canonical requirement signature: ";
      auto canRequirementSig =
        CanGenericSignature::getCanonical(requirementsSig->getGenericParams(),
                                          requirementsSig->getRequirements(),
                                          /*skipValidation=*/true);
      canRequirementSig->print(llvm::errs());
      llvm::errs() << "\n";
    }

    // Explicitly calculate this bit.
    (void) PD->existentialTypeSupported();

    // Explicity compute the requirement signature to detect errors.
    (void) PD->getRequirementSignature();

    checkExplicitAvailability(PD);
  }

  void visitVarDecl(VarDecl *VD) {
    // Delay type-checking on VarDecls until we see the corresponding
    // PatternBindingDecl.
  }

  /// Determine whether the given declaration requires a definition.
  ///
  /// Only valid for declarations that can have definitions, i.e.,
  /// functions, initializers, etc.
  static bool requiresDefinition(Decl *decl) {
    // Invalid, implicit, and Clang-imported declarations never
    // require a definition.
    if (decl->isInvalid() || decl->isImplicit() || decl->hasClangNode())
      return false;

    // Protocol requirements do not require definitions.
    if (isa<ProtocolDecl>(decl->getDeclContext()))
      return false;

    // Functions can have _silgen_name, semantics, and NSManaged attributes.
    if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
      if (func->getAttrs().hasAttribute<SILGenNameAttr>() ||
          func->getAttrs().hasAttribute<SemanticsAttr>() ||
          func->getAttrs().hasAttribute<NSManagedAttr>())
        return false;
    }

    // Declarations in SIL and module interface files don't require
    // definitions.
    if (auto sourceFile = decl->getDeclContext()->getParentSourceFile()) {
      switch (sourceFile->Kind) {
      case SourceFileKind::SIL:
      case SourceFileKind::Interface:
        return false;
      case SourceFileKind::Library:
      case SourceFileKind::Main:
      case SourceFileKind::REPL:
        break;
      }
    }

    // Everything else requires a definition.
    return true;
  }


  bool shouldSkipBodyTypechecking(const AbstractFunctionDecl *AFD) {
    // Make sure we're in the mode that's skipping function bodies.
    if (!getASTContext().TypeCheckerOpts.SkipNonInlinableFunctionBodies)
      return false;

    // Make sure there even _is_ a body that we can skip.
    if (!AFD->getBodySourceRange().isValid())
      return false;

    // If we're gonna serialize the body, we can't skip it.
    if (AFD->getResilienceExpansion() == ResilienceExpansion::Minimal)
      return false;

    return true;
  }

  void visitFuncDecl(FuncDecl *FD) {
    // Force these requests in case they emit diagnostics.
    (void) FD->getInterfaceType();
    (void) FD->getOperatorDecl();

    if (!FD->isInvalid()) {
      checkGenericParams(FD);
      TypeChecker::checkReferencedGenericParams(FD);
      TypeChecker::checkProtocolSelfRequirements(FD);
    }

    checkAccessControl(FD);

    TypeChecker::checkParameterAttributes(FD->getParameters());
    TypeChecker::checkDeclAttributes(FD);

    if (!checkOverrides(FD)) {
      // If a method has an 'override' keyword but does not
      // override anything, complain.
      if (auto *OA = FD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!FD->getOverriddenDecl()) {
          FD->diagnose(diag::method_does_not_override)
              .highlight(OA->getLocation());
          OA->setInvalid();
        }
      }
    }

    if (requiresDefinition(FD) && !FD->hasBody()) {
      // Complain if we should have a body.
      FD->diagnose(diag::func_decl_without_brace);
    } else if (FD->getDeclContext()->isLocalContext()) {
      // Check local function bodies right away.
      TypeChecker::typeCheckAbstractFunctionBody(FD);
    } else if (shouldSkipBodyTypechecking(FD)) {
      FD->setBodySkipped(FD->getBodySourceRange());
    } else {
      // FIXME: Remove TypeChecker dependency.
      auto &TC = *Ctx.getLegacyGlobalTypeChecker();
      TC.definedFunctions.push_back(FD);
    }

    checkExplicitAvailability(FD);

    if (FD->getDeclContext()->getSelfClassDecl())
      checkDynamicSelfType(FD, FD->getResultInterfaceType());

    checkDefaultArguments(FD->getParameters());

    // Validate 'static'/'class' on functions in extensions.
    auto StaticSpelling = FD->getStaticSpelling();
    if (StaticSpelling != StaticSpellingKind::None &&
        isa<ExtensionDecl>(FD->getDeclContext())) {
      if (auto *NTD = FD->getDeclContext()->getSelfNominalTypeDecl()) {
        if (!isa<ClassDecl>(NTD)) {
          if (StaticSpelling == StaticSpellingKind::KeywordClass) {
            FD->diagnose(diag::class_func_not_in_class, false)
                .fixItReplace(FD->getStaticLoc(), "static");
            NTD->diagnose(diag::extended_type_declared_here);
          }
        }
      }
    }

    // Member functions need some special validation logic.
    if (FD->getDeclContext()->isTypeContext()) {
      if (FD->isOperator() && !isMemberOperator(FD, nullptr)) {
        auto selfNominal = FD->getDeclContext()->getSelfNominalTypeDecl();
        auto isProtocol = selfNominal && isa<ProtocolDecl>(selfNominal);
        // We did not find 'Self'. Complain.
        FD->diagnose(diag::operator_in_unrelated_type,
                     FD->getDeclContext()->getDeclaredInterfaceType(), isProtocol,
                     FD->getFullName());
      }
    }

    // If the function is exported to C, it must be representable in (Obj-)C.
    // FIXME: This needs to be moved to its own request if we want to
    // productize @_cdecl.
    if (auto CDeclAttr = FD->getAttrs().getAttribute<swift::CDeclAttr>()) {
      Optional<ForeignErrorConvention> errorConvention;
      if (isRepresentableInObjC(FD, ObjCReason::ExplicitlyCDecl,
                                errorConvention)) {
        if (FD->hasThrows()) {
          FD->setForeignErrorConvention(*errorConvention);
          getASTContext().Diags.diagnose(CDeclAttr->getLocation(),
                                         diag::cdecl_throws);
        }
      }
    }
  }

  void visitModuleDecl(ModuleDecl *) { }

  void visitEnumCaseDecl(EnumCaseDecl *ECD) {
    // The type-checker doesn't care about how these are grouped.
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    (void) EED->getInterfaceType();
    auto *ED = EED->getParentEnum();

    TypeChecker::checkDeclAttributes(EED);

    if (auto *PL = EED->getParameterList()) {
      TypeChecker::checkParameterAttributes(PL);

      checkDefaultArguments(PL);
    }

    auto &DE = getASTContext().Diags;
    // We don't yet support raw values on payload cases.
    if (EED->hasAssociatedValues()) {
      if (auto rawTy = ED->getRawType()) {
        EED->diagnose(diag::enum_with_raw_type_case_with_argument);
        DE.diagnose(ED->getInherited().front().getSourceRange().Start,
                    diag::enum_raw_type_here, rawTy);
        EED->setInvalid();
      }
    }

    // Force the raw value expr then yell if our parent doesn't have a raw type.
    Expr *RVE = EED->getRawValueExpr();
    if (RVE && !ED->hasRawType()) {
      DE.diagnose(RVE->getLoc(), diag::enum_raw_value_without_raw_type);
      EED->setInvalid();
    }

    checkAccessControl(EED);
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    // Produce any diagnostics for the extended type.
    auto extType = ED->getExtendedType();

    auto nominal = ED->computeExtendedNominal();
    if (nominal == nullptr) {
      const bool wasAlreadyInvalid = ED->isInvalid();
      ED->setInvalid();
      if (extType && !extType->hasError() && extType->getAnyNominal()) {
        // If we've got here, then we have some kind of extension of a prima
        // fascie non-nominal type.  This can come up when we're projecting
        // typealiases out of bound generic types.
        //
        // struct Array<T> { typealias Indices = Range<Int> }
        // extension Array.Indices.Bound {}
        //
        // Offer to rewrite it to the underlying nominal type.
        auto canExtType = extType->getCanonicalType();
        ED->diagnose(diag::invalid_nominal_extension, extType, canExtType)
          .highlight(ED->getExtendedTypeRepr()->getSourceRange());
        ED->diagnose(diag::invalid_nominal_extension_rewrite, canExtType)
          .fixItReplace(ED->getExtendedTypeRepr()->getSourceRange(),
                        canExtType->getString());
      } else if (!wasAlreadyInvalid) {
        // If nothing else applies, fall back to a generic diagnostic.
        ED->diagnose(diag::non_nominal_extension, extType);
      }
      return;
    }

    // Produce any diagnostics for the generic signature.
    (void) ED->getGenericSignature();

    if (extType && !extType->hasError()) {
      // The first condition catches syntactic forms like
      //     protocol A & B { ... } // may be protocols or typealiases
      // The second condition also looks through typealiases and catches
      //    typealias T = P1 & P2 // P2 is a refined protocol of P1
      //    extension T { ... }
      // However, it is trickier to catch cases like
      //    typealias T = P2 & P1 // P2 is a refined protocol of P1
      //    extension T { ... }
      // so we don't do that here.
      auto extTypeRepr = ED->getExtendedTypeRepr();
      auto *extTypeNominal = extType->getAnyNominal();
      bool firstNominalIsNotMostSpecific =
        extTypeNominal && extTypeNominal != nominal;
      if (isa<CompositionTypeRepr>(extTypeRepr)
          || firstNominalIsNotMostSpecific) {
        auto firstNominalType = nominal->getDeclaredType();
        auto diag = ED->diagnose(diag::composition_in_extended_type,
                                 firstNominalType);
        diag.highlight(extTypeRepr->getSourceRange());
        if (firstNominalIsNotMostSpecific) {
          diag.flush();
          Type mostSpecificProtocol = extTypeNominal->getDeclaredType();
          ED->diagnose(diag::composition_in_extended_type_alternative,
                       mostSpecificProtocol)
            .fixItReplace(extTypeRepr->getSourceRange(),
                          mostSpecificProtocol->getString());
        } else {
          diag.fixItReplace(extTypeRepr->getSourceRange(),
                            firstNominalType->getString());
        }
      }
    }

    checkInheritanceClause(ED);

    // Only generic and protocol types are permitted to have
    // trailing where clauses.
    if (auto trailingWhereClause = ED->getTrailingWhereClause()) {
      if (!ED->getGenericParams() && !ED->isInvalid()) {
        ED->diagnose(diag::extension_nongeneric_trailing_where,
                     nominal->getFullName())
          .highlight(trailingWhereClause->getSourceRange());
      }
    }

    checkGenericParams(ED);

    for (Decl *Member : ED->getMembers())
      visit(Member);

    TypeChecker::checkPatternBindingCaptures(ED);

    TypeChecker::checkConformancesInContext(ED, ED);

    TypeChecker::checkDeclAttributes(ED);
    checkAccessControl(ED);

    checkExplicitAvailability(ED);
  }

  void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
    // See swift::performTypeChecking for TopLevelCodeDecl handling.
    llvm_unreachable("TopLevelCodeDecls are handled elsewhere");
  }
  
  void visitIfConfigDecl(IfConfigDecl *ICD) {
    // The active members of the #if block will be type checked along with
    // their enclosing declaration.
    TypeChecker::checkDeclAttributes(ICD);
  }

  void visitPoundDiagnosticDecl(PoundDiagnosticDecl *PDD) {
    if (PDD->hasBeenEmitted()) { return; }
    PDD->markEmitted();
    getASTContext()
        .Diags
        .diagnose(PDD->getMessage()->getStartLoc(),
                  PDD->isError() ? diag::pound_error : diag::pound_warning,
                  PDD->getMessage()->getValue())
        .highlight(PDD->getMessage()->getSourceRange());
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    (void) CD->getInterfaceType();

    // Compute these requests in case they emit diagnostics.
    (void) CD->getInitKind();

    if (!CD->isInvalid()) {
      checkGenericParams(CD);
      TypeChecker::checkReferencedGenericParams(CD);
      TypeChecker::checkProtocolSelfRequirements(CD);
    }

    TypeChecker::checkDeclAttributes(CD);
    TypeChecker::checkParameterAttributes(CD->getParameters());

    // Check whether this initializer overrides an initializer in its
    // superclass.
    if (!checkOverrides(CD)) {
      // If an initializer has an override attribute but does not override
      // anything or overrides something that doesn't need an 'override'
      // keyword (e.g., a convenience initializer), complain.
      // anything, or overrides something that complain.
      if (auto *attr = CD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!CD->getOverriddenDecl()) {
          CD->diagnose(diag::initializer_does_not_override)
              .highlight(attr->getLocation());
          attr->setInvalid();
        } else if (attr->isImplicit()) {
          // Don't diagnose implicit attributes.
        } else if (overrideRequiresKeyword(CD->getOverriddenDecl())
                     == OverrideRequiresKeyword::Never) {
          // Special case: we are overriding a 'required' initializer, so we
          // need (only) the 'required' keyword.
          if (cast<ConstructorDecl>(CD->getOverriddenDecl())->isRequired()) {
            if (CD->getAttrs().hasAttribute<RequiredAttr>()) {
              CD->diagnose(diag::required_initializer_override_keyword)
                  .fixItRemove(attr->getLocation());
            } else {
              CD->diagnose(diag::required_initializer_override_wrong_keyword)
                  .fixItReplace(attr->getLocation(), "required");
              CD->getAttrs().add(new (getASTContext())
                                     RequiredAttr(/*IsImplicit=*/true));
            }

            auto *reqInit =
                findNonImplicitRequiredInit(CD->getOverriddenDecl());
            reqInit->diagnose(diag::overridden_required_initializer_here);
          } else {
            // We tried to override a convenience initializer.
            CD->diagnose(diag::initializer_does_not_override)
                .highlight(attr->getLocation());
            CD->getOverriddenDecl()->diagnose(
                diag::convenience_init_override_here);
          }
        }
      }

      // A failable initializer cannot override a non-failable one.
      // This would normally be diagnosed by the covariance rules;
      // however, those are disabled so that we can provide a more
      // specific diagnostic here.
      if (CD->isFailable() &&
          CD->getOverriddenDecl() &&
          !CD->getOverriddenDecl()->isFailable()) {
        CD->diagnose(diag::failable_initializer_override, CD->getFullName());
        auto *OD = CD->getOverriddenDecl();
        OD->diagnose(diag::nonfailable_initializer_override_here,
                     OD->getFullName());
      }
    }

    // If this initializer overrides a 'required' initializer, it must itself
    // be marked 'required'.
    if (!CD->getAttrs().hasAttribute<RequiredAttr>()) {
      if (CD->getOverriddenDecl() && CD->getOverriddenDecl()->isRequired()) {
        CD->diagnose(diag::required_initializer_missing_keyword)
            .fixItInsert(CD->getLoc(), "required ");

        auto *reqInit = findNonImplicitRequiredInit(CD->getOverriddenDecl());
        reqInit->diagnose(diag::overridden_required_initializer_here);

        CD->getAttrs().add(new (getASTContext())
                               RequiredAttr(/*IsImplicit=*/true));
      }
    }

    if (CD->isRequired()) {
      if (auto nominal = CD->getDeclContext()->getSelfNominalTypeDecl()) {
        AccessLevel requiredAccess;
        switch (nominal->getFormalAccess()) {
        case AccessLevel::Open:
          requiredAccess = AccessLevel::Public;
          break;
        case AccessLevel::Public:
        case AccessLevel::Internal:
          requiredAccess = AccessLevel::Internal;
          break;
        case AccessLevel::FilePrivate:
        case AccessLevel::Private:
          requiredAccess = AccessLevel::FilePrivate;
          break;
        }
        if (CD->getFormalAccess() < requiredAccess) {
          auto diag = CD->diagnose(diag::required_initializer_not_accessible,
                                   nominal->getFullName());
          fixItAccess(diag, CD, requiredAccess);
        }
      }
    }

    checkAccessControl(CD);

    if (requiresDefinition(CD) && !CD->hasBody()) {
      // Complain if we should have a body.
      CD->diagnose(diag::missing_initializer_def);
    } else if (CD->getDeclContext()->isLocalContext()) {
      // Check local function bodies right away.
      TypeChecker::typeCheckAbstractFunctionBody(CD);
    } else if (shouldSkipBodyTypechecking(CD)) {
      CD->setBodySkipped(CD->getBodySourceRange());
    } else {
      // FIXME: Remove TypeChecker dependency.
      auto &TC = *Ctx.getLegacyGlobalTypeChecker();
      TC.definedFunctions.push_back(CD);
    }

    checkDefaultArguments(CD->getParameters());
  }

  void visitDestructorDecl(DestructorDecl *DD) {
    TypeChecker::checkDeclAttributes(DD);

    if (DD->getDeclContext()->isLocalContext()) {
      // Check local function bodies right away.
      TypeChecker::typeCheckAbstractFunctionBody(DD);
    } else if (shouldSkipBodyTypechecking(DD)) {
      DD->setBodySkipped(DD->getBodySourceRange());
    } else {
      // FIXME: Remove TypeChecker dependency.
      auto &TC = *Ctx.getLegacyGlobalTypeChecker();
      TC.definedFunctions.push_back(DD);
    }
  }
};
} // end anonymous namespace

void TypeChecker::typeCheckDecl(Decl *D) {
  DeclChecker(D->getASTContext()).visit(D);
}
