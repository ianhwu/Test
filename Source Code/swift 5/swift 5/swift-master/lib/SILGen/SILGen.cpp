//===--- SILGen.cpp - Implements Lowering of ASTs -> SIL ------------------===//
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

#define DEBUG_TYPE "silgen"
#include "ManagedValue.h"
#include "RValue.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ResilienceExpansion.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/Timer.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILProfiler.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/Debug.h"
using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
// SILGenModule Class implementation
//===----------------------------------------------------------------------===//

SILGenModule::SILGenModule(SILModule &M, ModuleDecl *SM)
    : M(M), Types(M.Types), SwiftModule(SM), TopLevelSGF(nullptr) {
  SILOptions &Opts = M.getOptions();
  if (!Opts.UseProfile.empty()) {
    auto ReaderOrErr = llvm::IndexedInstrProfReader::create(Opts.UseProfile);
    if (auto E = ReaderOrErr.takeError()) {
      diagnose(SourceLoc(), diag::profile_read_error, Opts.UseProfile,
               llvm::toString(std::move(E)));
      Opts.UseProfile.erase();
    }
    M.setPGOReader(std::move(ReaderOrErr.get()));
  }
}

SILGenModule::~SILGenModule() {
  assert(!TopLevelSGF && "active source file lowering!?");
  M.verify();
}

static SILDeclRef
getBridgingFn(Optional<SILDeclRef> &cacheSlot,
              SILGenModule &SGM,
              Identifier moduleName,
              StringRef functionName,
              Optional<std::initializer_list<Type>> inputTypes,
              Optional<Type> outputType) {
  // FIXME: the optionality of outputType and the presence of trustInputTypes
  // are hacks for cases where coming up with those types is complicated, i.e.,
  // when dealing with generic bridging functions.

  if (!cacheSlot) {
    ASTContext &ctx = SGM.M.getASTContext();
    ModuleDecl *mod = ctx.getLoadedModule(moduleName);
    if (!mod) {
      SGM.diagnose(SourceLoc(), diag::bridging_module_missing,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    SmallVector<ValueDecl *, 2> decls;
    mod->lookupValue(ctx.getIdentifier(functionName),
                     NLKind::QualifiedLookup, decls);
    if (decls.empty()) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_missing,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }
    if (decls.size() != 1) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_overloaded,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    auto *fd = dyn_cast<FuncDecl>(decls.front());
    if (!fd) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_not_function,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    // Check that the function takes the expected arguments and returns the
    // expected result type.
    SILDeclRef c(fd);
    auto funcTy =
        SGM.Types.getConstantFunctionType(TypeExpansionContext::minimal(), c);
    SILFunctionConventions fnConv(funcTy, SGM.M);

    auto toSILType = [&SGM](Type ty) {
      return SGM.Types.getLoweredType(ty, TypeExpansionContext::minimal());
    };

    if (inputTypes) {
      if (fnConv.hasIndirectSILResults()
          || funcTy->getNumParameters() != inputTypes->size()
          || !std::equal(
                 fnConv.getParameterSILTypes().begin(),
                 fnConv.getParameterSILTypes().end(),
                 makeTransformIterator(inputTypes->begin(), toSILType))) {
        SGM.diagnose(fd->getLoc(), diag::bridging_function_not_correct_type,
                     moduleName.str(), functionName);
        llvm::report_fatal_error("unable to set up the ObjC bridge!");
      }
    }

    if (outputType
        && fnConv.getSingleSILResultType() != toSILType(*outputType)) {
      SGM.diagnose(fd->getLoc(), diag::bridging_function_not_correct_type,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    cacheSlot = c;
  }

  LLVM_DEBUG(llvm::dbgs() << "bridging function "
                          << moduleName << '.' << functionName
                          << " mapped to ";
             cacheSlot->print(llvm::dbgs()));

  return *cacheSlot;
}

#define REQUIRED(X) { Types.get##X##Type() }
#define OPTIONAL(X) { OptionalType::get(Types.get##X##Type()) }
#define GENERIC(X) None

#define GET_BRIDGING_FN(Module, FromKind, FromTy, ToKind, ToTy) \
  SILDeclRef SILGenModule::get##FromTy##To##ToTy##Fn() { \
    return getBridgingFn(FromTy##To##ToTy##Fn, *this, \
                         getASTContext().Id_##Module, \
                         "_convert" #FromTy "To" #ToTy, \
                         FromKind(FromTy), \
                         ToKind(ToTy)); \
  }

GET_BRIDGING_FN(Darwin, REQUIRED, Bool, REQUIRED, DarwinBoolean)
GET_BRIDGING_FN(Darwin, REQUIRED, DarwinBoolean, REQUIRED, Bool)
GET_BRIDGING_FN(ObjectiveC, REQUIRED, Bool, REQUIRED, ObjCBool)
GET_BRIDGING_FN(ObjectiveC, REQUIRED, ObjCBool, REQUIRED, Bool)
GET_BRIDGING_FN(Foundation, OPTIONAL, NSError, REQUIRED, Error)
GET_BRIDGING_FN(Foundation, REQUIRED, Error, REQUIRED, NSError)
GET_BRIDGING_FN(WinSDK, REQUIRED, Bool, REQUIRED, WindowsBool)
GET_BRIDGING_FN(WinSDK, REQUIRED, WindowsBool, REQUIRED, Bool)

#undef GET_BRIDGING_FN
#undef REQUIRED
#undef OPTIONAL
#undef GENERIC

static FuncDecl *diagnoseMissingIntrinsic(SILGenModule &sgm,
                                          SILLocation loc,
                                          const char *name) {
  sgm.diagnose(loc, diag::bridging_function_missing,
               sgm.getASTContext().StdlibModuleName.str(), name);
  return nullptr;
}

#define FUNC_DECL(NAME, ID)                             \
  FuncDecl *SILGenModule::get##NAME(SILLocation loc) {  \
    if (auto fn = getASTContext().get##NAME())   \
      return fn;                                        \
    return diagnoseMissingIntrinsic(*this, loc, ID);    \
  }
#include "swift/AST/KnownDecls.def"

ProtocolDecl *SILGenModule::getObjectiveCBridgeable(SILLocation loc) {
  if (ObjectiveCBridgeable)
    return *ObjectiveCBridgeable;

  // Find the _ObjectiveCBridgeable protocol.
  auto &ctx = getASTContext();
  auto proto = ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable);
  if (!proto)
    diagnose(loc, diag::bridging_objcbridgeable_missing);

  ObjectiveCBridgeable = proto;
  return proto;
}

FuncDecl *SILGenModule::getBridgeToObjectiveCRequirement(SILLocation loc) {
  if (BridgeToObjectiveCRequirement)
    return *BridgeToObjectiveCRequirement;

  // Find the _ObjectiveCBridgeable protocol.
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) {
    BridgeToObjectiveCRequirement = nullptr;
    return nullptr;
  }

  // Look for _bridgeToObjectiveC().
  auto &ctx = getASTContext();
  DeclName name(ctx, ctx.Id_bridgeToObjectiveC, llvm::ArrayRef<Identifier>());
  auto *found = dyn_cast_or_null<FuncDecl>(
    proto->getSingleRequirement(name));

  if (!found)
    diagnose(loc, diag::bridging_objcbridgeable_broken, name);

  BridgeToObjectiveCRequirement = found;
  return found;
}

FuncDecl *SILGenModule::getUnconditionallyBridgeFromObjectiveCRequirement(
    SILLocation loc) {
  if (UnconditionallyBridgeFromObjectiveCRequirement)
    return *UnconditionallyBridgeFromObjectiveCRequirement;

  // Find the _ObjectiveCBridgeable protocol.
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) {
    UnconditionallyBridgeFromObjectiveCRequirement = nullptr;
    return nullptr;
  }

  // Look for _bridgeToObjectiveC().
  auto &ctx = getASTContext();
  DeclName name(ctx, ctx.getIdentifier("_unconditionallyBridgeFromObjectiveC"),
                llvm::makeArrayRef(Identifier()));
  auto *found = dyn_cast_or_null<FuncDecl>(
    proto->getSingleRequirement(name));

  if (!found)
    diagnose(loc, diag::bridging_objcbridgeable_broken, name);

  UnconditionallyBridgeFromObjectiveCRequirement = found;
  return found;
}

AssociatedTypeDecl *
SILGenModule::getBridgedObjectiveCTypeRequirement(SILLocation loc) {
  if (BridgedObjectiveCType)
    return *BridgedObjectiveCType;

  // Find the _ObjectiveCBridgeable protocol.
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) {
    BridgeToObjectiveCRequirement = nullptr;
    return nullptr;
  }

  // Look for _bridgeToObjectiveC().
  auto &ctx = getASTContext();
  auto *found = proto->getAssociatedType(ctx.Id_ObjectiveCType);
  if (!found)
    diagnose(loc, diag::bridging_objcbridgeable_broken, ctx.Id_ObjectiveCType);

  BridgedObjectiveCType = found;
  return found;
}

ProtocolConformance *
SILGenModule::getConformanceToObjectiveCBridgeable(SILLocation loc, Type type) {
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) return nullptr;

  // Find the conformance to _ObjectiveCBridgeable.
  auto result = SwiftModule->lookupConformance(type, proto);
  if (result.isInvalid())
    return nullptr;

  return result.getConcrete();
}

ProtocolDecl *SILGenModule::getBridgedStoredNSError(SILLocation loc) {
  if (BridgedStoredNSError)
    return *BridgedStoredNSError;

  // Find the _BridgedStoredNSError protocol.
  auto &ctx = getASTContext();
  auto proto = ctx.getProtocol(KnownProtocolKind::BridgedStoredNSError);
  BridgedStoredNSError = proto;
  return proto;
}

VarDecl *SILGenModule::getNSErrorRequirement(SILLocation loc) {
  if (NSErrorRequirement)
    return *NSErrorRequirement;

  // Find the _BridgedStoredNSError protocol.
  auto proto = getBridgedStoredNSError(loc);
  if (!proto) {
    NSErrorRequirement = nullptr;
    return nullptr;
  }

  // Look for _nsError.
  auto &ctx = getASTContext();
  auto *found = dyn_cast_or_null<VarDecl>(
      proto->getSingleRequirement(ctx.Id_nsError));

  NSErrorRequirement = found;
  return found;
}

ProtocolConformanceRef
SILGenModule::getConformanceToBridgedStoredNSError(SILLocation loc, Type type) {
  auto proto = getBridgedStoredNSError(loc);
  if (!proto)
    return ProtocolConformanceRef::forInvalid();

  // Find the conformance to _BridgedStoredNSError.
  return SwiftModule->lookupConformance(type, proto);
}

ProtocolConformance *SILGenModule::getNSErrorConformanceToError() {
  if (NSErrorConformanceToError)
    return *NSErrorConformanceToError;

  auto &ctx = getASTContext();
  auto nsErrorTy = ctx.getNSErrorType();
  if (!nsErrorTy) {
    NSErrorConformanceToError = nullptr;
    return nullptr;
  }

  auto error = ctx.getErrorDecl();
  if (!error) {
    NSErrorConformanceToError = nullptr;
    return nullptr;
  }

  auto conformance =
    SwiftModule->lookupConformance(nsErrorTy, cast<ProtocolDecl>(error));

  if (conformance.isConcrete())
    NSErrorConformanceToError = conformance.getConcrete();
  else
    NSErrorConformanceToError = nullptr;
  return *NSErrorConformanceToError;
}

SILFunction *
SILGenModule::getKeyPathProjectionCoroutine(bool isReadAccess,
                                            KeyPathTypeKind typeKind) {
  bool isBaseInout;
  bool isResultInout;
  StringRef functionName;
  NominalTypeDecl *keyPathDecl;
  if (isReadAccess) {
    assert(typeKind == KPTK_KeyPath ||
           typeKind == KPTK_WritableKeyPath ||
           typeKind == KPTK_ReferenceWritableKeyPath);
    functionName = "swift_readAtKeyPath";
    isBaseInout = false;
    isResultInout = false;
    keyPathDecl = getASTContext().getKeyPathDecl();
  } else if (typeKind == KPTK_WritableKeyPath) {
    functionName = "swift_modifyAtWritableKeyPath";
    isBaseInout = true;
    isResultInout = true;
    keyPathDecl = getASTContext().getWritableKeyPathDecl();
  } else if (typeKind == KPTK_ReferenceWritableKeyPath) {
    functionName = "swift_modifyAtReferenceWritableKeyPath";
    isBaseInout = false;
    isResultInout = true;
    keyPathDecl = getASTContext().getReferenceWritableKeyPathDecl();
  } else {
    llvm_unreachable("bad combination");
  }

  auto fn = M.lookUpFunction(functionName);
  if (fn) return fn;

  auto rootType = CanGenericTypeParamType::get(0, 0, getASTContext());
  auto valueType = CanGenericTypeParamType::get(0, 1, getASTContext());

  // Build the generic signature <A, B>.
  auto sig = GenericSignature::get({rootType, valueType}, {});

  auto keyPathTy = BoundGenericType::get(keyPathDecl, Type(),
                                         { rootType, valueType })
    ->getCanonicalType();

  // (@in_guaranteed/@inout Root, @guaranteed KeyPath<Root, Value>)
  SILParameterInfo params[] = {
    { rootType,
      isBaseInout ? ParameterConvention::Indirect_Inout
                  : ParameterConvention::Indirect_In_Guaranteed },
    { keyPathTy, ParameterConvention::Direct_Guaranteed },
  };

  // -> @yields @in_guaranteed/@inout Value
  SILYieldInfo yields[] = {
    { valueType,
      isResultInout ? ParameterConvention::Indirect_Inout
                    : ParameterConvention::Indirect_In_Guaranteed },
  };

  auto extInfo = SILFunctionType::ExtInfo(
      SILFunctionTypeRepresentation::Thin,
      /*pseudogeneric*/ false,
      /*non-escaping*/ false, DifferentiabilityKind::NonDifferentiable);

  auto functionTy = SILFunctionType::get(sig, extInfo,
                                         SILCoroutineKind::YieldOnce,
                                         ParameterConvention::Direct_Unowned,
                                         params,
                                         yields,
                                         /*results*/ {},
                                         /*error result*/ {},
                                         SubstitutionMap(), false,
                                         getASTContext());

  auto env = sig->getGenericEnvironment();

  SILGenFunctionBuilder builder(*this);
  fn = builder.createFunction(SILLinkage::PublicExternal,
                              functionName,
                              functionTy,
                              env,
                              /*location*/ None,
                              IsNotBare,
                              IsNotTransparent,
                              IsNotSerialized,
                              IsNotDynamic);

  return fn;
}


SILFunction *SILGenModule::emitTopLevelFunction(SILLocation Loc) {
  ASTContext &C = getASTContext();
  auto extInfo = SILFunctionType::ExtInfo()
    .withRepresentation(SILFunctionType::Representation::CFunctionPointer);

  // Use standard library types if we have them; otherwise, fall back to
  // builtins.
  CanType Int32Ty;
  if (auto Int32Decl = C.getInt32Decl()) {
    Int32Ty = Int32Decl->getDeclaredInterfaceType()->getCanonicalType();
  } else {
    Int32Ty = CanType(BuiltinIntegerType::get(32, C));
  }

  CanType PtrPtrInt8Ty = C.TheRawPointerType;
  if (auto PointerDecl = C.getUnsafeMutablePointerDecl()) {
    if (auto Int8Decl = C.getInt8Decl()) {
      Type Int8Ty = Int8Decl->getDeclaredInterfaceType();
      Type PointerInt8Ty = BoundGenericType::get(PointerDecl,
                                                 nullptr,
                                                 Int8Ty);
      Type OptPointerInt8Ty = OptionalType::get(PointerInt8Ty);
      PtrPtrInt8Ty = BoundGenericType::get(PointerDecl,
                                           nullptr,
                                           OptPointerInt8Ty)
        ->getCanonicalType();
    }
  }

  SILParameterInfo params[] = {
    SILParameterInfo(Int32Ty, ParameterConvention::Direct_Unowned),
    SILParameterInfo(PtrPtrInt8Ty, ParameterConvention::Direct_Unowned),
  };

  CanSILFunctionType topLevelType = SILFunctionType::get(nullptr, extInfo,
                                   SILCoroutineKind::None,
                                   ParameterConvention::Direct_Unowned,
                                   params, /*yields*/ {},
                                   SILResultInfo(Int32Ty,
                                                 ResultConvention::Unowned),
                                   None,
                                   SubstitutionMap(), false,
                                   C);

  SILGenFunctionBuilder builder(*this);
  return builder.createFunction(
      SILLinkage::Public, SWIFT_ENTRY_POINT_FUNCTION, topLevelType, nullptr,
      Loc, IsBare, IsNotTransparent, IsNotSerialized, IsNotDynamic,
      ProfileCounter(), IsNotThunk, SubclassScope::NotApplicable);
}

SILFunction *SILGenModule::getEmittedFunction(SILDeclRef constant,
                                              ForDefinition_t forDefinition) {
  auto found = emittedFunctions.find(constant);
  if (found != emittedFunctions.end()) {
    SILFunction *F = found->second;
    if (forDefinition) {
      // In all the cases where getConstantLinkage returns something
      // different for ForDefinition, it returns an available-externally
      // linkage.
      if (isAvailableExternally(F->getLinkage())) {
        F->setLinkage(constant.getLinkage(ForDefinition));
      }
    }
    return F;
  }

  return nullptr;
}

static SILFunction *getFunctionToInsertAfter(SILGenModule &SGM,
                                             SILDeclRef insertAfter) {
  // If the decl ref was emitted, emit after its function.
  while (insertAfter) {
    auto found = SGM.emittedFunctions.find(insertAfter);
    if (found != SGM.emittedFunctions.end()) {
      return found->second;
    }

    // Otherwise, try to insert after the function we would be transitively
    // be inserted after.
    auto foundDelayed = SGM.delayedFunctions.find(insertAfter);
    if (foundDelayed != SGM.delayedFunctions.end()) {
      insertAfter = foundDelayed->second.insertAfter;
    } else {
      break;
    }
  }

  // If the decl ref is nil, just insert at the beginning.
  return nullptr;
}

static bool haveProfiledAssociatedFunction(SILDeclRef constant) {
  return constant.isDefaultArgGenerator() || constant.isForeign ||
         constant.isCurried;
}

/// Set up the function for profiling instrumentation.
static void setUpForProfiling(SILDeclRef constant, SILFunction *F,
                              ForDefinition_t forDefinition) {
  if (!forDefinition)
    return;

  ASTNode profiledNode;
  if (!haveProfiledAssociatedFunction(constant)) {
    if (constant.hasDecl()) {
      if (auto *fd = constant.getFuncDecl()) {
        if (fd->hasBody()) {
          F->createProfiler(fd, constant, forDefinition);
          profiledNode = fd->getBody(/*canSynthesize=*/false);
        }
      }
    } else if (auto *ace = constant.getAbstractClosureExpr()) {
      F->createProfiler(ace, constant, forDefinition);
      profiledNode = ace;
    }
    // Set the function entry count for PGO.
    if (SILProfiler *SP = F->getProfiler())
      F->setEntryCount(SP->getExecutionCount(profiledNode));
  }
}

static bool isEmittedOnDemand(SILModule &M, SILDeclRef constant) {
  if (!constant.hasDecl())
    return false;

  if (constant.isCurried ||
      constant.isForeign ||
      constant.isDirectReference)
    return false;

  auto *d = constant.getDecl();
  auto *dc = d->getDeclContext()->getModuleScopeContext();

  if (isa<ClangModuleUnit>(dc))
    return true;

  if (auto *func = dyn_cast<FuncDecl>(d))
    if (func->hasForcedStaticDispatch())
      return true;

  if (auto *sf = dyn_cast<SourceFile>(dc))
    if (M.isWholeModule() || M.getAssociatedContext() == dc)
      return false;

  return false;
}

SILFunction *SILGenModule::getFunction(SILDeclRef constant,
                                       ForDefinition_t forDefinition) {
  // If we already emitted the function, return it (potentially preparing it
  // for definition).
  if (auto emitted = getEmittedFunction(constant, forDefinition)) {
    setUpForProfiling(constant, emitted, forDefinition);
    return emitted;
  }

  // Note: Do not provide any SILLocation. You can set it afterwards.
  SILGenFunctionBuilder builder(*this);
  auto *F = builder.getOrCreateFunction(constant.hasDecl() ? constant.getDecl()
                                                           : (Decl *)nullptr,
                                        constant, forDefinition);
  setUpForProfiling(constant, F, forDefinition);

  assert(F && "SILFunction should have been defined");

  emittedFunctions[constant] = F;

  if (isEmittedOnDemand(M, constant) &&
      !delayedFunctions.count(constant)) {
    auto *d = constant.getDecl();
    if (auto *func = dyn_cast<FuncDecl>(d)) {
      if (constant.kind == SILDeclRef::Kind::Func)
        emitFunction(func);
    } else if (auto *ctor = dyn_cast<ConstructorDecl>(d)) {
      // For factories, we don't need to emit a special thunk; the normal
      // foreign-to-native thunk is sufficient.
      if (!ctor->isFactoryInit() &&
          constant.kind == SILDeclRef::Kind::Allocator)
        emitConstructor(ctor);
    }
  }

  // If we delayed emitting this function previously, we need it now.
  auto foundDelayed = delayedFunctions.find(constant);
  if (foundDelayed != delayedFunctions.end()) {
    // Move the function to its proper place within the module.
    M.functions.remove(F);
    SILFunction *insertAfter = getFunctionToInsertAfter(*this,
                                              foundDelayed->second.insertAfter);
    if (!insertAfter) {
      M.functions.push_front(F);
    } else {
      M.functions.insertAfter(insertAfter->getIterator(), F);
    }

    forcedFunctions.push_back(*foundDelayed);
    delayedFunctions.erase(foundDelayed);
  } else {
    // We would have registered a delayed function as "last emitted" when we
    // enqueued. If the function wasn't delayed, then we're emitting it now.
    lastEmittedFunction = constant;
  }

  return F;
}

bool SILGenModule::hasFunction(SILDeclRef constant) {
  return emittedFunctions.count(constant);
}

void SILGenModule::visitFuncDecl(FuncDecl *fd) { emitFunction(fd); }

/// Emit a function now, if it's externally usable or has been referenced in
/// the current TU, or remember how to emit it later if not.
template<typename /*void (SILFunction*)*/ Fn>
static void emitOrDelayFunction(SILGenModule &SGM,
                                SILDeclRef constant,
                                Fn &&emitter,
                                bool forceEmission = false) {
  auto emitAfter = SGM.lastEmittedFunction;

  SILFunction *f = nullptr;

  // If the function is explicit or may be externally referenced, or if we're
  // forcing emission, we must emit it.
  bool mayDelay;
  // Shared thunks and Clang-imported definitions can always be delayed.
  if (constant.isThunk() || constant.isClangImported()) {
    mayDelay = !forceEmission;
  // Implicit decls may be delayed if they can't be used externally.
  } else {
    auto linkage = constant.getLinkage(ForDefinition);
    mayDelay = !forceEmission &&
               (constant.isImplicit() &&
                !isPossiblyUsedExternally(linkage, SGM.M.isWholeModule()));
  }

  // Avoid emitting a delayable definition if it hasn't already been referenced.
  if (mayDelay)
    f = SGM.getEmittedFunction(constant, ForDefinition);
  else
    f = SGM.getFunction(constant, ForDefinition);

  // If we don't want to emit now, remember how for later.
  if (!f) {
    SGM.delayedFunctions.insert({constant, {emitAfter,
                                            std::forward<Fn>(emitter)}});
    // Even though we didn't emit the function now, update the
    // lastEmittedFunction so that we preserve the original ordering that
    // the symbols would have been emitted in.
    SGM.lastEmittedFunction = constant;
    return;
  }

  emitter(f);
}

void SILGenModule::preEmitFunction(SILDeclRef constant,
                              llvm::PointerUnion<ValueDecl *,
                                                 Expr *> astNode,
                                   SILFunction *F,
                                   SILLocation Loc) {
  // By default, use the astNode to create the location.
  if (Loc.isNull()) {
    if (auto *decl = astNode.get<ValueDecl *>())
      Loc = RegularLocation(decl);
    else
      Loc = RegularLocation(astNode.get<Expr *>());
  }

  assert(F->empty() && "already emitted function?!");

  if (F->getLoweredFunctionType()->isPolymorphic())
    F->setGenericEnvironment(Types.getConstantGenericEnvironment(constant));

  // Create a debug scope for the function using astNode as source location.
  F->setDebugScope(new (M) SILDebugScope(Loc, F));

  LLVM_DEBUG(llvm::dbgs() << "lowering ";
             F->printName(llvm::dbgs());
             llvm::dbgs() << " : ";
             F->getLoweredType().print(llvm::dbgs());
             llvm::dbgs() << '\n';
             if (astNode) {
               if (auto *decl = astNode.dyn_cast<ValueDecl *>()) {
                 decl->dump(llvm::dbgs());
               } else {
                 astNode.get<Expr *>()->dump(llvm::dbgs());
                 llvm::dbgs() << "\n";
               }
               llvm::dbgs() << '\n';
             });
}

void SILGenModule::postEmitFunction(SILDeclRef constant,
                                    SILFunction *F) {
  emitLazyConformancesForFunction(F);

  assert(!F->isExternalDeclaration() && "did not emit any function body?!");
  LLVM_DEBUG(llvm::dbgs() << "lowered sil:\n";
             F->print(llvm::dbgs()));
  F->verify();
}

void SILGenModule::
emitMarkFunctionEscapeForTopLevelCodeGlobals(SILLocation loc,
                                             CaptureInfo captureInfo) {
  assert(TopLevelSGF && TopLevelSGF->B.hasValidInsertionPoint()
         && "no valid code generator for top-level function?!");

  SmallVector<SILValue, 4> Captures;
  
  for (auto capture : captureInfo.getCaptures()) {
    // Decls captured by value don't escape.
    auto It = TopLevelSGF->VarLocs.find(capture.getDecl());
    if (It == TopLevelSGF->VarLocs.end() ||
        !It->getSecond().value->getType().isAddress())
      continue;
    
    Captures.push_back(It->second.value);
  }
  
  if (!Captures.empty())
    TopLevelSGF->B.createMarkFunctionEscape(loc, Captures);
}

void SILGenModule::emitAbstractFuncDecl(AbstractFunctionDecl *AFD) {
  // Emit any default argument generators.
  emitDefaultArgGenerators(AFD, AFD->getParameters());

  // If this is a function at global scope, it may close over a global variable.
  // If we're emitting top-level code, then emit a "mark_function_escape" that
  // lists the captured global variables so that definite initialization can
  // reason about this escape point.
  if (!AFD->getDeclContext()->isLocalContext() &&
      TopLevelSGF && TopLevelSGF->B.hasValidInsertionPoint()) {
    emitMarkFunctionEscapeForTopLevelCodeGlobals(AFD, AFD->getCaptureInfo());
  }
  
  // If the declaration is exported as a C function, emit its native-to-foreign
  // thunk too, if it wasn't already forced.
  if (AFD->getAttrs().hasAttribute<CDeclAttr>()) {
    auto thunk = SILDeclRef(AFD).asForeign();
    if (!hasFunction(thunk))
      emitNativeToForeignThunk(thunk);
  }
}

void SILGenModule::emitFunction(FuncDecl *fd) {
  SILDeclRef::Loc decl = fd;

  emitAbstractFuncDecl(fd);

  if (fd->hasBody()) {
    FrontendStatsTracer Tracer(getASTContext().Stats, "SILGen-funcdecl", fd);
    PrettyStackTraceDecl stackTrace("emitting SIL for", fd);

    SILDeclRef constant(decl);

    bool ForCoverageMapping = doesASTRequireProfiling(M, fd);

    emitOrDelayFunction(*this, constant, [this,constant,fd](SILFunction *f){
      preEmitFunction(constant, fd, f, fd);
      PrettyStackTraceSILFunction X("silgen emitFunction", f);
      SILGenFunction(*this, *f, fd).emitFunction(fd);
      postEmitFunction(constant, f);
    }, /*forceEmission=*/ForCoverageMapping);
  }
}

void SILGenModule::addGlobalVariable(VarDecl *global) {
  // We create SILGlobalVariable here.
  getSILGlobalVariable(global, ForDefinition);
}

void SILGenModule::emitConstructor(ConstructorDecl *decl) {
  // FIXME: Handle 'self' like any other argument here.
  // Emit any default argument getter functions.
  emitAbstractFuncDecl(decl);

  // We never emit constructors in protocols.
  if (isa<ProtocolDecl>(decl->getDeclContext()))
    return;

  // Always-unavailable imported constructors are factory methods
  // that have been imported as constructors and then hidden by an
  // imported init method.
  if (decl->hasClangNode() &&
      decl->getAttrs().isUnavailable(decl->getASTContext()))
    return;

  SILDeclRef constant(decl);
  DeclContext *declCtx = decl->getDeclContext();

  bool ForCoverageMapping = doesASTRequireProfiling(M, decl);

  if (declCtx->getSelfClassDecl()) {
    // Designated initializers for classes, as well as @objc convenience
    // initializers, have have separate entry points for allocation and
    // initialization.
    if (decl->isDesignatedInit() || decl->isObjC()) {
      emitOrDelayFunction(
          *this, constant, [this, constant, decl](SILFunction *f) {
            preEmitFunction(constant, decl, f, decl);
            PrettyStackTraceSILFunction X("silgen emitConstructor", f);
            SILGenFunction(*this, *f, decl).emitClassConstructorAllocator(decl);
            postEmitFunction(constant, f);
          });

      // Constructors may not have bodies if they've been imported, or if they've
      // been parsed from a module interface.
      if (decl->hasBody()) {
        SILDeclRef initConstant(decl, SILDeclRef::Kind::Initializer);
        emitOrDelayFunction(
            *this, initConstant,
            [this, initConstant, decl](SILFunction *initF) {
              preEmitFunction(initConstant, decl, initF, decl);
              PrettyStackTraceSILFunction X("silgen constructor initializer",
                                            initF);
              initF->createProfiler(decl, initConstant, ForDefinition);
              SILGenFunction(*this, *initF, decl)
                .emitClassConstructorInitializer(decl);
              postEmitFunction(initConstant, initF);
            },
            /*forceEmission=*/ForCoverageMapping);
      }
      return;
    }
  }

  // Struct and enum constructors do everything in a single function, as do
  // non-@objc convenience initializers for classes.
  if (decl->hasBody()) {
    emitOrDelayFunction(
        *this, constant, [this, constant, decl](SILFunction *f) {
          preEmitFunction(constant, decl, f, decl);
          PrettyStackTraceSILFunction X("silgen emitConstructor", f);
          f->createProfiler(decl, constant, ForDefinition);
          SILGenFunction(*this, *f, decl).emitValueConstructor(decl);
          postEmitFunction(constant, f);
        });
  }
}

void SILGenModule::emitEnumConstructor(EnumElementDecl *decl) {
  // Enum element constructors are always emitted by need, so don't need
  // delayed emission.
  SILDeclRef constant(decl);
  SILFunction *f = getFunction(constant, ForDefinition);
  preEmitFunction(constant, decl, f, decl);
  PrettyStackTraceSILFunction X("silgen enum constructor", f);
  SILGenFunction(*this, *f, decl->getDeclContext()).emitEnumConstructor(decl);
  postEmitFunction(constant, f);
}

SILFunction *SILGenModule::emitClosure(AbstractClosureExpr *ce) {
  SILDeclRef constant(ce);
  SILFunction *f = getFunction(constant, ForDefinition);

  // Generate the closure function, if we haven't already.
  //
  // We may visit the same closure expr multiple times in some cases,
  // for instance, when closures appear as in-line initializers of stored
  // properties. In these cases the closure will be emitted into every
  // initializer of the containing type.
  if (!f->isExternalDeclaration())
    return f;
  preEmitFunction(constant, ce, f, ce);
  PrettyStackTraceSILFunction X("silgen closureexpr", f);
  SILGenFunction(*this, *f, ce).emitClosure(ce);
  postEmitFunction(constant, f);
  return f;
}

/// Determine whether the given class requires a separate instance
/// variable initialization method.
static bool requiresIVarInitialization(SILGenModule &SGM, ClassDecl *cd) {
  if (!cd->requiresStoredPropertyInits())
    return false;

  for (Decl *member : cd->getMembers()) {
    auto pbd = dyn_cast<PatternBindingDecl>(member);
    if (!pbd) continue;

    for (auto i : range(pbd->getNumPatternEntries()))
      if (pbd->getExecutableInit(i))
        return true;
  }

  return false;
}

bool SILGenModule::hasNonTrivialIVars(ClassDecl *cd) {
  for (Decl *member : cd->getMembers()) {
    auto *vd = dyn_cast<VarDecl>(member);
    if (!vd || !vd->hasStorage()) continue;

    auto &ti = Types.getTypeLowering(
        vd->getType(), TypeExpansionContext::maximalResilienceExpansionOnly());
    if (!ti.isTrivial())
      return true;
  }

  return false;
}

bool SILGenModule::requiresIVarDestroyer(ClassDecl *cd) {
  // Only needed if we have non-trivial ivars, we're not a root class, and
  // the superclass is not @objc.
  return (hasNonTrivialIVars(cd) &&
          cd->getSuperclass() &&
          !cd->getSuperclass()->getClassOrBoundGenericClass()->hasClangNode());
}

/// TODO: This needs a better name.
void SILGenModule::emitObjCAllocatorDestructor(ClassDecl *cd,
                                               DestructorDecl *dd) {
  // Emit the native deallocating destructor for -dealloc.
  // Destructors are a necessary part of class metadata, so can't be delayed.
  if (dd->hasBody()) {
    SILDeclRef dealloc(dd, SILDeclRef::Kind::Deallocator);
    SILFunction *f = getFunction(dealloc, ForDefinition);
    preEmitFunction(dealloc, dd, f, dd);
    PrettyStackTraceSILFunction X("silgen emitDestructor -dealloc", f);
    f->createProfiler(dd, dealloc, ForDefinition);
    SILGenFunction(*this, *f, dd).emitObjCDestructor(dealloc);
    postEmitFunction(dealloc, f);
  }

  // Emit the Objective-C -dealloc entry point if it has
  // something to do beyond messaging the superclass's -dealloc.
  if (dd->hasBody() && !dd->getBody()->empty())
    emitObjCDestructorThunk(dd);

  // Emit the ivar initializer, if needed.
  if (requiresIVarInitialization(*this, cd)) {
    auto ivarInitializer = SILDeclRef(cd, SILDeclRef::Kind::IVarInitializer)
      .asForeign();
    SILFunction *f = getFunction(ivarInitializer, ForDefinition);
    preEmitFunction(ivarInitializer, dd, f, dd);
    PrettyStackTraceSILFunction X("silgen emitDestructor ivar initializer", f);
    SILGenFunction(*this, *f, cd).emitIVarInitializer(ivarInitializer);
    postEmitFunction(ivarInitializer, f);
  }

  // Emit the ivar destroyer, if needed.
  if (hasNonTrivialIVars(cd)) {
    auto ivarDestroyer = SILDeclRef(cd, SILDeclRef::Kind::IVarDestroyer)
      .asForeign();
    SILFunction *f = getFunction(ivarDestroyer, ForDefinition);
    preEmitFunction(ivarDestroyer, dd, f, dd);
    PrettyStackTraceSILFunction X("silgen emitDestructor ivar destroyer", f);
    SILGenFunction(*this, *f, cd).emitIVarDestroyer(ivarDestroyer);
    postEmitFunction(ivarDestroyer, f);
  }
}

void SILGenModule::emitDestructor(ClassDecl *cd, DestructorDecl *dd) {
  emitAbstractFuncDecl(dd);
  
  // Emit the ivar destroyer, if needed.
  if (requiresIVarDestroyer(cd)) {
    SILDeclRef ivarDestroyer(cd, SILDeclRef::Kind::IVarDestroyer);
    SILFunction *f = getFunction(ivarDestroyer, ForDefinition);
    preEmitFunction(ivarDestroyer, dd, f, dd);
    PrettyStackTraceSILFunction X("silgen emitDestructor ivar destroyer", f);
    SILGenFunction(*this, *f, dd).emitIVarDestroyer(ivarDestroyer);
    postEmitFunction(ivarDestroyer, f);
  }

  // If the class would use the Objective-C allocator, only emit -dealloc.
  if (usesObjCAllocator(cd)) {
    emitObjCAllocatorDestructor(cd, dd);
    return;
  }

  // Emit the destroying destructor.
  // Destructors are a necessary part of class metadata, so can't be delayed.
  if (dd->hasBody()) {
    SILDeclRef destroyer(dd, SILDeclRef::Kind::Destroyer);
    SILFunction *f = getFunction(destroyer, ForDefinition);
    preEmitFunction(destroyer, dd, f, dd);
    PrettyStackTraceSILFunction X("silgen emitDestroyingDestructor", f);
    SILGenFunction(*this, *f, dd).emitDestroyingDestructor(dd);
    f->setDebugScope(new (M) SILDebugScope(dd, f));
    postEmitFunction(destroyer, f);
  }

  // Emit the deallocating destructor.
  {
    SILDeclRef deallocator(dd, SILDeclRef::Kind::Deallocator);
    SILFunction *f = getFunction(deallocator, ForDefinition);
    preEmitFunction(deallocator, dd, f, dd);
    PrettyStackTraceSILFunction X("silgen emitDeallocatingDestructor", f);
    f->createProfiler(dd, deallocator, ForDefinition);
    SILGenFunction(*this, *f, dd).emitDeallocatingDestructor(dd);
    f->setDebugScope(new (M) SILDebugScope(dd, f));
    postEmitFunction(deallocator, f);
  }
}

void SILGenModule::emitDefaultArgGenerator(SILDeclRef constant,
                                           ParamDecl *param) {
  auto initDC = param->getDefaultArgumentInitContext();

  switch (param->getDefaultArgumentKind()) {
  case DefaultArgumentKind::None:
    llvm_unreachable("No default argument here?");

  case DefaultArgumentKind::Normal: {
    auto arg = param->getTypeCheckedDefaultExpr();
    emitOrDelayFunction(*this, constant,
        [this,constant,arg,initDC](SILFunction *f) {
      preEmitFunction(constant, arg, f, arg);
      PrettyStackTraceSILFunction X("silgen emitDefaultArgGenerator ", f);
      SILGenFunction SGF(*this, *f, initDC);
      SGF.emitGeneratorFunction(constant, arg);
      postEmitFunction(constant, f);
    });
    return;
  }

  case DefaultArgumentKind::StoredProperty: {
    auto arg = param->getStoredProperty();
    emitOrDelayFunction(*this, constant,
        [this,constant,arg,initDC](SILFunction *f) {
      preEmitFunction(constant, arg, f, arg);
      PrettyStackTraceSILFunction X("silgen emitDefaultArgGenerator ", f);
      SILGenFunction SGF(*this, *f, initDC);
      SGF.emitGeneratorFunction(constant, arg);
      postEmitFunction(constant, f);
    });
    return;
  }

  case DefaultArgumentKind::Inherited:
  case DefaultArgumentKind::Column:
  case DefaultArgumentKind::File:
  case DefaultArgumentKind::Line:
  case DefaultArgumentKind::Function:
  case DefaultArgumentKind::DSOHandle:
  case DefaultArgumentKind::NilLiteral:
  case DefaultArgumentKind::EmptyArray:
  case DefaultArgumentKind::EmptyDictionary:
    return;
  }
}

void SILGenModule::
emitStoredPropertyInitialization(PatternBindingDecl *pbd, unsigned i) {
  auto *var = pbd->getAnchoringVarDecl(i);
  auto *init = pbd->getInit(i);
  auto *initDC = pbd->getInitContext(i);
  auto captureInfo = pbd->getCaptureInfo(i);
  assert(!pbd->isInitializerSubsumed(i));

  // If this is the backing storage for a property with an attached wrapper
  // that was initialized with `=`, use that expression as the initializer.
  if (auto originalProperty = var->getOriginalWrappedProperty()) {
    if (originalProperty
            ->isPropertyMemberwiseInitializedWithWrappedType()) {
      auto wrapperInfo =
          originalProperty->getPropertyWrapperBackingPropertyInfo();
      if (wrapperInfo.originalInitialValue)
        init = wrapperInfo.originalInitialValue;
    }
  }

  SILDeclRef constant(var, SILDeclRef::Kind::StoredPropertyInitializer);
  emitOrDelayFunction(*this, constant,
                      [this,var,captureInfo,constant,init,initDC](SILFunction *f) {
    preEmitFunction(constant, init, f, init);
    PrettyStackTraceSILFunction X("silgen emitStoredPropertyInitialization", f);
    f->createProfiler(init, constant, ForDefinition);
    SILGenFunction SGF(*this, *f, initDC);

    // If this is a stored property initializer inside a type at global scope,
    // it may close over a global variable. If we're emitting top-level code,
    // then emit a "mark_function_escape" that lists the captured global
    // variables so that definite initialization can reason about this
    // escape point.
    if (!var->getDeclContext()->isLocalContext() &&
        TopLevelSGF && TopLevelSGF->B.hasValidInsertionPoint()) {
      emitMarkFunctionEscapeForTopLevelCodeGlobals(var, captureInfo);
    }

    SGF.emitGeneratorFunction(constant, init, /*EmitProfilerIncrement=*/true);
    postEmitFunction(constant, f);
  });
}

void SILGenModule::
emitPropertyWrapperBackingInitializer(VarDecl *var) {
  SILDeclRef constant(var, SILDeclRef::Kind::PropertyWrapperBackingInitializer);
  emitOrDelayFunction(*this, constant, [this, constant, var](SILFunction *f) {
    preEmitFunction(constant, var, f, var);
    PrettyStackTraceSILFunction X(
        "silgen emitPropertyWrapperBackingInitializer", f);
    auto wrapperInfo = var->getPropertyWrapperBackingPropertyInfo();
    assert(wrapperInfo.initializeFromOriginal);
    f->createProfiler(wrapperInfo.initializeFromOriginal, constant,
                      ForDefinition);
    auto varDC = var->getInnermostDeclContext();
    SILGenFunction SGF(*this, *f, varDC);
    SGF.emitGeneratorFunction(constant, wrapperInfo.initializeFromOriginal);
    postEmitFunction(constant, f);
  });
}

SILFunction *SILGenModule::emitLazyGlobalInitializer(StringRef funcName,
                                                 PatternBindingDecl *binding,
                                                     unsigned pbdEntry) {
  ASTContext &C = M.getASTContext();
  auto *onceBuiltin =
      cast<FuncDecl>(getBuiltinValueDecl(C, C.getIdentifier("once")));
  auto blockParam = onceBuiltin->getParameters()->get(1);
  auto *type = blockParam->getType()->castTo<FunctionType>();
  Type initType = FunctionType::get({}, TupleType::getEmpty(C),
                                    type->getExtInfo());
  auto initSILType = cast<SILFunctionType>(
      Types.getLoweredRValueType(TypeExpansionContext::minimal(), initType));

  SILGenFunctionBuilder builder(*this);
  auto *f = builder.createFunction(
      SILLinkage::Private, funcName, initSILType, nullptr, SILLocation(binding),
      IsNotBare, IsNotTransparent, IsNotSerialized, IsNotDynamic);
  f->setDebugScope(new (M) SILDebugScope(RegularLocation(binding), f));
  auto dc = binding->getDeclContext();
  SILGenFunction(*this, *f, dc).emitLazyGlobalInitializer(binding, pbdEntry);
  emitLazyConformancesForFunction(f);
  f->verify();

  return f;
}

void SILGenModule::emitGlobalAccessor(VarDecl *global,
                                      SILGlobalVariable *onceToken,
                                      SILFunction *onceFunc) {
  SILDeclRef accessor(global, SILDeclRef::Kind::GlobalAccessor);
  emitOrDelayFunction(*this, accessor,
                      [this,accessor,global,onceToken,onceFunc](SILFunction *f){
    preEmitFunction(accessor, global, f, global);
    PrettyStackTraceSILFunction X("silgen emitGlobalAccessor", f);
    SILGenFunction(*this, *f, global->getDeclContext())
      .emitGlobalAccessor(global, onceToken, onceFunc);
    postEmitFunction(accessor, f);
  });
}

void SILGenModule::emitDefaultArgGenerators(SILDeclRef::Loc decl,
                                            ParameterList *paramList) {
  unsigned index = 0;
  for (auto param : *paramList) {
    if (param->isDefaultArgument())
      emitDefaultArgGenerator(SILDeclRef::getDefaultArgGenerator(decl, index),
                              param);
    ++index;
  }
}

void SILGenModule::emitObjCMethodThunk(FuncDecl *method) {
  auto thunk = SILDeclRef(method).asForeign();

  // Don't emit the thunk if it already exists.
  if (hasFunction(thunk))
    return;

  // ObjC entry points are always externally usable, so can't be delay-emitted.

  SILFunction *f = getFunction(thunk, ForDefinition);
  preEmitFunction(thunk, method, f, method);
  PrettyStackTraceSILFunction X("silgen emitObjCMethodThunk", f);
  f->setBare(IsBare);
  f->setThunk(IsThunk);
  SILGenFunction(*this, *f, method).emitNativeToForeignThunk(thunk);
  postEmitFunction(thunk, f);
}

void SILGenModule::emitObjCPropertyMethodThunks(AbstractStorageDecl *prop) {
  auto *getter = prop->getOpaqueAccessor(AccessorKind::Get);

  // If we don't actually need an entry point for the getter, do nothing.
  if (!getter || !requiresObjCMethodEntryPoint(getter))
    return;

  auto getterRef = SILDeclRef(getter, SILDeclRef::Kind::Func).asForeign();

  // Don't emit the thunks if they already exist.
  if (hasFunction(getterRef))
    return;

  RegularLocation ThunkBodyLoc(prop);
  ThunkBodyLoc.markAutoGenerated();
  // ObjC entry points are always externally usable, so emitting can't be
  // delayed.
  {
    SILFunction *f = getFunction(getterRef, ForDefinition);
    preEmitFunction(getterRef, prop, f, ThunkBodyLoc);
    PrettyStackTraceSILFunction X("silgen objc property getter thunk", f);
    f->setBare(IsBare);
    f->setThunk(IsThunk);
    SILGenFunction(*this, *f, getter).emitNativeToForeignThunk(getterRef);
    postEmitFunction(getterRef, f);
  }

  if (!prop->isSettable(prop->getDeclContext()))
    return;

  // FIXME: Add proper location.
  auto *setter = prop->getOpaqueAccessor(AccessorKind::Set);
  auto setterRef = SILDeclRef(setter, SILDeclRef::Kind::Func).asForeign();

  SILFunction *f = getFunction(setterRef, ForDefinition);
  preEmitFunction(setterRef, prop, f, ThunkBodyLoc);
  PrettyStackTraceSILFunction X("silgen objc property setter thunk", f);
  f->setBare(IsBare);
  f->setThunk(IsThunk);
  SILGenFunction(*this, *f, setter).emitNativeToForeignThunk(setterRef);
  postEmitFunction(setterRef, f);
}

void SILGenModule::emitObjCConstructorThunk(ConstructorDecl *constructor) {
  auto thunk = SILDeclRef(constructor, SILDeclRef::Kind::Initializer)
    .asForeign();

  // Don't emit the thunk if it already exists.
  if (hasFunction(thunk))
    return;
  // ObjC entry points are always externally usable, so emitting can't be
  // delayed.

  SILFunction *f = getFunction(thunk, ForDefinition);
  preEmitFunction(thunk, constructor, f, constructor);
  PrettyStackTraceSILFunction X("silgen objc constructor thunk", f);
  f->setBare(IsBare);
  f->setThunk(IsThunk);
  SILGenFunction(*this, *f, constructor).emitNativeToForeignThunk(thunk);
  postEmitFunction(thunk, f);
}

void SILGenModule::emitObjCDestructorThunk(DestructorDecl *destructor) {
  auto thunk = SILDeclRef(destructor, SILDeclRef::Kind::Deallocator)
    .asForeign();

  // Don't emit the thunk if it already exists.
  if (hasFunction(thunk))
    return;
  SILFunction *f = getFunction(thunk, ForDefinition);
  preEmitFunction(thunk, destructor, f, destructor);
  PrettyStackTraceSILFunction X("silgen objc destructor thunk", f);
  f->setBare(IsBare);
  f->setThunk(IsThunk);
  SILGenFunction(*this, *f, destructor).emitNativeToForeignThunk(thunk);
  postEmitFunction(thunk, f);
}

void SILGenModule::visitPatternBindingDecl(PatternBindingDecl *pd) {
  assert(!TopLevelSGF && "script mode PBDs should be in TopLevelCodeDecls");
  for (auto i : range(pd->getNumPatternEntries()))
    if (pd->getExecutableInit(i))
      emitGlobalInitialization(pd, i);
}

void SILGenModule::visitVarDecl(VarDecl *vd) {
  if (vd->hasStorage())
    addGlobalVariable(vd);

  vd->visitEmittedAccessors([&](AccessorDecl *accessor) {
    emitFunction(accessor);
  });

  tryEmitPropertyDescriptor(vd);
}

void SILGenModule::visitSubscriptDecl(SubscriptDecl *sd) {
  llvm_unreachable("top-level subscript?");
}

bool
SILGenModule::canStorageUseStoredKeyPathComponent(AbstractStorageDecl *decl,
                                                  ResilienceExpansion expansion) {
  // If the declaration is resilient, we have to treat the component as
  // computed.
  if (decl->isResilient(M.getSwiftModule(), expansion))
    return false;

  auto strategy = decl->getAccessStrategy(AccessSemantics::Ordinary,
                                          decl->supportsMutation()
                                            ? AccessKind::ReadWrite
                                            : AccessKind::Read,
                                          M.getSwiftModule(),
                                          expansion);
  switch (strategy.getKind()) {
  case AccessStrategy::Storage: {
    // Keypaths rely on accessors to handle the special behavior of weak or
    // unowned properties.
    if (decl->getInterfaceType()->is<ReferenceStorageType>())
      return false;
    // If the stored value would need to be reabstracted in fully opaque
    // context, then we have to treat the component as computed.
    auto componentObjTy = decl->getValueInterfaceType();
    if (auto genericEnv =
              decl->getInnermostDeclContext()->getGenericEnvironmentOfContext())
      componentObjTy = genericEnv->mapTypeIntoContext(componentObjTy);
    auto storageTy = M.Types.getSubstitutedStorageType(
        TypeExpansionContext::minimal(), decl, componentObjTy);
    auto opaqueTy = M.Types.getLoweredRValueType(
        TypeExpansionContext::noOpaqueTypeArchetypesSubstitution(expansion),
        AbstractionPattern::getOpaque(), componentObjTy);

    return storageTy.getASTType() == opaqueTy;
  }
  case AccessStrategy::DirectToAccessor:
  case AccessStrategy::DispatchToAccessor:
  case AccessStrategy::MaterializeToTemporary:
    return false;
  }
  llvm_unreachable("unhandled strategy");
}

static bool canStorageUseTrivialDescriptor(SILGenModule &SGM,
                                           AbstractStorageDecl *decl) {
  // A property can use a trivial property descriptor if the key path component
  // that an external module would form given publicly-exported information
  // about the property is never equivalent to the canonical component for the
  // key path.
  // This means that the property isn't stored (without promising to be always
  // stored) and doesn't have a setter with less-than-public visibility.
  auto expansion = ResilienceExpansion::Maximal;

  if (!SGM.M.getSwiftModule()->isResilient()) {
    if (SGM.canStorageUseStoredKeyPathComponent(decl, expansion)) {
      // External modules can't directly access storage, unless this is a
      // property in a fixed-layout type.
      return !decl->isFormallyResilient();
    }

    // If the type is computed and doesn't have a setter that's hidden from
    // the public, then external components can form the canonical key path
    // without our help.
    auto *setter = decl->getOpaqueAccessor(AccessorKind::Set);
    if (!setter)
      return true;

    if (setter->getFormalAccessScope(nullptr, true).isPublic())
      return true;

    return false;
  }

  // A resilient module needs to handle binaries compiled against its older
  // versions. This means we have to be a bit more conservative, since in
  // earlier versions, a settable property may have withheld the setter,
  // or a fixed-layout type may not have been.
  // Without availability information, only get-only computed properties
  // can resiliently use trivial descriptors.
  return (!SGM.canStorageUseStoredKeyPathComponent(decl, expansion) &&
          !decl->supportsMutation());
}

void SILGenModule::tryEmitPropertyDescriptor(AbstractStorageDecl *decl) {
  // TODO: Key path code emission doesn't handle opaque values properly yet.
  if (!SILModuleConventions(M).useLoweredAddresses())
    return;
  
  if (!decl->exportsPropertyDescriptor())
    return;

  PrettyStackTraceDecl stackTrace("emitting property descriptor for", decl);

  Type baseTy;
  if (decl->getDeclContext()->isTypeContext()) {
    // TODO: Static properties should eventually be referenceable as
    // keypaths from T.Type -> Element, viz `baseTy = MetatypeType::get(baseTy)`
    assert(!decl->isStatic());
    
    baseTy = decl->getDeclContext()->getSelfInterfaceType()
                 ->getCanonicalType(decl->getInnermostDeclContext()
                                        ->getGenericSignatureOfContext());
  } else {
    // TODO: Global variables should eventually be referenceable as
    // key paths from (), viz. baseTy = TupleType::getEmpty(getASTContext());
    llvm_unreachable("should not export a property descriptor yet");
  }

  auto genericEnv = decl->getInnermostDeclContext()
                        ->getGenericEnvironmentOfContext();
  unsigned baseOperand = 0;
  bool needsGenericContext = true;
  
  if (canStorageUseTrivialDescriptor(*this, decl)) {
    (void)SILProperty::create(M, /*serialized*/ false, decl, None);
    return;
  }
  
  SubstitutionMap subs;
  if (genericEnv)
    subs = genericEnv->getForwardingSubstitutionMap();
  
  auto component = emitKeyPathComponentForDecl(SILLocation(decl),
                                               genericEnv,
                                               ResilienceExpansion::Maximal,
                                               baseOperand, needsGenericContext,
                                               subs, decl, {},
                                               baseTy->getCanonicalType(),
                                               /*property descriptor*/ true);
  
  (void)SILProperty::create(M, /*serialized*/ false, decl, component);
}

void SILGenModule::visitIfConfigDecl(IfConfigDecl *ICD) {
  // Nothing to do for these kinds of decls - anything active has been added
  // to the enclosing declaration.
}

void SILGenModule::visitPoundDiagnosticDecl(PoundDiagnosticDecl *PDD) {
  // Nothing to do for #error/#warning; they've already been emitted.
}

void SILGenModule::visitTopLevelCodeDecl(TopLevelCodeDecl *td) {
  assert(TopLevelSGF && "top-level code in a non-main source file!");

  if (!TopLevelSGF->B.hasValidInsertionPoint())
    return;

  // A single SILFunction may be used to lower multiple top-level decls. When
  // this happens, fresh profile counters must be assigned to the new decl.
  TopLevelSGF->F.discardProfiler();
  TopLevelSGF->F.createProfiler(td, SILDeclRef(), ForDefinition);

  TopLevelSGF->emitProfilerIncrement(td->getBody());
 
  DebugScope DS(*TopLevelSGF, CleanupLocation(td));

  for (auto &ESD : td->getBody()->getElements()) {
    if (!TopLevelSGF->B.hasValidInsertionPoint()) {
      if (auto *S = ESD.dyn_cast<Stmt*>()) {
        if (S->isImplicit())
          continue;
      } else if (auto *E = ESD.dyn_cast<Expr*>()) {
        if (E->isImplicit())
          continue;
      }

      diagnose(ESD.getStartLoc(), diag::unreachable_code);
      // There's no point in trying to emit anything else.
      return;
    }

    if (auto *S = ESD.dyn_cast<Stmt*>()) {
      TopLevelSGF->emitStmt(S);
    } else if (auto *E = ESD.dyn_cast<Expr*>()) {
      TopLevelSGF->emitIgnoredExpr(E);
    } else {
      TopLevelSGF->visit(ESD.get<Decl*>());
    }
  }
}

namespace {

/// An RAII class to scope source file codegen.
class SourceFileScope {
  SILGenModule &sgm;
  SourceFile *sf;
  Optional<Scope> scope;
public:
  SourceFileScope(SILGenModule &sgm, SourceFile *sf) : sgm(sgm), sf(sf) {
    // If this is the script-mode file for the module, create a toplevel.
    if (sf->isScriptMode()) {
      assert(!sgm.TopLevelSGF && "already emitted toplevel?!");
      assert(!sgm.M.lookUpFunction(SWIFT_ENTRY_POINT_FUNCTION)
             && "already emitted toplevel?!");

      RegularLocation TopLevelLoc = RegularLocation::getModuleLocation();
      SILFunction *toplevel = sgm.emitTopLevelFunction(TopLevelLoc);

      // Assign a debug scope pointing into the void to the top level function.
      toplevel->setDebugScope(new (sgm.M) SILDebugScope(TopLevelLoc, toplevel));

      sgm.TopLevelSGF = new SILGenFunction(sgm, *toplevel, sf);
      sgm.TopLevelSGF->MagicFunctionName = sgm.SwiftModule->getName();
      auto moduleCleanupLoc = CleanupLocation::getModuleCleanupLocation();
      sgm.TopLevelSGF->prepareEpilog(Type(), true, moduleCleanupLoc);

      // Create the argc and argv arguments.
      auto prologueLoc = RegularLocation::getModuleLocation();
      prologueLoc.markAsPrologue();
      auto entry = sgm.TopLevelSGF->B.getInsertionBB();
      auto paramTypeIter =
          sgm.TopLevelSGF->F.getConventions().getParameterSILTypes().begin();
      entry->createFunctionArgument(*paramTypeIter);
      entry->createFunctionArgument(*std::next(paramTypeIter));

      scope.emplace(sgm.TopLevelSGF->Cleanups, moduleCleanupLoc);
    }
  }

  ~SourceFileScope() {
    if (sgm.TopLevelSGF) {
      scope.reset();

      // Unregister the top-level function emitter.
      auto &SGF = *sgm.TopLevelSGF;
      sgm.TopLevelSGF = nullptr;

      // Write out the epilog.
      auto moduleLoc = RegularLocation::getModuleLocation();
      moduleLoc.markAutoGenerated();
      auto returnInfo = SGF.emitEpilogBB(moduleLoc);
      auto returnLoc = returnInfo.second;
      returnLoc.markAutoGenerated();

      SILType returnType = SGF.F.getConventions().getSingleSILResultType();
      auto emitTopLevelReturnValue = [&](unsigned value) -> SILValue {
        // Create an integer literal for the value.
        auto litType = SILType::getBuiltinIntegerType(32, sgm.getASTContext());
        SILValue retValue =
          SGF.B.createIntegerLiteral(moduleLoc, litType, value);

        // Wrap that in a struct if necessary.
        if (litType != returnType) {
          retValue = SGF.B.createStruct(moduleLoc, returnType, retValue);
        }
        return retValue;
      };

      // Fallthrough should signal a normal exit by returning 0.
      SILValue returnValue;
      if (SGF.B.hasValidInsertionPoint())
        returnValue = emitTopLevelReturnValue(0);

      // Handle the implicit rethrow block.
      auto rethrowBB = SGF.ThrowDest.getBlock();
      SGF.ThrowDest = JumpDest::invalid();

      // If the rethrow block wasn't actually used, just remove it.
      if (rethrowBB->pred_empty()) {
        SGF.eraseBasicBlock(rethrowBB);

      // Otherwise, we need to produce a unified return block.
      } else {
        auto returnBB = SGF.createBasicBlock();
        if (SGF.B.hasValidInsertionPoint())
          SGF.B.createBranch(returnLoc, returnBB, returnValue);
        returnValue =
            returnBB->createPhiArgument(returnType, ValueOwnershipKind::Owned);
        SGF.B.emitBlock(returnBB);

        // Emit the rethrow block.
        SILGenSavedInsertionPoint savedIP(SGF, rethrowBB,
                                          FunctionSection::Postmatter);

        // Log the error.
        SILValue error = rethrowBB->getArgument(0);
        SGF.B.createBuiltin(moduleLoc,
                            sgm.getASTContext().getIdentifier("errorInMain"),
                            sgm.Types.getEmptyTupleType(), {}, {error});

        // Then end the lifetime of the error.
        //
        // We do this to appease the ownership verifier. We do not care about
        // actually destroying the value since we are going to immediately exit,
        // so this saves us a slight bit of code-size since end_lifetime is
        // stripped out after ownership is removed.
        SGF.B.createEndLifetime(moduleLoc, error);

        // Signal an abnormal exit by returning 1.
        SGF.Cleanups.emitCleanupsForReturn(CleanupLocation::get(moduleLoc),
                                           IsForUnwind);
        SGF.B.createBranch(returnLoc, returnBB, emitTopLevelReturnValue(1));
      }

      // Return.
      if (SGF.B.hasValidInsertionPoint())
        SGF.B.createReturn(returnLoc, returnValue);

      // Okay, we're done emitting the top-level function; destroy the
      // emitter and verify the result.
      SILFunction *toplevel = &SGF.getFunction();
      delete &SGF;

      LLVM_DEBUG(llvm::dbgs() << "lowered toplevel sil:\n";
                 toplevel->print(llvm::dbgs()));
      toplevel->verify();
      sgm.emitLazyConformancesForFunction(toplevel);
    }

    // If the source file contains an artificial main, emit the implicit
    // toplevel code.
    if (auto mainClass = sf->getMainClass()) {
      assert(!sgm.M.lookUpFunction(SWIFT_ENTRY_POINT_FUNCTION)
             && "already emitted toplevel before main class?!");

      RegularLocation TopLevelLoc = RegularLocation::getModuleLocation();
      SILFunction *toplevel = sgm.emitTopLevelFunction(TopLevelLoc);

      // Assign a debug scope pointing into the void to the top level function.
      toplevel->setDebugScope(new (sgm.M) SILDebugScope(TopLevelLoc, toplevel));

      // Create the argc and argv arguments.
      SILGenFunction SGF(sgm, *toplevel, sf);
      auto entry = SGF.B.getInsertionBB();
      auto paramTypeIter =
          SGF.F.getConventions().getParameterSILTypes().begin();
      entry->createFunctionArgument(*paramTypeIter);
      entry->createFunctionArgument(*std::next(paramTypeIter));
      SGF.emitArtificialTopLevel(mainClass);
    }
  }
};

} // end anonymous namespace

void SILGenModule::emitSourceFile(SourceFile *sf) {
  SourceFileScope scope(*this, sf);
  FrontendStatsTracer StatsTracer(getASTContext().Stats, "SILgen-file", sf);
  for (Decl *D : sf->Decls) {
    FrontendStatsTracer StatsTracer(getASTContext().Stats, "SILgen-decl", D);
    visit(D);
  }

  for (TypeDecl *TD : sf->LocalTypeDecls) {
    FrontendStatsTracer StatsTracer(getASTContext().Stats, "SILgen-tydecl", TD);
    // FIXME: Delayed parsing would prevent these types from being added to the
    //        module in the first place.
    if (TD->getDeclContext()->getInnermostSkippedFunctionContext())
      continue;
    visit(TD);
  }
}

//===----------------------------------------------------------------------===//
// SILModule::constructSIL method implementation
//===----------------------------------------------------------------------===//

std::unique_ptr<SILModule>
SILModule::constructSIL(ModuleDecl *mod, TypeConverter &tc,
                        SILOptions &options, FileUnit *SF) {
  FrontendStatsTracer tracer(mod->getASTContext().Stats, "SILGen");
  const DeclContext *DC;
  if (SF) {
    DC = SF;
  } else {
    DC = mod;
  }

  std::unique_ptr<SILModule> M(
      new SILModule(mod, tc, options, DC, /*wholeModule*/ SF == nullptr));
  SILGenModule SGM(*M, mod);

  if (SF) {
    if (auto *file = dyn_cast<SourceFile>(SF)) {
      SGM.emitSourceFile(file);
    } else if (auto *file = dyn_cast<SerializedASTFile>(SF)) {
      if (file->isSIB())
        M->getSILLoader()->getAllForModule(mod->getName(), file);
    }
  } else {
    for (auto file : mod->getFiles()) {
      auto nextSF = dyn_cast<SourceFile>(file);
      if (!nextSF || nextSF->ASTStage != SourceFile::TypeChecked)
        continue;
      SGM.emitSourceFile(nextSF);
    }

    // Also make sure to process any intermediate files that may contain SIL
    bool hasSIB = std::any_of(mod->getFiles().begin(),
                              mod->getFiles().end(),
                              [](const FileUnit *File) -> bool {
      auto *SASTF = dyn_cast<SerializedASTFile>(File);
      return SASTF && SASTF->isSIB();
    });
    if (hasSIB)
      M->getSILLoader()->getAllForModule(mod->getName(), nullptr);
  }

  // Emit any delayed definitions that were forced.
  // Emitting these may in turn force more definitions, so we have to take care
  // to keep pumping the queues.
  while (!SGM.forcedFunctions.empty()
         || !SGM.pendingConformances.empty()) {
    while (!SGM.forcedFunctions.empty()) {
      auto &front = SGM.forcedFunctions.front();
      front.second.emitter(SGM.getFunction(front.first, ForDefinition));
      SGM.forcedFunctions.pop_front();
    }
    while (!SGM.pendingConformances.empty()) {
      SGM.getWitnessTable(SGM.pendingConformances.front());
      SGM.pendingConformances.pop_front();
    }
  }

  return M;
}

std::unique_ptr<SILModule>
swift::performSILGeneration(ModuleDecl *mod, Lowering::TypeConverter &tc,
                            SILOptions &options) {
  return SILModule::constructSIL(mod, tc, options, nullptr);
}

std::unique_ptr<SILModule>
swift::performSILGeneration(FileUnit &sf, Lowering::TypeConverter &tc,
                            SILOptions &options) {
  return SILModule::constructSIL(sf.getParentModule(), tc, options, &sf);
}
