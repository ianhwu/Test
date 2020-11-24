//===--- ASTMangler.h - Swift AST symbol mangling ---------------*- C++ -*-===//
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

#ifndef SWIFT_AST_ASTMANGLER_H
#define SWIFT_AST_ASTMANGLER_H

#include "swift/Basic/Mangler.h"
#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"

namespace clang {
class NamedDecl;
}

namespace swift {

class AbstractClosureExpr;
class ConformanceAccessPath;
class RootProtocolConformance;

namespace Mangle {

/// The mangler for AST declarations.
class ASTMangler : public Mangler {
protected:
  CanGenericSignature CurGenericSignature;
  ModuleDecl *Mod = nullptr;

  /// Optimize out protocol names if a type only conforms to one protocol.
  bool OptimizeProtocolNames = true;

  /// If enabled, use Objective-C runtime names when mangling @objc Swift
  /// protocols.
  bool UseObjCProtocolNames = false;

  /// If enabled, non-canonical types are allowed and type alias types get a
  /// special mangling.
  bool DWARFMangling;

  /// If enabled, entities that ought to have names but don't get a placeholder.
  ///
  /// If disabled, it is an error to try to mangle such an entity.
  bool AllowNamelessEntities = false;

  /// If enabled, some entities will be emitted as symbolic reference
  /// placeholders. The offsets of these references will be stored in the
  /// `SymbolicReferences` vector, and it is up to the consumer of the mangling
  /// to fill these in.
  bool AllowSymbolicReferences = false;

public:
  using SymbolicReferent = llvm::PointerUnion<const NominalTypeDecl *,
                                              const OpaqueTypeDecl *>;
protected:

  /// If set, the mangler calls this function to determine whether to symbolic
  /// reference a given entity. If null, the mangler acts as if it's set to
  /// always return true.
  std::function<bool (SymbolicReferent)> CanSymbolicReference;
  
  bool canSymbolicReference(SymbolicReferent referent) {
    return AllowSymbolicReferences
      && (!CanSymbolicReference || CanSymbolicReference(referent));
  }

  std::vector<std::pair<SymbolicReferent, unsigned>> SymbolicReferences;
  
public:
  enum class SymbolKind {
    Default,
    DynamicThunk,
    SwiftAsObjCThunk,
    ObjCAsSwiftThunk,
    DirectMethodReferenceThunk,
  };

  ASTMangler(bool DWARFMangling = false)
    : DWARFMangling(DWARFMangling) {}

  void addTypeSubstitution(Type type) {
    type = dropProtocolsFromAssociatedTypes(type);
    addSubstitution(type.getPointer());
  }
  bool tryMangleTypeSubstitution(Type type) {
    type = dropProtocolsFromAssociatedTypes(type);
    return tryMangleSubstitution(type.getPointer());
  }

  std::string mangleClosureEntity(const AbstractClosureExpr *closure,
                                  SymbolKind SKind);

  std::string mangleEntity(const ValueDecl *decl, bool isCurried,
                           SymbolKind SKind = SymbolKind::Default);

  std::string mangleDestructorEntity(const DestructorDecl *decl,
                                     bool isDeallocating, SymbolKind SKind);

  std::string mangleConstructorEntity(const ConstructorDecl *ctor,
                                      bool isAllocating, bool isCurried,
                                      SymbolKind SKind = SymbolKind::Default);

  std::string mangleIVarInitDestroyEntity(const ClassDecl *decl,
                                          bool isDestroyer, SymbolKind SKind);

  std::string mangleAccessorEntity(AccessorKind kind,
                                   const AbstractStorageDecl *decl,
                                   bool isStatic,
                                   SymbolKind SKind);

  std::string mangleGlobalGetterEntity(const ValueDecl *decl,
                                       SymbolKind SKind = SymbolKind::Default);

  std::string mangleDefaultArgumentEntity(const DeclContext *func,
                                          unsigned index,
                                          SymbolKind SKind);

  std::string mangleInitializerEntity(const VarDecl *var, SymbolKind SKind);
  std::string mangleBackingInitializerEntity(const VarDecl *var,
                                             SymbolKind SKind);

  std::string mangleNominalType(const NominalTypeDecl *decl);

  std::string mangleVTableThunk(const FuncDecl *Base,
                                const FuncDecl *Derived);

  std::string mangleConstructorVTableThunk(const ConstructorDecl *Base,
                                           const ConstructorDecl *Derived,
                                           bool isAllocating);

  std::string mangleWitnessTable(const RootProtocolConformance *C);

  std::string mangleWitnessThunk(const ProtocolConformance *Conformance,
                                 const ValueDecl *Requirement);

  std::string mangleClosureWitnessThunk(const ProtocolConformance *Conformance,
                                        const AbstractClosureExpr *Closure);

  std::string mangleGlobalVariableFull(const VarDecl *decl);

  std::string mangleGlobalInit(const VarDecl *decl, int counter,
                               bool isInitFunc);

  std::string mangleReabstractionThunkHelper(CanSILFunctionType ThunkType,
                                             Type FromType, Type ToType,
                                             Type SelfType,
                                             ModuleDecl *Module);
  
  std::string mangleKeyPathGetterThunkHelper(const AbstractStorageDecl *property,
                                             GenericSignature signature,
                                             CanType baseType,
                                             SubstitutionMap subs,
                                             ResilienceExpansion expansion);
  std::string mangleKeyPathSetterThunkHelper(const AbstractStorageDecl *property,
                                             GenericSignature signature,
                                             CanType baseType,
                                             SubstitutionMap subs,
                                             ResilienceExpansion expansion);
  std::string mangleKeyPathEqualsHelper(ArrayRef<CanType> indices,
                                        GenericSignature signature,
                                        ResilienceExpansion expansion);
  std::string mangleKeyPathHashHelper(ArrayRef<CanType> indices,
                                      GenericSignature signature,
                                      ResilienceExpansion expansion);

  std::string mangleTypeForDebugger(Type decl, const DeclContext *DC);

  std::string mangleOpaqueTypeDescriptor(const OpaqueTypeDecl *decl);
  
  std::string mangleDeclType(const ValueDecl *decl);
  
  std::string mangleObjCRuntimeName(const NominalTypeDecl *Nominal);

  std::string mangleTypeWithoutPrefix(Type type) {
    appendType(type);
    return finalize();
  }

  std::string mangleTypeAsUSR(Type decl);

  std::string mangleTypeAsContextUSR(const NominalTypeDecl *type);

  std::string mangleDeclAsUSR(const ValueDecl *Decl, StringRef USRPrefix);

  std::string mangleAccessorEntityAsUSR(AccessorKind kind,
                                        const AbstractStorageDecl *decl,
                                        StringRef USRPrefix);

  std::string mangleLocalTypeDecl(const TypeDecl *type);

  enum SpecialContext {
    ObjCContext,
    ClangImporterContext,
  };
  
  static Optional<SpecialContext>
  getSpecialManglingContext(const ValueDecl *decl, bool useObjCProtocolNames);

  static const clang::NamedDecl *
  getClangDeclForMangling(const ValueDecl *decl);

protected:

  void appendSymbolKind(SymbolKind SKind);

  void appendType(Type type, const ValueDecl *forDecl = nullptr);
  
  void appendDeclName(const ValueDecl *decl);

  GenericTypeParamType *appendAssocType(DependentMemberType *DepTy,
                                        bool &isAssocTypeAtDepth);

  void appendOpWithGenericParamIndex(StringRef,
                                     const GenericTypeParamType *paramTy);

  void bindGenericParameters(const DeclContext *DC);

  void bindGenericParameters(CanGenericSignature sig);

  /// Mangles a sugared type iff we are mangling for the debugger.
  template <class T> void appendSugaredType(Type type,
                                            const ValueDecl *forDecl) {
    assert(DWARFMangling &&
           "sugared types are only legal when mangling for the debugger");
    auto *BlandTy = cast<T>(type.getPointer())->getSinglyDesugaredType();
    appendType(BlandTy, forDecl);
  }

  void appendBoundGenericArgs(Type type, bool &isFirstArgList);

  /// Append the bound generics arguments for the given declaration context
  /// based on a complete substitution map.
  ///
  /// \returns the number of generic parameters that were emitted
  /// thus far.
  unsigned appendBoundGenericArgs(DeclContext *dc,
                                  SubstitutionMap subs,
                                  bool &isFirstArgList);

  /// Append any retroactive conformances.
  void appendRetroactiveConformances(Type type);
  void appendRetroactiveConformances(SubstitutionMap subMap,
                                     ModuleDecl *fromModule);
  void appendImplFunctionType(SILFunctionType *fn);

  void appendContextOf(const ValueDecl *decl);

  void appendContext(const DeclContext *ctx);

  void appendModule(const ModuleDecl *module);

  void appendProtocolName(const ProtocolDecl *protocol,
                          bool allowStandardSubstitution = true);

  void appendAnyGenericType(const GenericTypeDecl *decl);

  void appendFunction(AnyFunctionType *fn, bool isFunctionMangling = false,
                      const ValueDecl *forDecl = nullptr);
  void appendFunctionType(AnyFunctionType *fn, bool isAutoClosure = false,
                          const ValueDecl *forDecl = nullptr);

  void appendFunctionSignature(AnyFunctionType *fn,
                               const ValueDecl *forDecl = nullptr);

  void appendFunctionInputType(ArrayRef<AnyFunctionType::Param> params,
                               const ValueDecl *forDecl = nullptr);
  void appendFunctionResultType(Type resultType,
                                const ValueDecl *forDecl = nullptr);

  void appendTypeList(Type listTy, const ValueDecl *forDecl = nullptr);
  void appendTypeListElement(Identifier name, Type elementType,
                             ParameterTypeFlags flags,
                             const ValueDecl *forDecl = nullptr);

  /// Append a generic signature to the mangling.
  ///
  /// \param sig The generic signature.
  ///
  /// \param contextSig The signature of the known context. This function
  /// will only mangle the difference between \c sig and \c contextSig.
  ///
  /// \returns \c true if a generic signature was appended, \c false
  /// if it was empty.
  bool appendGenericSignature(GenericSignature sig,
                              GenericSignature contextSig = nullptr);

  void appendRequirement(const Requirement &reqt);

  void appendGenericSignatureParts(TypeArrayView<GenericTypeParamType> params,
                                   unsigned initialParamDepth,
                                   ArrayRef<Requirement> requirements);

  DependentMemberType *dropProtocolFromAssociatedType(DependentMemberType *dmt);
  Type dropProtocolsFromAssociatedTypes(Type type);

  void appendAssociatedTypeName(DependentMemberType *dmt);

  void appendClosureEntity(const SerializedAbstractClosureExpr *closure);
  
  void appendClosureEntity(const AbstractClosureExpr *closure);

  void appendClosureComponents(Type Ty, unsigned discriminator, bool isImplicit,
                               const DeclContext *parentContext);

  void appendDefaultArgumentEntity(const DeclContext *ctx, unsigned index);

  void appendInitializerEntity(const VarDecl *var);
  void appendBackingInitializerEntity(const VarDecl *var);

  CanType getDeclTypeForMangling(const ValueDecl *decl,
                                 GenericSignature &genericSig,
                                 GenericSignature &parentGenericSig);

  void appendDeclType(const ValueDecl *decl, bool isFunctionMangling = false);

  bool tryAppendStandardSubstitution(const GenericTypeDecl *type);

  void appendConstructorEntity(const ConstructorDecl *ctor, bool isAllocating);
  
  void appendDestructorEntity(const DestructorDecl *decl, bool isDeallocating);

  /// \param accessorKindCode The code to describe the accessor and addressor
  /// kind. Usually retrieved using getCodeForAccessorKind.
  /// \param decl The storage decl for which to mangle the accessor
  /// \param isStatic Whether or not the accessor is static
  void appendAccessorEntity(StringRef accessorKindCode,
                            const AbstractStorageDecl *decl, bool isStatic);

  void appendEntity(const ValueDecl *decl, StringRef EntityOp, bool isStatic);

  void appendEntity(const ValueDecl *decl);

  void appendProtocolConformance(const ProtocolConformance *conformance);
  void appendProtocolConformanceRef(const RootProtocolConformance *conformance);
  void appendConcreteProtocolConformance(
                                        const ProtocolConformance *conformance);
  void appendDependentProtocolConformance(const ConformanceAccessPath &path);
  void appendOpParamForLayoutConstraint(LayoutConstraint Layout);
  
  void appendSymbolicReference(SymbolicReferent referent);
  
  void appendOpaqueDeclName(const OpaqueTypeDecl *opaqueDecl);
};

} // end namespace Mangle
} // end namespace swift

#endif // SWIFT_AST_ASTMANGLER_H
