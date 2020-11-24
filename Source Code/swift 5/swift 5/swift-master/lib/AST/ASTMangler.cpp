//===--- ASTMangler.cpp - Swift AST symbol mangling -----------------------===//
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
//  This file implements declaration name mangling in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/Ownership.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/Basic/Defer.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Strings.h"
#include "clang/Basic/CharInfo.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace swift::Mangle;

static StringRef getCodeForAccessorKind(AccessorKind kind) {
  switch (kind) {
  case AccessorKind::Get:
    return "g";
  case AccessorKind::Set:
    return "s";
  case AccessorKind::WillSet:
    return "w";
  case AccessorKind::DidSet:
    return "W";
  case AccessorKind::Read:
    return "r";
  case AccessorKind::Modify:
    return "M";
  case AccessorKind::Address:
    // 'l' is for location. 'A' was taken.
    return "lu";
  case AccessorKind::MutableAddress:
    return "au";
  }
  llvm_unreachable("bad accessor kind");
}

std::string ASTMangler::mangleClosureEntity(const AbstractClosureExpr *closure,
                                            SymbolKind SKind) {
  beginMangling();
  appendClosureEntity(closure);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleEntity(const ValueDecl *decl, bool isCurried,
                                     SymbolKind SKind) {
  beginMangling();
  appendEntity(decl);
  if (isCurried)
    appendOperator("Tc");
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleDestructorEntity(const DestructorDecl *decl,
                                               bool isDeallocating,
                                               SymbolKind SKind) {
  beginMangling();
  appendDestructorEntity(decl, isDeallocating);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleConstructorEntity(const ConstructorDecl *ctor,
                                                bool isAllocating,
                                                bool isCurried,
                                                SymbolKind SKind) {
  beginMangling();
  appendConstructorEntity(ctor, isAllocating);
  if (isCurried)
    appendOperator("Tc");
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleIVarInitDestroyEntity(const ClassDecl *decl,
                                                    bool isDestroyer,
                                                    SymbolKind SKind) {
  beginMangling();
  appendContext(decl);
  appendOperator(isDestroyer ? "fE" : "fe");
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleAccessorEntity(AccessorKind kind,
                                             const AbstractStorageDecl *decl,
                                             bool isStatic,
                                             SymbolKind SKind) {
  beginMangling();
  appendAccessorEntity(getCodeForAccessorKind(kind), decl, isStatic);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleGlobalGetterEntity(const ValueDecl *decl,
                                                 SymbolKind SKind) {
  assert(isa<VarDecl>(decl) && "Only variables can have global getters");
  beginMangling();
  appendEntity(decl, "vG", /*isStatic*/false);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleDefaultArgumentEntity(const DeclContext *func,
                                                    unsigned index,
                                                    SymbolKind SKind) {
  beginMangling();
  appendDefaultArgumentEntity(func, index);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleInitializerEntity(const VarDecl *var,
                                                SymbolKind SKind) {
  beginMangling();
  appendInitializerEntity(var);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleBackingInitializerEntity(const VarDecl *var,
                                                       SymbolKind SKind) {
  beginMangling();
  appendBackingInitializerEntity(var);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleNominalType(const NominalTypeDecl *decl) {
  beginMangling();
  appendAnyGenericType(decl);
  return finalize();
}

std::string ASTMangler::mangleVTableThunk(const FuncDecl *Base,
                                          const FuncDecl *Derived) {
  beginMangling();

  appendEntity(Derived);
  appendEntity(Base);
  appendOperator("TV");

  return finalize();
}

std::string ASTMangler::mangleConstructorVTableThunk(
                                               const ConstructorDecl *Base,
                                               const ConstructorDecl *Derived,
                                               bool isAllocating) {
  beginMangling();

  appendConstructorEntity(Derived, isAllocating);
  appendConstructorEntity(Base, isAllocating);
  appendOperator("TV");

  return finalize();
}

std::string ASTMangler::mangleWitnessTable(const RootProtocolConformance *C) {
  beginMangling();
  if (isa<NormalProtocolConformance>(C)) {
    appendProtocolConformance(C);
    appendOperator("WP");
  } else {
    appendProtocolName(cast<SelfProtocolConformance>(C)->getProtocol());
    appendOperator("WS");
  }
  return finalize();
}

std::string ASTMangler::mangleWitnessThunk(
                                     const ProtocolConformance *Conformance,
                                           const ValueDecl *Requirement) {
  beginMangling();
  // Concrete witness thunks get a special mangling.
  if (Conformance) {
    if (!isa<SelfProtocolConformance>(Conformance)) {
      appendProtocolConformance(Conformance);
    }
  }

  if (auto ctor = dyn_cast<ConstructorDecl>(Requirement)) {
    appendConstructorEntity(ctor, /*isAllocating=*/true);
  } else {
    assert(isa<FuncDecl>(Requirement) && "expected function");
    appendEntity(cast<FuncDecl>(Requirement));
  }

  if (Conformance) {
    appendOperator(isa<SelfProtocolConformance>(Conformance) ? "TS" : "TW");
  }
  return finalize();
}

std::string ASTMangler::mangleClosureWitnessThunk(
                                         const ProtocolConformance *Conformance,
                                         const AbstractClosureExpr *Closure) {
  beginMangling();
  appendProtocolConformance(Conformance);
  appendClosureEntity(Closure);
  appendOperator("TW");
  return finalize();
}

std::string ASTMangler::mangleGlobalVariableFull(const VarDecl *decl) {
  // As a special case, Clang functions and globals don't get mangled at all.
  // FIXME: When we can import C++, use Clang's mangler.
  if (auto clangDecl =
      dyn_cast_or_null<clang::DeclaratorDecl>(decl->getClangDecl())) {
    if (auto asmLabel = clangDecl->getAttr<clang::AsmLabelAttr>()) {
      Buffer << '\01' << asmLabel->getLabel();
    } else {
      Buffer << clangDecl->getName();
    }
    return finalize();
  }
  beginMangling();
  appendEntity(decl);
  return finalize();
}

std::string ASTMangler::mangleKeyPathGetterThunkHelper(
                                            const AbstractStorageDecl *property,
                                            GenericSignature signature,
                                            CanType baseType,
                                            SubstitutionMap subs,
                                            ResilienceExpansion expansion) {
  beginMangling();
  appendEntity(property);
  if (signature)
    appendGenericSignature(signature);
  appendType(baseType);
  if (isa<SubscriptDecl>(property)) {
    // Subscripts can be generic, and different key paths could capture the same
    // subscript at different generic arguments.
    for (auto sub : subs.getReplacementTypes()) {
      appendType(sub->mapTypeOutOfContext()->getCanonicalType());
    }
  }
  appendOperator("TK");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleKeyPathSetterThunkHelper(
                                          const AbstractStorageDecl *property,
                                          GenericSignature signature,
                                          CanType baseType,
                                          SubstitutionMap subs,
                                          ResilienceExpansion expansion) {
  beginMangling();
  appendEntity(property);
  if (signature)
    appendGenericSignature(signature);
  appendType(baseType);
  if (isa<SubscriptDecl>(property)) {
    // Subscripts can be generic, and different key paths could capture the same
    // subscript at different generic arguments.
    for (auto sub : subs.getReplacementTypes()) {
      appendType(sub->mapTypeOutOfContext()->getCanonicalType());
    }
  }
  appendOperator("Tk");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleKeyPathEqualsHelper(ArrayRef<CanType> indices,
                                                  GenericSignature signature,
                                                  ResilienceExpansion expansion) {
  beginMangling();
  for (auto &index : indices)
    appendType(index);
  if (signature)
    appendGenericSignature(signature);
  appendOperator("TH");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleKeyPathHashHelper(ArrayRef<CanType> indices,
                                                GenericSignature signature,
                                                ResilienceExpansion expansion) {
  beginMangling();
  for (auto &index : indices)
    appendType(index);
  if (signature)
    appendGenericSignature(signature);
  appendOperator("Th");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleGlobalInit(const VarDecl *decl, int counter,
                                         bool isInitFunc) {
  auto topLevelContext = decl->getDeclContext()->getModuleScopeContext();
  auto fileUnit = cast<FileUnit>(topLevelContext);
  Identifier discriminator = fileUnit->getDiscriminatorForPrivateValue(decl);
  assert(!discriminator.empty());
  assert(!isNonAscii(discriminator.str()) &&
         "discriminator contains non-ASCII characters");
  assert(!clang::isDigit(discriminator.str().front()) &&
         "not a valid identifier");

  Buffer << "globalinit_";
  appendIdentifier(discriminator.str());
  Buffer << (isInitFunc ? "_func" : "_token");
  Buffer << counter;
  return finalize();
}

std::string ASTMangler::mangleReabstractionThunkHelper(
                                            CanSILFunctionType ThunkType,
                                            Type FromType,
                                            Type ToType,
                                            Type SelfType,
                                            ModuleDecl *Module) {
  Mod = Module;
  assert(ThunkType->getSubstitutions().empty() && "not implemented");
  GenericSignature GenSig = ThunkType->getSubstGenericSignature();
  if (GenSig)
    CurGenericSignature = GenSig->getCanonicalSignature();

  beginMangling();
  appendType(FromType);
  appendType(ToType);
  if (SelfType)
    appendType(SelfType);

  if (GenSig)
    appendGenericSignature(GenSig);

  if (SelfType)
    appendOperator("Ty");
  else
    appendOperator("TR");

  return finalize();
}

std::string ASTMangler::mangleTypeForDebugger(Type Ty, const DeclContext *DC) {
  PrettyStackTraceType prettyStackTrace(Ty->getASTContext(),
                                        "mangling type for debugger", Ty);

  DWARFMangling = true;
  OptimizeProtocolNames = false;
  beginMangling();

  if (DC)
    bindGenericParameters(DC);

  appendType(Ty);
  appendOperator("D");
  return finalize();
}

std::string ASTMangler::mangleDeclType(const ValueDecl *decl) {
  DWARFMangling = true;
  beginMangling();
  
  appendDeclType(decl);
  appendOperator("D");
  return finalize();
}

#ifdef USE_NEW_MANGLING_FOR_OBJC_RUNTIME_NAMES
static bool isPrivate(const NominalTypeDecl *Nominal) {
  return Nominal->getFormalAccess() <= AccessLevel::FilePrivate;
}
#endif

std::string ASTMangler::mangleObjCRuntimeName(const NominalTypeDecl *Nominal) {
#ifdef USE_NEW_MANGLING_FOR_OBJC_RUNTIME_NAMES
  // Using the new mangling for ObjC runtime names (except for top-level
  // classes). This is currently disabled to support old archives.
  // TODO: re-enable this as we switch to the new mangling for ObjC names.
  DeclContext *Ctx = Nominal->getDeclContext();

  if (Ctx->isModuleScopeContext() && !isPrivate(Nominal)) {
    // Use the old mangling for non-private top-level classes and protocols.
    // This is what the ObjC runtime needs to demangle.
    // TODO: Use new mangling scheme as soon as the ObjC runtime
    // can demangle it.
    //
    // Don't use word-substitutions and punycode encoding.
    MaxNumWords = 0;
    UsePunycode = false;
    UseSubstitutions = false;
    Buffer << "_Tt";
    bool isProto = false;
    if (isa<ClassDecl>(Nominal)) {
      Buffer << 'C';
    } else {
      isProto = true;
      assert(isa<ProtocolDecl>(Nominal));
      Buffer << 'P';
    }
    appendModule(Ctx->getParentModule());
    appendIdentifier(Nominal->getName().str());
    if (isProto)
      Buffer << '_';
    return finalize();
  }
  // For all other cases, we can use the new mangling.
  beginMangling();
  appendAnyGenericType(Nominal);
  return finalize();
#else
  // Use the old mangling for ObjC runtime names.
  beginMangling();
  appendAnyGenericType(Nominal);
  std::string NewName = finalize();
  Demangle::Demangler Dem;
  Demangle::Node *Root = Dem.demangleSymbol(NewName);
  assert(Root->getKind() == Node::Kind::Global);
  Node *NomTy = Root->getFirstChild();
  if (NomTy->getKind() == Node::Kind::Protocol) {
    // Protocol types are mangled as protocol lists.
    Node *PTy = Dem.createNode(Node::Kind::Type);
    PTy->addChild(NomTy, Dem);
    Node *TList = Dem.createNode(Node::Kind::TypeList);
    TList->addChild(PTy, Dem);
    NomTy = Dem.createNode(Node::Kind::ProtocolList);
    NomTy->addChild(TList, Dem);
  }
  // Add a TypeMangling node at the top
  Node *Ty = Dem.createNode(Node::Kind::Type);
  Ty->addChild(NomTy, Dem);
  Node *TyMangling = Dem.createNode(Node::Kind::TypeMangling);
  TyMangling->addChild(Ty, Dem);
  Node *NewGlobal = Dem.createNode(Node::Kind::Global);
  NewGlobal->addChild(TyMangling, Dem);
  std::string OldName = mangleNodeOld(NewGlobal);
  return OldName;
#endif
}

std::string ASTMangler::mangleTypeAsContextUSR(const NominalTypeDecl *type) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  appendContext(type);
  return finalize();
}

std::string ASTMangler::mangleTypeAsUSR(Type Ty) {
  DWARFMangling = true;
  beginMangling();
  
  if (auto *fnType = Ty->getAs<AnyFunctionType>()) {
    appendFunction(fnType, false);
  } else {
    appendType(Ty);
  }

  appendOperator("D");
  return finalize();
}

std::string ASTMangler::mangleDeclAsUSR(const ValueDecl *Decl,
                                        StringRef USRPrefix) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  Buffer << USRPrefix;
  bindGenericParameters(Decl->getDeclContext());

  if (auto Ctor = dyn_cast<ConstructorDecl>(Decl)) {
    appendConstructorEntity(Ctor, /*isAllocating=*/false);
  } else if (auto Dtor = dyn_cast<DestructorDecl>(Decl)) {
    appendDestructorEntity(Dtor, /*isDeallocating=*/false);
  } else if (auto GTD = dyn_cast<GenericTypeDecl>(Decl)) {
    appendAnyGenericType(GTD);
  } else if (isa<AssociatedTypeDecl>(Decl)) {
    appendContextOf(Decl);
    appendDeclName(Decl);
    appendOperator("Qa");
  } else {
    appendEntity(Decl);
  }

  // We have a custom prefix, so finalize() won't verify for us. If we're not
  // in invalid code (coming from an IDE caller) verify manually.
  if (!Decl->isInvalid())
    verify(Storage.str().drop_front(USRPrefix.size()));
  return finalize();
}

std::string ASTMangler::mangleAccessorEntityAsUSR(AccessorKind kind,
                                                  const AbstractStorageDecl *decl,
                                                  StringRef USRPrefix) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  Buffer << USRPrefix;
  appendAccessorEntity(getCodeForAccessorKind(kind), decl, /*isStatic*/ false);
  // We have a custom prefix, so finalize() won't verify for us. Do it manually.
  verify(Storage.str().drop_front(USRPrefix.size()));
  return finalize();
}

std::string ASTMangler::mangleLocalTypeDecl(const TypeDecl *type) {
  beginManglingWithoutPrefix();
  AllowNamelessEntities = true;
  OptimizeProtocolNames = false;

  if (auto GTD = dyn_cast<GenericTypeDecl>(type)) {
    appendAnyGenericType(GTD);
  } else {
    assert(isa<AssociatedTypeDecl>(type));
    appendContextOf(type);
    appendDeclName(type);
    appendOperator("Qa");
  }

  return finalize();
}

void ASTMangler::appendSymbolKind(SymbolKind SKind) {
  switch (SKind) {
    case SymbolKind::Default: return;
    case SymbolKind::DynamicThunk: return appendOperator("TD");
    case SymbolKind::SwiftAsObjCThunk: return appendOperator("To");
    case SymbolKind::ObjCAsSwiftThunk: return appendOperator("TO");
    case SymbolKind::DirectMethodReferenceThunk: return appendOperator("Td");
  }
}

static bool getUnnamedParamIndex(const ParameterList *ParamList,
                                 const ParamDecl *D,
                                 unsigned &UnnamedIndex) {
  for (auto Param : *ParamList) {
    if (!Param->hasName()) {
      if (Param == D)
        return true;
      ++UnnamedIndex;
    }
  }
  return false;
}

static unsigned getUnnamedParamIndex(const ParamDecl *D) {
  if (auto SD = dyn_cast<SubscriptDecl>(D->getDeclContext())) {
    unsigned UnnamedIndex = 0;
    auto *ParamList = SD->getIndices();
    if (getUnnamedParamIndex(ParamList, D, UnnamedIndex))
      return UnnamedIndex;
    llvm_unreachable("param not found");
  }

  ParameterList *ParamList;

  if (auto AFD = dyn_cast<AbstractFunctionDecl>(D->getDeclContext())) {
    ParamList = AFD->getParameters();
  } else if (auto EED = dyn_cast<EnumElementDecl>(D->getDeclContext())) {
    ParamList = EED->getParameterList();
  } else {
    auto ACE = cast<AbstractClosureExpr>(D->getDeclContext());
    ParamList = ACE->getParameters();
  }

  unsigned UnnamedIndex = 0;
  if (getUnnamedParamIndex(ParamList, D, UnnamedIndex))
    return UnnamedIndex;

  llvm_unreachable("param not found");
}

static StringRef getPrivateDiscriminatorIfNecessary(const ValueDecl *decl) {
  if (!decl->isOutermostPrivateOrFilePrivateScope())
    return StringRef();

  // Mangle non-local private declarations with a textual discriminator
  // based on their enclosing file.
  auto topLevelContext = decl->getDeclContext()->getModuleScopeContext();
  auto fileUnit = cast<FileUnit>(topLevelContext);

  Identifier discriminator =
      fileUnit->getDiscriminatorForPrivateValue(decl);
  assert(!discriminator.empty());
  assert(!isNonAscii(discriminator.str()) &&
         "discriminator contains non-ASCII characters");
  (void)&isNonAscii;
  assert(!clang::isDigit(discriminator.str().front()) &&
         "not a valid identifier");
  return discriminator.str();
}

/// If the declaration is an @objc protocol defined in Swift and the
/// Objective-C name has been overrridden from the default, return the
/// specified name.
///
/// \param useObjCProtocolNames When false, always returns \c None.
static Optional<std::string> getOverriddenSwiftProtocolObjCName(
                                                  const ValueDecl *decl,
                                                  bool useObjCProtocolNames) {
  if (!useObjCProtocolNames)
    return None;

  auto proto = dyn_cast<ProtocolDecl>(decl);
  if (!proto) return None;

  if (!proto->isObjC()) return None;

  // If there is an 'objc' attribute with a name, use that name.
  if (auto objc = proto->getAttrs().getAttribute<ObjCAttr>()) {
    if (auto name = objc->getName()) {
      llvm::SmallString<4> buffer;
      return std::string(name->getString(buffer));
    }
  }

  return None;
}

void ASTMangler::appendDeclName(const ValueDecl *decl) {
  DeclBaseName name = decl->getBaseName();
  assert(!name.isSpecial() && "Cannot print special names");

  auto *synthesizedTypeAttr =
      decl->getAttrs().getAttribute<ClangImporterSynthesizedTypeAttr>();

  if (synthesizedTypeAttr) {
    assert(!isDigit(synthesizedTypeAttr->originalTypeName[0]) &&
           "synthesized type's original name must be a valid Swift identifier");
    appendIdentifier(synthesizedTypeAttr->originalTypeName);
  } else if (name.isOperator()) {
    appendIdentifier(translateOperator(name.getIdentifier().str()));
    switch (decl->getAttrs().getUnaryOperatorKind()) {
      case UnaryOperatorKind::Prefix:
        appendOperator("op");
        break;
      case UnaryOperatorKind::Postfix:
        appendOperator("oP");
        break;
      case UnaryOperatorKind::None:
        appendOperator("oi");
        break;
    }
  } else if (auto objCName =
               getOverriddenSwiftProtocolObjCName(decl, UseObjCProtocolNames)) {
    // @objc Swift protocols should be mangled as Objective-C protocols,
    // so append the Objective-C runtime name.
    appendIdentifier(*objCName);
  } else if (!name.empty()) {
    appendIdentifier(name.getIdentifier().str());
  } else {
    assert(AllowNamelessEntities && "attempt to mangle unnamed decl");
    // Fall back to an unlikely name, so that we still generate a valid
    // mangled name.
    appendIdentifier("_");
  }

  if (decl->getDeclContext()->isLocalContext()) {
    if (auto *paramDecl = dyn_cast<ParamDecl>(decl)) {
      if (!decl->hasName()) {
        // Mangle unnamed params with their ordering.
        return appendOperator("L", Index(getUnnamedParamIndex(paramDecl)));
      }
    }
    // Mangle local declarations with a numeric discriminator.
    return appendOperator("L", Index(decl->getLocalDiscriminator()));
  }

  if (synthesizedTypeAttr) {
    StringRef relatedEntityKind = synthesizedTypeAttr->getManglingName();
    assert(relatedEntityKind.size() == 1 &&
           "'L' operator only supports a single letter payload");
    assert(((relatedEntityKind[0] >= 'a' && relatedEntityKind[0] <= 'j') ||
            (relatedEntityKind[0] >= 'A' && relatedEntityKind[0] <= 'J')) &&
           "Only [a-jA-J] are reserved for related entity kinds");
    return appendOperatorParam("L", relatedEntityKind);
  }

  StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(decl);
  if (!privateDiscriminator.empty()) {
    appendIdentifier(privateDiscriminator.str());
    return appendOperator("LL");
  }
}

static const char *getMetatypeRepresentationOp(MetatypeRepresentation Rep) {
  switch (Rep) {
    case MetatypeRepresentation::Thin:
      return "t";
    case MetatypeRepresentation::Thick:
      return "T";
    case MetatypeRepresentation::ObjC:
      return "o";
  }

  llvm_unreachable("Unhandled MetatypeRepresentation in switch.");
}

static bool isStdlibType(const TypeDecl *decl) {
  DeclContext *dc = decl->getDeclContext();
  return dc->isModuleScopeContext() && dc->getParentModule()->isStdlibModule();
}

/// Whether to mangle the given type as generic.
static bool shouldMangleAsGeneric(Type type) {
  if (!type)
    return false;

  if (auto typeAlias = dyn_cast<TypeAliasType>(type.getPointer()))
    return !typeAlias->getSubstitutionMap().empty();

  return type->isSpecialized();
}

void ASTMangler::appendOpaqueDeclName(const OpaqueTypeDecl *opaqueDecl) {
  if (canSymbolicReference(opaqueDecl)) {
    appendSymbolicReference(opaqueDecl);
  } else if (auto namingDecl = opaqueDecl->getNamingDecl()) {
    llvm::SaveAndRestore<CanGenericSignature> savedSignature(
        CurGenericSignature);
    appendEntity(namingDecl);
    appendOperator("QO");
  } else {
    llvm_unreachable("todo: independent opaque type decls");
  }
}

/// Mangle a type into the buffer.
///
void ASTMangler::appendType(Type type, const ValueDecl *forDecl) {
  assert((DWARFMangling || type->isCanonical()) &&
         "expecting canonical types when not mangling for the debugger");
  TypeBase *tybase = type.getPointer();
  switch (type->getKind()) {
    case TypeKind::TypeVariable:
      llvm_unreachable("mangling type variable");

    case TypeKind::Module:
      llvm_unreachable("Cannot mangle module type yet");

    case TypeKind::Error:
    case TypeKind::Unresolved:
      appendOperator("Xe");
      return;

      // We don't care about these types being a bit verbose because we
      // don't expect them to come up that often in API names.
    case TypeKind::BuiltinFloat:
      switch (cast<BuiltinFloatType>(tybase)->getFPKind()) {
        case BuiltinFloatType::IEEE16: appendOperator("Bf16_"); return;
        case BuiltinFloatType::IEEE32: appendOperator("Bf32_"); return;
        case BuiltinFloatType::IEEE64: appendOperator("Bf64_"); return;
        case BuiltinFloatType::IEEE80: appendOperator("Bf80_"); return;
        case BuiltinFloatType::IEEE128: appendOperator("Bf128_"); return;
        case BuiltinFloatType::PPC128: llvm_unreachable("ppc128 not supported");
      }
      llvm_unreachable("bad floating-point kind");
    case TypeKind::BuiltinInteger: {
      auto width = cast<BuiltinIntegerType>(tybase)->getWidth();
      if (width.isFixedWidth())
        appendOperator("Bi", Index(width.getFixedWidth() + 1));
      else if (width.isPointerWidth())
        appendOperator("Bw");
      else
        llvm_unreachable("impossible width value");
      return;
    }
    case TypeKind::BuiltinIntegerLiteral:
      return appendOperator("BI");
    case TypeKind::BuiltinRawPointer:
      return appendOperator("Bp");
    case TypeKind::BuiltinNativeObject:
      return appendOperator("Bo");
    case TypeKind::BuiltinBridgeObject:
      return appendOperator("Bb");
    case TypeKind::BuiltinUnsafeValueBuffer:
      return appendOperator("BB");
    case TypeKind::SILToken:
      return appendOperator("Bt");
    case TypeKind::BuiltinVector:
      appendType(cast<BuiltinVectorType>(tybase)->getElementType(),
                 forDecl);
      // The mangling calls for using the actual element count, which we have
      // to adjust by 1 in order to mangle it as an index.
      return appendOperator("Bv",
                  Index(cast<BuiltinVectorType>(tybase)->getNumElements() + 1));
    case TypeKind::TypeAlias: {
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      auto aliasTy = cast<TypeAliasType>(tybase);

      // It's not possible to mangle the context of the builtin module.
      // For the DWARF output we want to mangle the type alias + context,
      // unless the type alias references a builtin type.
      auto underlyingType = aliasTy->getSinglyDesugaredType();
      TypeAliasDecl *decl = aliasTy->getDecl();
      if (decl->getModuleContext() == decl->getASTContext().TheBuiltinModule) {
        return appendType(underlyingType, forDecl);
      }

      if (decl->getDeclaredInterfaceType()
            .subst(aliasTy->getSubstitutionMap()).getPointer()
            != aliasTy) {
        return appendType(underlyingType, forDecl);
      }

      if (aliasTy->getSubstitutionMap()) {
        // Try to mangle the entire name as a substitution.
        if (tryMangleTypeSubstitution(tybase))
          return;

        appendAnyGenericType(decl);
        bool isFirstArgList = true;
        appendBoundGenericArgs(type, isFirstArgList);
        appendRetroactiveConformances(type);
        appendOperator("G");
        addTypeSubstitution(type);
        return;
      }

      return appendAnyGenericType(decl);
    }

    case TypeKind::Paren:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<ParenType>(tybase)->getUnderlyingType());
      appendOperator("XSp");
      return;

    case TypeKind::ArraySlice:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<ArraySliceType>(tybase)->getBaseType());
      appendOperator("XSa");
      return;

    case TypeKind::Optional:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<OptionalType>(tybase)->getBaseType());
      appendOperator("XSq");
      return;

    case TypeKind::Dictionary:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<DictionaryType>(tybase)->getKeyType());
      appendType(cast<DictionaryType>(tybase)->getValueType());
      appendOperator("XSD");
      return;

    case TypeKind::ExistentialMetatype: {
      ExistentialMetatypeType *EMT = cast<ExistentialMetatypeType>(tybase);
      appendType(EMT->getInstanceType(), forDecl);
      if (EMT->hasRepresentation()) {
        appendOperator("Xm",
                       getMetatypeRepresentationOp(EMT->getRepresentation()));
      } else {
        appendOperator("Xp");
      }
      return;
    }
    case TypeKind::Metatype: {
      MetatypeType *MT = cast<MetatypeType>(tybase);
      appendType(MT->getInstanceType(), forDecl);
      if (MT->hasRepresentation()) {
        appendOperator("XM",
                       getMetatypeRepresentationOp(MT->getRepresentation()));
      } else {
        appendOperator("m");
      }
      return;
    }
    case TypeKind::LValue:
      llvm_unreachable("@lvalue types should not occur in function interfaces");

    case TypeKind::InOut:
      appendType(cast<InOutType>(tybase)->getObjectType(), forDecl);
      return appendOperator("z");

#define REF_STORAGE(Name, ...) \
    case TypeKind::Name##Storage: \
      appendType(cast<Name##StorageType>(tybase)->getReferentType(), forDecl); \
      return appendOperator(manglingOf(ReferenceOwnership::Name));
#include "swift/AST/ReferenceStorage.def"

    case TypeKind::Tuple:
      appendTypeList(type, forDecl);
      return appendOperator("t");

    case TypeKind::Protocol: {
      bool First = true;
      appendProtocolName(cast<ProtocolType>(tybase)->getDecl());
      appendListSeparator(First);
      return appendOperator("p");
    }

    case TypeKind::ProtocolComposition: {
      // We mangle ProtocolType and ProtocolCompositionType using the
      // same production:
      bool First = true;
      auto layout = type->getExistentialLayout();
      for (Type protoTy : layout.getProtocols()) {
        appendProtocolName(protoTy->castTo<ProtocolType>()->getDecl());
        appendListSeparator(First);
      }
      if (First)
        appendOperator("y");

      if (auto superclass = layout.explicitSuperclass) {
        appendType(superclass, forDecl);
        return appendOperator("Xc");
      } else if (layout.hasExplicitAnyObject) {
        return appendOperator("Xl");
      }
      return appendOperator("p");
    }

    case TypeKind::UnboundGeneric:
    case TypeKind::Class:
    case TypeKind::Enum:
    case TypeKind::Struct:
    case TypeKind::BoundGenericClass:
    case TypeKind::BoundGenericEnum:
    case TypeKind::BoundGenericStruct: {
      GenericTypeDecl *Decl;
      if (auto typeAlias = dyn_cast<TypeAliasType>(type.getPointer()))
        Decl = typeAlias->getDecl();
      else
        Decl = type->getAnyGeneric();
      if (shouldMangleAsGeneric(type)) {
        // Try to mangle the entire name as a substitution.
        if (tryMangleTypeSubstitution(tybase))
          return;

        if (isStdlibType(Decl) && Decl->getName().str() == "Optional") {
          auto GenArgs = type->castTo<BoundGenericType>()->getGenericArgs();
          assert(GenArgs.size() == 1);
          appendType(GenArgs[0], forDecl);
          appendOperator("Sg");
        } else {
          appendAnyGenericType(Decl);
          bool isFirstArgList = true;
          appendBoundGenericArgs(type, isFirstArgList);
          appendRetroactiveConformances(type);
          appendOperator("G");
        }
        addTypeSubstitution(type);
        return;
      }
      appendAnyGenericType(type->getAnyGeneric());
      return;
    }

    case TypeKind::SILFunction:
      return appendImplFunctionType(cast<SILFunctionType>(tybase));

      // type ::= archetype
    case TypeKind::PrimaryArchetype:
    case TypeKind::OpenedArchetype:
      llvm_unreachable("Cannot mangle free-standing archetypes");

    case TypeKind::OpaqueTypeArchetype: {
      // If this is the opaque return type of the declaration currently being
      // mangled, use a short mangling to represent it.
      auto opaqueType = cast<OpaqueTypeArchetypeType>(tybase);
      auto opaqueDecl = opaqueType->getDecl();
      if (opaqueDecl->getNamingDecl() == forDecl) {
        assert(opaqueType->getSubstitutions().isIdentity());
        return appendOperator("Qr");
      }
      
      // Otherwise, try to substitute it.
      if (tryMangleTypeSubstitution(type))
        return;
      
      // Use the fully elaborated explicit mangling.
      appendOpaqueDeclName(opaqueDecl);
      bool isFirstArgList = true;
      appendBoundGenericArgs(opaqueDecl,
                             opaqueType->getSubstitutions(),
                             isFirstArgList);
      appendRetroactiveConformances(opaqueType->getSubstitutions(),
                                    opaqueDecl->getParentModule());
      
      // TODO: If we support multiple opaque types in a return, put the
      // ordinal for this archetype here.
      appendOperator("Qo", Index(0));

      addTypeSubstitution(type);
      return;
    }
      
    case TypeKind::NestedArchetype: {
      auto nestedType = cast<NestedArchetypeType>(tybase);
      
      // Mangle associated types of opaque archetypes like dependent member
      // types, so that they can be accurately demangled at runtime.
      if (auto opaque =
            dyn_cast<OpaqueTypeArchetypeType>(nestedType->getRoot())) {
        if (tryMangleTypeSubstitution(nestedType))
          return;
        
        appendType(opaque);
        bool isAssocTypeAtDepth = false;
        appendAssocType(nestedType->getInterfaceType(), isAssocTypeAtDepth);
        appendOperator(isAssocTypeAtDepth ? "QX" : "Qx");
        addTypeSubstitution(nestedType);
        return;
      }
      
      appendType(nestedType->getParent());
      appendIdentifier(nestedType->getName().str());
      appendOperator("Qa");
      return;
    }
      
    case TypeKind::DynamicSelf: {
      auto dynamicSelf = cast<DynamicSelfType>(tybase);
      if (dynamicSelf->getSelfType()->getAnyNominal()) {
        appendType(dynamicSelf->getSelfType(), forDecl);
        return appendOperator("XD");
      }
      return appendType(dynamicSelf->getSelfType(), forDecl);
    }

    case TypeKind::GenericFunction: {
      auto genFunc = cast<GenericFunctionType>(tybase);
      appendFunctionType(genFunc, /*autoclosure*/ false, forDecl);
      appendGenericSignature(genFunc->getGenericSignature());
      appendOperator("u");
      return;
    }

    case TypeKind::GenericTypeParam: {
      auto paramTy = cast<GenericTypeParamType>(tybase);
      // A special mangling for the very first generic parameter. This shows up
      // frequently because it corresponds to 'Self' in protocol requirement
      // generic signatures.
      if (paramTy->getDepth() == 0 && paramTy->getIndex() == 0)
        return appendOperator("x");

      return appendOpWithGenericParamIndex("q", paramTy);
    }

    case TypeKind::DependentMember: {
      auto *DepTy = cast<DependentMemberType>(tybase);
      if (tryMangleTypeSubstitution(DepTy))
        return;

      bool isAssocTypeAtDepth = false;
      if (GenericTypeParamType *gpBase = appendAssocType(DepTy,
                                                         isAssocTypeAtDepth)) {
        if (gpBase->getDepth() == 0 && gpBase->getIndex() == 0) {
          appendOperator(isAssocTypeAtDepth ? "QZ" : "Qz");
        } else {
          appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "QY" : "Qy",
                                        gpBase);
        }
      } else {
        // Dependent members of non-generic-param types are not canonical, but
        // we may still want to mangle them for debugging or indexing purposes.
        appendType(DepTy->getBase(), forDecl);
        appendIdentifier(DepTy->getName().str());
        appendOperator("Qa");
      }
      addTypeSubstitution(DepTy);
      return;
    }
      
    case TypeKind::Function:
      appendFunctionType(cast<FunctionType>(tybase), /*autoclosure*/ false,
                         forDecl);
      return;
      
    case TypeKind::SILBox: {
      auto box = cast<SILBoxType>(tybase);
      auto layout = box->getLayout();
      bool firstField = true;
      for (auto &field : layout->getFields()) {
        appendType(field.getLoweredType(), forDecl);
        if (field.isMutable()) {
          // Use the `inout` mangling to represent a mutable field.
          appendOperator("z");
        }
        appendListSeparator(firstField);
      }
      if (firstField)
        appendOperator("y");

      if (auto sig = layout->getGenericSignature()) {
        bool firstType = true;
        for (Type type : box->getSubstitutions().getReplacementTypes()) {
          appendType(type, forDecl);
          appendListSeparator(firstType);
        }
        if (firstType)
          appendOperator("y");

        appendGenericSignature(sig);
        appendOperator("XX");
      } else {
        appendOperator("Xx");
      }

      return;
    }

    case TypeKind::SILBlockStorage:
      llvm_unreachable("should never be mangled");
  }
  llvm_unreachable("bad type kind");
}

GenericTypeParamType *ASTMangler::appendAssocType(DependentMemberType *DepTy,
                                                  bool &isAssocTypeAtDepth) {
  auto base = DepTy->getBase()->getCanonicalType();
  // 't_0_0.Member'
  if (auto gpBase = dyn_cast<GenericTypeParamType>(base)) {
    appendAssociatedTypeName(DepTy);
    isAssocTypeAtDepth = false;
    return gpBase;
  }

  // 't_0_0.Member.Member...'
  SmallVector<DependentMemberType*, 2> path;
  path.push_back(DepTy);
  while (auto dmBase = dyn_cast<DependentMemberType>(base)) {
    path.push_back(dmBase);
    base = dmBase.getBase();
  }
  if (auto gpRoot = dyn_cast<GenericTypeParamType>(base)) {
    bool first = true;
    for (auto *member : llvm::reverse(path)) {
      appendAssociatedTypeName(member);
      appendListSeparator(first);
    }
    isAssocTypeAtDepth = true;
    return gpRoot;
  }
  return nullptr;
}

void ASTMangler::appendOpWithGenericParamIndex(StringRef Op,
                                          const GenericTypeParamType *paramTy) {
  llvm::SmallVector<char, 8> OpBuf(Op.begin(), Op.end());
  if (paramTy->getDepth() > 0) {
    OpBuf.push_back('d');
    return appendOperator(StringRef(OpBuf.data(), OpBuf.size()),
                          Index(paramTy->getDepth() - 1),
                          Index(paramTy->getIndex()));
  }
  if (paramTy->getIndex() == 0) {
    OpBuf.push_back('z');
    return appendOperator(StringRef(OpBuf.data(), OpBuf.size()));
  }
  appendOperator(Op, Index(paramTy->getIndex() - 1));
}


/// Bind the generic parameters from the given signature.
void ASTMangler::bindGenericParameters(CanGenericSignature sig) {
  if (sig)
    CurGenericSignature = sig;
}

/// Bind the generic parameters from the given context and its parents.
void ASTMangler::bindGenericParameters(const DeclContext *DC) {
  if (auto sig = DC->getGenericSignatureOfContext())
    bindGenericParameters(sig->getCanonicalSignature());
}

unsigned ASTMangler::appendBoundGenericArgs(DeclContext *dc,
                                            SubstitutionMap subs,
                                            bool &isFirstArgList) {
  auto decl = dc->getInnermostDeclarationDeclContext();
  if (!decl) return 0;

  // For a non-protocol extension declaration, use the nominal type declaration
  // instead.
  //
  // This is important when extending a nested type, because the generic
  // parameters will line up with the (semantic) nesting of the nominal type.
  if (auto ext = dyn_cast<ExtensionDecl>(decl))
    decl = ext->getSelfNominalTypeDecl();

  // Handle the generic arguments of the parent.
  unsigned currentGenericParamIdx =
    appendBoundGenericArgs(decl->getDeclContext(), subs, isFirstArgList);

  // If this is potentially a generic context, emit a generic argument list.
  if (auto genericContext = decl->getAsGenericContext()) {
    if (isFirstArgList) {
      appendOperator("y");
      isFirstArgList = false;
    } else {
      appendOperator("_");
    }

    // If we are generic at this level, emit all of the replacements at
    // this level.
    if (genericContext->isGeneric()) {
      auto genericParams = subs.getGenericSignature()->getGenericParams();
      unsigned depth = genericParams[currentGenericParamIdx]->getDepth();
      auto replacements = subs.getReplacementTypes();
      for (unsigned lastGenericParamIdx = genericParams.size();
           (currentGenericParamIdx != lastGenericParamIdx &&
            genericParams[currentGenericParamIdx]->getDepth() == depth);
           ++currentGenericParamIdx) {
        Type replacementType = replacements[currentGenericParamIdx];
        if (replacementType->hasArchetype())
          replacementType = replacementType->mapTypeOutOfContext();

        appendType(replacementType);
      }
    }
  }

  return currentGenericParamIdx;
}

void ASTMangler::appendBoundGenericArgs(Type type, bool &isFirstArgList) {
  TypeBase *typePtr = type.getPointer();
  ArrayRef<Type> genericArgs;
  if (auto *typeAlias = dyn_cast<TypeAliasType>(typePtr)) {
    appendBoundGenericArgs(typeAlias->getDecl(),
                           typeAlias->getSubstitutionMap(),
                           isFirstArgList);
    return;
  }

  if (auto *unboundType = dyn_cast<UnboundGenericType>(typePtr)) {
    if (Type parent = unboundType->getParent())
      appendBoundGenericArgs(parent->getDesugaredType(), isFirstArgList);
  } else if (auto *nominalType = dyn_cast<NominalType>(typePtr)) {
    if (Type parent = nominalType->getParent())
      appendBoundGenericArgs(parent->getDesugaredType(), isFirstArgList);
  } else {
    auto boundType = cast<BoundGenericType>(typePtr);
    genericArgs = boundType->getGenericArgs();
    if (Type parent = boundType->getParent()) {
      GenericTypeDecl *decl = boundType->getAnyGeneric();
      if (!getSpecialManglingContext(decl, UseObjCProtocolNames))
        appendBoundGenericArgs(parent->getDesugaredType(), isFirstArgList);
    }
  }
  if (isFirstArgList) {
    appendOperator("y");
    isFirstArgList = false;
  } else {
    appendOperator("_");
  }
  for (Type arg : genericArgs) {
    appendType(arg);
  }
}

static bool conformanceHasIdentity(const RootProtocolConformance *root) {
  auto conformance = dyn_cast<NormalProtocolConformance>(root);
  if (!conformance) {
    assert(isa<SelfProtocolConformance>(root));
    return true;
  }

  // Synthesized non-unique conformances all get collapsed together at run time.
  if (conformance->isSynthesizedNonUnique())
    return false;

  // Objective-C protocol conformances are checked by the ObjC runtime.
  if (conformance->getProtocol()->isObjC())
    return false;

  return true;
}

/// Determine whether the given protocol conformance is itself retroactive,
/// meaning that there might be multiple conflicting conformances of the
/// same type to the same protocol.
static bool isRetroactiveConformance(const RootProtocolConformance *root) {
  auto conformance = dyn_cast<NormalProtocolConformance>(root);
  if (!conformance) {
    assert(isa<SelfProtocolConformance>(root));
    return false; // self-conformances are never retroactive.
  }

  return conformance->isRetroactive();
}

/// Determine whether the given protocol conformance contains a retroactive
/// protocol conformance anywhere in it.
static bool containsRetroactiveConformance(
                                      const ProtocolConformance *conformance,
                                      ModuleDecl *module) {
  // If the root conformance is retroactive, it's retroactive.
  const RootProtocolConformance *rootConformance =
      conformance->getRootConformance();
  if (isRetroactiveConformance(rootConformance) &&
      conformanceHasIdentity(rootConformance))
    return true;

  // If the conformance is conditional and any of the substitutions used to
  // satisfy the conditions are retroactive, it's retroactive.
  auto subMap = conformance->getSubstitutions(module);
  for (auto requirement : rootConformance->getConditionalRequirements()) {
    if (requirement.getKind() != RequirementKind::Conformance)
      continue;
    ProtocolDecl *proto =
        requirement.getSecondType()->castTo<ProtocolType>()->getDecl();
    auto conformance = subMap.lookupConformance(
        requirement.getFirstType()->getCanonicalType(), proto);
    if (conformance.isInvalid()) {
      // This should only happen when mangling invalid ASTs, but that happens
      // for indexing purposes.
      continue;
    }
    if (conformance.isConcrete() &&
        containsRetroactiveConformance(conformance.getConcrete(), module)) {
      return true;
    }
  }

  return false;
}

void ASTMangler::appendRetroactiveConformances(SubstitutionMap subMap,
                                               ModuleDecl *fromModule) {
  if (subMap.empty()) return;

  unsigned numProtocolRequirements = 0;
  for (auto conformance : subMap.getConformances()) {
    SWIFT_DEFER {
      ++numProtocolRequirements;
    };

    // Ignore abstract conformances.
    if (!conformance.isConcrete())
      continue;

    // Skip non-retroactive conformances.
    if (!containsRetroactiveConformance(conformance.getConcrete(), fromModule))
      continue;

    appendConcreteProtocolConformance(conformance.getConcrete());
    appendOperator("g", Index(numProtocolRequirements));
  }
}

void ASTMangler::appendRetroactiveConformances(Type type) {
  // Dig out the substitution map to use.
  SubstitutionMap subMap;
  ModuleDecl *module;
  if (auto typeAlias = dyn_cast<TypeAliasType>(type.getPointer())) {
    module = Mod ? Mod : typeAlias->getDecl()->getModuleContext();
    subMap = typeAlias->getSubstitutionMap();
  } else {
    if (type->hasUnboundGenericType())
      return;

    auto nominal = type->getAnyNominal();
    if (!nominal) return;

    module = Mod ? Mod : nominal->getModuleContext();
    subMap = type->getContextSubstitutionMap(module, nominal);
  }

  appendRetroactiveConformances(subMap, module);
}

static char getParamConvention(ParameterConvention conv) {
  // @in and @out are mangled the same because they're put in
  // different places.
  switch (conv) {
    case ParameterConvention::Indirect_In: return 'i';
    case ParameterConvention::Indirect_In_Constant:
      return 'c';
    case ParameterConvention::Indirect_Inout: return 'l';
    case ParameterConvention::Indirect_InoutAliasable: return 'b';
    case ParameterConvention::Indirect_In_Guaranteed: return 'n';
    case ParameterConvention::Direct_Owned: return 'x';
    case ParameterConvention::Direct_Unowned: return 'y';
    case ParameterConvention::Direct_Guaranteed: return 'g';
  }
  llvm_unreachable("bad parameter convention");
};

static char getResultConvention(ResultConvention conv) {
  switch (conv) {
    case ResultConvention::Indirect: return 'r';
    case ResultConvention::Owned: return 'o';
    case ResultConvention::Unowned: return 'd';
    case ResultConvention::UnownedInnerPointer: return 'u';
    case ResultConvention::Autoreleased: return 'a';
  }
  llvm_unreachable("bad result convention");
};

void ASTMangler::appendImplFunctionType(SILFunctionType *fn) {

  llvm::SmallVector<char, 32> OpArgs;

  if (fn->isPolymorphic() && fn->isPseudogeneric())
    OpArgs.push_back('P');

  if (!fn->isNoEscape())
    OpArgs.push_back('e');

  // <impl-callee-convention>
  if (fn->getExtInfo().hasContext()) {
    OpArgs.push_back(getParamConvention(fn->getCalleeConvention()));
  } else {
    OpArgs.push_back('t');
  }

  switch (fn->getRepresentation()) {
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Thin:
      break;
    case SILFunctionTypeRepresentation::Block:
      OpArgs.push_back('B');
      break;
    case SILFunctionTypeRepresentation::CFunctionPointer:
      OpArgs.push_back('C');
      break;
    case SILFunctionTypeRepresentation::ObjCMethod:
      OpArgs.push_back('O');
      break;
    case SILFunctionTypeRepresentation::Method:
      OpArgs.push_back('M');
      break;
    case SILFunctionTypeRepresentation::Closure:
      OpArgs.push_back('K');
      break;
    case SILFunctionTypeRepresentation::WitnessMethod:
      OpArgs.push_back('W');
      break;
  }

  // Mangle the parameters.
  for (auto param : fn->getParameters()) {
    OpArgs.push_back(getParamConvention(param.getConvention()));
    appendType(param.getInterfaceType());
  }

  // Mangle the results.
  for (auto result : fn->getResults()) {
    OpArgs.push_back(getResultConvention(result.getConvention()));
    appendType(result.getInterfaceType());
  }

  // Mangle the error result if present.
  if (fn->hasErrorResult()) {
    auto error = fn->getErrorResult();
    OpArgs.push_back('z');
    OpArgs.push_back(getResultConvention(error.getConvention()));
    appendType(error.getInterfaceType());
  }
  if (fn->isPolymorphic())
    appendGenericSignature(fn->getSubstGenericSignature());

  OpArgs.push_back('_');

  appendOperator("I", StringRef(OpArgs.data(), OpArgs.size()));
}

Optional<ASTMangler::SpecialContext>
ASTMangler::getSpecialManglingContext(const ValueDecl *decl,
                                      bool useObjCProtocolNames) {
  #if SWIFT_BUILD_ONLY_SYNTAXPARSERLIB
    return None; // not needed for the parser library.
  #endif

  // Declarations provided by a C module have a special context mangling.
  //   known-context ::= 'So'
  //
  // Also handle top-level imported declarations that don't have corresponding
  // Clang decls. Check getKind() directly to avoid a layering dependency.
  //   known-context ::= 'SC'
  if (auto file = dyn_cast<FileUnit>(decl->getDeclContext())) {
    if (file->getKind() == FileUnitKind::ClangModule ||
        file->getKind() == FileUnitKind::DWARFModule) {
      if (decl->getClangDecl())
        return ASTMangler::ObjCContext;
      return ASTMangler::ClangImporterContext;
    }
  }

  // If @objc Swift protocols should be mangled as Objective-C protocols,
  // they are defined in the Objective-C context.
  if (getOverriddenSwiftProtocolObjCName(decl, useObjCProtocolNames))
    return ASTMangler::ObjCContext;

  // Nested types imported from C should also get use the special "So" context.
  if (isa<TypeDecl>(decl)) {
    if (auto *clangDecl = cast_or_null<clang::NamedDecl>(decl->getClangDecl())){
      bool hasNameForLinkage;
      if (auto *tagDecl = dyn_cast<clang::TagDecl>(clangDecl))
        hasNameForLinkage = tagDecl->hasNameForLinkage();
      else
        hasNameForLinkage = !clangDecl->getDeclName().isEmpty();
      if (hasNameForLinkage) {
        auto *clangDC = clangDecl->getDeclContext();
        if (isa<clang::NamespaceDecl>(clangDC)) return None;
        assert(clangDC->getRedeclContext()->isTranslationUnit() &&
               "non-top-level Clang types not supported yet");
        (void)clangDC;
        return ASTMangler::ObjCContext;
      }
    }
  }

  // Importer-synthesized types should always be mangled in the
  // ClangImporterContext, even if an __attribute__((swift_name())) nests them
  // inside a Swift type syntactically.
  if (decl->getAttrs().hasAttribute<ClangImporterSynthesizedTypeAttr>())
    return ASTMangler::ClangImporterContext;

  return None;
}

/// Mangle the context of the given declaration as a <context.
/// This is the top-level entrypoint for mangling <context>.
void ASTMangler::appendContextOf(const ValueDecl *decl) {
  // Check for a special mangling context.
  if (auto context = getSpecialManglingContext(decl, UseObjCProtocolNames)) {
    switch (*context) {
    case ClangImporterContext:
      return appendOperator("SC");
    case ObjCContext:
      return appendOperator("So");
    }
  }

  // Just mangle the decl's DC.
  appendContext(decl->getDeclContext());
}

namespace {
  class FindFirstVariable :
    public PatternVisitor<FindFirstVariable, VarDecl *> {
  public:
    VarDecl *visitNamedPattern(NamedPattern *P) {
      return P->getDecl();
    }

    VarDecl *visitTuplePattern(TuplePattern *P) {
      for (auto &elt : P->getElements()) {
        VarDecl *var = visit(elt.getPattern());
        if (var) return var;
      }
      return nullptr;
    }

    VarDecl *visitParenPattern(ParenPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitVarPattern(VarPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitTypedPattern(TypedPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitAnyPattern(AnyPattern *P) {
      return nullptr;
    }

    // Refutable patterns shouldn't ever come up.
#define REFUTABLE_PATTERN(ID, BASE)                                        \
    VarDecl *visit##ID##Pattern(ID##Pattern *P) {                          \
      llvm_unreachable("shouldn't be visiting a refutable pattern here!"); \
    }
#define PATTERN(ID, BASE)
#include "swift/AST/PatternNodes.def"
  };
} // end anonymous namespace

/// Find the first identifier bound by the given binding.  This
/// assumes that field and global-variable bindings always bind at
/// least one name, which is probably a reasonable assumption but may
/// not be adequately enforced.
static Optional<VarDecl*> findFirstVariable(PatternBindingDecl *binding) {
  for (auto idx : range(binding->getNumPatternEntries())) {
    auto var = FindFirstVariable().visit(binding->getPattern(idx));
    if (var) return var;
  }
  // Pattern-binding bound without variables exists in erroneous code, e.g.
  // during code completion.
  return None;
}

void ASTMangler::appendContext(const DeclContext *ctx) {
  switch (ctx->getContextKind()) {
  case DeclContextKind::Module:
    return appendModule(cast<ModuleDecl>(ctx));

  case DeclContextKind::FileUnit:
    assert(!isa<BuiltinUnit>(ctx) && "mangling member of builtin module!");
    appendContext(ctx->getParent());
    return;

  case DeclContextKind::SerializedLocal: {
    auto local = cast<SerializedLocalDeclContext>(ctx);
    switch (local->getLocalDeclContextKind()) {
    case LocalDeclContextKind::AbstractClosure:
      appendClosureEntity(cast<SerializedAbstractClosureExpr>(local));
      return;
    case LocalDeclContextKind::DefaultArgumentInitializer: {
      auto argInit = cast<SerializedDefaultArgumentInitializer>(local);
      appendDefaultArgumentEntity(ctx->getParent(), argInit->getIndex());
      return;
    }
    case LocalDeclContextKind::PatternBindingInitializer: {
      auto patternInit = cast<SerializedPatternBindingInitializer>(local);
      if (auto var = findFirstVariable(patternInit->getBinding())) {
        appendInitializerEntity(var.getValue());
      } else {
        // This is incorrect in that it does not produce a /unique/ mangling,
        // but it will at least produce a /valid/ mangling.
        appendContext(ctx->getParent());
      }
      return;
    }
    case LocalDeclContextKind::TopLevelCodeDecl:
      return appendContext(local->getParent());
    }
  }

  case DeclContextKind::GenericTypeDecl:
    appendAnyGenericType(cast<GenericTypeDecl>(ctx));
    return;

  case DeclContextKind::ExtensionDecl: {
    auto ExtD = cast<ExtensionDecl>(ctx);
    auto decl = ExtD->getExtendedNominal();
    // Recover from erroneous extension.
    if (!decl)
      return appendContext(ExtD->getDeclContext());

    if (!ExtD->isEquivalentToExtendedContext()) {
    // Mangle the extension if:
    // - the extension is defined in a different module from the original
    //   nominal type decl,
    // - the extension is constrained, or
    // - the extension is to a protocol.
    // FIXME: In a world where protocol extensions are dynamically dispatched,
    // "extension is to a protocol" would no longer be a reason to use the
    // extension mangling, because an extension method implementation could be
    // resiliently moved into the original protocol itself.
      auto sig = ExtD->getGenericSignature();
      // If the extension is constrained, mangle the generic signature that
      // constrains it.
      appendAnyGenericType(decl);
      appendModule(ExtD->getParentModule());
      if (sig && ExtD->isConstrainedExtension()) {
        Mod = ExtD->getModuleContext();
        auto nominalSig = ExtD->getSelfNominalTypeDecl()
                            ->getGenericSignatureOfContext();
        appendGenericSignature(sig, nominalSig);
      }
      return appendOperator("E");
    }
    return appendAnyGenericType(decl);
  }

  case DeclContextKind::AbstractClosureExpr:
    return appendClosureEntity(cast<AbstractClosureExpr>(ctx));

  case DeclContextKind::AbstractFunctionDecl: {
    auto fn = cast<AbstractFunctionDecl>(ctx);

    // Constructors and destructors as contexts are always mangled
    // using the non-(de)allocating variants.
    if (auto ctor = dyn_cast<ConstructorDecl>(fn)) {
      return appendConstructorEntity(ctor, /*allocating*/ false);
    }
    
    if (auto dtor = dyn_cast<DestructorDecl>(fn))
      return appendDestructorEntity(dtor, /*deallocating*/ false);
    
    return appendEntity(fn);
  }

  case DeclContextKind::EnumElementDecl: {
    auto eed = cast<EnumElementDecl>(ctx);
    return appendEntity(eed);
  }

  case DeclContextKind::SubscriptDecl: {
    auto sd = cast<SubscriptDecl>(ctx);
    return appendEntity(sd);
  }
      
  case DeclContextKind::Initializer:
    switch (cast<Initializer>(ctx)->getInitializerKind()) {
    case InitializerKind::DefaultArgument: {
      auto argInit = cast<DefaultArgumentInitializer>(ctx);
      return appendDefaultArgumentEntity(ctx->getParent(), argInit->getIndex());
    }

    case InitializerKind::PatternBinding: {
      auto patternInit = cast<PatternBindingInitializer>(ctx);
      if (auto var = findFirstVariable(patternInit->getBinding())) {
        appendInitializerEntity(var.getValue());
      } else {
        // This is incorrect in that it does not produce a /unique/ mangling,
        // but it will at least produce a /valid/ mangling.
        appendContext(ctx->getParent());
      }
      return;
    }
    }
    llvm_unreachable("bad initializer kind");

  case DeclContextKind::TopLevelCodeDecl:
    // Mangle the containing module context.
    return appendContext(ctx->getParent());
  }

  llvm_unreachable("bad decl context");
}

void ASTMangler::appendModule(const ModuleDecl *module) {
  assert(!module->getParent() && "cannot mangle nested modules!");

  // Try the special 'swift' substitution.
  if (module->isStdlibModule())
    return appendOperator("s");

  StringRef ModName = module->getName().str();
  if (ModName == MANGLING_MODULE_OBJC)
    return appendOperator("So");
  if (ModName == MANGLING_MODULE_CLANG_IMPORTER)
    return appendOperator("SC");

  appendIdentifier(ModName);
}

/// Mangle the name of a protocol as a substitution candidate.
void ASTMangler::appendProtocolName(const ProtocolDecl *protocol,
                                    bool allowStandardSubstitution) {
  if (allowStandardSubstitution && tryAppendStandardSubstitution(protocol))
    return;

  // We can use a symbolic reference if they're allowed in this context.
  if (canSymbolicReference(protocol)) {
    // Try to use a symbolic reference substitution.
    if (tryMangleSubstitution(protocol))
      return;
  
    appendSymbolicReference(protocol);
    // Substitutions can refer back to the symbolic reference.
    addSubstitution(protocol);
    return;
  }

  appendContextOf(protocol);
  auto *clangDecl = protocol->getClangDecl();
  if (auto *clangProto = cast_or_null<clang::ObjCProtocolDecl>(clangDecl))
    appendIdentifier(clangProto->getName());
  else
    appendDeclName(protocol);
}

const clang::NamedDecl *ASTMangler::getClangDeclForMangling(const ValueDecl *vd) {
  auto namedDecl =  dyn_cast_or_null<clang::NamedDecl>(vd->getClangDecl());
  if (!namedDecl)
    return nullptr;
  
  // Use an anonymous enum's enclosing typedef for the mangled name, if
  // present. This matches C++'s rules for linkage names of tag declarations.
  if (namedDecl->getDeclName().isEmpty())
    if (auto *tagDecl = dyn_cast<clang::TagDecl>(namedDecl))
      if (auto *typedefDecl = tagDecl->getTypedefNameForAnonDecl())
        namedDecl = typedefDecl;

  if (namedDecl->getDeclName().isEmpty())
    return nullptr;

  return namedDecl;
}

void ASTMangler::appendSymbolicReference(SymbolicReferent referent) {
  // Drop in a placeholder. The real reference value has to be filled in during
  // lowering to IR.
  auto offset = Buffer.str().size();
  Buffer << StringRef("\0\0\0\0\0", 5);
  SymbolicReferences.emplace_back(referent, offset);
}

void ASTMangler::appendAnyGenericType(const GenericTypeDecl *decl) {
  // Check for certain standard types.
  if (tryAppendStandardSubstitution(decl))
    return;

  // Mangle opaque type names.
  if (auto opaque = dyn_cast<OpaqueTypeDecl>(decl)) {
    appendOpaqueDeclName(opaque);
    return;
  }
  
  auto *nominal = dyn_cast<NominalTypeDecl>(decl);

  // For generic types, this uses the unbound type.
  if (nominal) {
    if (tryMangleTypeSubstitution(nominal->getDeclaredType()))
      return;
  } else {
    if (tryMangleSubstitution(cast<TypeAliasDecl>(decl)))
      return;
  }
  
  // Try to mangle a symbolic reference for a nominal type.
  if (nominal && canSymbolicReference(nominal)) {
    appendSymbolicReference(nominal);
    // Substitutions can refer back to the symbolic reference.
    addTypeSubstitution(nominal->getDeclaredType());
    return;
  }

  appendContextOf(decl);

  // Always use Clang names for imported Clang declarations, unless they don't
  // have one.
  auto tryAppendClangName = [this, decl]() -> bool {
    auto namedDecl = getClangDeclForMangling(decl);
    if (!namedDecl)
      return false;

    appendIdentifier(namedDecl->getName());

    // The important distinctions to maintain here are Objective-C's various
    // namespaces: protocols, tags (struct/enum/union), and unqualified names.
    // We continue to mangle "class" the standard Swift way because it feels
    // weird to call that an alias, but they're really in the same namespace.
    if (isa<clang::ObjCInterfaceDecl>(namedDecl)) {
      appendOperator("C");
    } else if (isa<clang::ObjCProtocolDecl>(namedDecl)) {
      appendOperator("P");
    } else if (isa<clang::TagDecl>(namedDecl)) {
      // Note: This includes enums, but that's okay. A Clang enum is not always
      // imported as a Swift enum.
      appendOperator("V");
    } else if (isa<clang::TypedefNameDecl>(namedDecl) ||
               isa<clang::ObjCCompatibleAliasDecl>(namedDecl)) {
      appendOperator("a");
    } else if (isa<clang::NamespaceDecl>(namedDecl)) {
      // Note: Namespaces are not really structs, but since namespaces are
      // imported as enums, be consistent.
      appendOperator("V");
    } else {
      llvm_unreachable("unknown imported Clang type");
    }

    return true;
  };

  if (!tryAppendClangName()) {
    appendDeclName(decl);

    switch (decl->getKind()) {
    default:
      llvm_unreachable("not a nominal type");

    case DeclKind::TypeAlias:
      appendOperator("a");
      break;
    case DeclKind::Protocol:
      appendOperator("P");
      break;
    case DeclKind::Class:
      appendOperator("C");
      break;
    case DeclKind::Enum:
      appendOperator("O");
      break;
    case DeclKind::Struct:
      appendOperator("V");
      break;
    }
  }

  if (nominal)
    addTypeSubstitution(nominal->getDeclaredType());
  else
    addSubstitution(cast<TypeAliasDecl>(decl));
}

void ASTMangler::appendFunction(AnyFunctionType *fn, bool isFunctionMangling,
                                const ValueDecl *forDecl) {
  // Append parameter labels right before the signature/type.
  auto parameters = fn->getParams();
  auto firstLabel = std::find_if(
                  parameters.begin(), parameters.end(),
                  [&](AnyFunctionType::Param param) { return param.hasLabel(); });

  if (firstLabel != parameters.end()) {
    for (auto param : parameters) {
      auto label = param.getLabel();
      if (!label.empty())
        appendIdentifier(label.str());
      else
        appendOperator("_");
    }
  } else if (!parameters.empty()) {
    appendOperator("y");
  }

  if (isFunctionMangling) {
    appendFunctionSignature(fn, forDecl);
  } else {
    appendFunctionType(fn, /*autoclosure*/ false, forDecl);
  }
}

void ASTMangler::appendFunctionType(AnyFunctionType *fn, bool isAutoClosure,
                                    const ValueDecl *forDecl) {
  assert((DWARFMangling || fn->isCanonical()) &&
         "expecting canonical types when not mangling for the debugger");

  appendFunctionSignature(fn, forDecl);

  // Note that we do not currently use thin representations in the AST
  // for the types of function decls.  This may need to change at some
  // point, in which case the uncurry logic can probably migrate to that
  // case.
  //
  // It would have been cleverer if we'd used 'f' for thin functions
  // and something else for uncurried functions, but oh well.
  //
  // Or maybe we can change the mangling at the same time we make
  // changes to better support thin functions.
  switch (fn->getRepresentation()) {
  case AnyFunctionType::Representation::Block:
    // We distinguish escaping and non-escaping blocks, but only in the DWARF
    // mangling, because the ABI is already set.
    if (!fn->isNoEscape() && DWARFMangling)
      return appendOperator("XL");
    return appendOperator("XB");
  case AnyFunctionType::Representation::Thin:
    return appendOperator("Xf");
  case AnyFunctionType::Representation::Swift:
    if (isAutoClosure) {
      if (fn->isNoEscape())
        return appendOperator("XK");
      else
        return appendOperator("XA");
    } else if (fn->isNoEscape()) {
      return appendOperator("XE");
    }
    return appendOperator("c");

  case AnyFunctionType::Representation::CFunctionPointer:
    return appendOperator("XC");
  }
}

void ASTMangler::appendFunctionSignature(AnyFunctionType *fn,
                                         const ValueDecl *forDecl) {
  appendFunctionResultType(fn->getResult(), forDecl);
  appendFunctionInputType(fn->getParams(), forDecl);
  if (fn->throws())
    appendOperator("K");
}

void ASTMangler::appendFunctionInputType(
    ArrayRef<AnyFunctionType::Param> params,
    const ValueDecl *forDecl) {
  switch (params.size()) {
  case 0:
    appendOperator("y");
    break;

  case 1: {
    const auto &param = params.front();
    auto type = param.getPlainType();

    // If the sole unlabeled parameter has a non-tuple type, encode
    // the parameter list as a single type.
    if (!param.hasLabel() && !param.isVariadic() &&
        !isa<TupleType>(type.getPointer())) {
      appendTypeListElement(Identifier(), type, param.getParameterFlags(),
                            forDecl);
      break;
    }

    // If this is a tuple type with a single labeled element
    // let's handle it as a general case.
    LLVM_FALLTHROUGH;
  }

  default:
    bool isFirstParam = true;
    for (auto &param : params) {
      appendTypeListElement(Identifier(), param.getPlainType(),
                            param.getParameterFlags(), forDecl);
      appendListSeparator(isFirstParam);
    }
    appendOperator("t");
    break;
  }
}

void ASTMangler::appendFunctionResultType(Type resultType,
                                          const ValueDecl *forDecl) {
  return resultType->isVoid() ? appendOperator("y")
                              : appendType(resultType, forDecl);
}

void ASTMangler::appendTypeList(Type listTy, const ValueDecl *forDecl) {
  if (TupleType *tuple = listTy->getAs<TupleType>()) {
    if (tuple->getNumElements() == 0)
      return appendOperator("y");
    bool firstField = true;
    for (auto &field : tuple->getElements()) {
      assert(field.getParameterFlags().isNone());
      appendTypeListElement(field.getName(), field.getRawType(),
                            ParameterTypeFlags(),
                            forDecl);
      appendListSeparator(firstField);
    }
  } else {
    appendType(listTy, forDecl);
    appendListSeparator();
  }
}

void ASTMangler::appendTypeListElement(Identifier name, Type elementType,
                                       ParameterTypeFlags flags,
                                       const ValueDecl *forDecl) {
  if (auto *fnType = elementType->getAs<FunctionType>())
    appendFunctionType(fnType, flags.isAutoClosure(), forDecl);
  else
    appendType(elementType, forDecl);

  switch (flags.getValueOwnership()) {
  case ValueOwnership::Default:
    /* nothing */
    break;
  case ValueOwnership::InOut:
    appendOperator("z");
    break;
  case ValueOwnership::Shared:
    appendOperator("h");
    break;
  case ValueOwnership::Owned:
    appendOperator("n");
    break;
  }
  if (!name.empty())
    appendIdentifier(name.str());
  if (flags.isVariadic())
    appendOperator("d");
}

bool ASTMangler::appendGenericSignature(GenericSignature sig,
                                        GenericSignature contextSig) {
  auto canSig = sig->getCanonicalSignature();
  CurGenericSignature = canSig;

  unsigned initialParamDepth;
  TypeArrayView<GenericTypeParamType> genericParams;
  ArrayRef<Requirement> requirements;
  SmallVector<Requirement, 4> requirementsBuffer;
  if (contextSig) {
    // If the signature is the same as the context signature, there's nothing
    // to do.
    if (contextSig->getCanonicalSignature() == canSig) {
      return false;
    }

    // The signature depth starts above the depth of the context signature.
    if (!contextSig->getGenericParams().empty()) {
      initialParamDepth = contextSig->getGenericParams().back()->getDepth() + 1;
    }

    // Find the parameters at this depth (or greater).
    genericParams = canSig->getGenericParams();
    unsigned firstParam = genericParams.size();
    while (firstParam > 1 &&
           genericParams[firstParam-1]->getDepth() >= initialParamDepth)
      --firstParam;
    genericParams = genericParams.slice(firstParam);

    // Special case: if we would be mangling zero generic parameters, but
    // the context signature is a single, unconstrained generic parameter,
    // it's better to mangle the complete canonical signature because we
    // have a special-case mangling for that.
    if (genericParams.empty() &&
        contextSig->getGenericParams().size() == 1 &&
        contextSig->getRequirements().empty()) {
      initialParamDepth = 0;
      genericParams = canSig->getGenericParams();
      requirements = canSig->getRequirements();
    } else {
      requirementsBuffer = canSig->requirementsNotSatisfiedBy(contextSig);
      requirements = requirementsBuffer;
    }
  } else {
    // Use the complete canonical signature.
    initialParamDepth = 0;
    genericParams = canSig->getGenericParams();
    requirements = canSig->getRequirements();
  }

  if (genericParams.empty() && requirements.empty())
    return false;

  appendGenericSignatureParts(genericParams, initialParamDepth, requirements);
  return true;
}

void ASTMangler::appendRequirement(const Requirement &reqt) {

  Type FirstTy = reqt.getFirstType()->getCanonicalType();

  switch (reqt.getKind()) {
  case RequirementKind::Layout: {
  } break;
  case RequirementKind::Conformance: {
    Type SecondTy = reqt.getSecondType();
    appendProtocolName(SecondTy->castTo<ProtocolType>()->getDecl());
  } break;
  case RequirementKind::Superclass:
  case RequirementKind::SameType: {
    Type SecondTy = reqt.getSecondType();
    appendType(SecondTy->getCanonicalType());
  } break;
  }

  if (auto *DT = FirstTy->getAs<DependentMemberType>()) {
    bool isAssocTypeAtDepth = false;
    if (tryMangleTypeSubstitution(DT)) {
      switch (reqt.getKind()) {
        case RequirementKind::Conformance:
          return appendOperator("RQ");
        case RequirementKind::Layout:
          appendOperator("RL");
          appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
          return;
        case RequirementKind::Superclass:
          return appendOperator("RB");
        case RequirementKind::SameType:
          return appendOperator("RS");
      }
      llvm_unreachable("bad requirement type");
    }
    GenericTypeParamType *gpBase = appendAssocType(DT, isAssocTypeAtDepth);
    addTypeSubstitution(DT);
    assert(gpBase);
    switch (reqt.getKind()) {
      case RequirementKind::Conformance:
        return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RP" : "Rp",
                                             gpBase);
      case RequirementKind::Layout:
        appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RM" : "Rm", gpBase);
        appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
        return;
      case RequirementKind::Superclass:
        return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RC" : "Rc",
                                             gpBase);
      case RequirementKind::SameType:
        return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RT" : "Rt",
                                             gpBase);
    }
    llvm_unreachable("bad requirement type");
  }
  GenericTypeParamType *gpBase = FirstTy->castTo<GenericTypeParamType>();
  switch (reqt.getKind()) {
    case RequirementKind::Conformance:
      return appendOpWithGenericParamIndex("R", gpBase);
    case RequirementKind::Layout:
      appendOpWithGenericParamIndex("Rl", gpBase);
      appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
      return;
    case RequirementKind::Superclass:
      return appendOpWithGenericParamIndex("Rb", gpBase);
    case RequirementKind::SameType:
      return appendOpWithGenericParamIndex("Rs", gpBase);
  }
  llvm_unreachable("bad requirement type");
}

void ASTMangler::appendGenericSignatureParts(
                                     TypeArrayView<GenericTypeParamType> params,
                                     unsigned initialParamDepth,
                                     ArrayRef<Requirement> requirements) {
  // Mangle the requirements.
  for (const Requirement &reqt : requirements) {
    appendRequirement(reqt);
  }

  if (params.size() == 1 && params[0]->getDepth() == initialParamDepth)
    return appendOperator("l");

  llvm::SmallVector<char, 16> OpStorage;
  llvm::raw_svector_ostream OpBuffer(OpStorage);

  // Mangle the number of parameters.
  unsigned depth = 0;
  unsigned count = 0;
  
  // Since it's unlikely (but not impossible) to have zero generic parameters
  // at a depth, encode indexes starting from 1, and use a special mangling
  // for zero.
  auto mangleGenericParamCount = [&](unsigned depth, unsigned count) {
    if (depth < initialParamDepth)
      return;
    if (count == 0)
      OpBuffer << 'z';
    else
      OpBuffer << Index(count - 1);
  };
  
  // As a special case, mangle nothing if there's a single generic parameter
  // at the initial depth.
  for (auto param : params) {
    if (param->getDepth() != depth) {
      assert(param->getDepth() > depth && "generic params not ordered");
      while (depth < param->getDepth()) {
        mangleGenericParamCount(depth, count);
        ++depth;
        count = 0;
      }
    }
    assert(param->getIndex() == count && "generic params not ordered");
    ++count;
  }
  mangleGenericParamCount(depth, count);
  OpBuffer << 'l';

  appendOperator("r", StringRef(OpStorage.data(), OpStorage.size()));
}

// If the base type is known to have a single protocol conformance
// in the current generic context, then we don't need to disambiguate the
// associated type name by protocol.
DependentMemberType *
ASTMangler::dropProtocolFromAssociatedType(DependentMemberType *dmt) {
  auto baseTy = dmt->getBase();
  bool unambiguous = (!dmt->getAssocType() ||
                      CurGenericSignature->getConformsTo(baseTy).size() <= 1);

  if (auto *baseDMT = baseTy->getAs<DependentMemberType>())
    baseTy = dropProtocolFromAssociatedType(baseDMT);

  if (unambiguous)
    return DependentMemberType::get(baseTy, dmt->getName());

  return DependentMemberType::get(baseTy, dmt->getAssocType());
}

Type
ASTMangler::dropProtocolsFromAssociatedTypes(Type type) {
  if (!OptimizeProtocolNames || !CurGenericSignature)
    return type;

  if (!type->hasDependentMember())
    return type;

  return type.transform([&](Type t) -> Type {
    if (auto *dmt = dyn_cast<DependentMemberType>(t.getPointer()))
      return dropProtocolFromAssociatedType(dmt);
    return t;
  });
}

void ASTMangler::appendAssociatedTypeName(DependentMemberType *dmt) {
  if (auto assocTy = dmt->getAssocType()) {
    appendIdentifier(assocTy->getName().str());

    // If the base type is known to have a single protocol conformance
    // in the current generic context, then we don't need to disambiguate the
    // associated type name by protocol.
    if (!OptimizeProtocolNames || !CurGenericSignature
        || CurGenericSignature->getConformsTo(dmt->getBase()).size() > 1) {
      appendAnyGenericType(assocTy->getProtocol());
    }
    return;
  }

  appendIdentifier(dmt->getName().str());
}

void ASTMangler::appendClosureEntity(
                              const SerializedAbstractClosureExpr *closure) {
  appendClosureComponents(closure->getType(), closure->getDiscriminator(),
                          closure->isImplicit(), closure->getParent());
}

void ASTMangler::appendClosureEntity(const AbstractClosureExpr *closure) {
  appendClosureComponents(closure->getType(), closure->getDiscriminator(),
                          isa<AutoClosureExpr>(closure), closure->getParent());
}

void ASTMangler::appendClosureComponents(Type Ty, unsigned discriminator,
                                         bool isImplicit,
                                         const DeclContext *parentContext) {
  assert(discriminator != AbstractClosureExpr::InvalidDiscriminator
         && "closure must be marked correctly with discriminator");

  appendContext(parentContext);

  if (!Ty)
    Ty = ErrorType::get(parentContext->getASTContext());

  Ty = Ty->mapTypeOutOfContext();
  appendType(Ty->getCanonicalType());
  appendOperator(isImplicit ? "fu" : "fU", Index(discriminator));
}

void ASTMangler::appendDefaultArgumentEntity(const DeclContext *func,
                                             unsigned index) {
  appendContext(func);
  appendOperator("fA", Index(index));
}

void ASTMangler::appendInitializerEntity(const VarDecl *var) {
  appendEntity(var, "vp", var->isStatic());
  appendOperator("fi");
}

void ASTMangler::appendBackingInitializerEntity(const VarDecl *var) {
  appendEntity(var, "vp", var->isStatic());
  appendOperator("fP");
}

/// Is this declaration a method for mangling purposes? If so, we'll leave the
/// Self type out of its mangling.
static bool isMethodDecl(const Decl *decl) {
  return isa<AbstractFunctionDecl>(decl)
    && decl->getDeclContext()->isTypeContext();
}

CanType ASTMangler::getDeclTypeForMangling(
                                       const ValueDecl *decl,
                                       GenericSignature &genericSig,
                                       GenericSignature &parentGenericSig) {
  genericSig = GenericSignature();
  parentGenericSig = GenericSignature();

  auto &C = decl->getASTContext();
  if (decl->isInvalid()) {
    if (isa<AbstractFunctionDecl>(decl))
      return CanFunctionType::get({AnyFunctionType::Param(C.TheErrorType)},
                                  C.TheErrorType);
    return C.TheErrorType;
  }


  auto canTy = decl->getInterfaceType()
                   ->getReferenceStorageReferent()
                   ->getCanonicalType();

  if (auto gft = dyn_cast<GenericFunctionType>(canTy)) {
    genericSig = gft.getGenericSignature();
    CurGenericSignature = gft.getGenericSignature();

    canTy = CanFunctionType::get(gft.getParams(), gft.getResult(),
                                 gft->getExtInfo());
  }

  if (!canTy->hasError()) {
    // Shed the 'self' type and generic requirements from method manglings.
    if (isMethodDecl(decl)) {
      // Drop the Self argument clause from the type.
      canTy = cast<AnyFunctionType>(canTy).getResult();
    }

    if (isMethodDecl(decl) || isa<SubscriptDecl>(decl))
      parentGenericSig = decl->getDeclContext()->getGenericSignatureOfContext();
  }

  return canTy;
}

void ASTMangler::appendDeclType(const ValueDecl *decl, bool isFunctionMangling) {
  Mod = decl->getModuleContext();
  GenericSignature genericSig = GenericSignature();
  GenericSignature parentGenericSig = GenericSignature();
  auto type = getDeclTypeForMangling(decl, genericSig, parentGenericSig);

  if (AnyFunctionType *FuncTy = type->getAs<AnyFunctionType>()) {
    appendFunction(FuncTy, isFunctionMangling, decl);
  } else {
    appendType(type, decl);
  }

  // Mangle the generic signature, if any.
  if (genericSig && appendGenericSignature(genericSig, parentGenericSig)) {
    // The 'F' function mangling doesn't need a 'u' for its generic signature.
    if (!isFunctionMangling)
      appendOperator("u");
  }
}

bool ASTMangler::tryAppendStandardSubstitution(const GenericTypeDecl *decl) {
  // Bail out if our parent isn't the swift standard library.
  if (!isStdlibType(decl))
    return false;

  if (isa<NominalTypeDecl>(decl)) {
    if (char Subst = getStandardTypeSubst(decl->getName().str())) {
      if (!SubstMerging.tryMergeSubst(*this, Subst, /*isStandardSubst*/ true)) {
        appendOperator("S", StringRef(&Subst, 1));
      }
      return true;
    }
  }
  return false;
}

void ASTMangler::appendConstructorEntity(const ConstructorDecl *ctor,
                                         bool isAllocating) {
  appendContextOf(ctor);
  appendDeclType(ctor);
  StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(ctor);
  if (!privateDiscriminator.empty()) {
    appendIdentifier(privateDiscriminator);
    appendOperator("Ll");
  }
  appendOperator(isAllocating ? "fC" : "fc");
}

void ASTMangler::appendDestructorEntity(const DestructorDecl *dtor,
                                     bool isDeallocating) {
  appendContextOf(dtor);
  appendOperator(isDeallocating ? "fD" : "fd");
}

void ASTMangler::appendAccessorEntity(StringRef accessorKindCode,
                                      const AbstractStorageDecl *decl,
                                      bool isStatic) {
  appendContextOf(decl);
  bindGenericParameters(decl->getDeclContext());
  if (isa<VarDecl>(decl)) {
    appendDeclName(decl);
    appendDeclType(decl);
    appendOperator("v", accessorKindCode);
  } else if (isa<SubscriptDecl>(decl)) {
    appendDeclType(decl);

    StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(decl);
    if (!privateDiscriminator.empty()) {
      appendIdentifier(privateDiscriminator);
      appendOperator("Ll");
    }

    appendOperator("i", accessorKindCode);
  } else {
    llvm_unreachable("Unknown type of AbstractStorageDecl");
  }
  if (isStatic)
    appendOperator("Z");
}

void ASTMangler::appendEntity(const ValueDecl *decl, StringRef EntityOp,
                              bool isStatic) {
  appendContextOf(decl);
  appendDeclName(decl);
  appendDeclType(decl);
  appendOperator(EntityOp);
  if (isStatic)
    appendOperator("Z");
}

void ASTMangler::appendEntity(const ValueDecl *decl) {
  assert(!isa<ConstructorDecl>(decl));
  assert(!isa<DestructorDecl>(decl));
  
  // Handle accessors specially, they are mangled as modifiers on the accessed
  // declaration.
  if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
    return appendAccessorEntity(
        getCodeForAccessorKind(accessor->getAccessorKind()),
        accessor->getStorage(), accessor->isStatic());
  }

  if (auto storageDecl = dyn_cast<AbstractStorageDecl>(decl))
    return appendAccessorEntity("p", storageDecl, decl->isStatic());
  if (isa<GenericTypeParamDecl>(decl))
    return appendEntity(decl, "fp", decl->isStatic());

  assert(isa<AbstractFunctionDecl>(decl) || isa<EnumElementDecl>(decl));

  appendContextOf(decl);
  appendDeclName(decl);
  appendDeclType(decl, /*isFunctionMangling*/ true);
  appendOperator("F");
  if (decl->isStatic())
    appendOperator("Z");
}

void
ASTMangler::appendProtocolConformance(const ProtocolConformance *conformance) {
  GenericSignature contextSig = GenericSignature();
  auto topLevelContext =
      conformance->getDeclContext()->getModuleScopeContext();
  Mod = topLevelContext->getParentModule();

  auto conformingType = conformance->getType();
  appendType(conformingType->getCanonicalType());

  appendProtocolName(conformance->getProtocol());

  bool needsModule = true;
  if (auto *file = dyn_cast<FileUnit>(topLevelContext)) {
    if (file->getKind() == FileUnitKind::ClangModule ||
        file->getKind() == FileUnitKind::DWARFModule) {
      if (conformance->getProtocol()->hasClangNode())
        appendOperator("So");
      else
        appendOperator("SC");
      needsModule = false;
    }
  }
  if (needsModule)
    appendModule(Mod);

  contextSig =
    conformingType->getAnyNominal()->getGenericSignatureOfContext();

  if (GenericSignature Sig = conformance->getGenericSignature()) {
    appendGenericSignature(Sig, contextSig);
  }
}

void ASTMangler::appendProtocolConformanceRef(
                                const RootProtocolConformance *conformance) {
  // FIXME: Symbolic reference to the protocol conformance descriptor.
  appendProtocolName(conformance->getProtocol());

  // For retroactive conformances, add a reference to the module in which the
  // conformance resides. Otherwise, use an operator to indicate which known
  // module it's associated with.
  if (!conformanceHasIdentity(conformance)) {
    // Same as "conformance module matches type", below.
    appendOperator("HP");
  } else if (isRetroactiveConformance(conformance)) {
    appendModule(conformance->getDeclContext()->getParentModule());
  } else if (conformance->getDeclContext()->getParentModule() ==
               conformance->getType()->getAnyNominal()->getParentModule()) {
    appendOperator("HP");
  } else {
    appendOperator("Hp");
  }
}

/// Retrieve the index of the conformance requirement indicated by the
/// conformance access path entry within the given set of requirements.
static unsigned conformanceRequirementIndex(
                                      const ConformanceAccessPath::Entry &entry,
                                      ArrayRef<Requirement> requirements) {
  unsigned result = 0;
  for (const auto &req : requirements) {
    if (req.getKind() != RequirementKind::Conformance)
      continue;

    if (req.getFirstType()->isEqual(entry.first) &&
        req.getSecondType()->castTo<ProtocolType>()->getDecl() == entry.second)
      return result;

    ++result;
  }

  llvm_unreachable("Conformance access path step is missing from requirements");
}

void ASTMangler::appendDependentProtocolConformance(
                                            const ConformanceAccessPath &path) {
  ProtocolDecl *currentProtocol = nullptr;
  for (const auto &entry : path) {
    // After each step, update the current protocol to refer to where we
    // are.
    SWIFT_DEFER {
      currentProtocol = entry.second;
    };

    // The first entry is the "root". Find this requirement in the generic
    // signature.
    if (!currentProtocol) {
      appendType(entry.first);
      appendProtocolName(entry.second);
      auto index =
        conformanceRequirementIndex(entry,
                                    CurGenericSignature->getRequirements());
      // This is never an unknown index and so must be adjusted by 2 per ABI.
      appendOperator("HD", Index(index + 2));
      continue;
    }

    // Conformances are relative to the current protocol's requirement
    // signature.
    auto index =
      conformanceRequirementIndex(entry,
                                  currentProtocol->getRequirementSignature());

    // Inherited conformance.
    bool isInheritedConformance =
      entry.first->isEqual(currentProtocol->getProtocolSelfType());
    if (isInheritedConformance) {
      appendProtocolName(entry.second);
      // For now, this is never an unknown index and so must be adjusted by 2.
      appendOperator("HI", Index(index + 2));
      continue;
    }

    // Associated conformance.
    // FIXME: Symbolic reference.
    appendType(entry.first);
    appendProtocolName(entry.second);

    // For resilient protocols, the index is unknown, so we use the special
    // value 1; otherwise we adjust by 2.
    bool isResilient =
      currentProtocol->isResilient(Mod, ResilienceExpansion::Maximal);
    appendOperator("HA", Index(isResilient ? 1 : index + 2));
  }
}

void ASTMangler::appendConcreteProtocolConformance(
                                      const ProtocolConformance *conformance) {
  auto module = conformance->getDeclContext()->getParentModule();

  // It's possible that we might not have a generic signature here to get
  // the conformance access path (for example, when mangling types for
  // debugger). In that case, we can use the generic signature of the
  // conformance (if it's present).
  auto conformanceSig = conformance->getGenericSignature();
  auto shouldUseConformanceSig = !CurGenericSignature && conformanceSig;
  llvm::SaveAndRestore<CanGenericSignature> savedSignature(
      CurGenericSignature, shouldUseConformanceSig
                               ? conformanceSig->getCanonicalSignature()
                               : CurGenericSignature);

  // Conforming type.
  Type conformingType = conformance->getType();
  if (conformingType->hasArchetype())
    conformingType = conformingType->mapTypeOutOfContext();
  appendType(conformingType->getCanonicalType());

  // Protocol conformance reference.
  appendProtocolConformanceRef(conformance->getRootConformance());

  // Conditional conformance requirements.
  bool firstRequirement = true;
  for (const auto &conditionalReq : conformance->getConditionalRequirements()) {
    switch (conditionalReq.getKind()) {
    case RequirementKind::Layout:
    case RequirementKind::SameType:
    case RequirementKind::Superclass:
      continue;

    case RequirementKind::Conformance: {
      auto type = conditionalReq.getFirstType();
      if (type->hasArchetype())
        type = type->mapTypeOutOfContext();
      CanType canType = type->getCanonicalType(CurGenericSignature);
      auto proto =
        conditionalReq.getSecondType()->castTo<ProtocolType>()->getDecl();
      if (canType->isTypeParameter()) {
        assert(CurGenericSignature &&
               "Need a generic signature to resolve conformance");
        auto conformanceAccessPath =
            CurGenericSignature->getConformanceAccessPath(type, proto);
        appendDependentProtocolConformance(conformanceAccessPath);
      } else if (auto opaqueType = canType->getAs<OpaqueTypeArchetypeType>()) {
        GenericSignature opaqueSignature = opaqueType->getBoundSignature();
        GenericTypeParamType *opaqueTypeParam = opaqueSignature->getGenericParams().back();
        ConformanceAccessPath conformanceAccessPath =
            opaqueSignature->getConformanceAccessPath(opaqueTypeParam, proto);

        // Append the conformance access path with the signature of the opaque type.
        {
          llvm::SaveAndRestore<CanGenericSignature> savedSignature(
              CurGenericSignature, opaqueSignature->getCanonicalSignature());
          appendDependentProtocolConformance(conformanceAccessPath);
        }
        appendType(canType);
        appendOperator("HO");
      } else {
        auto conditionalConf = module->lookupConformance(canType, proto);
        appendConcreteProtocolConformance(conditionalConf.getConcrete());
      }
      appendListSeparator(firstRequirement);
      break;
    }
    }
  }
  if (firstRequirement)
    appendOperator("y");

  appendOperator("HC");
}

void ASTMangler::appendOpParamForLayoutConstraint(LayoutConstraint layout) {
  assert(layout);
  switch (layout->getKind()) {
  case LayoutConstraintKind::UnknownLayout:
    appendOperatorParam("U");
    break;
  case LayoutConstraintKind::RefCountedObject:
    appendOperatorParam("R");
    break;
  case LayoutConstraintKind::NativeRefCountedObject:
    appendOperatorParam("N");
    break;
  case LayoutConstraintKind::Class:
    appendOperatorParam("C");
    break;
  case LayoutConstraintKind::NativeClass:
    appendOperatorParam("D");
    break;
  case LayoutConstraintKind::Trivial:
    appendOperatorParam("T");
    break;
  case LayoutConstraintKind::TrivialOfExactSize:
    if (!layout->getAlignmentInBits())
      appendOperatorParam("e", Index(layout->getTrivialSizeInBits()));
    else
      appendOperatorParam("E", Index(layout->getTrivialSizeInBits()),
                          Index(layout->getAlignmentInBits()));
    break;
  case LayoutConstraintKind::TrivialOfAtMostSize:
    if (!layout->getAlignmentInBits())
      appendOperatorParam("m", Index(layout->getTrivialSizeInBits()));
    else
      appendOperatorParam("M", Index(layout->getTrivialSizeInBits()),
                          Index(layout->getAlignmentInBits()));
    break;
  }
}

std::string ASTMangler::mangleOpaqueTypeDescriptor(const OpaqueTypeDecl *decl) {
  beginMangling();
  appendOpaqueDeclName(decl);
  appendOperator("MQ");
  return finalize();
}
