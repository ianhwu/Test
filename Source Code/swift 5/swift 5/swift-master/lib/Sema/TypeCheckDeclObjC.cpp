//===--- TypeCheckDeclObjC.cpp - Type Checking for ObjC Declarations ------===//
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
// This file implements semantic analysis for Objective-C-specific
// aspects of declarations.
//
//===----------------------------------------------------------------------===//
#include "TypeCheckObjC.h"
#include "TypeChecker.h"
#include "TypeCheckProtocol.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/StringExtras.h"
using namespace swift;

#pragma mark Determine whether an entity is representable in Objective-C.

bool swift::shouldDiagnoseObjCReason(ObjCReason reason, ASTContext &ctx) {
  switch(reason) {
  case ObjCReason::ExplicitlyCDecl:
  case ObjCReason::ExplicitlyDynamic:
  case ObjCReason::ExplicitlyObjC:
  case ObjCReason::ExplicitlyIBOutlet:
  case ObjCReason::ExplicitlyIBAction:
  case ObjCReason::ExplicitlyIBSegueAction:
  case ObjCReason::ExplicitlyNSManaged:
  case ObjCReason::MemberOfObjCProtocol:
  case ObjCReason::OverridesObjC:
  case ObjCReason::WitnessToObjC:
  case ObjCReason::ImplicitlyObjC:
  case ObjCReason::MemberOfObjCExtension:
    return true;

  case ObjCReason::ExplicitlyIBInspectable:
  case ObjCReason::ExplicitlyGKInspectable:
    return !ctx.LangOpts.EnableSwift3ObjCInference;

  case ObjCReason::MemberOfObjCSubclass:
  case ObjCReason::MemberOfObjCMembersClass:
  case ObjCReason::ElementOfObjCEnum:
  case ObjCReason::Accessor:
    return false;
  }
  llvm_unreachable("unhandled reason");
}

unsigned swift::getObjCDiagnosticAttrKind(ObjCReason reason) {
  switch (reason) {
  case ObjCReason::ExplicitlyCDecl:
  case ObjCReason::ExplicitlyDynamic:
  case ObjCReason::ExplicitlyObjC:
  case ObjCReason::ExplicitlyIBOutlet:
  case ObjCReason::ExplicitlyIBAction:
  case ObjCReason::ExplicitlyIBSegueAction:
  case ObjCReason::ExplicitlyNSManaged:
  case ObjCReason::MemberOfObjCProtocol:
  case ObjCReason::OverridesObjC:
  case ObjCReason::WitnessToObjC:
  case ObjCReason::ImplicitlyObjC:
  case ObjCReason::ExplicitlyIBInspectable:
  case ObjCReason::ExplicitlyGKInspectable:
  case ObjCReason::MemberOfObjCExtension:
    return static_cast<unsigned>(reason);

  case ObjCReason::MemberOfObjCSubclass:
  case ObjCReason::MemberOfObjCMembersClass:
  case ObjCReason::ElementOfObjCEnum:
  case ObjCReason::Accessor:
    llvm_unreachable("should not diagnose this @objc reason");
  }
  llvm_unreachable("unhandled reason");
}

/// Emit an additional diagnostic describing why we are applying @objc to the
/// decl, if this is not obvious from the decl itself.
static void describeObjCReason(const ValueDecl *VD, ObjCReason Reason) {
  if (Reason == ObjCReason::MemberOfObjCProtocol) {
    VD->diagnose(diag::objc_inferring_on_objc_protocol_member);
  } else if (Reason == ObjCReason::OverridesObjC) {
    unsigned kind = isa<VarDecl>(VD) ? 0
                  : isa<SubscriptDecl>(VD) ? 1
                  : isa<ConstructorDecl>(VD) ? 2
                  : 3;

    auto overridden = VD->getOverriddenDecl();
    overridden->diagnose(diag::objc_overriding_objc_decl,
                         kind, VD->getOverriddenDecl()->getFullName());
  } else if (Reason == ObjCReason::WitnessToObjC) {
    auto requirement = Reason.getObjCRequirement();
    requirement->diagnose(diag::objc_witness_objc_requirement,
                VD->getDescriptiveKind(), requirement->getFullName(),
                cast<ProtocolDecl>(requirement->getDeclContext())
                  ->getFullName());
  }
}

static void diagnoseTypeNotRepresentableInObjC(const DeclContext *DC,
                                               Type T,
                                               SourceRange TypeRange) {
  auto &diags = DC->getASTContext().Diags;

  // Special diagnostic for tuples.
  if (T->is<TupleType>()) {
    if (T->isVoid())
      diags.diagnose(TypeRange.Start, diag::not_objc_empty_tuple)
          .highlight(TypeRange);
    else
      diags.diagnose(TypeRange.Start, diag::not_objc_tuple)
          .highlight(TypeRange);
    return;
  }

  // Special diagnostic for classes.
  if (auto *CD = T->getClassOrBoundGenericClass()) {
    if (!CD->isObjC())
      diags.diagnose(TypeRange.Start, diag::not_objc_swift_class)
          .highlight(TypeRange);
    return;
  }

  // Special diagnostic for structs.
  if (T->is<StructType>()) {
    diags.diagnose(TypeRange.Start, diag::not_objc_swift_struct)
        .highlight(TypeRange);
    return;
  }

  // Special diagnostic for enums.
  if (T->is<EnumType>()) {
    diags.diagnose(TypeRange.Start, diag::not_objc_swift_enum)
        .highlight(TypeRange);
    return;
  }

  // Special diagnostic for protocols and protocol compositions.
  if (T->isExistentialType()) {
    if (T->isAny()) {
      // Any is not @objc.
      diags.diagnose(TypeRange.Start,
                     diag::not_objc_empty_protocol_composition);
      return;
    }

    auto layout = T->getExistentialLayout();

    // See if the superclass is not @objc.
    if (auto superclass = layout.explicitSuperclass) {
      if (!superclass->getClassOrBoundGenericClass()->isObjC()) {
        diags.diagnose(TypeRange.Start, diag::not_objc_class_constraint,
                       superclass);
        return;
      }
    }

    // Find a protocol that is not @objc.
    bool sawErrorProtocol = false;
    for (auto P : layout.getProtocols()) {
      auto *PD = P->getDecl();

      if (PD->isSpecificProtocol(KnownProtocolKind::Error)) {
        sawErrorProtocol = true;
        break;
      }

      if (!PD->isObjC()) {
        diags.diagnose(TypeRange.Start, diag::not_objc_protocol,
                       PD->getDeclaredType());
        return;
      }
    }

    if (sawErrorProtocol) {
      diags.diagnose(TypeRange.Start,
                     diag::not_objc_error_protocol_composition);
      return;
    }

    return;
  }

  if (T->is<ArchetypeType>() || T->isTypeParameter()) {
    diags.diagnose(TypeRange.Start, diag::not_objc_generic_type_param)
        .highlight(TypeRange);
    return;
  }

  if (auto fnTy = T->getAs<FunctionType>()) {
    if (fnTy->getExtInfo().throws() ) {
      diags.diagnose(TypeRange.Start, diag::not_objc_function_type_throwing)
        .highlight(TypeRange);
      return;
    }

    diags.diagnose(TypeRange.Start, diag::not_objc_function_type_param)
      .highlight(TypeRange);
    return;
  }
}

static void diagnoseFunctionParamNotRepresentable(
    const AbstractFunctionDecl *AFD, unsigned NumParams,
    unsigned ParamIndex, const ParamDecl *P, ObjCReason Reason) {
  if (!shouldDiagnoseObjCReason(Reason, AFD->getASTContext()))
    return;

  if (NumParams == 1) {
    AFD->diagnose(diag::objc_invalid_on_func_single_param_type,
                  getObjCDiagnosticAttrKind(Reason));
  } else {
    AFD->diagnose(diag::objc_invalid_on_func_param_type,
                  ParamIndex + 1, getObjCDiagnosticAttrKind(Reason));
  }
  SourceRange SR;
  if (auto typeRepr = P->getTypeRepr())
    SR = typeRepr->getSourceRange();
  diagnoseTypeNotRepresentableInObjC(AFD, P->getType(), SR);
  describeObjCReason(AFD, Reason);
}

static bool isParamListRepresentableInObjC(const AbstractFunctionDecl *AFD,
                                           const ParameterList *PL,
                                           ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsObjC.
  ASTContext &ctx = AFD->getASTContext();
  auto &diags = ctx.Diags;
  bool Diagnose = shouldDiagnoseObjCReason(Reason, ctx);
  bool IsObjC = true;
  unsigned NumParams = PL->size();
  for (unsigned ParamIndex = 0; ParamIndex != NumParams; ParamIndex++) {
    auto param = PL->get(ParamIndex);

    // Swift Varargs are not representable in Objective-C.
    if (param->isVariadic()) {
      if (Diagnose && shouldDiagnoseObjCReason(Reason, ctx)) {
        diags.diagnose(param->getStartLoc(), diag::objc_invalid_on_func_variadic,
                       getObjCDiagnosticAttrKind(Reason))
          .highlight(param->getSourceRange());
        describeObjCReason(AFD, Reason);
      }

      return false;
    }

    // Swift inout parameters are not representable in Objective-C.
    if (param->isInOut()) {
      if (Diagnose && shouldDiagnoseObjCReason(Reason, ctx)) {
        diags.diagnose(param->getStartLoc(), diag::objc_invalid_on_func_inout,
                       getObjCDiagnosticAttrKind(Reason))
          .highlight(param->getSourceRange());
        describeObjCReason(AFD, Reason);
      }

      return false;
    }

    if (param->getType()->hasError())
      return false;
    
    if (param->getType()->isRepresentableIn(
          ForeignLanguage::ObjectiveC,
          const_cast<AbstractFunctionDecl *>(AFD)))
      continue;

    // Permit '()' when this method overrides a method with a
    // foreign error convention that replaces NSErrorPointer with ()
    // and this is the replaced parameter.
    AbstractFunctionDecl *overridden;
    if (param->getType()->isVoid() && AFD->hasThrows() &&
        (overridden = AFD->getOverriddenDecl())) {
      auto foreignError = overridden->getForeignErrorConvention();
      if (foreignError &&
          foreignError->isErrorParameterReplacedWithVoid() &&
          foreignError->getErrorParameterIndex() == ParamIndex) {
        continue;
      }
    }

    IsObjC = false;
    if (!Diagnose) {
      // Save some work and return as soon as possible if we are not
      // producing diagnostics.
      return IsObjC;
    }
    diagnoseFunctionParamNotRepresentable(AFD, NumParams, ParamIndex,
                                          param, Reason);
  }
  return IsObjC;
}

/// Check whether the given declaration contains its own generic parameters,
/// and therefore is not representable in Objective-C.
static bool checkObjCWithGenericParams(const AbstractFunctionDecl *AFD,
                                       ObjCReason Reason) {
  bool Diagnose = shouldDiagnoseObjCReason(Reason, AFD->getASTContext());

  if (AFD->getGenericParams()) {
    // Diagnose this problem, if asked to.
    if (Diagnose) {
      AFD->diagnose(diag::objc_invalid_with_generic_params,
                    getObjCDiagnosticAttrKind(Reason));
      describeObjCReason(AFD, Reason);
    }

    return true;
  }

  return false;
}

/// CF types cannot have @objc methods, because they don't have real class
/// objects.
static bool checkObjCInForeignClassContext(const ValueDecl *VD,
                                           ObjCReason Reason) {
  bool Diagnose = shouldDiagnoseObjCReason(Reason, VD->getASTContext());

  auto type = VD->getDeclContext()->getDeclaredInterfaceType();
  if (!type)
    return false;

  auto clas = type->getClassOrBoundGenericClass();
  if (!clas)
    return false;

  switch (clas->getForeignClassKind()) {
  case ClassDecl::ForeignKind::Normal:
    return false;

  case ClassDecl::ForeignKind::CFType:
    if (Diagnose) {
      VD->diagnose(diag::objc_invalid_on_foreign_class,
                   getObjCDiagnosticAttrKind(Reason));
      describeObjCReason(VD, Reason);
    }
    break;

  case ClassDecl::ForeignKind::RuntimeOnly:
    if (Diagnose) {
      VD->diagnose(diag::objc_in_objc_runtime_visible,
                   VD->getDescriptiveKind(), getObjCDiagnosticAttrKind(Reason),
                   clas->getName());
      describeObjCReason(VD, Reason);
    }
    break;
  }

  return true;
}

static VersionRange getMinOSVersionForClassStubs(const llvm::Triple &target) {
  if (target.isMacOSX())
    return VersionRange::allGTE(llvm::VersionTuple(10, 15, 0));
  if (target.isiOS()) // also returns true on tvOS
    return VersionRange::allGTE(llvm::VersionTuple(13, 0, 0));
  if (target.isWatchOS())
    return VersionRange::allGTE(llvm::VersionTuple(6, 0, 0));
  return VersionRange::all();
}

static bool checkObjCClassStubAvailability(ASTContext &ctx, const Decl *decl) {
  auto minRange = getMinOSVersionForClassStubs(ctx.LangOpts.Target);

  auto targetRange = AvailabilityContext::forDeploymentTarget(ctx);
  if (targetRange.getOSVersion().isContainedIn(minRange))
    return true;

  auto declRange = AvailabilityInference::availableRange(decl, ctx);
  return declRange.getOSVersion().isContainedIn(minRange);
}

static const ClassDecl *getResilientAncestor(ModuleDecl *mod,
                                             const ClassDecl *classDecl) {
  auto *superclassDecl = classDecl;

  for (;;) {
    if (superclassDecl->hasResilientMetadata(mod,
                                             ResilienceExpansion::Maximal))
      return superclassDecl;

    superclassDecl = superclassDecl->getSuperclassDecl();
  }
}

/// Check whether the given declaration occurs within a constrained
/// extension, or an extension of a generic class, or an
/// extension of an Objective-C runtime visible class, and
/// therefore is not representable in Objective-C.
static bool checkObjCInExtensionContext(const ValueDecl *value,
                                        bool diagnose) {
  auto DC = value->getDeclContext();

  if (auto ED = dyn_cast<ExtensionDecl>(DC)) {
    if (ED->getTrailingWhereClause()) {
      if (diagnose) {
        value->diagnose(diag::objc_in_extension_context);
      }
      return true;
    }

    if (auto classDecl = ED->getSelfClassDecl()) {
      auto *mod = value->getModuleContext();
      auto &ctx = mod->getASTContext();

      if (!checkObjCClassStubAvailability(ctx, value)) {
        if (classDecl->checkAncestry().contains(
              AncestryFlags::ResilientOther) ||
            classDecl->hasResilientMetadata(mod,
                                            ResilienceExpansion::Maximal)) {
          if (diagnose) {
            auto &target = ctx.LangOpts.Target;
            auto platform = prettyPlatformString(targetPlatform(ctx.LangOpts));
            auto range = getMinOSVersionForClassStubs(target);
            auto *ancestor = getResilientAncestor(mod, classDecl);
            value->diagnose(diag::objc_in_resilient_extension,
                            value->getDescriptiveKind(),
                            ancestor->getName(),
                            platform,
                            range.getLowerEndpoint());
          }
          return true;
        }
      }

      if (classDecl->isGenericContext()) {
        if (!classDecl->usesObjCGenericsModel()) {
          if (diagnose) {
            value->diagnose(diag::objc_in_generic_extension,
                            classDecl->isGeneric());
          }
          return true;
        }
      }
    }
  }

  return false;
}

/// Determines whether the given type is a valid Objective-C class type that
/// can be returned as a result of a throwing function.
static bool isValidObjectiveCErrorResultType(DeclContext *dc, Type type) {
  switch (type->getForeignRepresentableIn(ForeignLanguage::ObjectiveC, dc)
            .first) {
  case ForeignRepresentableKind::Trivial:
  case ForeignRepresentableKind::None:
    // Special case: If the type is Unmanaged<T>, then return true, because
    // Unmanaged<T> can be represented in Objective-C (if T can be).
    if (auto BGT = type->getAs<BoundGenericType>()) {
      if (BGT->getDecl() == dc->getASTContext().getUnmanagedDecl()) {
        return true;
      }
    }
    return false;

  case ForeignRepresentableKind::Object:
  case ForeignRepresentableKind::Bridged:
  case ForeignRepresentableKind::BridgedError:
  case ForeignRepresentableKind::StaticBridged:
    return true;
  }

  llvm_unreachable("Unhandled ForeignRepresentableKind in switch.");
}

bool swift::isRepresentableInObjC(
       const AbstractFunctionDecl *AFD,
       ObjCReason Reason,
       Optional<ForeignErrorConvention> &errorConvention) {
  // Clear out the error convention. It will be added later if needed.
  errorConvention = None;

  // If you change this function, you must add or modify a test in PrintAsObjC.
  ASTContext &ctx = AFD->getASTContext();

  bool Diagnose = shouldDiagnoseObjCReason(Reason, ctx);

  if (checkObjCInForeignClassContext(AFD, Reason))
    return false;
  if (checkObjCWithGenericParams(AFD, Reason))
    return false;
  if (checkObjCInExtensionContext(AFD, Diagnose))
    return false;

  if (AFD->isOperator()) {
    AFD->diagnose((isa<ProtocolDecl>(AFD->getDeclContext())
                    ? diag::objc_operator_proto
                    : diag::objc_operator));
    return false;
  }

  if (auto accessor = dyn_cast<AccessorDecl>(AFD)) {
    // Accessors can only be @objc if the storage declaration is.
    // Global computed properties may however @_cdecl their accessors.
    auto storage = accessor->getStorage();
    if (!storage->isObjC() && Reason != ObjCReason::ExplicitlyCDecl &&
        Reason != ObjCReason::WitnessToObjC &&
        Reason != ObjCReason::MemberOfObjCProtocol) {
      if (Diagnose) {
        auto error = accessor->isGetter()
                  ? (isa<VarDecl>(storage)
                       ? diag::objc_getter_for_nonobjc_property
                       : diag::objc_getter_for_nonobjc_subscript)
                  : (isa<VarDecl>(storage)
                       ? diag::objc_setter_for_nonobjc_property
                       : diag::objc_setter_for_nonobjc_subscript);

        accessor->diagnose(error);
        describeObjCReason(accessor, Reason);
      }
      return false;
    }

    switch (accessor->getAccessorKind()) {
    case AccessorKind::DidSet:
    case AccessorKind::WillSet:
        // willSet/didSet implementations are never exposed to objc, they are
        // always directly dispatched from the synthesized setter.
      if (Diagnose) {
        accessor->diagnose(diag::objc_observing_accessor);
        describeObjCReason(accessor, Reason);
      }
      return false;

    case AccessorKind::Get:
    case AccessorKind::Set:
      return true;

    case AccessorKind::Address:
    case AccessorKind::MutableAddress:
      if (Diagnose) {
        accessor->diagnose(diag::objc_addressor);
        describeObjCReason(accessor, Reason);
      }
      return false;

    case AccessorKind::Read:
    case AccessorKind::Modify:
      if (Diagnose) {
        accessor->diagnose(diag::objc_coroutine_accessor);
        describeObjCReason(accessor, Reason);
      }
      return false;
    }
    llvm_unreachable("bad kind");
  }

  // As a special case, an initializer with a single, named parameter of type
  // '()' is always representable in Objective-C. This allows us to cope with
  // zero-parameter methods with selectors that are longer than "init". For
  // example, this allows:
  //
  // \code
  // class Foo {
  //   @objc init(malice: ()) { } // selector is "initWithMalice"
  // }
  // \endcode
  bool isSpecialInit = false;
  if (auto init = dyn_cast<ConstructorDecl>(AFD))
    isSpecialInit = init->isObjCZeroParameterWithLongSelector();

  if (!isSpecialInit &&
      !isParamListRepresentableInObjC(AFD,
                                      AFD->getParameters(),
                                      Reason)) {
    return false;
  }

  if (auto FD = dyn_cast<FuncDecl>(AFD)) {
    Type ResultType = FD->mapTypeIntoContext(FD->getResultInterfaceType());
    if (!ResultType->hasError() &&
        !ResultType->isVoid() &&
        !ResultType->isUninhabited() &&
        !ResultType->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                       const_cast<FuncDecl *>(FD))) {
      if (Diagnose) {
        AFD->diagnose(diag::objc_invalid_on_func_result_type,
                      getObjCDiagnosticAttrKind(Reason));
        SourceRange Range =
            FD->getBodyResultTypeLoc().getTypeRepr()->getSourceRange();
        diagnoseTypeNotRepresentableInObjC(FD, ResultType, Range);
        describeObjCReason(FD, Reason);
      }
      return false;
    }
  }

  // Throwing functions must map to a particular error convention.
  if (AFD->hasThrows()) {
    DeclContext *dc = const_cast<AbstractFunctionDecl *>(AFD);
    SourceLoc throwsLoc;
    Type resultType;

    const ConstructorDecl *ctor = nullptr;
    if (auto func = dyn_cast<FuncDecl>(AFD)) {
      resultType = func->getResultInterfaceType();
      throwsLoc = func->getThrowsLoc();
    } else {
      ctor = cast<ConstructorDecl>(AFD);
      throwsLoc = ctor->getThrowsLoc();
    }

    ForeignErrorConvention::Kind kind;
    CanType errorResultType;
    Type optOptionalType;
    if (ctor) {
      // Initializers always use the nil result convention.
      kind = ForeignErrorConvention::NilResult;

      // Only non-failing initializers can throw.
      if (ctor->isFailable()) {
        if (Diagnose) {
          AFD->diagnose(diag::objc_invalid_on_failing_init,
                        getObjCDiagnosticAttrKind(Reason))
            .highlight(throwsLoc);
          describeObjCReason(AFD, Reason);
        }

        return false;
      }
    } else if (resultType->isVoid()) {
      // Functions that return nothing (void) can be throwing; they indicate
      // failure with a 'false' result.
      kind = ForeignErrorConvention::ZeroResult;
      NominalTypeDecl *boolDecl = ctx.getObjCBoolDecl();
      // On Linux, we might still run @objc tests even though there's
      // no ObjectiveC Foundation, so use Swift.Bool instead of crapping
      // out.
      if (boolDecl == nullptr)
        boolDecl = ctx.getBoolDecl();

      if (boolDecl == nullptr) {
        AFD->diagnose(diag::broken_bool);
        return false;
      }

      errorResultType = boolDecl->getDeclaredType()->getCanonicalType();
    } else if (!resultType->getOptionalObjectType() &&
               isValidObjectiveCErrorResultType(dc, resultType)) {
      // Functions that return a (non-optional) type bridged to Objective-C
      // can be throwing; they indicate failure with a nil result.
      kind = ForeignErrorConvention::NilResult;
    } else if ((optOptionalType = resultType->getOptionalObjectType()) &&
               isValidObjectiveCErrorResultType(dc, optOptionalType)) {
      // Cannot return an optional bridged type, because 'nil' is reserved
      // to indicate failure. Call this out in a separate diagnostic.
      if (Diagnose) {
        AFD->diagnose(diag::objc_invalid_on_throwing_optional_result,
                      getObjCDiagnosticAttrKind(Reason),
                      resultType)
          .highlight(throwsLoc);
        describeObjCReason(AFD, Reason);
      }
      return false;
    } else {
      // Other result types are not permitted.
      if (Diagnose) {
        AFD->diagnose(diag::objc_invalid_on_throwing_result,
                      getObjCDiagnosticAttrKind(Reason),
                      resultType)
          .highlight(throwsLoc);
        describeObjCReason(AFD, Reason);
      }
      return false;
    }

    // The error type is always 'AutoreleasingUnsafeMutablePointer<NSError?>?'.
    auto nsErrorTy = ctx.getNSErrorType();
    Type errorParameterType;
    if (nsErrorTy) {
      errorParameterType = OptionalType::get(nsErrorTy);
      errorParameterType
        = BoundGenericType::get(
            ctx.getAutoreleasingUnsafeMutablePointerDecl(),
            nullptr,
            errorParameterType);
      errorParameterType = OptionalType::get(errorParameterType);
    }

    // Determine the parameter index at which the error will go.
    unsigned errorParameterIndex;
    bool foundErrorParameterIndex = false;

    // If there is an explicit @objc attribute with a name, look for
    // the "error" selector piece.
    if (auto objc = AFD->getAttrs().getAttribute<ObjCAttr>()) {
      if (auto objcName = objc->getName()) {
        auto selectorPieces = objcName->getSelectorPieces();
        for (unsigned i = selectorPieces.size(); i > 0; --i) {
          // If the selector piece is "error", this is the location of
          // the error parameter.
          auto piece = selectorPieces[i-1];
          if (piece == ctx.Id_error) {
            errorParameterIndex = i-1;
            foundErrorParameterIndex = true;
            break;
          }

          // If the first selector piece ends with "Error", it's here.
          if (i == 1 && camel_case::getLastWord(piece.str()) == "Error") {
            errorParameterIndex = i-1;
            foundErrorParameterIndex = true;
            break;
          }
        }
      }
    }

    // If the selector did not provide an index for the error, find
    // the last parameter that is not a trailing closure.
    if (!foundErrorParameterIndex) {
      auto *paramList = AFD->getParameters();
      errorParameterIndex = paramList->size();

      // Note: the errorParameterIndex is actually a SIL function
      // parameter index, which means tuples are exploded. Normally
      // tuple types cannot be bridged to Objective-C, except for
      // one special case -- a constructor with a single named parameter
      // 'foo' of tuple type becomes a zero-argument selector named
      // 'initFoo'.
      if (auto *CD = dyn_cast<ConstructorDecl>(AFD))
        if (CD->isObjCZeroParameterWithLongSelector())
          errorParameterIndex--;

      while (errorParameterIndex > 0) {
        // Skip over trailing closures.
        auto type = paramList->get(errorParameterIndex - 1)->getType();

        // It can't be a trailing closure unless it has a specific form.
        // Only consider the rvalue type.
        type = type->getRValueType();

        // Look through one level of optionality.
        if (auto objectType = type->getOptionalObjectType())
          type = objectType;

        // Is it a function type?
        if (!type->is<AnyFunctionType>()) break;
        --errorParameterIndex;
      }
    }

    // Form the error convention.
    CanType canErrorParameterType;
    if (errorParameterType)
      canErrorParameterType = errorParameterType->getCanonicalType();
    switch (kind) {
    case ForeignErrorConvention::ZeroResult:
      errorConvention = ForeignErrorConvention::getZeroResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType,
                          errorResultType);
      break;

    case ForeignErrorConvention::NonZeroResult:
      errorConvention = ForeignErrorConvention::getNonZeroResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType,
                          errorResultType);
      break;

    case ForeignErrorConvention::ZeroPreservedResult:
      errorConvention = ForeignErrorConvention::getZeroPreservedResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;

    case ForeignErrorConvention::NilResult:
      errorConvention = ForeignErrorConvention::getNilResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;

    case ForeignErrorConvention::NonNilError:
      errorConvention = ForeignErrorConvention::getNilResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;
    }
  }

  return true;
}

bool swift::isRepresentableInObjC(const VarDecl *VD, ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsObjC.
  
  if (VD->isInvalid())
    return false;

  Type T = VD->getDeclContext()->mapTypeIntoContext(VD->getInterfaceType());
  if (auto *RST = T->getAs<ReferenceStorageType>()) {
    // In-memory layout of @weak and @unowned does not correspond to anything
    // in Objective-C, but this does not really matter here, since Objective-C
    // uses getters and setters to operate on the property.
    // Because of this, look through @weak and @unowned.
    T = RST->getReferentType();
  }
  ASTContext &ctx = VD->getASTContext();
  bool Result = T->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                     VD->getDeclContext());
  bool Diagnose = shouldDiagnoseObjCReason(Reason, ctx);

  if (Result && checkObjCInExtensionContext(VD, Diagnose))
    return false;

  if (checkObjCInForeignClassContext(VD, Reason))
    return false;

  if (!Diagnose || Result)
    return Result;

  SourceRange TypeRange = VD->getTypeSourceRangeForDiagnostics();
  // TypeRange can be invalid; e.g. '@objc let foo = SwiftType()'
  if (TypeRange.isInvalid())
    TypeRange = VD->getNameLoc();

  VD->diagnose(diag::objc_invalid_on_var, getObjCDiagnosticAttrKind(Reason))
      .highlight(TypeRange);
  diagnoseTypeNotRepresentableInObjC(VD->getDeclContext(),
                                     VD->getInterfaceType(),
                                     TypeRange);
  describeObjCReason(VD, Reason);

  return Result;
}

bool swift::isRepresentableInObjC(const SubscriptDecl *SD, ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsObjC.
  ASTContext &ctx = SD->getASTContext();
  bool Diagnose = shouldDiagnoseObjCReason(Reason, ctx);

  if (checkObjCInForeignClassContext(SD, Reason))
    return false;

  // ObjC doesn't support class subscripts.
  if (!SD->isInstanceMember()) {
    if (Diagnose) {
      SD->diagnose(diag::objc_invalid_on_static_subscript,
                   SD->getDescriptiveKind(), Reason);
      describeObjCReason(SD, Reason);
    }
    return true;
  }

  // Figure out the type of the indices.
  auto SubscriptType = SD->getInterfaceType()->getAs<AnyFunctionType>();
  if (!SubscriptType)
    return false;

  if (SubscriptType->getParams().size() != 1)
    return false;

  auto IndexParam = SubscriptType->getParams()[0];
  if (IndexParam.isInOut())
    return false;

  Type IndexType = SubscriptType->getParams()[0].getParameterType();
  if (IndexType->hasError())
    return false;

  bool IndexResult =
    IndexType->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                 SD->getDeclContext());

  Type ElementType = SD->getElementInterfaceType();
  bool ElementResult = ElementType->isRepresentableIn(
        ForeignLanguage::ObjectiveC, SD->getDeclContext());
  bool Result = IndexResult && ElementResult;

  if (Result && checkObjCInExtensionContext(SD, Diagnose))
    return false;

  if (!Diagnose || Result)
    return Result;

  SourceRange TypeRange;
  if (!IndexResult)
    TypeRange = SD->getIndices()->getSourceRange();
  else
    TypeRange = SD->getElementTypeLoc().getSourceRange();
  SD->diagnose(diag::objc_invalid_on_subscript,
               getObjCDiagnosticAttrKind(Reason))
    .highlight(TypeRange);

  diagnoseTypeNotRepresentableInObjC(SD->getDeclContext(),
                                     !IndexResult ? IndexType
                                                  : ElementType,
                                     TypeRange);
  describeObjCReason(SD, Reason);

  return Result;
}

bool swift::canBeRepresentedInObjC(const ValueDecl *decl) {
  ASTContext &ctx = decl->getASTContext();
  if (!ctx.LangOpts.EnableObjCInterop)
    return false;

  if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    Optional<ForeignErrorConvention> errorConvention;
    return isRepresentableInObjC(func, ObjCReason::MemberOfObjCMembersClass,
                                 errorConvention);
  }

  if (auto var = dyn_cast<VarDecl>(decl))
    return isRepresentableInObjC(var, ObjCReason::MemberOfObjCMembersClass);

  if (auto subscript = dyn_cast<SubscriptDecl>(decl))
    return isRepresentableInObjC(subscript,
                                 ObjCReason::MemberOfObjCMembersClass);

  return false;
}

#pragma mark "@objc declaration handling"

/// Whether this declaration is a member of a class extension marked @objc.
static bool isMemberOfObjCClassExtension(const ValueDecl *VD) {
  auto ext = dyn_cast<ExtensionDecl>(VD->getDeclContext());
  if (!ext) return false;

  return ext->getSelfClassDecl() && ext->getAttrs().hasAttribute<ObjCAttr>();
}

/// Whether this declaration is a member of a class with the `@objcMembers`
/// attribute.
static bool isMemberOfObjCMembersClass(const ValueDecl *VD) {
  auto classDecl = VD->getDeclContext()->getSelfClassDecl();
  if (!classDecl) return false;

  return classDecl->checkAncestry(AncestryFlags::ObjCMembers);
}

// A class is @objc if it does not have generic ancestry, and it either has
// an explicit @objc attribute, or its superclass is @objc.
static Optional<ObjCReason> shouldMarkClassAsObjC(const ClassDecl *CD) {
  ASTContext &ctx = CD->getASTContext();
  auto ancestry = CD->checkAncestry();

  if (auto attr = CD->getAttrs().getAttribute<ObjCAttr>()) {
    if (ancestry.contains(AncestryFlags::Generic)) {
      if (attr->hasName() && !CD->isGenericContext()) {
        // @objc with a name on a non-generic subclass of a generic class is
        // just controlling the runtime name. Don't diagnose this case.
        const_cast<ClassDecl *>(CD)->getAttrs().add(
          new (ctx) ObjCRuntimeNameAttr(*attr));
        return None;
      }

      ctx.Diags.diagnose(attr->getLocation(), diag::objc_for_generic_class)
        .fixItRemove(attr->getRangeWithAt());
    }

    // If the class has resilient ancestry, @objc just controls the runtime
    // name unless all targets where the class is available support
    // class stubs.
    if (ancestry.contains(AncestryFlags::ResilientOther) &&
        !checkObjCClassStubAvailability(ctx, CD)) {
      if (attr->hasName()) {
        const_cast<ClassDecl *>(CD)->getAttrs().add(
          new (ctx) ObjCRuntimeNameAttr(*attr));
        return None;
      }


      auto &target = ctx.LangOpts.Target;
      auto platform = prettyPlatformString(targetPlatform(ctx.LangOpts));
      auto range = getMinOSVersionForClassStubs(target);
      auto *ancestor = getResilientAncestor(CD->getParentModule(), CD);
      ctx.Diags.diagnose(attr->getLocation(),
                         diag::objc_for_resilient_class,
                         ancestor->getName(),
                         platform,
                         range.getLowerEndpoint())
        .fixItRemove(attr->getRangeWithAt());
    }

    // Only allow ObjC-rooted classes to be @objc.
    // (Leave a hole for test cases.)
    if (ancestry.contains(AncestryFlags::ObjC) &&
        !ancestry.contains(AncestryFlags::ClangImported)) {
      if (ctx.LangOpts.EnableObjCAttrRequiresFoundation)
        ctx.Diags.diagnose(attr->getLocation(),
                           diag::invalid_objc_swift_rooted_class)
          .fixItRemove(attr->getRangeWithAt());
      if (!ctx.LangOpts.EnableObjCInterop)
        ctx.Diags.diagnose(attr->getLocation(), diag::objc_interop_disabled)
          .fixItRemove(attr->getRangeWithAt());
    }

    return ObjCReason(ObjCReason::ExplicitlyObjC);
  }

  if (ancestry.contains(AncestryFlags::ObjC)) {
    if (ancestry.contains(AncestryFlags::Generic)) {
      return None;
    }

    if (ancestry.contains(AncestryFlags::ResilientOther) &&
        !checkObjCClassStubAvailability(ctx, CD)) {
      return None;
    }

    return ObjCReason(ObjCReason::ImplicitlyObjC);
  }

  return None;
}

/// Figure out if a declaration should be exported to Objective-C.
Optional<ObjCReason> shouldMarkAsObjC(const ValueDecl *VD, bool allowImplicit) {
  // If Objective-C interoperability is disabled, nothing gets marked as @objc.
  if (!VD->getASTContext().LangOpts.EnableObjCInterop)
    return None;

  if (auto classDecl = dyn_cast<ClassDecl>(VD)) {
    return shouldMarkClassAsObjC(classDecl);
  }

  // Infer @objc for @_dynamicReplacement(for:) when replaced decl is @objc.
  if (isa<AbstractFunctionDecl>(VD) || isa<AbstractStorageDecl>(VD))
    if (auto *replacementAttr =
            VD->getAttrs().getAttribute<DynamicReplacementAttr>()) {
      if (auto *replaced = replacementAttr->getReplacedFunction()) {
        if (replaced->isObjC())
          return ObjCReason(ObjCReason::ImplicitlyObjC);
      } else if (auto *replaced =
                     TypeChecker::findReplacedDynamicFunction(VD)) {
        if (replaced->isObjC())
          return ObjCReason(ObjCReason::ImplicitlyObjC);
      }
    }

  // Destructors are always @objc, with -dealloc as their entry point.
  if (isa<DestructorDecl>(VD))
    return ObjCReason(ObjCReason::ImplicitlyObjC);

  ProtocolDecl *protocolContext =
      dyn_cast<ProtocolDecl>(VD->getDeclContext());
  bool isMemberOfObjCProtocol =
      protocolContext && protocolContext->isObjC();

  // Local function to determine whether we can implicitly infer @objc.
  auto canInferImplicitObjC = [&](bool allowAnyAccess) {
    if (VD->isInvalid())
      return false;
    if (VD->isOperator())
      return false;

    // Implicitly generated declarations are not @objc, except for constructors.
    if (!allowImplicit && VD->isImplicit())
      return false;

    if (!allowAnyAccess && VD->getFormalAccess() <= AccessLevel::FilePrivate)
      return false;

    if (auto accessor = dyn_cast<AccessorDecl>(VD)) {
      switch (accessor->getAccessorKind()) {
      case AccessorKind::DidSet:
      case AccessorKind::Modify:
      case AccessorKind::Read:
      case AccessorKind::WillSet:
        return false;

      case AccessorKind::MutableAddress:
      case AccessorKind::Address:
      case AccessorKind::Get:
      case AccessorKind::Set:
        break;
      }
    }
    return true;
  };

  // explicitly declared @objc.
  if (VD->getAttrs().hasAttribute<ObjCAttr>())
    return ObjCReason(ObjCReason::ExplicitlyObjC);
  // Getter or setter for an @objc property or subscript.
  if (auto accessor = dyn_cast<AccessorDecl>(VD)) {
    if (accessor->getAccessorKind() == AccessorKind::Get ||
        accessor->getAccessorKind() == AccessorKind::Set) {
      if (accessor->getStorage()->isObjC())
        return ObjCReason(ObjCReason::Accessor);

      return None;
    }
  }
  // @IBOutlet, @IBAction, @IBSegueAction, @NSManaged, and @GKInspectable imply
  // @objc.
  //
  // @IBInspectable and @GKInspectable imply @objc quietly in Swift 3
  // (where they warn on failure) and loudly in Swift 4 (error on failure).
  if (VD->getAttrs().hasAttribute<IBOutletAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBOutlet);
  if (VD->getAttrs().hasAttribute<IBActionAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBAction);
  if (VD->getAttrs().hasAttribute<IBSegueActionAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBSegueAction);
  if (VD->getAttrs().hasAttribute<IBInspectableAttr>())
    return ObjCReason(ObjCReason::ExplicitlyIBInspectable);
  if (VD->getAttrs().hasAttribute<GKInspectableAttr>())
    return ObjCReason(ObjCReason::ExplicitlyGKInspectable);
  if (VD->getAttrs().hasAttribute<NSManagedAttr>())
    return ObjCReason(ObjCReason::ExplicitlyNSManaged);
  // A member of an @objc protocol is implicitly @objc.
  if (isMemberOfObjCProtocol) {
    if (!VD->isProtocolRequirement())
      return None;
    return ObjCReason(ObjCReason::MemberOfObjCProtocol);
  }

  // A @nonobjc is not @objc, even if it is an override of an @objc, so check
  // for @nonobjc first.
  if (VD->getAttrs().hasAttribute<NonObjCAttr>() ||
      (isa<ExtensionDecl>(VD->getDeclContext()) &&
       cast<ExtensionDecl>(VD->getDeclContext())->getAttrs()
        .hasAttribute<NonObjCAttr>()))
    return None;

  if (isMemberOfObjCClassExtension(VD) && 
      canInferImplicitObjC(/*allowAnyAccess*/true))
    return ObjCReason(ObjCReason::MemberOfObjCExtension);
  if (isMemberOfObjCMembersClass(VD) && 
      canInferImplicitObjC(/*allowAnyAccess*/false))
    return ObjCReason(ObjCReason::MemberOfObjCMembersClass);

  // An override of an @objc declaration is implicitly @objc.
  if (VD->getOverriddenDecl() && VD->getOverriddenDecl()->isObjC())
    return ObjCReason(ObjCReason::OverridesObjC);
  // A witness to an @objc protocol requirement is implicitly @objc.
  if (VD->getDeclContext()->getSelfClassDecl()) {
    auto requirements =
      findWitnessedObjCRequirements(VD, /*anySingleRequirement=*/true);
    if (!requirements.empty())
      return ObjCReason::witnessToObjC(requirements.front());
  }

  ASTContext &ctx = VD->getASTContext();

  // Under Swift 3's @objc inference rules, 'dynamic' infers '@objc'.
  if (auto attr = VD->getAttrs().getAttribute<DynamicAttr>()) {
    bool isGetterOrSetter =
      isa<AccessorDecl>(VD) && cast<AccessorDecl>(VD)->isGetterOrSetter();

    if (ctx.LangOpts.EnableSwift3ObjCInference) {
      // If we've been asked to warn about deprecated @objc inference, do so
      // now.
      if (ctx.LangOpts.WarnSwift3ObjCInference !=
            Swift3ObjCInferenceWarnings::None &&
          !isGetterOrSetter) {
        VD->diagnose(diag::objc_inference_swift3_dynamic)
          .highlight(attr->getLocation())
          .fixItInsert(VD->getAttributeInsertionLoc(/*forModifier=*/false),
                      "@objc ");
      }

      return ObjCReason(ObjCReason::ExplicitlyDynamic);
    }
  }

  // If we aren't provided Swift 3's @objc inference rules, we're done.
  if (!ctx.LangOpts.EnableSwift3ObjCInference)
    return None;

  // Infer '@objc' for valid, non-implicit, non-operator, members of classes
  // (and extensions thereof) whose class hierarchies originate in Objective-C,
  // e.g., which derive from NSObject, so long as the members have internal
  // access or greater.
  if (!canInferImplicitObjC(/*allowAnyAccess*/false))
    return None;

  // If this declaration is part of a class with implicitly @objc members,
  // make it implicitly @objc. However, if the declaration cannot be represented
  // as @objc, don't diagnose.
  if (auto classDecl = VD->getDeclContext()->getSelfClassDecl()) {
    // One cannot define @objc members of any foreign classes.
    if (classDecl->isForeign())
      return None;

    if (classDecl->checkAncestry(AncestryFlags::ObjC))
      return ObjCReason(ObjCReason::MemberOfObjCSubclass);
  }

  return None;
}

/// Determine whether the given type is a C integer type.
static bool isCIntegerType(Type type) {
  auto nominal = type->getAnyNominal();
  if (!nominal) return false;

  ASTContext &ctx = nominal->getASTContext();
  auto stdlibModule = ctx.getStdlibModule();
  if (nominal->getParentModule() != stdlibModule)
    return false;

  // Check for each of the C integer type equivalents in the standard library.
  auto matchesStdlibTypeNamed = [&](StringRef name) {
    auto identifier = ctx.getIdentifier(name);
    SmallVector<ValueDecl *, 2> foundDecls;
    stdlibModule->lookupValue(identifier, NLKind::UnqualifiedLookup,
                              foundDecls);
    for (auto found : foundDecls) {
      auto foundType = dyn_cast<TypeDecl>(found);
      if (!foundType)
        continue;

      if (foundType->getDeclaredInterfaceType()->isEqual(type))
        return true;
    }

    return false;
  };

#define MAP_BUILTIN_TYPE(_, __)
#define MAP_BUILTIN_INTEGER_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME) \
  if (matchesStdlibTypeNamed(#SWIFT_TYPE_NAME))                       \
    return true;
#include "swift/ClangImporter/BuiltinMappedTypes.def"

  return false;
}

/// Determine whether the given enum should be @objc.
static bool isEnumObjC(EnumDecl *enumDecl) {
  // FIXME: Use shouldMarkAsObjC once it loses it's TypeChecker argument.

  // If there is no @objc attribute, it's not @objc.
  if (!enumDecl->getAttrs().hasAttribute<ObjCAttr>())
    return false;

  Type rawType = enumDecl->getRawType();

  // @objc enums must have a raw type.
  if (!rawType) {
    enumDecl->diagnose(diag::objc_enum_no_raw_type);
    return false;
  }

  // If the raw type contains an error, we've already diagnosed it.
  if (rawType->hasError())
    return false;

  // The raw type must be one of the C integer types.
  if (!isCIntegerType(rawType)) {
    SourceRange errorRange;
    if (!enumDecl->getInherited().empty())
      errorRange = enumDecl->getInherited().front().getSourceRange();
    enumDecl->diagnose(diag::objc_enum_raw_type_not_integer, rawType)
      .highlight(errorRange);
    return false;
  }

  // We need at least one case to have a raw value.
  if (enumDecl->getAllElements().empty()) {
    enumDecl->diagnose(diag::empty_enum_raw_type);
  }
  
  return true;
}

/// Record that a declaration is @objc.
static void markAsObjC(ValueDecl *D, ObjCReason reason,
                       Optional<ForeignErrorConvention> errorConvention);


llvm::Expected<bool>
IsObjCRequest::evaluate(Evaluator &evaluator, ValueDecl *VD) const {
  auto dc = VD->getDeclContext();
  Optional<ObjCReason> isObjC;
  if (dc->getSelfClassDecl() && !isa<TypeDecl>(VD)) {
    // Members of classes can be @objc.
    isObjC = shouldMarkAsObjC(VD, isa<ConstructorDecl>(VD));
  }
  else if (isa<ClassDecl>(VD)) {
    // Classes can be @objc.

    // Protocols and enums can also be @objc, but this is covered by the
    // isObjC() check at the beginning.
    isObjC = shouldMarkAsObjC(VD, /*allowImplicit=*/false);
  } else if (auto enumDecl = dyn_cast<EnumDecl>(VD)) {
    // Enums can be @objc so long as they have a raw type that is representable
    // as an arithmetic type in C.
    if (isEnumObjC(enumDecl))
      isObjC = ObjCReason(ObjCReason::ExplicitlyObjC);
  } else if (auto enumElement = dyn_cast<EnumElementDecl>(VD)) {
    // Enum elements can be @objc so long as the containing enum is @objc.
    if (enumElement->getParentEnum()->isObjC()) {
      if (enumElement->getAttrs().hasAttribute<ObjCAttr>())
        isObjC = ObjCReason::ExplicitlyObjC;
      else
        isObjC = ObjCReason::ElementOfObjCEnum;
    }
  } else if (auto proto = dyn_cast<ProtocolDecl>(VD)) {
    if (proto->getAttrs().hasAttribute<ObjCAttr>()) {
      isObjC = ObjCReason(ObjCReason::ExplicitlyObjC);

      // If the protocol is @objc, it may only refine other @objc protocols.
      // FIXME: Revisit this restriction.
      for (auto inherited : proto->getInheritedProtocols()) {
        if (!inherited->isObjC()) {
          proto->diagnose(diag::objc_protocol_inherits_non_objc_protocol,
                          proto->getDeclaredType(),
                          inherited->getDeclaredType());
          inherited->diagnose(diag::kind_declname_declared_here,
                              DescriptiveDeclKind::Protocol,
                              inherited->getName());
          isObjC = None;
        }
      }
    }
  } else if (isa<ProtocolDecl>(dc) && cast<ProtocolDecl>(dc)->isObjC()) {
    // Members of @objc protocols are @objc.
    isObjC = shouldMarkAsObjC(VD, isa<ConstructorDecl>(VD));
  } else {
    // Cannot be @objc.
  }

  // Perform some icky stateful hackery to mark this declaration as
  // not being @objc.
  auto makeNotObjC = [&] {
    if (auto objcAttr = VD->getAttrs().getAttribute<ObjCAttr>()) {
      objcAttr->setInvalid();
    }
  };

  // If this declaration should not be exposed to Objective-C, we're done.
  if (!isObjC) {
    makeNotObjC();
    return false;
  }

  if (auto accessor = dyn_cast<AccessorDecl>(VD)) {
    auto storage = accessor->getStorage();
    if (auto storageObjCAttr = storage->getAttrs().getAttribute<ObjCAttr>()) {
      // If @objc on the storage declaration was inferred using a
      // deprecated rule, but this accessor is @objc in its own right,
      // complain.
      ASTContext &ctx = dc->getASTContext();
      if (storageObjCAttr && storageObjCAttr->isSwift3Inferred() &&
          shouldDiagnoseObjCReason(*isObjC, ctx)) {
        storage->diagnose(diag::accessor_swift3_objc_inference,
                 storage->getDescriptiveKind(), storage->getFullName(),
                 isa<SubscriptDecl>(storage), accessor->isSetter())
          .fixItInsert(storage->getAttributeInsertionLoc(/*forModifier=*/false),
                       "@objc ");
      }
    }
  }

  // If needed, check whether this declaration is representable in Objective-C.
  Optional<ForeignErrorConvention> errorConvention;
  if (auto var = dyn_cast<VarDecl>(VD)) {
    if (!isRepresentableInObjC(var, *isObjC)) {
      makeNotObjC();
      return false;
    }
  } else if (auto subscript = dyn_cast<SubscriptDecl>(VD)) {
    if (!isRepresentableInObjC(subscript, *isObjC)) {
      makeNotObjC();
      return false;
    }
  } else if (isa<DestructorDecl>(VD)) {
    // Destructors need no additional checking.
  } else if (auto func = dyn_cast<AbstractFunctionDecl>(VD)) {
    if (!isRepresentableInObjC(func, *isObjC, errorConvention)) {
      makeNotObjC();
      return false;
    }
  }

  // Note that this declaration is exposed to Objective-C.
  markAsObjC(VD, *isObjC, errorConvention);

  return true;
}

/// Infer the Objective-C name for a given declaration.
static ObjCSelector inferObjCName(ValueDecl *decl) {
  if (auto destructor = dyn_cast<DestructorDecl>(decl))
    return destructor->getObjCSelector();

  auto attr = decl->getAttrs().getAttribute<ObjCAttr>();

  /// Set the @objc name.
  ASTContext &ctx = decl->getASTContext();
  auto setObjCName = [&](ObjCSelector selector) {
    // If there already is an @objc attribute, update its name.
    if (attr) {
      const_cast<ObjCAttr *>(attr)->setName(selector, /*implicit=*/true);
      return;
    }

    // Otherwise, create an @objc attribute with the implicit name.
    attr = ObjCAttr::create(ctx, selector, /*implicitName=*/true);
    decl->getAttrs().add(attr);
  };

  // If this declaration overrides an @objc declaration, use its name.
  if (auto overridden = decl->getOverriddenDecl()) {
    if (overridden->isObjC()) {
      // Handle methods first.
      if (auto overriddenFunc = dyn_cast<AbstractFunctionDecl>(overridden)) {
        // Determine the selector of the overridden method.
        ObjCSelector overriddenSelector = overriddenFunc->getObjCSelector();

        // Determine whether there is a name conflict.
        bool shouldFixName = !attr || !attr->hasName();
        if (attr && attr->hasName() && *attr->getName() != overriddenSelector) {
          // If the user explicitly wrote the incorrect name, complain.
          if (!attr->isNameImplicit()) {
            {
              auto diag = ctx.Diags.diagnose(
                            attr->AtLoc,
                            diag::objc_override_method_selector_mismatch,
                            *attr->getName(), overriddenSelector);
              fixDeclarationObjCName(diag, decl, attr->getName(),
                                     overriddenSelector);
            }

            overriddenFunc->diagnose(diag::overridden_here);
          }

          shouldFixName = true;
        }

        // If we have to set the name, do so.
        if (shouldFixName) {
          // Override the name on the attribute.
          setObjCName(overriddenSelector);
        }
        return overriddenSelector;
      }

      // Handle properties.
      if (auto overriddenProp = dyn_cast<VarDecl>(overridden)) {
        Identifier overriddenName = overriddenProp->getObjCPropertyName();
        ObjCSelector overriddenNameAsSel(ctx, 0, overriddenName);

        // Determine whether there is a name conflict.
        bool shouldFixName = !attr || !attr->hasName();
        if (attr && attr->hasName() &&
            *attr->getName() != overriddenNameAsSel) {
          // If the user explicitly wrote the wrong name, complain.
          if (!attr->isNameImplicit()) {
            ctx.Diags.diagnose(attr->AtLoc,
                        diag::objc_override_property_name_mismatch,
                        attr->getName()->getSelectorPieces()[0],
                        overriddenName)
              .fixItReplaceChars(attr->getNameLocs().front(),
                                 attr->getRParenLoc(),
                                 overriddenName.str());
            overridden->diagnose(diag::overridden_here);
          }

          shouldFixName = true;
        }

        // Fix the name, if needed.
        if (shouldFixName) {
          setObjCName(overriddenNameAsSel);
        }
        return overriddenNameAsSel;
      }
    }
  }

  // If the decl already has a name, do nothing; the protocol conformance
  // checker will handle any mismatches.
  if (attr && attr->hasName())
    return *attr->getName();

  // When no override determined the Objective-C name, look for
  // requirements for which this declaration is a witness.
  Optional<ObjCSelector> requirementObjCName;
  ValueDecl *firstReq = nullptr;
  for (auto req : findWitnessedObjCRequirements(decl)) {
    // If this is the first requirement, take its name.
    if (!requirementObjCName) {
      requirementObjCName = req->getObjCRuntimeName();
      firstReq = req;
      continue;
    }

    // If this requirement has a different name from one we've seen,
    // note the ambiguity.
    if (*requirementObjCName != *req->getObjCRuntimeName()) {
      decl->diagnose(diag::objc_ambiguous_inference,
                     decl->getDescriptiveKind(), decl->getFullName(),
                     *requirementObjCName, *req->getObjCRuntimeName());

      // Note the candidates and what Objective-C names they provide.
      auto diagnoseCandidate = [&](ValueDecl *req) {
        auto proto = cast<ProtocolDecl>(req->getDeclContext());
        auto diag = decl->diagnose(diag::objc_ambiguous_inference_candidate,
                                   req->getFullName(),
                                   proto->getFullName(),
                                   *req->getObjCRuntimeName());
        fixDeclarationObjCName(diag, decl,
                               decl->getObjCRuntimeName(/*skipIsObjC=*/true),
                               req->getObjCRuntimeName());
      };
      diagnoseCandidate(firstReq);
      diagnoseCandidate(req);

      // Suggest '@nonobjc' to suppress this error, and not try to
      // infer @objc for anything.
      decl->diagnose(diag::req_near_match_nonobjc, true)
        .fixItInsert(decl->getAttributeInsertionLoc(false), "@nonobjc ");
      break;
    }
  }

  // If we have a name, install it via an @objc attribute.
  if (requirementObjCName) {
    setObjCName(*requirementObjCName);
    return *requirementObjCName;
  }

  return *decl->getObjCRuntimeName(true);
}

/// Mark the given declaration as being Objective-C compatible (or
/// not) as appropriate.
///
/// If the declaration has a @nonobjc attribute, diagnose an error
/// using the given Reason, if present.
void markAsObjC(ValueDecl *D, ObjCReason reason,
                Optional<ForeignErrorConvention> errorConvention) {
  ASTContext &ctx = D->getASTContext();

  // By now, the caller will have handled the case where an implicit @objc
  // could be overridden by @nonobjc. If we see a @nonobjc and we are trying
  // to add an @objc for whatever reason, diagnose an error.
  if (auto *attr = D->getAttrs().getAttribute<NonObjCAttr>()) {
    if (!shouldDiagnoseObjCReason(reason, ctx))
      reason = ObjCReason::ImplicitlyObjC;

    D->diagnose(diag::nonobjc_not_allowed,
                getObjCDiagnosticAttrKind(reason));

    attr->setInvalid();
  }

  if (auto method = dyn_cast<AbstractFunctionDecl>(D)) {
    // Determine the foreign error convention.
    if (auto baseMethod = method->getOverriddenDecl()) {
      // If the overridden method has a foreign error convention,
      // adopt it.  Set the foreign error convention for a throwing
      // method.  Note that the foreign error convention affects the
      // selector, so we perform this before inferring a selector.
      if (method->hasThrows()) {
        if (auto baseErrorConvention
              = baseMethod->getForeignErrorConvention()) {
          errorConvention = baseErrorConvention;
        }

        assert(errorConvention && "Missing error convention");
        method->setForeignErrorConvention(*errorConvention);
      }
    } else if (method->hasThrows()) {
      // Attach the foreign error convention.
      assert(errorConvention && "Missing error convention");
      method->setForeignErrorConvention(*errorConvention);
    }

    // Infer the Objective-C name for this method.
    auto selector = inferObjCName(method);

    // Swift does not permit class methods with Objective-C selectors 'load',
    // 'alloc', or 'allocWithZone:'. Check for these cases.
    if (!method->isInstanceMember()) {
      auto isForbiddenSelector = [&](ObjCSelector sel)
      -> Optional<Diag<unsigned, DeclName, ObjCSelector>> {
        switch (sel.getNumArgs()) {
        case 0:
          if (sel.getSelectorPieces().front() == ctx.Id_load ||
              sel.getSelectorPieces().front() == ctx.Id_alloc)
            return diag::objc_class_method_not_permitted;
          // Swift 3 and earlier allowed you to override `initialize`, but
          // Swift's semantics do not guarantee that it will be called at
          // the point you expect. It is disallowed in Swift 4 and later.
          if (sel.getSelectorPieces().front() == ctx.Id_initialize)
            return diag::objc_class_method_not_permitted;
          return None;
        case 1:
          if (sel.getSelectorPieces().front() == ctx.Id_allocWithZone)
            return diag::objc_class_method_not_permitted;
          return None;
        default:
          return None;
        }
      };
      if (auto diagID = isForbiddenSelector(selector)) {
        auto diagInfo = getObjCMethodDiagInfo(method);
        method->diagnose(*diagID, diagInfo.first, diagInfo.second, selector);
      }
    }

    // Record the method in the class, if it's a member of one.
    if (auto classDecl = D->getDeclContext()->getSelfClassDecl()) {
      classDecl->recordObjCMethod(method, selector);
    }

    // Record the method in the source file.
    if (auto sourceFile = method->getParentSourceFile()) {
      sourceFile->ObjCMethods[selector].push_back(method);
    }
  } else if (isa<VarDecl>(D)) {
    // Infer the Objective-C name for this property.
    (void)inferObjCName(D);

    // FIXME: We should have a class-based table to check for conflicts.
  }

  // Special handling for Swift 3 @objc inference rules that are no longer
  // present in later versions of Swift.
  if (reason == ObjCReason::MemberOfObjCSubclass) {
    // If we've been asked to unconditionally warn about these deprecated
    // @objc inference rules, do so now. However, we don't warn about
    // accessors---just the main storage declarations.
    if (ctx.LangOpts.WarnSwift3ObjCInference ==
          Swift3ObjCInferenceWarnings::Complete &&
        !(isa<AccessorDecl>(D) && cast<AccessorDecl>(D)->isGetterOrSetter())) {
      D->diagnose(diag::objc_inference_swift3_objc_derived);
      D->diagnose(diag::objc_inference_swift3_addobjc)
        .fixItInsert(D->getAttributeInsertionLoc(/*forModifier=*/false),
                     "@objc ");
      D->diagnose(diag::objc_inference_swift3_addnonobjc)
        .fixItInsert(D->getAttributeInsertionLoc(/*forModifier=*/false),
                     "@nonobjc ");
    }

    // Mark the attribute as having used Swift 3 inference, or create an
    // implicit @objc for that purpose.
    auto attr = D->getAttrs().getAttribute<ObjCAttr>();
    if (!attr) {
      attr = ObjCAttr::createUnnamedImplicit(ctx);
      D->getAttrs().add(attr);
    }
    attr->setSwift3Inferred();
  }
}

void swift::diagnoseAttrsRequiringFoundation(SourceFile &SF) {
  auto &Ctx = SF.getASTContext();

  bool ImportsFoundationModule = false;

  if (Ctx.LangOpts.EnableObjCInterop) {
    if (!Ctx.LangOpts.EnableObjCAttrRequiresFoundation)
      return;
    if (SF.Kind == SourceFileKind::SIL)
      return;
  }

  for (auto import : namelookup::getAllImports(&SF)) {
    if (import.second->getName() == Ctx.Id_Foundation) {
      ImportsFoundationModule = true;
      break;
    }
  }

  if (ImportsFoundationModule)
    return;

  for (auto Attr : SF.AttrsRequiringFoundation) {
    if (!Ctx.LangOpts.EnableObjCInterop)
      Ctx.Diags.diagnose(Attr->getLocation(), diag::objc_interop_disabled)
        .fixItRemove(Attr->getRangeWithAt());
    Ctx.Diags.diagnose(Attr->getLocation(),
                       diag::attr_used_without_required_module,
                       Attr, Ctx.Id_Foundation)
      .highlight(Attr->getRangeWithAt());
  }
}

/// Compute the information used to describe an Objective-C redeclaration.
std::pair<unsigned, DeclName> swift::getObjCMethodDiagInfo(
                                AbstractFunctionDecl *member) {
  if (isa<ConstructorDecl>(member))
    return { 0 + member->isImplicit(), member->getFullName() };

  if (isa<DestructorDecl>(member))
    return { 2 + member->isImplicit(), member->getFullName() };

  if (auto accessor = dyn_cast<AccessorDecl>(member)) {
    switch (accessor->getAccessorKind()) {
#define OBJC_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
    case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
      llvm_unreachable("Not an Objective-C entry point");

    case AccessorKind::Get:
      if (auto var = dyn_cast<VarDecl>(accessor->getStorage()))
        return { 5, var->getFullName() };

      return { 6, Identifier() };

    case AccessorKind::Set:
      if (auto var = dyn_cast<VarDecl>(accessor->getStorage()))
        return { 7, var->getFullName() };
      return { 8, Identifier() };
    }

    llvm_unreachable("Unhandled AccessorKind in switch.");
  }

  // Normal method.
  auto func = cast<FuncDecl>(member);
  return { 4, func->getFullName() };
}

bool swift::fixDeclarationName(InFlightDiagnostic &diag, const ValueDecl *decl,
                               DeclName targetName) {
  if (decl->isImplicit()) return false;
  if (decl->getFullName() == targetName) return false;

  // Handle properties directly.
  if (auto var = dyn_cast<VarDecl>(decl)) {
    // Replace the name.
    SmallString<64> scratch;
    diag.fixItReplace(var->getNameLoc(), targetName.getString(scratch));
    return false;
  }

  // We only handle functions from here on.
  auto func = dyn_cast<AbstractFunctionDecl>(decl);
  if (!func) return true;

  auto name = func->getFullName();

  // Fix the name of the function itself.
  if (name.getBaseName() != targetName.getBaseName()) {
    diag.fixItReplace(func->getLoc(), targetName.getBaseName().userFacingName());
  }

  // Fix the argument names that need fixing.
  assert(name.getArgumentNames().size()
          == targetName.getArgumentNames().size());
  auto params = func->getParameters();
  for (unsigned i = 0, n = name.getArgumentNames().size(); i != n; ++i) {
    auto origArg = name.getArgumentNames()[i];
    auto targetArg = targetName.getArgumentNames()[i];

    if (origArg == targetArg)
      continue;

    auto *param = params->get(i);

    // The parameter has an explicitly-specified API name, and it's wrong.
    if (param->getArgumentNameLoc() != param->getLoc() &&
        param->getArgumentNameLoc().isValid()) {
      // ... but the internal parameter name was right. Just zap the
      // incorrect explicit specialization.
      if (param->getName() == targetArg) {
        diag.fixItRemoveChars(param->getArgumentNameLoc(),
                              param->getLoc());
        continue;
      }

      // Fix the API name.
      StringRef targetArgStr = targetArg.empty()? "_" : targetArg.str();
      diag.fixItReplace(param->getArgumentNameLoc(), targetArgStr);
      continue;
    }

    // The parameter did not specify a separate API name. Insert one.
    if (targetArg.empty())
      diag.fixItInsert(param->getLoc(), "_ ");
    else {
      llvm::SmallString<8> targetArgStr;
      targetArgStr += targetArg.str();
      targetArgStr += ' ';
      diag.fixItInsert(param->getLoc(), targetArgStr);
    }
  }

  return false;
}

bool swift::fixDeclarationObjCName(InFlightDiagnostic &diag, const ValueDecl *decl,
                                   Optional<ObjCSelector> nameOpt,
                                   Optional<ObjCSelector> targetNameOpt,
                                   bool ignoreImpliedName) {
  if (decl->isImplicit())
    return false;

  // Subscripts cannot be renamed, so handle them directly.
  if (isa<SubscriptDecl>(decl)) {
    diag.fixItInsert(decl->getAttributeInsertionLoc(/*forModifier=*/false),
                     "@objc ");
    return false;
  }

  auto name = *nameOpt;
  auto targetName = *targetNameOpt;

  // Dig out the existing '@objc' attribute on the witness. We don't care
  // about implicit ones because they don't have useful source location
  // information.
  auto attr = decl->getAttrs().getAttribute<ObjCAttr>();
  if (attr && attr->isImplicit())
    attr = nullptr;

  // If there is an @objc attribute with an explicit, incorrect witness
  // name, go fix the witness name.
  if (attr && name != targetName &&
      attr->hasName() && !attr->isNameImplicit()) {
    // Find the source range covering the full name.
    SourceLoc startLoc;
    if (attr->getNameLocs().empty())
      startLoc = attr->getRParenLoc();
    else
      startLoc = attr->getNameLocs().front();

    // Replace the name with the name of the requirement.
    SmallString<64> scratch;
    diag.fixItReplaceChars(startLoc, attr->getRParenLoc(),
                           targetName.getString(scratch));
    return false;
  }

  // We need to create or amend an @objc attribute with the appropriate name.

  // Form the Fix-It text.
  SourceLoc startLoc;
  SmallString<64> fixItText;
  {
    assert((!attr || !attr->hasName() || attr->isNameImplicit() ||
            name == targetName) && "Nothing to diagnose!");
    llvm::raw_svector_ostream out(fixItText);

    // If there is no @objc attribute, we need to add our own '@objc'.
    if (!attr) {
      startLoc = decl->getAttributeInsertionLoc(/*forModifier=*/false);
      out << "@objc";
    } else {
      startLoc = Lexer::getLocForEndOfToken(decl->getASTContext().SourceMgr,
                                            attr->getRange().End);
    }

    // If the names of the witness and requirement differ, we need to
    // specify the name.
    if (name != targetName || ignoreImpliedName) {
      out << "(";
      out << targetName;
      out << ")";
    }

    if (!attr)
      out << " ";
  }

  diag.fixItInsert(startLoc, fixItText);
  return false;
}

namespace {
  /// Produce a deterministic ordering of the given declarations.
  class OrderDeclarations {
    SourceManager &SrcMgr;

  public:
    OrderDeclarations(SourceManager &srcMgr) : SrcMgr(srcMgr) { }

    bool operator()(ValueDecl *lhs, ValueDecl *rhs) const {
      // If the declarations come from different modules, order based on the
      // module.
      ModuleDecl *lhsModule = lhs->getDeclContext()->getParentModule();
      ModuleDecl *rhsModule = rhs->getDeclContext()->getParentModule();
      if (lhsModule != rhsModule) {
        return lhsModule->getName().str() < rhsModule->getName().str();
      }

      // If the two declarations are in the same source file, order based on
      // location within that source file.
      SourceFile *lhsSF = lhs->getDeclContext()->getParentSourceFile();
      SourceFile *rhsSF = rhs->getDeclContext()->getParentSourceFile();
      if (lhsSF == rhsSF) {
        // If only one location is valid, the valid location comes first.
        if (lhs->getLoc().isValid() != rhs->getLoc().isValid()) {
          return lhs->getLoc().isValid();
        }

        // Prefer the declaration that comes first in the source file.
        return SrcMgr.isBeforeInBuffer(lhs->getLoc(), rhs->getLoc());
      }

      // The declarations are in different source files (or unknown source
      // files) of the same module. Order based on name.
      // FIXME: This isn't a total ordering.
      return lhs->getFullName() < rhs->getFullName();
    }
  };
} // end anonymous namespace

/// Lookup for an Objective-C method with the given selector in the
/// given class or any of its superclasses.
static AbstractFunctionDecl *lookupObjCMethodInClass(
                               ClassDecl *classDecl,
                               ObjCSelector selector,
                               bool isInstanceMethod,
                               bool isInitializer,
                               SourceManager &srcMgr,
                               bool inheritingInits = true) {
  if (!classDecl)
    return nullptr;

  // Look for an Objective-C method in this class.
  auto methods = classDecl->lookupDirect(selector, isInstanceMethod);
  if (!methods.empty()) {
    // If we aren't inheriting initializers, remove any initializers from the
    // list.
    if (!inheritingInits &&
        std::find_if(methods.begin(), methods.end(),
                     [](AbstractFunctionDecl *func) {
                       return isa<ConstructorDecl>(func);
                     }) != methods.end()) {
      SmallVector<AbstractFunctionDecl *, 4> nonInitMethods;
      std::copy_if(methods.begin(), methods.end(),
                   std::back_inserter(nonInitMethods),
                   [&](AbstractFunctionDecl *func) {
                     return !isa<ConstructorDecl>(func);
                   });
      if (nonInitMethods.empty())
        return nullptr;

      return *std::min_element(nonInitMethods.begin(), nonInitMethods.end(),
                               OrderDeclarations(srcMgr));
    }

    return *std::min_element(methods.begin(), methods.end(),
                             OrderDeclarations(srcMgr));
  }

  // Recurse into the superclass.
  if (!classDecl->hasSuperclass())
    return nullptr;

  // Determine whether we are (still) inheriting initializers.
  inheritingInits = inheritingInits &&
                    classDecl->inheritsSuperclassInitializers();
  if (isInitializer && !inheritingInits)
    return nullptr;

  return lookupObjCMethodInClass(classDecl->getSuperclassDecl(), selector,
                                 isInstanceMethod, isInitializer, srcMgr,
                                 inheritingInits);
}

bool swift::diagnoseUnintendedObjCMethodOverrides(SourceFile &sf) {
  auto &Ctx = sf.getASTContext();
  auto &methods = sf.ObjCMethodList;

  // If no Objective-C methods were defined in this file, we're done.
  if (methods.empty())
    return false;

  // Sort the methods by declaration order.
  std::sort(methods.begin(), methods.end(), OrderDeclarations(Ctx.SourceMgr));

  // For each Objective-C method declared in this file, check whether
  // it overrides something in one of its superclasses. We
  // intentionally don't respect access control here, since everything
  // is visible to the Objective-C runtime.
  bool diagnosedAny = false;
  for (auto method : methods) {
    // If the method has an @objc override, we don't need to do any
    // more checking.
    if (auto overridden = method->getOverriddenDecl()) {
      if (overridden->isObjC())
        continue;
    }

    // Skip deinitializers.
    if (isa<DestructorDecl>(method))
      continue;

    // Skip invalid declarations.
    if (method->isInvalid())
      continue;

    // Skip declarations with an invalid 'override' attribute on them.
    if (auto attr = method->getAttrs().getAttribute<OverrideAttr>(true)) {
      if (attr->isInvalid())
        continue;
    }

    auto classDecl = method->getDeclContext()->getSelfClassDecl();
    if (!classDecl)
      continue; // error-recovery path, only

    if (!classDecl->hasSuperclass())
      continue;

    // Look for a method that we have overridden in one of our
    // superclasses.
    // Note: This should be treated as a lookup for intra-module dependency
    // purposes, but a subclass already depends on its superclasses and any
    // extensions for many other reasons.
    auto selector = method->getObjCSelector();
    AbstractFunctionDecl *overriddenMethod
      = lookupObjCMethodInClass(classDecl->getSuperclassDecl(),
                                selector,
                                method->isObjCInstanceMethod(),
                                isa<ConstructorDecl>(method),
                                Ctx.SourceMgr);
    if (!overriddenMethod)
      continue;

    // Ignore stub implementations.
    if (auto overriddenCtor = dyn_cast<ConstructorDecl>(overriddenMethod)) {
      if (overriddenCtor->hasStubImplementation())
        continue;
    }

    // Diagnose the override.
    auto methodDiagInfo = getObjCMethodDiagInfo(method);
    auto overriddenDiagInfo = getObjCMethodDiagInfo(overriddenMethod);
    Ctx.Diags.diagnose(method, diag::objc_override_other,
                       methodDiagInfo.first,
                       methodDiagInfo.second,
                       overriddenDiagInfo.first,
                       overriddenDiagInfo.second,
                       selector,
                       overriddenMethod->getDeclContext()
                         ->getDeclaredInterfaceType());
    const ValueDecl *overriddenDecl = overriddenMethod;
    if (overriddenMethod->isImplicit())
      if (auto accessor = dyn_cast<AccessorDecl>(overriddenMethod))
        overriddenDecl = accessor->getStorage();
    Ctx.Diags.diagnose(overriddenDecl, diag::objc_declared_here,
                       overriddenDiagInfo.first, overriddenDiagInfo.second);

    diagnosedAny = true;
  }

  return diagnosedAny;
}

/// Retrieve the source file for the given Objective-C member conflict.
static MutableArrayRef<AbstractFunctionDecl *>
getObjCMethodConflictDecls(const SourceFile::ObjCMethodConflict &conflict) {
  ClassDecl *classDecl = std::get<0>(conflict);
  ObjCSelector selector = std::get<1>(conflict);
  bool isInstanceMethod = std::get<2>(conflict);

  return classDecl->lookupDirect(selector, isInstanceMethod);
}

/// Given a set of conflicting Objective-C methods, remove any methods
/// that are legitimately overridden in Objective-C, i.e., because
/// they occur in different modules, one is defined in the class, and
/// the other is defined in an extension (category) thereof.
static void removeValidObjCConflictingMethods(
              MutableArrayRef<AbstractFunctionDecl *> &methods) {
  // Erase any invalid or stub declarations. We don't want to complain about
  // them, because we might already have complained about
  // redeclarations based on Swift matching.
  auto newEnd = std::remove_if(methods.begin(), methods.end(),
                               [&](AbstractFunctionDecl *method) {
                                 if (method->isInvalid())
                                   return true;

                                 if (auto ad = dyn_cast<AccessorDecl>(method)) {
                                   return ad->getStorage()->isInvalid();
                                 } 
                                 
                                 if (auto ctor 
                                       = dyn_cast<ConstructorDecl>(method)) {
                                   if (ctor->hasStubImplementation())
                                     return true;

                                   return false;
                                 }

                                 return false;
                               });
  methods = methods.slice(0, newEnd - methods.begin());
}

bool swift::diagnoseObjCMethodConflicts(SourceFile &sf) {
  // If there were no conflicts, we're done.
  if (sf.ObjCMethodConflicts.empty())
    return false;

  auto &Ctx = sf.getASTContext();

  OrderDeclarations ordering(Ctx.SourceMgr);

  // Sort the set of conflicts so we get a deterministic order for
  // diagnostics. We use the first conflicting declaration in each set to
  // perform the sort.
  auto localConflicts = sf.ObjCMethodConflicts;
  std::sort(localConflicts.begin(), localConflicts.end(),
            [&](const SourceFile::ObjCMethodConflict &lhs,
                const SourceFile::ObjCMethodConflict &rhs) {
              return ordering(getObjCMethodConflictDecls(lhs)[1],
                              getObjCMethodConflictDecls(rhs)[1]);
            });

  // Diagnose each conflict.
  bool anyConflicts = false;
  for (const auto &conflict : localConflicts) {
    ObjCSelector selector = std::get<1>(conflict);

    auto methods = getObjCMethodConflictDecls(conflict);

    // Prune out cases where it is acceptable to have a conflict.
    removeValidObjCConflictingMethods(methods);
    if (methods.size() < 2)
      continue;

    // Diagnose the conflict.
    anyConflicts = true;

    // If the first method is in an extension and the second is not, swap them
    // so the primary diagnostic is on the extension method.
    if (isa<ExtensionDecl>(methods[0]->getDeclContext()) &&
        !isa<ExtensionDecl>(methods[1]->getDeclContext())) {
      std::swap(methods[0], methods[1]);

    // Within a source file, use our canonical ordering.
    } else if (methods[0]->getParentSourceFile() ==
               methods[1]->getParentSourceFile() &&
              !ordering(methods[0], methods[1])) {
      std::swap(methods[0], methods[1]);
    }

    // Otherwise, fall back to the order in which the declarations were type
    // checked.

    auto originalMethod = methods.front();
    auto conflictingMethods = methods.slice(1);

    auto origDiagInfo = getObjCMethodDiagInfo(originalMethod);
    for (auto conflictingDecl : conflictingMethods) {
      auto diagInfo = getObjCMethodDiagInfo(conflictingDecl);

      const ValueDecl *originalDecl = originalMethod;
      if (originalMethod->isImplicit())
        if (auto accessor = dyn_cast<AccessorDecl>(originalMethod))
          originalDecl = accessor->getStorage();

      if (diagInfo == origDiagInfo) {
        Ctx.Diags.diagnose(conflictingDecl, diag::objc_redecl_same,
                           diagInfo.first, diagInfo.second, selector);
        Ctx.Diags.diagnose(originalDecl, diag::invalid_redecl_prev,
                           originalDecl->getBaseName());
      } else {
        Ctx.Diags.diagnose(conflictingDecl, diag::objc_redecl,
                           diagInfo.first,
                            diagInfo.second,
                           origDiagInfo.first,
                           origDiagInfo.second,
                           selector);
        Ctx.Diags.diagnose(originalDecl, diag::objc_declared_here,
                           origDiagInfo.first, origDiagInfo.second);
      }
    }
  }

  return anyConflicts;
}

/// Retrieve the source location associated with this declaration
/// context.
static SourceLoc getDeclContextLoc(DeclContext *dc) {
  if (auto ext = dyn_cast<ExtensionDecl>(dc))
    return ext->getLoc();

  return cast<NominalTypeDecl>(dc)->getLoc();
}

bool swift::diagnoseObjCUnsatisfiedOptReqConflicts(SourceFile &sf) {
  // If there are no unsatisfied, optional @objc requirements, we're done.
  if (sf.ObjCUnsatisfiedOptReqs.empty())
    return false;

  auto &Ctx = sf.getASTContext();

  // Sort the set of local unsatisfied requirements, so we get a
  // deterministic order for diagnostics.
  auto &localReqs = sf.ObjCUnsatisfiedOptReqs;
  std::sort(localReqs.begin(), localReqs.end(),
            [&](const SourceFile::ObjCUnsatisfiedOptReq &lhs,
                const SourceFile::ObjCUnsatisfiedOptReq &rhs) -> bool {
              return Ctx.SourceMgr.isBeforeInBuffer(getDeclContextLoc(lhs.first),
                                                    getDeclContextLoc(rhs.first));
            });

  // Check each of the unsatisfied optional requirements.
  bool anyDiagnosed = false;
  for (const auto &unsatisfied : localReqs) {
    // Check whether there is a conflict here.
    ClassDecl *classDecl = unsatisfied.first->getSelfClassDecl();
    auto req = unsatisfied.second;
    auto selector = req->getObjCSelector();
    bool isInstanceMethod = req->isInstanceMember();
    // FIXME: Also look in superclasses?
    auto conflicts = classDecl->lookupDirect(selector, isInstanceMethod);
    if (conflicts.empty())
      continue;

    // Diagnose the conflict.
    auto reqDiagInfo = getObjCMethodDiagInfo(unsatisfied.second);
    auto conflictDiagInfo = getObjCMethodDiagInfo(conflicts[0]);
    auto protocolName
      = cast<ProtocolDecl>(req->getDeclContext())->getFullName();
    Ctx.Diags.diagnose(conflicts[0],
                       diag::objc_optional_requirement_conflict,
                       conflictDiagInfo.first,
                       conflictDiagInfo.second,
                       reqDiagInfo.first,
                       reqDiagInfo.second,
                       selector,
                       protocolName);

    // Fix the name of the witness, if we can.
    if (req->getFullName() != conflicts[0]->getFullName() &&
        req->getKind() == conflicts[0]->getKind() &&
        isa<AccessorDecl>(req) == isa<AccessorDecl>(conflicts[0])) {
      // They're of the same kind: fix the name.
      unsigned kind;
      if (isa<ConstructorDecl>(req))
        kind = 1;
      else if (auto accessor = dyn_cast<AccessorDecl>(req))
        kind = isa<SubscriptDecl>(accessor->getStorage()) ? 3 : 2;
      else if (isa<FuncDecl>(req))
        kind = 0;
      else {
        llvm_unreachable("unhandled @objc declaration kind");
      }

      auto diag = Ctx.Diags.diagnose(conflicts[0],
                                     diag::objc_optional_requirement_swift_rename,
                                     kind, req->getFullName());

      // Fix the Swift name.
      fixDeclarationName(diag, conflicts[0], req->getFullName());

      // Fix the '@objc' attribute, if needed.
      if (!conflicts[0]->canInferObjCFromRequirement(req))
        fixDeclarationObjCName(diag, conflicts[0],
                               conflicts[0]->getObjCRuntimeName(),
                               req->getObjCRuntimeName(),
                               /*ignoreImpliedName=*/true);
    }

    // @nonobjc will silence this warning.
    bool hasExplicitObjCAttribute = false;
    if (auto objcAttr = conflicts[0]->getAttrs().getAttribute<ObjCAttr>())
      hasExplicitObjCAttribute = !objcAttr->isImplicit();
    if (!hasExplicitObjCAttribute)
      Ctx.Diags.diagnose(conflicts[0], diag::req_near_match_nonobjc, true)
        .fixItInsert(
          conflicts[0]->getAttributeInsertionLoc(/*forModifier=*/false),
          "@nonobjc ");

    Ctx.Diags.diagnose(getDeclContextLoc(unsatisfied.first),
                       diag::protocol_conformance_here,
                       true,
                       classDecl->getFullName(),
                      protocolName);
    Ctx.Diags.diagnose(req, diag::kind_declname_declared_here,
                       DescriptiveDeclKind::Requirement, reqDiagInfo.second);

    anyDiagnosed = true;
  }

  return anyDiagnosed;
}
