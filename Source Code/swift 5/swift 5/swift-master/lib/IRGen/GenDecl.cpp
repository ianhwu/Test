//===--- GenDecl.cpp - IR Generation for Declarations ---------------------===//
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
//  This file implements IR generation for local and global
//  declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/IRGen/Linking.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "Callee.h"
#include "ConformanceDescription.h"
#include "ConstantBuilder.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenCall.h"
#include "GenClass.h"
#include "GenDecl.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenOpaque.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "MetadataRequest.h"
#include "ProtocolInfo.h"
#include "Signature.h"
#include "StructLayout.h"

using namespace swift;
using namespace irgen;

llvm::cl::opt<bool> UseBasicDynamicReplacement(
    "basic-dynamic-replacement", llvm::cl::init(false),
    llvm::cl::desc("Basic implementation of dynamic replacement"));

namespace {
  
/// Add methods, properties, and protocol conformances from a JITed extension
/// to an ObjC class using the ObjC runtime.
///
/// This must happen after ObjCProtocolInitializerVisitor if any @objc protocols
/// were defined in the TU.
class CategoryInitializerVisitor
  : public ClassMemberVisitor<CategoryInitializerVisitor>
{
  IRGenFunction &IGF;
  IRGenModule &IGM = IGF.IGM;
  IRBuilder &Builder = IGF.Builder;
  
  llvm::Constant *class_replaceMethod;
  llvm::Constant *class_addProtocol;
  
  llvm::Value *classMetadata;
  llvm::Constant *metaclassMetadata;
  
public:
  CategoryInitializerVisitor(IRGenFunction &IGF, ExtensionDecl *ext)
    : IGF(IGF)
  {
    class_replaceMethod = IGM.getClassReplaceMethodFn();
    class_addProtocol = IGM.getClassAddProtocolFn();

    CanType origTy = ext->getSelfNominalTypeDecl()
        ->getDeclaredType()->getCanonicalType();
    classMetadata = emitClassHeapMetadataRef(IGF, origTy,
                                             MetadataValueType::ObjCClass,
                                             MetadataState::Complete,
                                             /*allow uninitialized*/ false);
    classMetadata = Builder.CreateBitCast(classMetadata, IGM.ObjCClassPtrTy);
    metaclassMetadata = IGM.getAddrOfMetaclassObject(
                                       origTy.getClassOrBoundGenericClass(),
                                                         NotForDefinition);
    metaclassMetadata = llvm::ConstantExpr::getBitCast(metaclassMetadata,
                                                   IGM.ObjCClassPtrTy);

    // Register ObjC protocol conformances.
    for (auto *p : ext->getLocalProtocols()) {
      if (!p->isObjC())
        continue;
      
      llvm::Value *protoRef = IGM.getAddrOfObjCProtocolRef(p, NotForDefinition);
      auto proto = Builder.CreateLoad(protoRef, IGM.getPointerAlignment());
      Builder.CreateCall(class_addProtocol, {classMetadata, proto});
    }
  }
  
  void visitMembers(ExtensionDecl *ext) {
    for (Decl *member : ext->getMembers())
      visit(member);
  }

  void visitTypeDecl(TypeDecl *type) {
    // We'll visit nested types separately if necessary.
  }

  void visitMissingMemberDecl(MissingMemberDecl *placeholder) {}
  
  void visitFuncDecl(FuncDecl *method) {
    if (!requiresObjCMethodDescriptor(method)) return;

    // Don't emit getters/setters for @NSManaged methods.
    if (method->getAttrs().hasAttribute<NSManagedAttr>())
      return;

    llvm::Constant *name, *imp, *types;
    emitObjCMethodDescriptorParts(IGM, method, /*concrete*/true,
                                  name, types, imp);
    
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);
    
    llvm::Value *args[] = {
      method->isStatic() ? metaclassMetadata : classMetadata,
      sel,
      imp,
      types
    };
    
    Builder.CreateCall(class_replaceMethod, args);
  }

  // Can't be added in an extension.
  void visitDestructorDecl(DestructorDecl *dtor) {}

  void visitConstructorDecl(ConstructorDecl *constructor) {
    if (!requiresObjCMethodDescriptor(constructor)) return;
    llvm::Constant *name, *imp, *types;
    emitObjCMethodDescriptorParts(IGM, constructor, /*concrete*/true,
                                  name, types, imp);

    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);

    llvm::Value *args[] = {
      classMetadata,
      sel,
      imp,
      types
    };

    Builder.CreateCall(class_replaceMethod, args);
  }

  void visitPatternBindingDecl(PatternBindingDecl *binding) {
    // Ignore the PBD and just handle the individual vars.
  }
  
  void visitVarDecl(VarDecl *prop) {
    if (!requiresObjCPropertyDescriptor(IGM, prop)) return;

    // FIXME: register property metadata in addition to the methods.
    // ObjC doesn't have a notion of class properties, so we'd only do this
    // for instance properties.

    // Don't emit getters/setters for @NSManaged properties.
    if (prop->getAttrs().hasAttribute<NSManagedAttr>())
      return;

    llvm::Constant *name, *imp, *types;
    emitObjCGetterDescriptorParts(IGM, prop,
                                  name, types, imp);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);
    auto theClass = prop->isStatic() ? metaclassMetadata : classMetadata;
    llvm::Value *getterArgs[] = {theClass, sel, imp, types};
    Builder.CreateCall(class_replaceMethod, getterArgs);

    if (prop->isSettable(prop->getDeclContext())) {
      emitObjCSetterDescriptorParts(IGM, prop,
                                    name, types, imp);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                               name);
      llvm::Value *setterArgs[] = {theClass, sel, imp, types};
      
      Builder.CreateCall(class_replaceMethod, setterArgs);
    }
  }

  void visitSubscriptDecl(SubscriptDecl *subscript) {
    assert(!subscript->isStatic() && "objc doesn't support class subscripts");
    if (!requiresObjCSubscriptDescriptor(IGM, subscript)) return;
    
    llvm::Constant *name, *imp, *types;
    emitObjCGetterDescriptorParts(IGM, subscript,
                                  name, types, imp);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);
    llvm::Value *getterArgs[] = {classMetadata, sel, imp, types};
    Builder.CreateCall(class_replaceMethod, getterArgs);

    if (subscript->supportsMutation()) {
      emitObjCSetterDescriptorParts(IGM, subscript,
                                    name, types, imp);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                               name);
      llvm::Value *setterArgs[] = {classMetadata, sel, imp, types};
      
      Builder.CreateCall(class_replaceMethod, setterArgs);
    }
  }
};

/// Create a descriptor for JITed @objc protocol using the ObjC runtime.
class ObjCProtocolInitializerVisitor
  : public ClassMemberVisitor<ObjCProtocolInitializerVisitor>
{
  IRGenFunction &IGF;
  IRGenModule &IGM = IGF.IGM;
  IRBuilder &Builder = IGF.Builder;

  llvm::Constant *objc_getProtocol,
                 *objc_allocateProtocol,
                 *objc_registerProtocol,
                 *protocol_addMethodDescription,
                 *protocol_addProtocol;
  
  llvm::Value *NewProto = nullptr;
  
public:
  ObjCProtocolInitializerVisitor(IRGenFunction &IGF)
    : IGF(IGF)
  {
    objc_getProtocol = IGM.getGetObjCProtocolFn();
    objc_allocateProtocol = IGM.getAllocateObjCProtocolFn();
    objc_registerProtocol = IGM.getRegisterObjCProtocolFn();
    protocol_addMethodDescription = IGM.getProtocolAddMethodDescriptionFn();
    protocol_addProtocol = IGM.getProtocolAddProtocolFn();
  }
  
  void visitMembers(ProtocolDecl *proto) {
    // Check if the ObjC runtime already has a descriptor for this
    // protocol. If so, use it.
    SmallString<32> buf;
    auto protocolName
      = IGM.getAddrOfGlobalString(proto->getObjCRuntimeName(buf));
    
    auto existing = Builder.CreateCall(objc_getProtocol, protocolName);
    auto isNull = Builder.CreateICmpEQ(existing,
                   llvm::ConstantPointerNull::get(IGM.ProtocolDescriptorPtrTy));

    auto existingBB = IGF.createBasicBlock("existing_protocol");
    auto newBB = IGF.createBasicBlock("new_protocol");
    auto contBB = IGF.createBasicBlock("cont");
    Builder.CreateCondBr(isNull, newBB, existingBB);
    
    // Nothing to do if there's already a descriptor.
    Builder.emitBlock(existingBB);
    Builder.CreateBr(contBB);
    
    Builder.emitBlock(newBB);
    
    // Allocate the protocol descriptor.
    NewProto = Builder.CreateCall(objc_allocateProtocol, protocolName);
    
    // Add the parent protocols.
    for (auto parentProto : proto->getInheritedProtocols()) {
      if (!parentProto->isObjC())
        continue;
      llvm::Value *parentRef = IGM.getAddrOfObjCProtocolRef(parentProto,
                                                            NotForDefinition);
      parentRef = IGF.Builder.CreateBitCast(parentRef,
                                            IGM.ProtocolDescriptorPtrTy
                                              ->getPointerTo());
      auto parent = Builder.CreateLoad(parentRef,
                                       IGM.getPointerAlignment());
      Builder.CreateCall(protocol_addProtocol, {NewProto, parent});
    }
    
    // Add the members.
    for (Decl *member : proto->getMembers())
      visit(member);
    
    // Register it.
    Builder.CreateCall(objc_registerProtocol, NewProto);
    Builder.CreateBr(contBB);
    
    // Store the reference to the runtime's idea of the protocol descriptor.
    Builder.emitBlock(contBB);
    auto result = Builder.CreatePHI(IGM.ProtocolDescriptorPtrTy, 2);
    result->addIncoming(existing, existingBB);
    result->addIncoming(NewProto, newBB);
    
    llvm::Value *ref = IGM.getAddrOfObjCProtocolRef(proto, NotForDefinition);
    ref = IGF.Builder.CreateBitCast(ref,
                                  IGM.ProtocolDescriptorPtrTy->getPointerTo());

    Builder.CreateStore(result, ref, IGM.getPointerAlignment());
  }

  void visitTypeDecl(TypeDecl *type) {
    // We'll visit nested types separately if necessary.
  }

  void visitMissingMemberDecl(MissingMemberDecl *placeholder) {}

  void visitAbstractFunctionDecl(AbstractFunctionDecl *method) {
    if (isa<AccessorDecl>(method)) {
      // Accessors are handled as part of their AbstractStorageDecls.
      return;
    }

    llvm::Constant *name, *imp, *types;
    emitObjCMethodDescriptorParts(IGM, method, /*concrete*/false,
                                  name, types, imp);
    
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), name);

    llvm::Value *args[] = {
      NewProto, sel, types,
      // required?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             !method->getAttrs().hasAttribute<OptionalAttr>()),
      // instance?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                   isa<ConstructorDecl>(method) || method->isInstanceMember()),
    };
    
    Builder.CreateCall(protocol_addMethodDescription, args);
  }
  
  void visitPatternBindingDecl(PatternBindingDecl *binding) {
    // Ignore the PBD and just handle the individual vars.
  }
  
  void visitAbstractStorageDecl(AbstractStorageDecl *prop) {
    // TODO: Add properties to protocol.
    
    llvm::Constant *name, *imp, *types;
    emitObjCGetterDescriptorParts(IGM, prop,
                                  name, types, imp);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), name);
    llvm::Value *getterArgs[] = {
      NewProto, sel, types,
      // required?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             !prop->getAttrs().hasAttribute<OptionalAttr>()),
      // instance?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             prop->isInstanceMember()),
    };
    Builder.CreateCall(protocol_addMethodDescription, getterArgs);
    
    if (prop->isSettable(nullptr)) {
      emitObjCSetterDescriptorParts(IGM, prop,
                                    name, types, imp);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), name);
      llvm::Value *setterArgs[] = {
        NewProto, sel, types,
        // required?
        llvm::ConstantInt::get(IGM.ObjCBoolTy,
                               !prop->getAttrs().hasAttribute<OptionalAttr>()),
        // instance?
        llvm::ConstantInt::get(IGM.ObjCBoolTy,
                               prop->isInstanceMember()),
      };
      Builder.CreateCall(protocol_addMethodDescription, setterArgs);
    }
  }
};

} // end anonymous namespace

namespace {
class PrettySourceFileEmission : public llvm::PrettyStackTraceEntry {
  const SourceFile &SF;
public:
  explicit PrettySourceFileEmission(const SourceFile &SF) : SF(SF) {}

  void print(raw_ostream &os) const override {
    os << "While emitting IR for source file " << SF.getFilename() << '\n';
  }
};
} // end anonymous namespace

/// Emit all the top-level code in the source file.
void IRGenModule::emitSourceFile(SourceFile &SF) {
  PrettySourceFileEmission StackEntry(SF);
  llvm::SaveAndRestore<SourceFile *> SetCurSourceFile(CurSourceFile, &SF);

  // Emit types and other global decls.
  for (auto *decl : SF.Decls)
    emitGlobalDecl(decl);
  for (auto *localDecl : SF.LocalTypeDecls)
    emitGlobalDecl(localDecl);
  for (auto *opaqueDecl : SF.getOpaqueReturnTypeDecls())
    maybeEmitOpaqueTypeDecl(opaqueDecl);

  SF.collectLinkLibraries([this](LinkLibrary linkLib) {
      this->addLinkLibrary(linkLib);
  });

  if (ObjCInterop)
    this->addLinkLibrary(LinkLibrary("objc", LibraryKind::Library));
  
  // FIXME: It'd be better to have the driver invocation or build system that
  // executes the linker introduce these compatibility libraries, since at
  // that point we know whether we're building an executable, which is the only
  // place where the compatibility libraries take effect. For the benefit of
  // build systems that build Swift code, but don't use Swift to drive
  // the linker, we can also use autolinking to pull in the compatibility
  // libraries. This may however cause the library to get pulled in in
  // situations where it isn't useful, such as for dylibs, though this is
  // harmless aside from code size.
  if (!IRGen.Opts.UseJIT) {
    if (auto compatibilityVersion
          = IRGen.Opts.AutolinkRuntimeCompatibilityLibraryVersion) {
      if (*compatibilityVersion <= llvm::VersionTuple(5, 0)) {
        this->addLinkLibrary(LinkLibrary("swiftCompatibility50",
                                         LibraryKind::Library,
                                         /*forceLoad*/ true));
      }
    }

    if (auto compatibilityVersion =
            IRGen.Opts.AutolinkRuntimeCompatibilityDynamicReplacementLibraryVersion) {
      if (*compatibilityVersion <= llvm::VersionTuple(5, 0)) {
        this->addLinkLibrary(LinkLibrary("swiftCompatibilityDynamicReplacements",
                                         LibraryKind::Library,
                                         /*forceLoad*/ true));
      }
    }
  }
}

/// Collect elements of an already-existing global list with the given
/// \c name into \c list.
///
/// We use this when Clang code generation might populate the list.
static void collectGlobalList(IRGenModule &IGM,
                              SmallVectorImpl<llvm::WeakTrackingVH> &list,
                              StringRef name) {
  if (auto *existing = IGM.Module.getGlobalVariable(name)) {
    auto *globals = cast<llvm::ConstantArray>(existing->getInitializer());
    for (auto &use : globals->operands()) {
      auto *global = use.get();
      list.push_back(global);
    }
    existing->eraseFromParent();
  }

  std::for_each(list.begin(), list.end(),
                [](const llvm::WeakTrackingVH &global) {
    assert(!isa<llvm::GlobalValue>(global) ||
           !cast<llvm::GlobalValue>(global)->isDeclaration() &&
           "all globals in the 'used' list must be definitions");
  });
}

/// Emit a global list, i.e. a global constant array holding all of a
/// list of values.  Generally these lists are for various LLVM
/// metadata or runtime purposes.
static llvm::GlobalVariable *
emitGlobalList(IRGenModule &IGM, ArrayRef<llvm::WeakTrackingVH> handles,
               StringRef name, StringRef section,
               llvm::GlobalValue::LinkageTypes linkage,
               llvm::Type *eltTy,
               bool isConstant) {
  // Do nothing if the list is empty.
  if (handles.empty()) return nullptr;

  // For global lists that actually get linked (as opposed to notional
  // ones like @llvm.used), it's important to set an explicit alignment
  // so that the linker doesn't accidentally put padding in the list.
  Alignment alignment = IGM.getPointerAlignment();

  // We have an array of value handles, but we need an array of constants.
  SmallVector<llvm::Constant*, 8> elts;
  elts.reserve(handles.size());
  for (auto &handle : handles) {
    auto elt = cast<llvm::Constant>(&*handle);
    if (elt->getType() != eltTy)
      elt = llvm::ConstantExpr::getBitCast(elt, eltTy);
    elts.push_back(elt);
  }

  auto varTy = llvm::ArrayType::get(eltTy, elts.size());
  auto init = llvm::ConstantArray::get(varTy, elts);
  auto var = new llvm::GlobalVariable(IGM.Module, varTy, isConstant, linkage,
                                      init, name);
  var->setSection(section);
  var->setAlignment(alignment.getValue());
  disableAddressSanitizer(IGM, var);

  // Mark the variable as used if doesn't have external linkage.
  // (Note that we'd specifically like to not put @llvm.used in itself.)
  if (llvm::GlobalValue::isLocalLinkage(linkage))
    IGM.addUsedGlobal(var);
  return var;
}

void IRGenModule::emitRuntimeRegistration() {
  // Duck out early if we have nothing to register.
  if (SwiftProtocols.empty() && ProtocolConformances.empty() &&
      RuntimeResolvableTypes.empty() &&
      (!ObjCInterop || (ObjCProtocols.empty() && ObjCClasses.empty() &&
                        ObjCCategoryDecls.empty())) &&
      FieldDescriptors.empty())
    return;
  
  // Find the entry point.
  SILFunction *EntryPoint =
    getSILModule().lookUpFunction(SWIFT_ENTRY_POINT_FUNCTION);
  
  // If we're debugging (and not in the REPL), we don't have a
  // main. Find a function marked with the LLDBDebuggerFunction
  // attribute instead.
  if (!EntryPoint && Context.LangOpts.DebuggerSupport) {
    for (SILFunction &SF : getSILModule()) {
      if (SF.hasLocation()) {
        if (Decl* D = SF.getLocation().getAsASTNode<Decl>()) {
          if (auto *FD = dyn_cast<FuncDecl>(D)) {
            if (FD->getAttrs().hasAttribute<LLDBDebuggerFunctionAttr>()) {
              EntryPoint = &SF;
              break;
            }
          }
        }
      }
    }
  }
  
  if (!EntryPoint)
    return;
    
  llvm::Function *EntryFunction = Module.getFunction(EntryPoint->getName());
  if (!EntryFunction)
    return;
  
  // Create a new function to contain our logic.
  auto fnTy = llvm::FunctionType::get(VoidTy, /*varArg*/ false);
  auto RegistrationFunction = llvm::Function::Create(fnTy,
                                           llvm::GlobalValue::PrivateLinkage,
                                           "runtime_registration",
                                           getModule());
  RegistrationFunction->setAttributes(constructInitialAttributes());
  
  // Insert a call into the entry function.
  {
    llvm::BasicBlock *EntryBB = &EntryFunction->getEntryBlock();
    llvm::BasicBlock::iterator IP = EntryBB->getFirstInsertionPt();
    IRBuilder Builder(getLLVMContext(),
                      DebugInfo && !Context.LangOpts.DebuggerSupport);
    Builder.llvm::IRBuilderBase::SetInsertPoint(EntryBB, IP);
    if (DebugInfo && !Context.LangOpts.DebuggerSupport)
      DebugInfo->setEntryPointLoc(Builder);
    Builder.CreateCall(RegistrationFunction, {});
  }
  
  IRGenFunction RegIGF(*this, RegistrationFunction);
  if (DebugInfo && !Context.LangOpts.DebuggerSupport)
    DebugInfo->emitArtificialFunction(RegIGF, RegistrationFunction);
  
  // Register ObjC protocols we added.
  if (ObjCInterop) {
    if (!ObjCProtocols.empty()) {
      // We need to initialize ObjC protocols in inheritance order, parents
      // first.
      
      llvm::DenseSet<ProtocolDecl*> protos;
      for (auto &proto : ObjCProtocols)
        protos.insert(proto.first);
      
      llvm::SmallVector<ProtocolDecl*, 4> protoInitOrder;

      std::function<void(ProtocolDecl*)> orderProtocol
        = [&](ProtocolDecl *proto) {
          // Recursively put parents first.
          for (auto parent : proto->getInheritedProtocols())
            orderProtocol(parent);

          // Skip if we don't need to reify this protocol.
          auto found = protos.find(proto);
          if (found == protos.end())
            return;
          protos.erase(found);
          protoInitOrder.push_back(proto);
        };
      
      while (!protos.empty()) {
        orderProtocol(*protos.begin());
      }

      // Visit the protocols in the order we established.
      for (auto *proto : protoInitOrder) {
        ObjCProtocolInitializerVisitor(RegIGF)
          .visitMembers(proto);
      }
    }
  }

  // Register Swift protocols if we added any.
  if (!SwiftProtocols.empty()) {
    llvm::Constant *protocols = emitSwiftProtocols();

    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto begin = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, protocols, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, SwiftProtocols.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, protocols, endIndices);

    RegIGF.Builder.CreateCall(getRegisterProtocolsFn(), {begin, end});
  }

  // Register Swift protocol conformances if we added any.
  if (llvm::Constant *conformances = emitProtocolConformances()) {
    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto begin = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, conformances, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, ProtocolConformances.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, conformances, endIndices);
    
    RegIGF.Builder.CreateCall(getRegisterProtocolConformancesFn(), {begin, end});
  }

  if (!RuntimeResolvableTypes.empty()) {
    llvm::Constant *records = emitTypeMetadataRecords();

    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto begin = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, records, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, RuntimeResolvableTypes.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, records, endIndices);

    RegIGF.Builder.CreateCall(getRegisterTypeMetadataRecordsFn(), {begin, end});
  }

  // Register Objective-C classes and extensions we added.
  if (ObjCInterop) {
    for (llvm::WeakTrackingVH &ObjCClass : ObjCClasses) {
      RegIGF.Builder.CreateCall(getInstantiateObjCClassFn(), {ObjCClass});
    }

    for (ExtensionDecl *ext : ObjCCategoryDecls) {
      CategoryInitializerVisitor(RegIGF, ext).visitMembers(ext);
    }
  }

  if (!FieldDescriptors.empty()) {
    emitFieldDescriptors();
  }

  RegIGF.Builder.CreateRetVoid();
}

/// Return the address of the context descriptor representing the given
/// decl context, used as a parent reference for another decl.
///
/// For a nominal type context, this returns the address of the nominal type
/// descriptor.
/// For an extension context, this returns the address of the extension
/// context descriptor.
/// For a module or file unit context, this returns the address of the module
/// context descriptor.
/// For any other kind of context, this returns an anonymous context descriptor
/// for the context.
ConstantReference
IRGenModule::getAddrOfContextDescriptorForParent(DeclContext *parent,
                                                 DeclContext *ofChild,
                                                 bool fromAnonymousContext) {
  switch (parent->getContextKind()) {
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::AbstractFunctionDecl:
  case DeclContextKind::SubscriptDecl:
  case DeclContextKind::EnumElementDecl:
  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::Initializer:
  case DeclContextKind::SerializedLocal:
    return {getAddrOfAnonymousContextDescriptor(
              fromAnonymousContext ? parent : ofChild),
            ConstantReference::Direct};

  case DeclContextKind::GenericTypeDecl:
    if (auto nomTy = dyn_cast<NominalTypeDecl>(parent)) {
      return {getAddrOfTypeContextDescriptor(nomTy, DontRequireMetadata),
              ConstantReference::Direct};
    }
    return {getAddrOfAnonymousContextDescriptor(
              fromAnonymousContext ? parent : ofChild),
            ConstantReference::Direct};

  case DeclContextKind::ExtensionDecl: {
    auto ext = cast<ExtensionDecl>(parent);
    // If the extension is equivalent to its extended context (that is, it's
    // in the same module as the original non-protocol type and
    // has no constraints), then we can use the original nominal type context
    // (assuming there is one).
    if (ext->isEquivalentToExtendedContext()) {
      auto nominal = ext->getExtendedNominal();
      // If the extended type is an ObjC class, it won't have a nominal type
      // descriptor, so we'll just emit an extension context.
      auto clas = dyn_cast<ClassDecl>(nominal);
      if (!clas || clas->isForeign() || hasKnownSwiftMetadata(*this, clas)) {
        // Some targets don't support relative references to undefined symbols.
        // If the extension is in a different file from the original type
        // declaration, it may not get emitted in this TU. Use an indirect
        // reference to work around the object format limitation.
        auto shouldBeIndirect =
            parent->getModuleScopeContext() != ofChild->getModuleScopeContext()
          ? ConstantReference::Indirect
          : ConstantReference::Direct;
        
        IRGen.noteUseOfTypeContextDescriptor(nominal, DontRequireMetadata);
        return getAddrOfLLVMVariableOrGOTEquivalent(
                                LinkEntity::forNominalTypeDescriptor(nominal),
                                shouldBeIndirect);
      }
    }
    return {getAddrOfExtensionContextDescriptor(ext),
            ConstantReference::Direct};
  }
      
  case DeclContextKind::FileUnit:
    parent = parent->getParentModule();
    LLVM_FALLTHROUGH;
      
  case DeclContextKind::Module:
    return {getAddrOfModuleContextDescriptor(cast<ModuleDecl>(parent)),
            ConstantReference::Direct};
  }
  llvm_unreachable("unhandled kind");
}

/// Return the address of the context descriptor representing the parent of
/// the given decl context.
///
/// For a nominal type context, this returns the address of the nominal type
/// descriptor.
/// For an extension context, this returns the address of the extension
/// context descriptor.
/// For a module or file unit context, this returns the address of the module
/// context descriptor.
/// For any other kind of context, this returns an anonymous context descriptor
/// for the context.
ConstantReference
IRGenModule::getAddrOfParentContextDescriptor(DeclContext *from,
                                              bool fromAnonymousContext) {
  // Some types get special treatment.
  if (auto Type = dyn_cast<NominalTypeDecl>(from)) {
    // Use a special module context if we have one.
    if (auto context =
            Mangle::ASTMangler::getSpecialManglingContext(
              Type, /*UseObjCProtocolNames=*/false)) {
      switch (*context) {
      case Mangle::ASTMangler::ObjCContext:
        return {getAddrOfObjCModuleContextDescriptor(),
                ConstantReference::Direct};
      case Mangle::ASTMangler::ClangImporterContext:
        return {getAddrOfClangImporterModuleContextDescriptor(),
                ConstantReference::Direct};
      }
    }

    // Wrap up private types in an anonymous context for the containing file
    // unit so that the runtime knows they have unstable identity.
    if (!fromAnonymousContext && Type->isOutermostPrivateOrFilePrivateScope())
      return {getAddrOfAnonymousContextDescriptor(Type),
              ConstantReference::Direct};
  }
  
  return getAddrOfContextDescriptorForParent(from->getParent(), from,
                                             fromAnonymousContext);
}

/// Add the given global value to @llvm.used.
///
/// This value must have a definition by the time the module is finalized.
void IRGenModule::addUsedGlobal(llvm::GlobalValue *global) {
  LLVMUsed.push_back(global);
}

/// Add the given global value to @llvm.compiler.used.
///
/// This value must have a definition by the time the module is finalized.
void IRGenModule::addCompilerUsedGlobal(llvm::GlobalValue *global) {
  LLVMCompilerUsed.push_back(global);
}

/// Add the given global value to the Objective-C class list.
void IRGenModule::addObjCClass(llvm::Constant *classPtr, bool nonlazy) {
  ObjCClasses.push_back(classPtr);
  if (nonlazy)
    ObjCNonLazyClasses.push_back(classPtr);
}

void IRGenModule::addRuntimeResolvableType(GenericTypeDecl *type) {
  // Collect the nominal type records we emit into a special section.
  RuntimeResolvableTypes.push_back(type);

  if (auto nominal = dyn_cast<NominalTypeDecl>(type)) {
    // As soon as the type metadata is available, all the type's conformances
    // must be available, too. The reason is that a type (with the help of its
    // metadata) can be checked at runtime if it conforms to a protocol.
    addLazyConformances(nominal);
  }
}

ConstantReference
IRGenModule::getConstantReferenceForProtocolDescriptor(ProtocolDecl *proto) {
  if (proto->isObjC()) {
    // ObjC protocol descriptors don't have a unique address, but get uniqued
    // by the Objective-C runtime at load time.
    // Get the indirected address of the protocol descriptor reference variable
    // that the ObjC runtime uniques.
    auto refVar = getAddrOfObjCProtocolRef(proto, NotForDefinition);
    return ConstantReference(refVar, ConstantReference::Indirect);
  }
  
  // Try to form a direct reference to the nominal type descriptor if it's in
  // the same binary, or use the GOT entry if it's from another binary.
  return getAddrOfLLVMVariableOrGOTEquivalent(
                                     LinkEntity::forProtocolDescriptor(proto));
}

void IRGenModule::addLazyConformances(DeclContext *dc) {
  for (const ProtocolConformance *conf :
         dc->getLocalConformances(ConformanceLookupKind::All, nullptr)) {
    IRGen.addLazyWitnessTable(conf);
  }
}

std::string IRGenModule::GetObjCSectionName(StringRef Section,
                                            StringRef MachOAttributes) {
  assert(Section.substr(0, 2) == "__" && "expected the name to begin with __");

  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("must know the object file format");
  case llvm::Triple::MachO:
    return MachOAttributes.empty()
               ? ("__DATA," + Section).str()
               : ("__DATA," + Section + "," + MachOAttributes).str();
  case llvm::Triple::ELF:
    return Section.substr(2).str();
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    return ("." + Section.substr(2) + "$B").str();
  case llvm::Triple::Wasm:
    return Section.substr(2).str();
  }

  llvm_unreachable("unexpected object file format");
}

void IRGenModule::SetCStringLiteralSection(llvm::GlobalVariable *GV,
                                           ObjCLabelType Type) {
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("must know the object file format");
  case llvm::Triple::MachO:
    switch (Type) {
    case ObjCLabelType::ClassName:
      GV->setSection("__TEXT,__objc_classname,cstring_literals");
      return;
    case ObjCLabelType::MethodVarName:
      GV->setSection("__TEXT,__objc_methname,cstring_literals");
      return;
    case ObjCLabelType::MethodVarType:
      GV->setSection("__TEXT,__objc_methtype,cstring_literals");
      return;
    case ObjCLabelType::PropertyName:
      GV->setSection("__TEXT,__cstring,cstring_literals");
      return;
    }
  case llvm::Triple::ELF:
    return;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    return;
  case llvm::Triple::Wasm:
    return;
  }

  llvm_unreachable("unexpected object file format");
}

void IRGenModule::emitGlobalLists() {
  if (ObjCInterop) {
    // Objective-C class references go in a variable with a meaningless
    // name but a magic section.
    emitGlobalList(*this, ObjCClasses, "objc_classes",
                   GetObjCSectionName("__objc_classlist",
                                      "regular,no_dead_strip"),
                   llvm::GlobalValue::InternalLinkage, Int8PtrTy, false);

    // So do categories.
    emitGlobalList(*this, ObjCCategories, "objc_categories",
                   GetObjCSectionName("__objc_catlist",
                                      "regular,no_dead_strip"),
                   llvm::GlobalValue::InternalLinkage, Int8PtrTy, false);

    // And categories on class stubs.
    emitGlobalList(*this, ObjCCategoriesOnStubs, "objc_categories_stubs",
                   GetObjCSectionName("__objc_catlist2",
                                      "regular,no_dead_strip"),
                   llvm::GlobalValue::InternalLinkage, Int8PtrTy, false);

    // Emit nonlazily realized class references in a second magic section to make
    // sure they are realized by the Objective-C runtime before any instances
    // are allocated.
    emitGlobalList(*this, ObjCNonLazyClasses, "objc_non_lazy_classes",
                   GetObjCSectionName("__objc_nlclslist",
                                      "regular,no_dead_strip"),
                   llvm::GlobalValue::InternalLinkage, Int8PtrTy, false);
  }

  // @llvm.used

  // Collect llvm.used globals already in the module (coming from ClangCodeGen).
  collectGlobalList(*this, LLVMUsed, "llvm.used");
  emitGlobalList(*this, LLVMUsed, "llvm.used", "llvm.metadata",
                 llvm::GlobalValue::AppendingLinkage,
                 Int8PtrTy,
                 false);

  // Collect llvm.compiler.used globals already in the module (coming
  // from ClangCodeGen).
  collectGlobalList(*this, LLVMCompilerUsed, "llvm.compiler.used");
  emitGlobalList(*this, LLVMCompilerUsed, "llvm.compiler.used", "llvm.metadata",
                 llvm::GlobalValue::AppendingLinkage,
                 Int8PtrTy,
                 false);
}

static bool hasCodeCoverageInstrumentation(SILFunction &f, SILModule &m) {
  return f.getProfiler() && m.getOptions().EmitProfileCoverageMapping;
}

void IRGenerator::emitGlobalTopLevel() {
  // Generate order numbers for the functions in the SIL module that
  // correspond to definitions in the LLVM module.
  unsigned nextOrderNumber = 0;
  for (auto &silFn : PrimaryIGM->getSILModule().getFunctions()) {
    // Don't bother adding external declarations to the function order.
    if (!silFn.isDefinition()) continue;
    FunctionOrder.insert(std::make_pair(&silFn, nextOrderNumber++));
  }

  // Ensure that relative symbols are collocated in the same LLVM module.
  for (auto &wt : PrimaryIGM->getSILModule().getWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getDeclContext());
    ensureRelativeSymbolCollocation(wt);
  }

  for (auto &wt : PrimaryIGM->getSILModule().getDefaultWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getProtocol()->getDeclContext());
    ensureRelativeSymbolCollocation(wt);
  }

  for (SILGlobalVariable &v : PrimaryIGM->getSILModule().getSILGlobals()) {
    Decl *decl = v.getDecl();
    CurrentIGMPtr IGM = getGenModule(decl ? decl->getDeclContext() : nullptr);
    IGM->emitSILGlobalVariable(&v);
  }
  
  // Emit SIL functions.
  for (SILFunction &f : PrimaryIGM->getSILModule()) {
    // Eagerly emit functions that are externally visible. Functions with code
    // coverage instrumentation must also be eagerly emitted. So must functions
    // that are a dynamic replacement for another.
    if (!f.isPossiblyUsedExternally() &&
        !f.getDynamicallyReplacedFunction() &&
        !hasCodeCoverageInstrumentation(f, PrimaryIGM->getSILModule()))
      continue;

    CurrentIGMPtr IGM = getGenModule(&f);
    IGM->emitSILFunction(&f);
  }

  // Emit static initializers.
  for (auto Iter : *this) {
    IRGenModule *IGM = Iter.second;
    IGM->emitSILStaticInitializers();
  }

  // Emit witness tables.
  for (SILWitnessTable &wt : PrimaryIGM->getSILModule().getWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getDeclContext());
    if (!canEmitWitnessTableLazily(&wt)) {
      IGM->emitSILWitnessTable(&wt);
    }
  }
  
  // Emit property descriptors.
  for (auto &prop : PrimaryIGM->getSILModule().getPropertyList()) {
    CurrentIGMPtr IGM = getGenModule(prop.getDecl()->getInnermostDeclContext());
    IGM->emitSILProperty(&prop);
  }
  
  // Emit code coverage mapping data.
  PrimaryIGM->emitCoverageMapping();

  for (auto Iter : *this) {
    IRGenModule *IGM = Iter.second;
    IGM->finishEmitAfterTopLevel();
  }
}

void IRGenModule::finishEmitAfterTopLevel() {
  // Emit the implicit import of the swift standard library.
  // FIXME: We'd get the exact set of implicit imports if we went through the
  // SourceFile's getImportedModules instead, but then we'd lose location info
  // for the explicit imports.
  if (DebugInfo) {
    if (ModuleDecl *TheStdlib = Context.getStdlibModule()) {
      if (TheStdlib != getSwiftModule()) {
        std::pair<swift::Identifier, swift::SourceLoc> AccessPath[] = {
          { Context.StdlibModuleName, swift::SourceLoc() }
        };

        auto Imp = ImportDecl::create(Context,
                                      getSwiftModule(),
                                      SourceLoc(),
                                      ImportKind::Module, SourceLoc(),
                                      AccessPath);
        Imp->setModule(TheStdlib);
        DebugInfo->emitImport(Imp);
      }
    }
  }
}

void IRGenerator::emitSwiftProtocols() {
  for (auto &m : *this) {
    m.second->emitSwiftProtocols();
  }
}

void IRGenerator::emitProtocolConformances() {
  for (auto &m : *this) {
    m.second->emitProtocolConformances();
  }
}

void IRGenerator::emitTypeMetadataRecords() {
  for (auto &m : *this) {
    m.second->emitTypeMetadataRecords();
  }
}

/// Emit any lazy definitions (of globals or functions or whatever
/// else) that we require.
void IRGenerator::emitLazyDefinitions() {
  while (!LazyTypeMetadata.empty() ||
         !LazyTypeContextDescriptors.empty() ||
         !LazyOpaqueTypeDescriptors.empty() ||
         !LazyFieldDescriptors.empty() ||
         !LazyFunctionDefinitions.empty() ||
         !LazyWitnessTables.empty()) {

    // Emit any lazy type metadata we require.
    while (!LazyTypeMetadata.empty()) {
      NominalTypeDecl *type = LazyTypeMetadata.pop_back_val();
      auto &entry = LazyTypeGlobals.find(type)->second;
      assert(hasLazyMetadata(type));
      assert(entry.IsMetadataUsed && !entry.IsMetadataEmitted);
      entry.IsMetadataEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      emitLazyTypeMetadata(*IGM.get(), type);
    }
    while (!LazyTypeContextDescriptors.empty()) {
      NominalTypeDecl *type = LazyTypeContextDescriptors.pop_back_val();
      auto &entry = LazyTypeGlobals.find(type)->second;
      assert(hasLazyMetadata(type));
      assert(entry.IsDescriptorUsed && !entry.IsDescriptorEmitted);
      entry.IsDescriptorEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      emitLazyTypeContextDescriptor(*IGM.get(), type,
                                    RequireMetadata_t(entry.IsMetadataUsed));
    }
    while (!LazyOpaqueTypeDescriptors.empty()) {
      OpaqueTypeDecl *type = LazyOpaqueTypeDescriptors.pop_back_val();
      auto &entry = LazyOpaqueTypes.find(type)->second;
      assert(hasLazyMetadata(type));
      assert(entry.IsDescriptorUsed && !entry.IsDescriptorEmitted);
      entry.IsDescriptorEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      IGM->emitOpaqueTypeDecl(type);
    }
    while (!LazyFieldDescriptors.empty()) {
      NominalTypeDecl *type = LazyFieldDescriptors.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      IGM->emitFieldDescriptor(type);
    }
    while (!LazyWitnessTables.empty()) {
      SILWitnessTable *wt = LazyWitnessTables.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(wt->getDeclContext());
      IGM->emitSILWitnessTable(wt);
    }

    // Emit any lazy function definitions we require.
    while (!LazyFunctionDefinitions.empty()) {
      SILFunction *f = LazyFunctionDefinitions.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(f);
      assert(!f->isPossiblyUsedExternally()
             && "function with externally-visible linkage emitted lazily?");
      IGM->emitSILFunction(f);
    }
  }

  FinishedEmittingLazyDefinitions = true;
}

void IRGenerator::addLazyFunction(SILFunction *f) {
  // Add it to the queue if it hasn't already been put there.
  if (!LazilyEmittedFunctions.insert(f).second)
    return;

  assert(!FinishedEmittingLazyDefinitions);
  LazyFunctionDefinitions.push_back(f);

  if (auto *dc = f->getDeclContext())
    if (dc->getParentSourceFile())
      return;

  if (CurrentIGM == nullptr)
    return;

  // Don't update the map if we already have an entry.
  DefaultIGMForFunction.insert(std::make_pair(f, CurrentIGM));
}

bool IRGenerator::hasLazyMetadata(TypeDecl *type) {
  assert(isa<NominalTypeDecl>(type) ||
         isa<OpaqueTypeDecl>(type));
  auto found = HasLazyMetadata.find(type);
  if (found != HasLazyMetadata.end())
    return found->second;

  auto canBeLazy = [&]() -> bool {
    auto *dc = type->getDeclContext();
    if (isa<ClangModuleUnit>(dc->getModuleScopeContext())) {
      if (auto nominal = dyn_cast<NominalTypeDecl>(type)) {
        return requiresForeignTypeMetadata(nominal);
      }
    } else if (dc->getParentModule() == SIL.getSwiftModule()) {
      // When compiling with -Onone keep all metadata for the debugger. Even if
      // it is not used by the program itself.
      if (!Opts.shouldOptimize())
        return false;
      if (Opts.UseJIT)
        return false;

      if (isa<ClassDecl>(type) || isa<ProtocolDecl>(type))
        return false;

      switch (type->getEffectiveAccess()) {
      case AccessLevel::Open:
      case AccessLevel::Public:
        // We can't remove metadata for externally visible types.
        return false;
      case AccessLevel::Internal:
        // In non-whole-module mode, internal types are also visible externally.
        return SIL.isWholeModule();
      case AccessLevel::FilePrivate:
      case AccessLevel::Private:
        return true;
      }
    }

    return false;
  };

  bool isLazy = canBeLazy();
  HasLazyMetadata[type] = isLazy;

  return isLazy;
}

void IRGenerator::noteUseOfTypeGlobals(NominalTypeDecl *type,
                                       bool isUseOfMetadata,
                                       RequireMetadata_t requireMetadata) {
  if (!type)
    return;
  
  // Force emission of ObjC protocol descriptors used by type refs.
  if (auto proto = dyn_cast<ProtocolDecl>(type)) {
    if (proto->isObjC()) {
      PrimaryIGM->getAddrOfObjCProtocolRecord(proto, NotForDefinition);
      return;
    }
  }

  // Force emission of ObjC class refs used by type refs.
  //
  // Otherwise, autolinking can fail if we try to load the class decl from the
  // name of the class.
  if (auto *classDecl = dyn_cast<ClassDecl>(type)) {
    // The logic behind this predicate is that:
    //
    // 1. We need to check if a class decl is foreign to exclude CF classes.
    //
    // 2. We want to check that the class decl is objc to exclude c++ classes in
    //    the future.
    //
    // 3. We check that we have a clang node since we want to ensure we have
    //    something coming from clang, rather than from swift which does not
    //    have this issue.
    if (classDecl->hasClangNode() && classDecl->isObjC() && !classDecl->isForeign()) {
      PrimaryIGM->getAddrOfObjCClassRef(classDecl);
      return;
    }
  }

  if (!hasLazyMetadata(type))
    return;

  // If the type can be generated in several TU with weak linkage we don't know
  // which one will be picked up so we have to require the metadata. Otherwise,
  // the situation can arise where one TU contains a type descriptor with a null
  // metadata access function and the other TU which requires metadata has a
  // type descriptor with a valid metadata access function but the linker picks
  // the first one.
  if (isAccessorLazilyGenerated(getTypeMetadataAccessStrategy(
          type->getDeclaredType()->getCanonicalType()))) {
    requireMetadata = RequireMetadata;
  }

  // Try to create a new record of the fact that we used this type.
  auto insertResult = LazyTypeGlobals.try_emplace(type);
  auto &entry = insertResult.first->second;

  bool metadataWasUsed = entry.IsMetadataUsed;
  bool descriptorWasUsed = entry.IsDescriptorUsed;

  bool isNovelUseOfMetadata = false;
  bool isNovelUseOfDescriptor = false;

  // Flag that we have a use of the metadata if
  //   - the reference was directly to the metadata
  //   - the reference was to the descriptor, but it requested the emission
  //     of metadata
  if (!metadataWasUsed && (isUseOfMetadata || requireMetadata)) {
    if (metadataWasUsed) return;
    entry.IsMetadataUsed = true;
    isNovelUseOfMetadata = true;
  }

  if (!descriptorWasUsed && !isUseOfMetadata) {
    if (descriptorWasUsed) return;
    entry.IsDescriptorUsed = true;
    isNovelUseOfDescriptor = true;
  }

  // Enqueue metadata emission if we have a novel use of it.
  if (isNovelUseOfMetadata) {
    assert(!FinishedEmittingLazyDefinitions);
    LazyTypeMetadata.push_back(type);
  }

  // Enqueue descriptor emission if we have a novel use of it or if we
  // need to re-emit it because we're suddenly using metadata for it.
  if (isNovelUseOfDescriptor ||
      (isNovelUseOfMetadata && entry.IsDescriptorEmitted)) {
    entry.IsDescriptorEmitted = false; // clear this in case it was true
    assert(!FinishedEmittingLazyDefinitions);
    LazyTypeContextDescriptors.push_back(type);
  }
}

void IRGenerator::noteUseOfFieldDescriptor(NominalTypeDecl *type) {
  if (!hasLazyMetadata(type))
    return;

  // Imported classes and protocols do not need field descriptors.
  if (type->hasClangNode() &&
      (isa<ClassDecl>(type) ||
       isa<ProtocolDecl>(type)))
    return;

  if (!LazilyEmittedFieldMetadata.insert(type).second)
    return;

  assert(!FinishedEmittingLazyDefinitions);
  LazyFieldDescriptors.push_back(type);
}

void IRGenerator::noteUseOfOpaqueTypeDescriptor(OpaqueTypeDecl *opaque) {
  if (!opaque)
    return;

  if (!hasLazyMetadata(opaque))
    return;

  auto insertResult = LazyOpaqueTypes.try_emplace(opaque);
  auto &entry = insertResult.first->second;

  bool isNovelUseOfDescriptor = !entry.IsDescriptorUsed;
  entry.IsDescriptorUsed = true;
  
  if (isNovelUseOfDescriptor) {
    LazyOpaqueTypeDescriptors.push_back(opaque);
  }
}

static std::string getDynamicReplacementSection(IRGenModule &IGM) {
  std::string sectionName;
  switch (IGM.TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit field records table for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_replace, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_replace";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5repl$B";
    break;
  }
  return sectionName;
}

static std::string getDynamicReplacementSomeSection(IRGenModule &IGM) {
  std::string sectionName;
  switch (IGM.TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit field records table for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_replac2, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_replac2";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5reps$B";
    break;
  }
  return sectionName;
}
llvm::GlobalVariable *IRGenModule::getGlobalForDynamicallyReplaceableThunk(
    LinkEntity &entity, llvm::Type *type, ForDefinition_t forDefinition) {
  return cast<llvm::GlobalVariable>(
      getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo()));
}

/// Creates a dynamic replacement chain entry for \p SILFn that contains either
/// the implementation function pointer \p or a nullptr, the next pointer of the
/// chain entry is set to nullptr.
///   struct ChainEntry {
///      void *funPtr;
///      struct ChainEntry *next;
///   }
static llvm::GlobalVariable *getChainEntryForDynamicReplacement(
    IRGenModule &IGM, LinkEntity entity, llvm::Function *implFunction = nullptr,
    ForDefinition_t forDefinition = ForDefinition) {

  auto linkEntry = IGM.getGlobalForDynamicallyReplaceableThunk(
      entity, IGM.DynamicReplacementLinkEntryTy, forDefinition);
  if (!forDefinition)
    return linkEntry;

  auto *funPtr =
      implFunction ? llvm::ConstantExpr::getBitCast(implFunction, IGM.Int8PtrTy)
                   : llvm::ConstantExpr::getNullValue(IGM.Int8PtrTy);
  auto *nextEntry =
      llvm::ConstantExpr::getNullValue(IGM.DynamicReplacementLinkEntryPtrTy);
  llvm::Constant *fields[] = {funPtr, nextEntry};
  auto *entry =
      llvm::ConstantStruct::get(IGM.DynamicReplacementLinkEntryTy, fields);
  linkEntry->setInitializer(entry);
  return linkEntry;
}

void IRGenerator::emitDynamicReplacements() {
  if (DynamicReplacements.empty())
    return;

  auto &IGM = *getPrimaryIGM();

  // Collect all the type metadata accessor replacements.
  SmallVector<OpaqueTypeArchetypeType *, 8> newFuncTypes;
  SmallVector<OpaqueTypeArchetypeType *, 8> origFuncTypes;
  llvm::SmallSet<OpaqueTypeArchetypeType *, 8> newUniqueOpaqueTypes;
  llvm::SmallSet<OpaqueTypeArchetypeType *, 8> origUniqueOpaqueTypes;
  for (auto *newFunc : DynamicReplacements) {
    if (!newFunc->getLoweredFunctionType()->hasOpaqueArchetype())
      continue;
    CanType(newFunc->getLoweredFunctionType()).visit([&](CanType ty) {
      if (auto opaque = ty->getAs<OpaqueTypeArchetypeType>())
        if (newUniqueOpaqueTypes.insert(opaque).second)
          newFuncTypes.push_back(opaque);
    });
    auto *origFunc = newFunc->getDynamicallyReplacedFunction();
    assert(origFunc);
    assert(origFunc->getLoweredFunctionType()->hasOpaqueArchetype());
    CanType(origFunc->getLoweredFunctionType()).visit([&](CanType ty) {
      if (auto opaque = ty->getAs<OpaqueTypeArchetypeType>())
        if (origUniqueOpaqueTypes.insert(opaque).second)
          origFuncTypes.push_back(opaque);
    });

    assert(origFuncTypes.size() == newFuncTypes.size());
  }

  // struct ReplacementScope {
  //   uint32t flags; // unused
  //   uint32t numReplacements;
  //   struct Entry {
  //     RelativeIndirectablePointer<KeyEntry, false> replacedFunctionKey;
  //     RelativeDirectPointer<void> newFunction;
  //     RelativeDirectPointer<LinkEntry> replacement;
  //     uint32_t flags; // shouldChain.
  //   }[0]
  // };
  ConstantInitBuilder builder(IGM);
  auto replacementScope = builder.beginStruct();
  replacementScope.addInt32(0); // unused flags.
  replacementScope.addInt32(DynamicReplacements.size() + newFuncTypes.size());

  auto replacementsArray =
      replacementScope.beginArray();
  for (auto *newFunc : DynamicReplacements) {
    LinkEntity entity =
        LinkEntity::forDynamicallyReplaceableFunctionVariable(newFunc);
    auto replacementLinkEntry =
        getChainEntryForDynamicReplacement(IGM, entity);
    // TODO: replacementLinkEntry->setZeroSection()
    auto *origFunc = newFunc->getDynamicallyReplacedFunction();
    assert(origFunc);
    auto keyRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
        LinkEntity::forDynamicallyReplaceableFunctionKey(origFunc));

    llvm::Constant *newFnPtr = llvm::ConstantExpr::getBitCast(
        IGM.getAddrOfSILFunction(newFunc, NotForDefinition), IGM.Int8PtrTy);

    auto replacement = replacementsArray.beginStruct();
    replacement.addRelativeAddress(keyRef); // tagged relative reference.
    replacement.addRelativeAddress(newFnPtr); // direct relative reference.
    replacement.addRelativeAddress(
        replacementLinkEntry); // direct relative reference.
    replacement.addInt32(
        Opts.EnableDynamicReplacementChaining ? 1 : 0);
    replacement.finishAndAddTo(replacementsArray);
  }
  // Emit replacements of the opaque type descriptor accessor.
  for (auto i : indices(origFuncTypes)) {
    LinkEntity entity = LinkEntity::forOpaqueTypeDescriptorAccessorVar(
        newFuncTypes[i]->getDecl());
    auto replacementLinkEntry = getChainEntryForDynamicReplacement(IGM, entity);
    auto keyRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
        LinkEntity::forOpaqueTypeDescriptorAccessorKey(
            origFuncTypes[i]->getDecl()));
    llvm::Constant *newFnPtr = llvm::ConstantExpr::getBitCast(
        IGM.getAddrOfOpaqueTypeDescriptorAccessFunction(
            newFuncTypes[i]->getDecl(), NotForDefinition, false),
        IGM.Int8PtrTy);
    auto replacement = replacementsArray.beginStruct();
    replacement.addRelativeAddress(keyRef);   // tagged relative reference.
    replacement.addRelativeAddress(newFnPtr); // direct relative reference.
    replacement.addRelativeAddress(
        replacementLinkEntry); // direct relative reference.
    replacement.addInt32(0);
    replacement.finishAndAddTo(replacementsArray);
  }
  replacementsArray.finishAndAddTo(replacementScope);

  auto var = replacementScope.finishAndCreateGlobal(
      "\x01l_unnamed_dynamic_replacements", IGM.getPointerAlignment(),
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
  IGM.setTrueConstGlobal(var);
  IGM.addUsedGlobal(var);

  // Emit the data for automatic replacement to happen on load.
  // struct AutomaticReplacements {
  //   uint32t flags; // unused
  //   uint32t numReplacements;
  //   struct Entry {
  //     RelativeDirectPointer<ReplacementScope> replacements;
  //     uint32_t flags; // unused.
  //   }[0]
  // };
  auto autoReplacements = builder.beginStruct();
  autoReplacements.addInt32(0); // unused flags.
  autoReplacements.addInt32(1); // number of replacement entries.
  auto autoReplacementsArray = autoReplacements.beginArray();
  autoReplacementsArray.addRelativeAddress(var);
  autoReplacementsArray.addInt32(0); // unused flags.
  autoReplacementsArray.finishAndAddTo(autoReplacements);
  auto autoReplVar = autoReplacements.finishAndCreateGlobal(
      "\x01l_auto_dynamic_replacements", IGM.getPointerAlignment(),
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
  autoReplVar->setSection(getDynamicReplacementSection(IGM));
  IGM.addUsedGlobal(autoReplVar);

  if (origFuncTypes.empty())
    return;
  // Emit records for replacing opaque type descriptor for some types.
  // struct AutomaticReplacementsSome {
  //   uint32t flags; // unused
  //   uint32t numReplacements;
  //   struct Entry {
  //     RelativeIndirectablePointer<OpaqueTypeDescriptor*> orig;
  //     RelativeDirectPointer<OpaqueTypeDescriptor*> replacement;
  //     uint32_t flags; // unused.
  //   }[numEntries]
  // };
  auto autoReplacementsSome = builder.beginStruct();
  autoReplacementsSome.addInt32(0); // unused flags.
  autoReplacementsSome.addInt32(
      origFuncTypes.size()); // number of replacement entries.
  auto someReplacementsArray = autoReplacementsSome.beginArray();
  for (auto i : indices(origFuncTypes)) {
    auto origDesc =
        LinkEntity::forOpaqueTypeDescriptor(origFuncTypes[i]->getDecl());
    auto replDesc =
        LinkEntity::forOpaqueTypeDescriptor(newFuncTypes[i]->getDecl());
    auto replacement = someReplacementsArray.beginStruct();
    replacement.addRelativeAddress(
        IGM.getAddrOfLLVMVariableOrGOTEquivalent(origDesc));
    replacement.addRelativeAddress(
        IGM.getAddrOfLLVMVariableOrGOTEquivalent(replDesc));
    replacement.finishAndAddTo(someReplacementsArray);
  }
  someReplacementsArray.finishAndAddTo(autoReplacementsSome);
  auto autoReplVar2 = autoReplacementsSome.finishAndCreateGlobal(
      "\x01l_auto_dynamic_replacements_some", IGM.getPointerAlignment(),
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
  autoReplVar2->setSection(getDynamicReplacementSomeSection(IGM));
}

void IRGenerator::emitEagerClassInitialization() {
  if (ClassesForEagerInitialization.empty())
    return;

  // Emit the register function in the primary module.
  IRGenModule *IGM = getPrimaryIGM();

  llvm::Function *RegisterFn = llvm::Function::Create(
                                llvm::FunctionType::get(IGM->VoidTy, false),
                                llvm::GlobalValue::PrivateLinkage,
                                "_swift_eager_class_initialization");
  IGM->Module.getFunctionList().push_back(RegisterFn);
  IRGenFunction RegisterIGF(*IGM, RegisterFn);
  RegisterFn->setAttributes(IGM->constructInitialAttributes());
  RegisterFn->setCallingConv(IGM->DefaultCC);

  for (ClassDecl *CD : ClassesForEagerInitialization) {
    auto Ty = CD->getDeclaredType()->getCanonicalType();
    llvm::Value *MetaData = RegisterIGF.emitTypeMetadataRef(Ty);
    assert(CD->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>());

    // Get the metadata to make sure that the class is registered. We need to 
    // add a use (empty inline asm instruction) for the metadata. Otherwise
    // llvm would optimize the metadata accessor call away because it's
    // defined as "readnone".
    llvm::FunctionType *asmFnTy =
      llvm::FunctionType::get(IGM->VoidTy, {MetaData->getType()},
                              false /* = isVarArg */);
    llvm::InlineAsm *inlineAsm =
      llvm::InlineAsm::get(asmFnTy, "", "r", true /* = SideEffects */);
    RegisterIGF.Builder.CreateAsmCall(inlineAsm, MetaData);
  }
  RegisterIGF.Builder.CreateRetVoid();

  // Add the registration function as a static initializer. We use a priority
  // slightly lower than used for C++ global constructors, so that the code is
  // executed before C++ global constructors (in case someone uses archives
  // from a C++ global constructor).
  llvm::appendToGlobalCtors(IGM->Module, RegisterFn, 60000, nullptr);
}

/// Emit symbols for eliminated dead methods, which can still be referenced
/// from other modules. This happens e.g. if a public class contains a (dead)
/// private method.
void IRGenModule::emitVTableStubs() {
  llvm::Function *stub = nullptr;
  for (auto I = getSILModule().zombies_begin();
           I != getSILModule().zombies_end(); ++I) {
    const SILFunction &F = *I;
    if (! F.isExternallyUsedSymbol())
      continue;
    
    if (!stub) {
      // Create a single stub function which calls swift_deletedMethodError().
      stub = llvm::Function::Create(llvm::FunctionType::get(VoidTy, false),
                                    llvm::GlobalValue::InternalLinkage,
                                    "_swift_dead_method_stub");
      stub->setAttributes(constructInitialAttributes());
      Module.getFunctionList().push_back(stub);
      stub->setCallingConv(DefaultCC);
      auto *entry = llvm::BasicBlock::Create(getLLVMContext(), "entry", stub);
      auto *errorFunc = getDeletedMethodErrorFn();
      llvm::CallInst::Create(errorFunc, ArrayRef<llvm::Value *>(), "", entry);
      new llvm::UnreachableInst(getLLVMContext(), entry);
    }

    // For each eliminated method symbol create an alias to the stub.
    auto *alias = llvm::GlobalAlias::create(llvm::GlobalValue::ExternalLinkage,
                                            F.getName(), stub);

    if (F.getEffectiveSymbolLinkage() == SILLinkage::Hidden)
      alias->setVisibility(llvm::GlobalValue::HiddenVisibility);
    else
      ApplyIRLinkage(IRLinkage::ExternalExport).to(alias);
  }
}

static IRLinkage
getIRLinkage(const UniversalLinkageInfo &info, SILLinkage linkage,
             ForDefinition_t isDefinition,
             bool isWeakImported) {
#define RESULT(LINKAGE, VISIBILITY, DLL_STORAGE)                               \
  IRLinkage{llvm::GlobalValue::LINKAGE##Linkage,                               \
            llvm::GlobalValue::VISIBILITY##Visibility,                         \
            llvm::GlobalValue::DLL_STORAGE##StorageClass}

  // Use protected visibility for public symbols we define on ELF.  ld.so
  // doesn't support relative relocations at load time, which interferes with
  // our metadata formats.  Default visibility should suffice for other object
  // formats.
  llvm::GlobalValue::VisibilityTypes PublicDefinitionVisibility =
      info.IsELFObject ? llvm::GlobalValue::ProtectedVisibility
                       : llvm::GlobalValue::DefaultVisibility;
  llvm::GlobalValue::DLLStorageClassTypes ExportedStorage =
      info.UseDLLStorage ? llvm::GlobalValue::DLLExportStorageClass
                         : llvm::GlobalValue::DefaultStorageClass;
  llvm::GlobalValue::DLLStorageClassTypes ImportedStorage =
      info.UseDLLStorage ? llvm::GlobalValue::DLLImportStorageClass
                         : llvm::GlobalValue::DefaultStorageClass;

  switch (linkage) {
  case SILLinkage::Public:
    return {llvm::GlobalValue::ExternalLinkage, PublicDefinitionVisibility,
            ExportedStorage};

  case SILLinkage::PublicNonABI:
    return isDefinition ? RESULT(WeakODR, Hidden, Default)
                        : RESULT(External, Hidden, Default);

  case SILLinkage::Shared:
  case SILLinkage::SharedExternal:
    return isDefinition ? RESULT(LinkOnceODR, Hidden, Default)
                        : RESULT(External, Hidden, Default);

  case SILLinkage::Hidden:
    return RESULT(External, Hidden, Default);

  case SILLinkage::Private: {
    if (info.forcePublicDecls() && !isDefinition)
      return getIRLinkage(info, SILLinkage::PublicExternal, isDefinition,
                          isWeakImported);
    auto linkage = info.needLinkerToMergeDuplicateSymbols()
                       ? llvm::GlobalValue::LinkOnceODRLinkage
                       : llvm::GlobalValue::InternalLinkage;
    auto visibility = info.shouldAllPrivateDeclsBeVisibleFromOtherFiles()
                          ? llvm::GlobalValue::HiddenVisibility
                          : llvm::GlobalValue::DefaultVisibility;
    return {linkage, visibility, llvm::GlobalValue::DefaultStorageClass};
  }

  case SILLinkage::PublicExternal: {
    if (isDefinition)
      return RESULT(AvailableExternally, Default, Default);

    auto linkage = isWeakImported ? llvm::GlobalValue::ExternalWeakLinkage
                                  : llvm::GlobalValue::ExternalLinkage;
    return {linkage, llvm::GlobalValue::DefaultVisibility, ImportedStorage};
  }

  case SILLinkage::HiddenExternal:
  case SILLinkage::PrivateExternal:
    if (isDefinition)
      return RESULT(AvailableExternally, Hidden, Default);

    return {llvm::GlobalValue::ExternalLinkage,
            llvm::GlobalValue::DefaultVisibility, ImportedStorage};

  }

  llvm_unreachable("bad SIL linkage");
}

/// Given that we're going to define a global value but already have a
/// forward-declaration of it, update its linkage.
void irgen::updateLinkageForDefinition(IRGenModule &IGM,
                                       llvm::GlobalValue *global,
                                       const LinkEntity &entity) {
  // TODO: there are probably cases where we can avoid redoing the
  // entire linkage computation.
  UniversalLinkageInfo linkInfo(IGM);
  bool weakImported = entity.isWeakImported(IGM.getSwiftModule());
  auto IRL =
      getIRLinkage(linkInfo, entity.getLinkage(ForDefinition),
                   ForDefinition, weakImported);
  ApplyIRLinkage(IRL).to(global);

  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  //
  // Exclude "main", because it should naturally be used, and because adding it
  // to llvm.used leaves a dangling use when the REPL attempts to discard
  // intermediate mains.
  if (LinkInfo::isUsed(IRL) && global->getName() != SWIFT_ENTRY_POINT_FUNCTION)
    IGM.addUsedGlobal(global);
}

LinkInfo LinkInfo::get(IRGenModule &IGM, const LinkEntity &entity,
                       ForDefinition_t isDefinition) {
  return LinkInfo::get(UniversalLinkageInfo(IGM),
                       IGM.getSwiftModule(),
                       entity, isDefinition);
}

LinkInfo LinkInfo::get(const UniversalLinkageInfo &linkInfo,
                       ModuleDecl *swiftModule,
                       const LinkEntity &entity,
                       ForDefinition_t isDefinition) {
  LinkInfo result;
  // FIXME: For anything in the standard library, we assume is locally defined.
  // The only two ways imported interfaces are currently created is via a shims
  // interface where the ClangImporter will correctly give us the proper DLL
  // storage for the declaration.  Otherwise, it is from a `@_silgen_name`
  // attributed declaration, which we explicitly handle elsewhere.  So, in the
  // case of a standard library build, just assume everything is locally
  // defined.  Ideally, we would integrate the linkage calculation properly to
  // avoid this special casing.
  ForDefinition_t isStdlibOrDefinition =
      ForDefinition_t(swiftModule->isStdlibModule() || isDefinition);

  entity.mangle(result.Name);
  bool weakImported = entity.isWeakImported(swiftModule);
  result.IRL = getIRLinkage(linkInfo, entity.getLinkage(isStdlibOrDefinition),
                            isDefinition, weakImported);
  result.ForDefinition = isDefinition;
  return result;
}

LinkInfo LinkInfo::get(const UniversalLinkageInfo &linkInfo, StringRef name,
                       SILLinkage linkage, ForDefinition_t isDefinition,
                       bool isWeakImported) {
  LinkInfo result;

  result.Name += name;
  result.IRL = getIRLinkage(linkInfo, linkage, isDefinition, isWeakImported);
  result.ForDefinition = isDefinition;
  return result;
}

static bool isPointerTo(llvm::Type *ptrTy, llvm::Type *objTy) {
  return cast<llvm::PointerType>(ptrTy)->getElementType() == objTy;
}

/// Get or create an LLVM function with these linkage rules.
llvm::Function *irgen::createFunction(IRGenModule &IGM,
                                      LinkInfo &linkInfo,
                                      const Signature &signature,
                                      llvm::Function *insertBefore,
                                      OptimizationMode FuncOptMode) {
  auto name = linkInfo.getName();

  llvm::Function *existing = IGM.Module.getFunction(name);
  if (existing) {
    if (isPointerTo(existing->getType(), signature.getType()))
      return cast<llvm::Function>(existing);

    IGM.error(SourceLoc(),
              "program too clever: function collides with existing symbol " +
                  name);

    // Note that this will implicitly unique if the .unique name is also taken.
    existing->setName(name + ".unique");
  }

  llvm::Function *fn =
    llvm::Function::Create(signature.getType(), linkInfo.getLinkage(), name);
  fn->setCallingConv(signature.getCallingConv());

  if (insertBefore) {
    IGM.Module.getFunctionList().insert(insertBefore->getIterator(), fn);
  } else {
    IGM.Module.getFunctionList().push_back(fn);
  }

  ApplyIRLinkage({linkInfo.getLinkage(), linkInfo.getVisibility(), linkInfo.getDLLStorage()})
      .to(fn);

  llvm::AttrBuilder initialAttrs;
  IGM.constructInitialFnAttributes(initialAttrs, FuncOptMode);
  // Merge initialAttrs with attrs.
  auto updatedAttrs =
    signature.getAttributes().addAttributes(IGM.getLLVMContext(),
                                      llvm::AttributeList::FunctionIndex,
                                            initialAttrs);
  if (!updatedAttrs.isEmpty())
    fn->setAttributes(updatedAttrs);

  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  //
  // Exclude "main", because it should naturally be used, and because adding it
  // to llvm.used leaves a dangling use when the REPL attempts to discard
  // intermediate mains.
  if (linkInfo.isUsed() && name != SWIFT_ENTRY_POINT_FUNCTION) {
    IGM.addUsedGlobal(fn);
  }

  return fn;
}

bool LinkInfo::isUsed(IRLinkage IRL) {
  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  return IRL.Linkage == llvm::GlobalValue::ExternalLinkage &&
         (IRL.Visibility == llvm::GlobalValue::DefaultVisibility ||
          IRL.Visibility == llvm::GlobalValue::ProtectedVisibility) &&
         (IRL.DLLStorage == llvm::GlobalValue::DefaultStorageClass ||
          IRL.DLLStorage == llvm::GlobalValue::DLLExportStorageClass);
}

/// Get or create an LLVM global variable with these linkage rules.
llvm::GlobalVariable *swift::irgen::createVariable(
    IRGenModule &IGM, LinkInfo &linkInfo, llvm::Type *storageType,
    Alignment alignment, DebugTypeInfo DbgTy, Optional<SILLocation> DebugLoc,
    StringRef DebugName, bool inFixedBuffer) {
  auto name = linkInfo.getName();
  llvm::GlobalValue *existingValue = IGM.Module.getNamedGlobal(name);
  if (existingValue) {
    auto existingVar = dyn_cast<llvm::GlobalVariable>(existingValue);
    if (existingVar && isPointerTo(existingVar->getType(), storageType))
      return existingVar;

    IGM.error(SourceLoc(),
              "program too clever: variable collides with existing symbol " +
                  name);

    // Note that this will implicitly unique if the .unique name is also taken.
    existingValue->setName(name + ".unique");
  }

  auto var = new llvm::GlobalVariable(IGM.Module, storageType,
                                      /*constant*/ false, linkInfo.getLinkage(),
                                      /*initializer*/ nullptr, name);
  ApplyIRLinkage({linkInfo.getLinkage(),
                  linkInfo.getVisibility(),
                  linkInfo.getDLLStorage()})
      .to(var);
  var->setAlignment(alignment.getValue());

  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  if (linkInfo.isUsed()) {
    IGM.addUsedGlobal(var);
  }

  if (IGM.DebugInfo && !DbgTy.isNull() && linkInfo.isForDefinition())
    IGM.DebugInfo->emitGlobalVariableDeclaration(
        var, DebugName.empty() ? name : DebugName, name, DbgTy,
        var->hasInternalLinkage(), inFixedBuffer, DebugLoc);

  return var;
}

void swift::irgen::disableAddressSanitizer(IRGenModule &IGM, llvm::GlobalVariable *var) {
  // Add an operand to llvm.asan.globals blacklisting this global variable.
  llvm::Metadata *metadata[] = {
    // The global variable to blacklist.
    llvm::ConstantAsMetadata::get(var),
    
    // Source location. Optional, unnecessary here.
    nullptr,
    
    // Name. Optional, unnecessary here.
    nullptr,
    
    // Whether the global is dynamically initialized.
    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
      llvm::Type::getInt1Ty(IGM.Module.getContext()), false)),
    
    // Whether the global is blacklisted.
    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
      llvm::Type::getInt1Ty(IGM.Module.getContext()), true))};
  
  auto *globalNode = llvm::MDNode::get(IGM.Module.getContext(), metadata);
  auto *asanMetadata = IGM.Module.getOrInsertNamedMetadata("llvm.asan.globals");
  asanMetadata->addOperand(globalNode);
}

/// Emit a global declaration.
void IRGenModule::emitGlobalDecl(Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Extension:
    return emitExtension(cast<ExtensionDecl>(D));

  case DeclKind::Protocol:
    return emitProtocolDecl(cast<ProtocolDecl>(D));

  case DeclKind::PatternBinding:
    // The global initializations are in SIL.
    return;

  case DeclKind::Param:
    llvm_unreachable("there are no global function parameters");

  case DeclKind::Subscript:
    llvm_unreachable("there are no global subscript operations");
      
  case DeclKind::EnumCase:
  case DeclKind::EnumElement:
    llvm_unreachable("there are no global enum elements");

  case DeclKind::Constructor:
    llvm_unreachable("there are no global constructor");

  case DeclKind::Destructor:
    llvm_unreachable("there are no global destructor");

  case DeclKind::MissingMember:
    llvm_unreachable("there are no global member placeholders");

  case DeclKind::TypeAlias:
  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType:
  case DeclKind::IfConfig: 
  case DeclKind::PoundDiagnostic:
    return;

  case DeclKind::Enum:
    return emitEnumDecl(cast<EnumDecl>(D));

  case DeclKind::Struct:
    return emitStructDecl(cast<StructDecl>(D));

  case DeclKind::Class:
    return emitClassDecl(cast<ClassDecl>(D));

  // These declarations are only included in the debug info.
  case DeclKind::Import:
    if (DebugInfo)
      DebugInfo->emitImport(cast<ImportDecl>(D));
    return;

  case DeclKind::Var:
  case DeclKind::Accessor:
  case DeclKind::Func:
    // Handled in SIL.
    return;
  
  case DeclKind::TopLevelCode:
    // All the top-level code will be lowered separately.
    return;
      
  // Operator decls aren't needed for IRGen.
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::PrecedenceGroup:
    return;

  case DeclKind::Module:
    return;
      
  case DeclKind::OpaqueType:
    // TODO: Eventually we'll need to emit descriptors to access the opaque
    // type's metadata.
    return;
  }

  llvm_unreachable("bad decl kind!");
}

Address IRGenModule::getAddrOfSILGlobalVariable(SILGlobalVariable *var,
                                                const TypeInfo &ti,
                                                ForDefinition_t forDefinition) {
  if (auto clangDecl = var->getClangDecl()) {
    auto addr = getAddrOfClangGlobalDecl(cast<clang::VarDecl>(clangDecl),
                                         forDefinition);

    // If we're not emitting this to define it, make sure we cast it to the
    // right type.
    if (!forDefinition) {
      auto ptrTy = ti.getStorageType()->getPointerTo();
      addr = llvm::ConstantExpr::getBitCast(addr, ptrTy);
    }

    auto alignment =
      Alignment(getClangASTContext().getDeclAlign(clangDecl).getQuantity());
    return Address(addr, alignment);
  }

  LinkEntity entity = LinkEntity::forSILGlobalVariable(var);
  ResilienceExpansion expansion = getResilienceExpansionForLayout(var);

  llvm::Type *storageType;
  llvm::Type *castStorageToType = nullptr;
  Size fixedSize;
  Alignment fixedAlignment;
  bool inFixedBuffer = false;

  if (var->isInitializedObject()) {
    assert(ti.isFixedSize(expansion));
    StructLayout *Layout = StaticObjectLayouts[var].get();
    if (!Layout) {
      // Create the layout (includes the llvm type) for the statically
      // initialized object and store it for later.
      ObjectInst *OI = cast<ObjectInst>(var->getStaticInitializerValue());
      llvm::SmallVector<SILType, 16> TailTypes;
      for (SILValue TailOp : OI->getTailElements()) {
        TailTypes.push_back(TailOp->getType());
      }
      Layout = getClassLayoutWithTailElems(*this,
                                           var->getLoweredType(), TailTypes);
      StaticObjectLayouts[var] = std::unique_ptr<StructLayout>(Layout);
    }
    storageType = Layout->getType();
    fixedSize = Layout->getSize();
    fixedAlignment = Layout->getAlignment();
    castStorageToType = cast<FixedTypeInfo>(ti).getStorageType();
    assert(fixedAlignment >= TargetInfo.HeapObjectAlignment);
  } else if (ti.isFixedSize(expansion)) {
    // Allocate static storage.
    auto &fixedTI = cast<FixedTypeInfo>(ti);
    storageType = fixedTI.getStorageType();
    fixedSize = fixedTI.getFixedSize();
    fixedAlignment = fixedTI.getFixedAlignment();
  } else {
    // Allocate a fixed-size buffer and possibly heap-allocate a payload at
    // runtime if the runtime size of the type does not fit in the buffer.
    inFixedBuffer = true;
    storageType = getFixedBufferTy();
    fixedSize = Size(DataLayout.getTypeAllocSize(storageType));
    fixedAlignment = Alignment(DataLayout.getABITypeAlignment(storageType));
  }

  // Check whether we've created the global variable already.
  // FIXME: We should integrate this into the LinkEntity cache more cleanly.
  auto gvar = Module.getGlobalVariable(var->getName(), /*allowInternal*/ true);
  if (gvar) {
    if (forDefinition)
      updateLinkageForDefinition(*this, gvar, entity);
  } else {
    LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
    llvm::Type *storageTypeWithContainer = storageType;
    if (var->isInitializedObject()) {
      // A statically initialized object must be placed into a container struct
      // because the swift_initStaticObject needs a swift_once_t at offset -1:
      //     struct Container {
      //       swift_once_t token[fixedAlignment / sizeof(swift_once_t)];
      //       HeapObject object;
      //     };
      std::string typeName = storageType->getStructName().str() + 'c';
      assert(fixedAlignment >= getPointerAlignment());
      unsigned numTokens = fixedAlignment.getValue() /
        getPointerAlignment().getValue();
      storageTypeWithContainer = llvm::StructType::create(getLLVMContext(),
              {llvm::ArrayType::get(OnceTy, numTokens), storageType}, typeName);
      gvar = createVariable(*this, link, storageTypeWithContainer,
                            fixedAlignment);
    } else {
      StringRef name;
      Optional<SILLocation> loc;
      if (var->getDecl()) {
        // Use the VarDecl for more accurate debugging information.
        loc = var->getDecl();
        name = var->getDecl()->getName().str();
      } else {
        if (var->hasLocation())
          loc = var->getLocation();
        name = var->getName();
      }
      auto DbgTy = DebugTypeInfo::getGlobal(var, storageTypeWithContainer,
                                            fixedSize, fixedAlignment);
      gvar = createVariable(*this, link, storageTypeWithContainer,
                            fixedAlignment, DbgTy, loc, name, inFixedBuffer);
    }
    /// Add a zero initializer.
    if (forDefinition)
      gvar->setInitializer(llvm::Constant::getNullValue(storageTypeWithContainer));
    else
      gvar->setComdat(nullptr);
  }
  llvm::Constant *addr = gvar;
  if (var->isInitializedObject()) {
    // Project out the object from the container.
    llvm::Constant *Indices[2] = {
      llvm::ConstantExpr::getIntegerValue(Int32Ty, APInt(32, 0)),
      llvm::ConstantExpr::getIntegerValue(Int32Ty, APInt(32, 1))
    };
    // Return the address of the initialized object itself (and not the address
    // to a reference to it).
    addr = llvm::ConstantExpr::getGetElementPtr(nullptr, gvar, Indices);
  }
  addr = llvm::ConstantExpr::getBitCast(
      addr,
      castStorageToType ? castStorageToType : storageType->getPointerTo());
  return Address(addr, Alignment(gvar->getAlignment()));
}

/// Return True if the function \p f is a 'readonly' function. Checking
/// for the SIL @_effects(readonly) attribute is not enough because this
/// definition does not match the definition of the LLVM readonly function
/// attribute. In this function we do the actual check.
static bool isReadOnlyFunction(SILFunction *f) {
  // Check if the function has any 'owned' parameters. Owned parameters may
  // call the destructor of the object which could violate the readonly-ness
  // of the function.
  if (f->hasOwnedParameters() || f->hasIndirectFormalResults())
    return false;

  auto Eff = f->getEffectsKind();

  // Swift's readonly does not automatically match LLVM's readonly.
  // Swift SIL optimizer relies on @_effects(readonly) to remove e.g.
  // dead code remaining from initializers of strings or dictionaries
  // of variables that are not used. But those initializers are often
  // not really readonly in terms of LLVM IR. For example, the
  // Dictionary.init() is marked as @_effects(readonly) in Swift, but
  // it does invoke reference-counting operations.
  if (Eff == EffectsKind::ReadOnly || Eff == EffectsKind::ReadNone) {
    // TODO: Analyze the body of function f and return true if it is
    // really readonly.
    return false;
  }

  return false;
}

static clang::GlobalDecl getClangGlobalDeclForFunction(const clang::Decl *decl) {
  if (auto ctor = dyn_cast<clang::CXXConstructorDecl>(decl))
    return clang::GlobalDecl(ctor, clang::Ctor_Complete);
  if (auto dtor = dyn_cast<clang::CXXDestructorDecl>(decl))
    return clang::GlobalDecl(dtor, clang::Dtor_Complete);
  return clang::GlobalDecl(cast<clang::FunctionDecl>(decl));
}

static void addLLVMFunctionAttributes(SILFunction *f, Signature &signature) {
  auto &attrs = signature.getMutableAttributes();
  switch (f->getInlineStrategy()) {
  case NoInline:
    attrs = attrs.addAttribute(signature.getType()->getContext(),
                               llvm::AttributeList::FunctionIndex,
                               llvm::Attribute::NoInline);
    break;
  case AlwaysInline:
    // FIXME: We do not currently transfer AlwaysInline since doing so results
    // in test failures, which must be investigated first.
  case InlineDefault:
    break;
  }

  if (isReadOnlyFunction(f)) {
    attrs = attrs.addAttribute(signature.getType()->getContext(),
                               llvm::AttributeList::FunctionIndex,
                               llvm::Attribute::ReadOnly);
  }
}

/// Create a key entry for a dynamic function replacement. A key entry refers to
/// the link entry for the dynamic replaceable function.
/// struct KeyEntry {
///   RelativeDirectPointer<LinkEntry> linkEntry;
///   int32_t flags;
/// }
static llvm::GlobalVariable *createGlobalForDynamicReplacementFunctionKey(
    IRGenModule &IGM, LinkEntity keyEntity, llvm::GlobalVariable *linkEntry) {
  auto key = IGM.getGlobalForDynamicallyReplaceableThunk(
      keyEntity, IGM.DynamicReplacementKeyTy, ForDefinition);

  ConstantInitBuilder builder(IGM);
  auto B = builder.beginStruct(IGM.DynamicReplacementKeyTy);
  B.addRelativeAddress(linkEntry);
  B.addInt32(0);
  B.finishAndSetAsInitializer(key);
  key->setConstant(true);
  IGM.setTrueConstGlobal(key);
  return key;
}

/// Creates the prolog for a dynamically replaceable function.
/// It checks if the replaced function or the original function should be called
/// (by calling the swift_getFunctionReplacement runtime function). In case of
/// the original function, it just jumps to the "real" code of the function,
/// otherwise it tail calls the replacement.
void IRGenModule::createReplaceableProlog(IRGenFunction &IGF, SILFunction *f) {
  LinkEntity varEntity =
      LinkEntity::forDynamicallyReplaceableFunctionVariable(f);
  LinkEntity keyEntity =
      LinkEntity::forDynamicallyReplaceableFunctionKey(f);
  Signature signature = getSignature(f->getLoweredFunctionType());

  // Create and initialize the first link entry for the chain of replacements.
  // The first implementation is initialized with 'implFn'.
  auto linkEntry = getChainEntryForDynamicReplacement(*this, varEntity, IGF.CurFn);

  // Create the key data structure. This is used from other modules to refer to
  // the chain of replacements.
  createGlobalForDynamicReplacementFunctionKey(*this, keyEntity, linkEntry);

  llvm::Constant *indices[] = {llvm::ConstantInt::get(Int32Ty, 0),
                               llvm::ConstantInt::get(Int32Ty, 0)};

  auto *fnPtrAddr =
      llvm::ConstantExpr::getInBoundsGetElementPtr(nullptr, linkEntry, indices);

  auto *ReplAddr =
    llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(fnPtrAddr,
      FunctionPtrTy->getPointerTo());

  auto *FnAddr = llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(
      IGF.CurFn, FunctionPtrTy);

  llvm::Value *ReplFn = nullptr, *rhs = nullptr;

  if (UseBasicDynamicReplacement) {
    ReplFn = IGF.Builder.CreateLoad(fnPtrAddr, getPointerAlignment());
    rhs = FnAddr;
  } else {
    // Call swift_getFunctionReplacement to check which function to call.
    auto *callRTFunc =
        IGF.Builder.CreateCall(getGetReplacementFn(), {ReplAddr, FnAddr});
    callRTFunc->setDoesNotThrow();
    ReplFn = callRTFunc;
    rhs = llvm::ConstantExpr::getNullValue(ReplFn->getType());
  }
  auto *hasReplFn = IGF.Builder.CreateICmpEQ(ReplFn, rhs);

  auto *replacedBB = IGF.createBasicBlock("forward_to_replaced");
  auto *origEntryBB = IGF.createBasicBlock("original_entry");
  IGF.Builder.CreateCondBr(hasReplFn, origEntryBB, replacedBB);

  IGF.Builder.emitBlock(replacedBB);

  // Call the replacement function.
  SmallVector<llvm::Value *, 16> forwardedArgs;
  for (auto &arg : IGF.CurFn->args())
    forwardedArgs.push_back(&arg);
  auto *fnType = signature.getType()->getPointerTo();
  auto *realReplFn = IGF.Builder.CreateBitCast(ReplFn, fnType);

  auto *Res = IGF.Builder.CreateCall(FunctionPointer(realReplFn, signature),
                                     forwardedArgs);
  Res->setTailCall();
  if (IGF.CurFn->getReturnType()->isVoidTy())
    IGF.Builder.CreateRetVoid();
  else
    IGF.Builder.CreateRet(Res);

  IGF.Builder.emitBlock(origEntryBB);

}

/// Emit the thunk that dispatches to the dynamically replaceable function.
static void emitDynamicallyReplaceableThunk(IRGenModule &IGM,
                                            LinkEntity varEntity,
                                            LinkEntity keyEntity,
                                            llvm::Function *dispatchFn,
                                            llvm::Function *implFn,
                                            Signature &signature) {

  // Create and initialize the first link entry for the chain of replacements.
  // The first implementation is initialized with 'implFn'.
  auto linkEntry = getChainEntryForDynamicReplacement(IGM, varEntity, implFn);

  // Create the key data structure. This is used from other modules to refer to
  // the chain of replacements.
  createGlobalForDynamicReplacementFunctionKey(IGM, keyEntity, linkEntry);

  // We should never inline the implementation function.
  implFn->addFnAttr(llvm::Attribute::NoInline);

  // Load the function and dispatch to it forwarding our arguments.
  IRGenFunction IGF(IGM, dispatchFn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, dispatchFn);
  llvm::Constant *indices[] = {llvm::ConstantInt::get(IGM.Int32Ty, 0),
                               llvm::ConstantInt::get(IGM.Int32Ty, 0)};
  auto *fnPtr = IGF.Builder.CreateLoad(
      llvm::ConstantExpr::getInBoundsGetElementPtr(nullptr, linkEntry, indices),
      IGM.getPointerAlignment());
  auto *typeFnPtr =
      IGF.Builder.CreateBitOrPointerCast(fnPtr, implFn->getType());
  SmallVector<llvm::Value *, 16> forwardedArgs;
  for (auto &arg : dispatchFn->args())
    forwardedArgs.push_back(&arg);
  auto *Res = IGF.Builder.CreateCall(FunctionPointer(typeFnPtr, signature),
                                     forwardedArgs);
  Res->setTailCall();
  if (implFn->getReturnType()->isVoidTy())
    IGF.Builder.CreateRetVoid();
  else
    IGF.Builder.CreateRet(Res);
}

void IRGenModule::emitOpaqueTypeDescriptorAccessor(OpaqueTypeDecl *opaque) {
  auto *namingDecl = opaque->getNamingDecl();
  auto *abstractStorage = dyn_cast<AbstractStorageDecl>(namingDecl);

  bool isNativeDynamic = false;
  bool isDynamicReplacement = false;

  // Don't emit accessors for abstract storage that is not dynamic or a dynamic
  // replacement.
  if (abstractStorage) {
    isNativeDynamic = abstractStorage->hasAnyNativeDynamicAccessors();
    isDynamicReplacement = abstractStorage->hasAnyDynamicReplacementAccessors();
    if (!isNativeDynamic && !isDynamicReplacement)
      return;
  }

  // Don't emit accessors for functions that are not dynamic or dynamic
  // replacements.
  if (!abstractStorage) {
    isNativeDynamic = opaque->getNamingDecl()->isNativeDynamic();
    isDynamicReplacement = opaque->getNamingDecl()
                               ->getAttrs()
                               .hasAttribute<DynamicReplacementAttr>();
    if (!isNativeDynamic && !isDynamicReplacement)
      return;
  }

  auto accessor =
      getAddrOfOpaqueTypeDescriptorAccessFunction(opaque, ForDefinition, false);

  if (isNativeDynamic) {
    auto thunk = accessor;
    auto impl = getAddrOfOpaqueTypeDescriptorAccessFunction(
        opaque, ForDefinition, true);
    auto varEntity = LinkEntity::forOpaqueTypeDescriptorAccessorVar(opaque);
    auto keyEntity = LinkEntity::forOpaqueTypeDescriptorAccessorKey(opaque);

    auto fnType = llvm::FunctionType::get(OpaqueTypeDescriptorPtrTy, {}, false);
    Signature signature(fnType, llvm::AttributeList(), SwiftCC);
    emitDynamicallyReplaceableThunk(*this, varEntity, keyEntity, thunk, impl,
                                    signature);
    // We should never inline the thunk function.
    thunk->addFnAttr(llvm::Attribute::NoInline);
    accessor = impl;
  }

  // The implementation just returns the opaque type descriptor.
  llvm::BasicBlock *entryBB =
      llvm::BasicBlock::Create(getLLVMContext(), "entry", accessor);
  IRBuilder B(getLLVMContext(), false);
  B.SetInsertPoint(entryBB);
  if (DebugInfo)
    DebugInfo->emitArtificialFunction(B, accessor);
  auto *desc = getAddrOfOpaqueTypeDescriptor(opaque, ConstantInit());
  B.CreateRet(desc);
}

/// Calls the previous implementation before this dynamic replacement became
/// active.
void IRGenModule::emitDynamicReplacementOriginalFunctionThunk(SILFunction *f) {
  assert(f->getDynamicallyReplacedFunction());

  if (UseBasicDynamicReplacement)
    return;

  auto entity = LinkEntity::forSILFunction(f, true);

  Signature signature = getSignature(f->getLoweredFunctionType());
  addLLVMFunctionAttributes(f, signature);

  LinkInfo implLink = LinkInfo::get(*this, entity, ForDefinition);
  auto implFn =
      createFunction(*this, implLink, signature, nullptr /*insertBefore*/,
                     f->getOptimizationMode());
  implFn->addFnAttr(llvm::Attribute::NoInline);

  IRGenFunction IGF(*this, implFn);
  if (DebugInfo)
    DebugInfo->emitArtificialFunction(IGF, implFn);

  LinkEntity varEntity =
      LinkEntity::forDynamicallyReplaceableFunctionVariable(f);
  auto linkEntry = getChainEntryForDynamicReplacement(*this, varEntity, nullptr,
                                                      NotForDefinition);

  // Load the function and dispatch to it forwarding our arguments.
  llvm::Constant *indices[] = {llvm::ConstantInt::get(Int32Ty, 0),
                               llvm::ConstantInt::get(Int32Ty, 0)};

  auto *fnPtrAddr =
    llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(
      llvm::ConstantExpr::getInBoundsGetElementPtr(nullptr, linkEntry, indices),
      FunctionPtrTy->getPointerTo());

  auto *OrigFn =
      IGF.Builder.CreateCall(getGetOrigOfReplaceableFn(), {fnPtrAddr});

  OrigFn->setDoesNotThrow();

  auto *typeFnPtr =
      IGF.Builder.CreateBitOrPointerCast(OrigFn, implFn->getType());

  SmallVector<llvm::Value *, 16> forwardedArgs;
  for (auto &arg : implFn->args())
    forwardedArgs.push_back(&arg);
  auto *Res = IGF.Builder.CreateCall(FunctionPointer(typeFnPtr, signature),
                                     forwardedArgs);

  if (implFn->getReturnType()->isVoidTy())
    IGF.Builder.CreateRetVoid();
  else
    IGF.Builder.CreateRet(Res);
}

/// Find the entry point for a SIL function.
llvm::Function *IRGenModule::getAddrOfSILFunction(
    SILFunction *f, ForDefinition_t forDefinition,
    bool isDynamicallyReplaceableImplementation,
    bool shouldCallPreviousImplementation) {
  assert(forDefinition || !isDynamicallyReplaceableImplementation);
  assert(!forDefinition || !shouldCallPreviousImplementation);

  LinkEntity entity =
      LinkEntity::forSILFunction(f, shouldCallPreviousImplementation);

  // Check whether we've created the function already.
  // FIXME: We should integrate this into the LinkEntity cache more cleanly.
  llvm::Function *fn = Module.getFunction(entity.mangleAsString());
  if (fn) {
    if (forDefinition) {
      updateLinkageForDefinition(*this, fn, entity);
    }
    return fn;
  }

  // If it's a Clang declaration, ask Clang to generate the IR declaration.
  // This might generate new functions, so we should do it before computing
  // the insert-before point.
  llvm::Constant *clangAddr = nullptr;
  if (auto clangDecl = f->getClangDecl()) {
    auto globalDecl = getClangGlobalDeclForFunction(clangDecl);
    clangAddr = getAddrOfClangGlobalDecl(globalDecl, forDefinition);
  }

  bool isDefinition = f->isDefinition();
  bool hasOrderNumber =
      isDefinition && !shouldCallPreviousImplementation;
  unsigned orderNumber = ~0U;
  llvm::Function *insertBefore = nullptr;

  // If the SIL function has a definition, we should have an order
  // number for it; make sure to insert it in that position relative
  // to other ordered functions.
  if (hasOrderNumber) {
    orderNumber = IRGen.getFunctionOrder(f);
    if (auto emittedFunctionIterator
          = EmittedFunctionsByOrder.findLeastUpperBound(orderNumber))
      insertBefore = *emittedFunctionIterator;
  }

  // If it's a Clang declaration, check whether Clang gave us a declaration.
  if (clangAddr) {
    fn = dyn_cast<llvm::Function>(clangAddr->stripPointerCasts());

    // If we have a function, move it to the appropriate position.
    if (fn) {
      if (hasOrderNumber) {
        auto &fnList = Module.getFunctionList();
        fnList.remove(fn);
        fnList.insert(llvm::Module::iterator(insertBefore), fn);

        EmittedFunctionsByOrder.insert(orderNumber, fn);
      }
      return fn;
    }

  // Otherwise, if we have a lazy definition for it, be sure to queue that up.
  } else if (isDefinition && !forDefinition && !f->isPossiblyUsedExternally() &&
             !hasCodeCoverageInstrumentation(*f, getSILModule())) {
    IRGen.addLazyFunction(f);
  }

  Signature signature = getSignature(f->getLoweredFunctionType());
  addLLVMFunctionAttributes(f, signature);

  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  fn = createFunction(*this, link, signature, insertBefore,
                      f->getOptimizationMode());

  // If `hasCReferences` is true, then the function is either marked with
  // @_silgen_name OR @_cdecl.  If it is the latter, it must have a definition
  // associated with it.  The combination of the two allows us to identify the
  // @_silgen_name functions.  These are locally defined function thunks used in
  // the standard library.  Do not give them DLLImport DLL Storage.
  if (!forDefinition) {
    fn->setComdat(nullptr);
    if (f->hasCReferences())
      fn->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
  }

  // If we have an order number for this function, set it up as appropriate.
  if (hasOrderNumber) {
    EmittedFunctionsByOrder.insert(orderNumber, fn);
  }
  return fn;
}

static llvm::GlobalVariable *createGOTEquivalent(IRGenModule &IGM,
                                                 llvm::Constant *global,
                                                 LinkEntity entity)
{
  // Determine the name of this entity.
  llvm::SmallString<64> globalName;
  entity.mangle(globalName);

  if (IGM.Triple.getObjectFormat() == llvm::Triple::COFF) {
    if (cast<llvm::GlobalValue>(global)->hasDLLImportStorageClass()) {
      llvm::GlobalVariable *GV =
          new llvm::GlobalVariable(IGM.Module, global->getType(),
                                   /*Constant=*/true,
                                   llvm::GlobalValue::ExternalLinkage,
                                   nullptr, llvm::Twine("__imp_") + globalName);
      GV->setExternallyInitialized(true);
      return GV;
    }
  }

  auto gotEquivalent = new llvm::GlobalVariable(IGM.Module,
                                      global->getType(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      global,
                                      llvm::Twine("got.") + globalName);
  
  // rdar://problem/50968433: Unnamed_addr constants appear to get emitted
  // with incorrect alignment by the LLVM JIT in some cases. Don't use
  // unnamed_addr as a workaround.
  // rdar://problem/53836960: i386 ld64 also mis-links relative references
  // to GOT entries.
  if (!IGM.getOptions().UseJIT
      && (!IGM.Triple.isOSDarwin()
          || IGM.Triple.getArch() != llvm::Triple::x86)) {
    gotEquivalent->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  } else {
    ApplyIRLinkage(IRLinkage::InternalLinkOnceODR)
      .to(gotEquivalent);
  }
  
  return gotEquivalent;
}

llvm::Constant *IRGenModule::getOrCreateGOTEquivalent(llvm::Constant *global,
                                                      LinkEntity entity) {
  auto &gotEntry = GlobalGOTEquivalents[entity];
  if (gotEntry) {
    return gotEntry;
  }

  // Use the global as the initializer for an anonymous constant. LLVM can treat
  // this as equivalent to the global's GOT entry.
  auto gotEquivalent = createGOTEquivalent(*this, global, entity);
  gotEntry = gotEquivalent;
  return gotEquivalent;
}

static llvm::Constant *getElementBitCast(llvm::Constant *ptr,
                                         llvm::Type *newEltType) {
  auto ptrType = cast<llvm::PointerType>(ptr->getType());
  if (ptrType->getElementType() == newEltType) {
    return ptr;
  } else {
    auto newPtrType = newEltType->getPointerTo(ptrType->getAddressSpace());
    return llvm::ConstantExpr::getBitCast(ptr, newPtrType);
  }
}

/// Return a reference to an object that's suitable for being used for
/// the given kind of reference.
///
/// Note that, if the requested reference kind is a relative reference.
/// the returned constant will not actually be a relative reference.
/// To form the actual relative reference, you must pass the returned
/// result to emitRelativeReference, passing the correct base-address
/// information.
ConstantReference
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity,
                                   ConstantInit definition,
                                   DebugTypeInfo debugType,
                                   SymbolReferenceKind refKind,
                                   llvm::Type *overrideDeclType) {
  switch (refKind) {
  case SymbolReferenceKind::Relative_Direct:
  case SymbolReferenceKind::Far_Relative_Direct:
    assert(!definition);
    // FIXME: don't just fall through; force the creation of a weak
    // definition so that we can emit a relative reference.
    LLVM_FALLTHROUGH;

  case SymbolReferenceKind::Absolute:
    return { getAddrOfLLVMVariable(entity, definition, debugType,
                                   overrideDeclType),
             ConstantReference::Direct };


  case SymbolReferenceKind::Relative_Indirectable:
  case SymbolReferenceKind::Far_Relative_Indirectable:
    assert(!definition);
    return getAddrOfLLVMVariableOrGOTEquivalent(entity);
  }
  llvm_unreachable("bad reference kind");
}

/// A convenient wrapper around getAddrOfLLVMVariable which uses the
/// default type as the definition type.
llvm::Constant *
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity,
                                   ForDefinition_t forDefinition,
                                   DebugTypeInfo debugType) {
  auto definition = forDefinition
      ? ConstantInit::getDelayed(entity.getDefaultDeclarationType(*this))
      : ConstantInit();
  return getAddrOfLLVMVariable(entity, definition, debugType);
}

/// Get or create an llvm::GlobalVariable.
///
/// If a definition type is given, the result will always be an
/// llvm::GlobalVariable of that type.  Otherwise, the result will
/// have type pointerToDefaultType and may involve bitcasts.
llvm::Constant *
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity,
                                   ConstantInit definition,
                                   DebugTypeInfo DbgTy,
                                   llvm::Type *overrideDeclType) {
  // This function assumes that 'globals' only contains GlobalValue
  // values for the entities that it will look up.

  llvm::Type *definitionType = (definition ? definition.getType() : nullptr);
  auto defaultType = overrideDeclType
    ? overrideDeclType
    : entity.getDefaultDeclarationType(*this);
  
  auto &entry = GlobalVars[entity];
  if (entry) {
    auto existing = cast<llvm::GlobalValue>(entry);

    // If we're looking to define something, we may need to replace a
    // forward declaration.
    if (definitionType) {
      assert(existing->isDeclaration() && "already defined");
      updateLinkageForDefinition(*this, existing, entity);

      // If the existing entry is a variable of the right type,
      // set the initializer on it and return.
      if (auto var = dyn_cast<llvm::GlobalVariable>(existing)) {
        if (definitionType == var->getValueType()) {
          if (definition.hasInit())
            definition.getInit().installInGlobal(var);
          return var;
        }
      }

      // Fall out to the case below, clearing the name so that
      // createVariable doesn't detect a collision.
      entry->setName("");

    // Otherwise, we have a previous declaration or definition which
    // we need to ensure has the right type.
    } else {
      return getElementBitCast(entry, defaultType);
    }
  }

  ForDefinition_t forDefinition = (ForDefinition_t) (definitionType != nullptr);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  // Clang may have defined the variable already.
  if (auto existing = Module.getNamedGlobal(link.getName()))
    return getElementBitCast(existing, defaultType);

  // If we're not defining the object now, forward declare it with the default
  // type.
  if (!definitionType) definitionType = defaultType;

  // Create the variable.
  auto var = createVariable(*this, link, definitionType,
                            entity.getAlignment(*this), DbgTy);

  // Install the concrete definition if we have one.
  if (definition && definition.hasInit()) {
    definition.getInit().installInGlobal(var);
  }

  // If we have an existing entry, destroy it, replacing it with the
  // new variable.
  if (entry) {
    auto existing = cast<llvm::GlobalValue>(entry);
    auto castVar = llvm::ConstantExpr::getBitCast(var, entry->getType());
    existing->replaceAllUsesWith(castVar);
    existing->eraseFromParent();
  }

  // If there's also an existing GOT-equivalent entry, rewrite it too, since
  // LLVM won't recognize a global with bitcasts in its initializers as GOT-
  // equivalent. rdar://problem/22388190
  auto foundGOTEntry = GlobalGOTEquivalents.find(entity);
  if (foundGOTEntry != GlobalGOTEquivalents.end() && foundGOTEntry->second) {
    auto existingGOTEquiv = cast<llvm::GlobalVariable>(foundGOTEntry->second);

    // Make a new GOT equivalent referring to the new variable with its
    // definition type.
    auto newGOTEquiv = createGOTEquivalent(*this, var, entity);
    auto castGOTEquiv = llvm::ConstantExpr::getBitCast(newGOTEquiv,
                                                   existingGOTEquiv->getType());
    existingGOTEquiv->replaceAllUsesWith(castGOTEquiv);
    existingGOTEquiv->eraseFromParent();
    GlobalGOTEquivalents[entity] = newGOTEquiv;
  }

  // Cache and return.
  entry = var;
  return var;
}

/// Get or create a "GOT equivalent" llvm::GlobalVariable, if applicable.
///
/// Creates a private, unnamed constant containing the address of another
/// global variable. LLVM can replace relative references to this variable with
/// relative references to the GOT entry for the variable in the object file.
ConstantReference
IRGenModule::getAddrOfLLVMVariableOrGOTEquivalent(LinkEntity entity,
                              ConstantReference::Directness forceIndirectness) {
  // Handle SILFunctions specially, because unlike other entities they aren't
  // variables and aren't kept in the GlobalVars table.
  if (entity.isSILFunction()) {
    auto *silFn = entity.getSILFunction();
    auto fn = getAddrOfSILFunction(silFn, NotForDefinition);
    if (silFn->isDefinition() &&
        !isAvailableExternally(silFn->getLinkage()) &&
        this == IRGen.getGenModule(silFn)) {
      return {fn, ConstantReference::Direct};
    }
    
    auto gotEquivalent = getOrCreateGOTEquivalent(fn, entity);
    return {gotEquivalent, ConstantReference::Indirect};
  }
  
  // ObjC class references can always be directly referenced, even in
  // the weird cases where we don't see a definition.
  if (entity.isObjCClassRef()) {
    auto value = getAddrOfObjCClassRef(
      const_cast<ClassDecl *>(cast<ClassDecl>(entity.getDecl())));
    return { cast<llvm::Constant>(value.getAddress()),
             ConstantReference::Direct };
  }
  
  // Ensure the variable is at least forward-declared.
  getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());

  // Guess whether a global entry is a definition from this TU. This isn't
  // bulletproof, but at the point we emit conformance tables, we're far enough
  // along that we should have emitted any metadata objects we were going to.
  auto isDefinition = [&](llvm::Constant *global) -> bool {
    // We only emit aliases for definitions. (An extern alias would be an
    // extern global.)
    if (isa<llvm::GlobalAlias>(global))
      return true;
    // Global vars are definitions if they have an initializer.
    if (auto var = dyn_cast<llvm::GlobalVariable>(global))
      return var->hasInitializer();
    // Assume anything else isn't a definition.
    return false;
  };
  
  //
  auto entry = GlobalVars[entity];
  
  /// Returns a direct reference.
  auto direct = [&]() -> ConstantReference {
    // FIXME: Relative references to aliases break MC on 32-bit Mach-O
    // platforms (rdar://problem/22450593 ), so substitute an alias with its
    // aliasee to work around that.
    if (auto alias = dyn_cast<llvm::GlobalAlias>(entry))
      return {alias->getAliasee(), ConstantReference::Direct};
    return {entry, ConstantReference::Direct};
  };
  
  /// Returns an indirect reference.
  auto indirect = [&]() -> ConstantReference {
    auto gotEquivalent = getOrCreateGOTEquivalent(
      cast<llvm::GlobalValue>(entry), entity);
    return {gotEquivalent, ConstantReference::Indirect};
  };
  
  // Return the GOT entry if we were asked to.
  if (forceIndirectness == ConstantReference::Indirect)
    return indirect();
  
  // The integrated REPL incrementally adds new definitions, so always use
  // indirect references in this mode.
  if (IRGen.Opts.IntegratedREPL)
    return indirect();

  // Nominal type descriptors are only emitted once and can only be
  // referenced directly from the same TU.
  if (entity.isNominalTypeDescriptor()) {
    auto *dc = entity.getDecl()->getDeclContext();
    if (isa<ClangModuleUnit>(dc->getModuleScopeContext()) &&
        this != IRGen.getGenModule(dc))
      return indirect();
  }

  // If the variable has already been defined in this TU,
  // then it definitely doesn't need a GOT entry, and we can
  // relative-reference it directly.
  if (!entity.isAvailableExternally(*this) || isDefinition(entry)) {
    return direct();
  }

  // If the entity will be emitted as part of the current source file
  // (if we know what that is), then we can reference it directly.
  if (CurSourceFile
      && !isa<ClangModuleUnit>(CurSourceFile)
      && CurSourceFile == entity.getSourceFileForEmission())
    return direct();
  
  // TODO: If we know the target entry is going to be linked into the same
  // binary, then we ought to be able to directly relative-reference the
  // symbol. However, some platforms don't have the necessary relocations to
  // represent a relative reference to an undefined symbol, so conservatively
  // produce an indirect reference in this case.

  // Fall back to an indirect reference if we can't establish that a direct
  // reference is OK.
  return indirect();
}

static TypeEntityReference
getContextDescriptorEntityReference(IRGenModule &IGM, const LinkEntity &entity){
  // TODO: consider using a symbolic reference (i.e. a symbol string
  // to be looked up dynamically) for types defined outside the module.
  auto ref = IGM.getAddrOfLLVMVariableOrGOTEquivalent(entity);
  auto kind = ref.isIndirect()
                ? TypeReferenceKind::IndirectTypeDescriptor
                : TypeReferenceKind::DirectTypeDescriptor;
  return TypeEntityReference(kind, ref.getValue());
}

static TypeEntityReference
getTypeContextDescriptorEntityReference(IRGenModule &IGM,
                                        NominalTypeDecl *decl) {
  auto entity = LinkEntity::forNominalTypeDescriptor(decl);
  IGM.IRGen.noteUseOfTypeContextDescriptor(decl, DontRequireMetadata);
  return getContextDescriptorEntityReference(IGM, entity);
}

static TypeEntityReference
getProtocolDescriptorEntityReference(IRGenModule &IGM, ProtocolDecl *protocol) {
  assert(!protocol->isObjC() &&
         "objc protocols don't have swift protocol descriptors");
  auto entity = LinkEntity::forProtocolDescriptor(protocol);
  return getContextDescriptorEntityReference(IGM, entity);
}

static TypeEntityReference
getObjCClassByNameReference(IRGenModule &IGM, ClassDecl *cls) {
  auto kind = TypeReferenceKind::DirectObjCClassName;
  SmallString<64> objcRuntimeNameBuffer;
  auto ref = IGM.getAddrOfGlobalString(
                                 cls->getObjCRuntimeName(objcRuntimeNameBuffer),
                                 /*willBeRelativelyAddressed=*/true);

  return TypeEntityReference(kind, ref);
}

TypeEntityReference
IRGenModule::getTypeEntityReference(GenericTypeDecl *decl) {
  if (auto protocol = dyn_cast<ProtocolDecl>(decl)) {
    assert(!protocol->isObjC() && "imported protocols not handled here");
    return getProtocolDescriptorEntityReference(*this, protocol);
  }
  
  if (auto opaque = dyn_cast<OpaqueTypeDecl>(decl)) {
    auto entity = LinkEntity::forOpaqueTypeDescriptor(opaque);
    IRGen.noteUseOfOpaqueTypeDescriptor(opaque);
    return getContextDescriptorEntityReference(*this, entity);
  }

  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    auto clas = dyn_cast<ClassDecl>(decl);
    if (!clas) {
      return getTypeContextDescriptorEntityReference(*this, nominal);
    }

    switch (clas->getForeignClassKind()) {
    case ClassDecl::ForeignKind::RuntimeOnly:
      return getObjCClassByNameReference(*this, clas);

    case ClassDecl::ForeignKind::CFType:
      return getTypeContextDescriptorEntityReference(*this, clas);

    case ClassDecl::ForeignKind::Normal:
      if (hasKnownSwiftMetadata(*this, clas)) {
        return getTypeContextDescriptorEntityReference(*this, clas);
      }

      // Note: we would like to use an Objective-C class reference, but the
      // Darwin linker currently has a bug where it will coalesce these symbols
      // *after* computing a relative offset, causing incorrect relative
      // offsets in the metadata. Therefore, reference Objective-C classes by
      // their runtime names.
      return getObjCClassByNameReference(*this, clas);
    }
  }
  llvm_unreachable("bad foreign type kind");
}

/// Form an LLVM constant for the relative distance between a reference
/// (appearing at gep (0, indices) of `base`) and `target`.
llvm::Constant *
IRGenModule::emitRelativeReference(ConstantReference target,
                                   llvm::Constant *base,
                                   ArrayRef<unsigned> baseIndices) {
  llvm::Constant *relativeAddr =
    emitDirectRelativeReference(target.getValue(), base, baseIndices);

  // If the reference is indirect, flag it by setting the low bit.
  // (All of the base, direct target, and GOT entry need to be pointer-aligned
  // for this to be OK.)
  if (target.isIndirect()) {
    relativeAddr = llvm::ConstantExpr::getAdd(relativeAddr,
                             llvm::ConstantInt::get(RelativeAddressTy, 1));
  }

  return relativeAddr;
}

/// Form an LLVM constant for the relative distance between a reference
/// (appearing at gep (0, indices...) of `base`) and `target`.  For now,
/// for this to succeed portably, both need to be globals defined in the
/// current translation unit.
llvm::Constant *
IRGenModule::emitDirectRelativeReference(llvm::Constant *target,
                                         llvm::Constant *base,
                                         ArrayRef<unsigned> baseIndices) {
  // Convert the target to an integer.
  auto targetAddr = llvm::ConstantExpr::getPtrToInt(target, SizeTy);

  SmallVector<llvm::Constant*, 4> indices;
  indices.push_back(llvm::ConstantInt::get(Int32Ty, 0));
  for (unsigned baseIndex : baseIndices) {
    indices.push_back(llvm::ConstantInt::get(Int32Ty, baseIndex));
  };

  // Drill down to the appropriate address in the base, then convert
  // that to an integer.
  auto baseElt = llvm::ConstantExpr::getInBoundsGetElementPtr(
                       base->getType()->getPointerElementType(), base, indices);
  auto baseAddr = llvm::ConstantExpr::getPtrToInt(baseElt, SizeTy);

  // The relative address is the difference between those.
  auto relativeAddr = llvm::ConstantExpr::getSub(targetAddr, baseAddr);

  // Relative addresses can be 32-bit even on 64-bit platforms.
  if (SizeTy != RelativeAddressTy)
    relativeAddr = llvm::ConstantExpr::getTrunc(relativeAddr,
                                                RelativeAddressTy);

  return relativeAddr;
}

/// Emit the protocol descriptors list and return it.
llvm::Constant *IRGenModule::emitSwiftProtocols() {
  if (SwiftProtocols.empty())
    return nullptr;

  // Define the global variable for the protocol list.
  ConstantInitBuilder builder(*this);
  auto recordsArray = builder.beginArray(ProtocolRecordTy);

  for (auto *protocol : SwiftProtocols) {
    auto record = recordsArray.beginStruct(ProtocolRecordTy);

    // Relative reference to the protocol descriptor.
    auto descriptorRef = getAddrOfLLVMVariableOrGOTEquivalent(
                                   LinkEntity::forProtocolDescriptor(protocol));
    record.addRelativeAddress(descriptorRef);

    record.finishAndAddTo(recordsArray);
  }

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.

  auto var = recordsArray.finishAndCreateGlobal(
                                            "\x01l_protocols",
                                            Alignment(4),
                                            /*isConstant*/ true,
                                            llvm::GlobalValue::PrivateLinkage);

  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit protocols for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_protos, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_protocols";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5prt$B";
    break;
  }

  var->setSection(sectionName);
  
  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}

void IRGenModule::addProtocolConformance(ConformanceDescription &&record) {
  // Add this protocol conformance.
  ProtocolConformances.push_back(std::move(record));
}

/// Emit the protocol conformance list and return it.
llvm::Constant *IRGenModule::emitProtocolConformances() {
  if (ProtocolConformances.empty())
    return nullptr;

  // Emit the conformances.
  for (const auto &record : ProtocolConformances)
    emitProtocolConformance(record);

  // Define the global variable for the conformance list.
  ConstantInitBuilder builder(*this);
  auto descriptorArray = builder.beginArray(RelativeAddressTy);

  for (const auto &record : ProtocolConformances) {
    auto conformance = record.conformance;
    auto entity = LinkEntity::forProtocolConformanceDescriptor(conformance);
    auto descriptor =
      getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());
    descriptorArray.addRelativeAddress(descriptor);
  }

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.

  auto var = descriptorArray.finishAndCreateGlobal(
                                          "\x01l_protocol_conformances",
                                          Alignment(4),
                                          /*isConstant*/ true,
                                          llvm::GlobalValue::PrivateLinkage);

  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit protocol conformances for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_proto, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_protocol_conformances";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5prtc$B";
    break;
  }

  var->setSection(sectionName);

  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}


/// Emit type metadata for types that might not have explicit protocol conformances.
llvm::Constant *IRGenModule::emitTypeMetadataRecords() {
  std::string sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_types, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_type_metadata";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5tymd$B";
    break;
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit type metadata table for "
                     "the selected object format.");
  }

  // Do nothing if the list is empty.
  if (RuntimeResolvableTypes.empty())
    return nullptr;

  // Define the global variable for the conformance list.
  // We have to do this before defining the initializer since the entries will
  // contain offsets relative to themselves.
  auto arrayTy = llvm::ArrayType::get(TypeMetadataRecordTy,
                                      RuntimeResolvableTypes.size());

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.
  auto var = new llvm::GlobalVariable(Module, arrayTy,
                                      /*isConstant*/ true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      /*initializer*/ nullptr,
                                      "\x01l_type_metadata_table");

  SmallVector<llvm::Constant *, 8> elts;
  for (auto type : RuntimeResolvableTypes) {
    auto ref = getTypeEntityReference(type);

    // Form the relative address, with the type reference kind in the low bits.
    unsigned arrayIdx = elts.size();
    llvm::Constant *relativeAddr =
      emitDirectRelativeReference(ref.getValue(), var, { arrayIdx, 0 });
    unsigned lowBits = static_cast<unsigned>(ref.getKind());
    if (lowBits != 0) {
      relativeAddr = llvm::ConstantExpr::getAdd(relativeAddr,
                       llvm::ConstantInt::get(RelativeAddressTy, lowBits));
    }

    llvm::Constant *recordFields[] = { relativeAddr };
    auto record = llvm::ConstantStruct::get(TypeMetadataRecordTy,
                                            recordFields);
    elts.push_back(record);
  }

  auto initializer = llvm::ConstantArray::get(arrayTy, elts);

  var->setInitializer(initializer);
  var->setSection(sectionName);
  var->setAlignment(4);

  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}

llvm::Constant *IRGenModule::emitFieldDescriptors() {
  std::string sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_fieldmd, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_fieldmd";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5flmd$B";
    break;
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit field records table for "
                     "the selected object format.");
  }

  // Do nothing if the list is empty.
  if (FieldDescriptors.empty())
    return nullptr;

  // Define the global variable for the field record list.
  // We have to do this before defining the initializer since the entries will
  // contain offsets relative to themselves.
  auto arrayTy =
      llvm::ArrayType::get(FieldDescriptorPtrTy, FieldDescriptors.size());

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.
  auto var = new llvm::GlobalVariable(
      Module, arrayTy,
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage,
      /*initializer*/ nullptr, "\x01l_type_metadata_table");

  SmallVector<llvm::Constant *, 8> elts;
  for (auto *descriptor : FieldDescriptors)
    elts.push_back(
        llvm::ConstantExpr::getBitCast(descriptor, FieldDescriptorPtrTy));

  var->setInitializer(llvm::ConstantArray::get(arrayTy, elts));
  var->setSection(sectionName);
  var->setAlignment(4);
  
  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}

/// Fetch a global reference to a reference to the given Objective-C class.
/// The result is of type ObjCClassPtrTy->getPointerTo().
Address IRGenModule::getAddrOfObjCClassRef(ClassDecl *theClass) {
  assert(ObjCInterop && "getting address of ObjC class ref in no-interop mode");

  LinkEntity entity = LinkEntity::forObjCClassRef(theClass);
  auto DbgTy = DebugTypeInfo::getObjCClass(
      theClass, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, ConstantInit(), DbgTy);

  // Define it lazily.
  if (auto global = dyn_cast<llvm::GlobalVariable>(addr)) {
    if (global->isDeclaration()) {
      global->setSection(GetObjCSectionName("__objc_classrefs",
                                            "regular,no_dead_strip"));
      global->setLinkage(llvm::GlobalVariable::PrivateLinkage);
      global->setExternallyInitialized(true);
      global->setInitializer(getAddrOfObjCClass(theClass, NotForDefinition));
      addCompilerUsedGlobal(global);
    }
  }

  return Address(addr, entity.getAlignment(*this));
}

/// Fetch a global reference to the given Objective-C class.  The
/// result is of type ObjCClassPtrTy.
llvm::Constant *IRGenModule::getAddrOfObjCClass(ClassDecl *theClass,
                                                ForDefinition_t forDefinition) {
  assert(ObjCInterop && "getting address of ObjC class in no-interop mode");
  assert(!theClass->isForeign());
  LinkEntity entity = LinkEntity::forObjCClass(theClass);
  auto DbgTy = DebugTypeInfo::getObjCClass(
      theClass, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, forDefinition, DbgTy);
  return addr;
}

/// Fetch the declaration of a metaclass object. The result is always a
/// GlobalValue of ObjCClassPtrTy, and is either the Objective-C metaclass or
/// the Swift metaclass stub, depending on whether the class is published as an
/// ObjC class.
llvm::Constant *
IRGenModule::getAddrOfMetaclassObject(ClassDecl *decl,
                                      ForDefinition_t forDefinition) {
  assert((!decl->isGenericContext() || decl->hasClangNode()) &&
         "generic classes do not have a static metaclass object");

  auto entity = decl->getMetaclassKind() == ClassDecl::MetaclassKind::ObjC
                    ? LinkEntity::forObjCMetaclass(decl)
                    : LinkEntity::forSwiftMetaclassStub(decl);

  auto DbgTy = DebugTypeInfo::getObjCClass(
      decl, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, forDefinition, DbgTy);
  return addr;
}

/// Fetch the declaration of an Objective-C metadata update callback.
llvm::Function *
IRGenModule::getAddrOfObjCMetadataUpdateFunction(ClassDecl *classDecl,
                                                 ForDefinition_t forDefinition) {
  assert(ObjCInterop);

  LinkEntity entity = LinkEntity::forObjCMetadataUpdateFunction(classDecl);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  // Class _Nullable callback(Class _Nonnull cls, void * _Nullable arg);
  Signature signature(ObjCUpdateCallbackTy, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the declaration of an Objective-C resilient class stub.
llvm::Constant *
IRGenModule::getAddrOfObjCResilientClassStub(ClassDecl *classDecl,
                                             ForDefinition_t forDefinition,
                                             TypeMetadataAddress addr) {
  assert(ObjCInterop);
  assert(getClassMetadataStrategy(classDecl) ==
         ClassMetadataStrategy::Resilient);

  LinkEntity entity = LinkEntity::forObjCResilientClassStub(classDecl, addr);
  return getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());
}

/// Fetch the type metadata access function for a non-generic type.
llvm::Function *
IRGenModule::getAddrOfTypeMetadataAccessFunction(CanType type,
                                              ForDefinition_t forDefinition) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  NominalTypeDecl *Nominal = type->getNominalOrBoundGenericNominal();
  IRGen.noteUseOfTypeMetadata(Nominal);

  LinkEntity entity = LinkEntity::forTypeMetadataAccessFunction(type);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *params[] = { SizeTy }; // MetadataRequest
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy, params, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the opaque type descriptor access function.
llvm::Function *IRGenModule::getAddrOfOpaqueTypeDescriptorAccessFunction(
    OpaqueTypeDecl *decl, ForDefinition_t forDefinition, bool implementation) {
  IRGen.noteUseOfOpaqueTypeDescriptor(decl);

  LinkEntity entity =
      implementation ? LinkEntity::forOpaqueTypeDescriptorAccessorImpl(decl)
                     : LinkEntity::forOpaqueTypeDescriptorAccessor(decl);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition)
      updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto fnType = llvm::FunctionType::get(OpaqueTypeDescriptorPtrTy, {}, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the type metadata access function for the given generic type.
llvm::Function *
IRGenModule::getAddrOfGenericTypeMetadataAccessFunction(
                                           NominalTypeDecl *nominal,
                                           ArrayRef<llvm::Type *> genericArgs,
                                           ForDefinition_t forDefinition) {
  assert(nominal->isGenericContext());
  assert(!genericArgs.empty() ||
         nominal->getGenericSignature()->areAllParamsConcrete());
  IRGen.noteUseOfTypeMetadata(nominal);

  auto type = nominal->getDeclaredType()->getCanonicalType();
  assert(type->hasUnboundGenericType());
  LinkEntity entity = LinkEntity::forTypeMetadataAccessFunction(type);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  // If we have more arguments than can be passed directly, all of the
  // generic arguments are passed as an array.
  llvm::Type *paramTypesArray[NumDirectGenericTypeMetadataAccessFunctionArgs+1];

  paramTypesArray[0] = SizeTy; // MetadataRequest
  size_t numParams = 1;

  size_t numGenericArgs = genericArgs.size();
  if (numGenericArgs > NumDirectGenericTypeMetadataAccessFunctionArgs) {
    paramTypesArray[1] = Int8PtrPtrTy;
    numParams++;
  } else {
    for (size_t i : indices(genericArgs))
      paramTypesArray[i + 1] = genericArgs[i];
    numParams += numGenericArgs;
  }

  auto paramTypes = llvm::makeArrayRef(paramTypesArray, numParams);
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy,
                                        paramTypes, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Get or create a type metadata cache variable.  These are an
/// implementation detail of type metadata access functions.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataLazyCacheVariable(CanType type) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  LinkEntity entity = LinkEntity::forTypeMetadataLazyCacheVariable(type);
  auto variable =
    getAddrOfLLVMVariable(entity, ForDefinition, DebugTypeInfo());

  // Zero-initialize if we're asking for a definition.
  cast<llvm::GlobalVariable>(variable)->setInitializer(
    llvm::ConstantPointerNull::get(TypeMetadataPtrTy));

  return variable;
}

/// Get or create a type metadata cache variable.  These are an
/// implementation detail of type metadata access functions.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataDemanglingCacheVariable(CanType type,
                                                      ConstantInit definition) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  LinkEntity entity = LinkEntity::forTypeMetadataDemanglingCacheVariable(type);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataSingletonInitializationCache(
                                           NominalTypeDecl *D,
                                           ForDefinition_t forDefinition) {
  auto entity = LinkEntity::forTypeMetadataSingletonInitializationCache(D);
  auto variable =
    getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());

  // Zero-initialize if we're asking for a definition.
  if (forDefinition) {
    cast<llvm::GlobalVariable>(variable)->setInitializer(
      llvm::Constant::getNullValue(variable->getType()->getPointerElementType()));
  }

  return variable;
}

llvm::GlobalValue *IRGenModule::defineAlias(LinkEntity entity,
                                            llvm::Constant *definition) {
  // Check for an existing forward declaration of the alias.
  auto &entry = GlobalVars[entity];
  llvm::GlobalValue *existingVal = nullptr;
  if (entry) {
    existingVal = cast<llvm::GlobalValue>(entry);
    // Clear the existing value's name so we can steal it.
    existingVal->setName("");
  }

  LinkInfo link = LinkInfo::get(*this, entity, ForDefinition);
  auto *ptrTy = cast<llvm::PointerType>(definition->getType());
  auto *alias = llvm::GlobalAlias::create(
      ptrTy->getElementType(), ptrTy->getAddressSpace(), link.getLinkage(),
      link.getName(), definition, &Module);
  ApplyIRLinkage({link.getLinkage(), link.getVisibility(), link.getDLLStorage()})
      .to(alias);

  if (link.isUsed()) {
    addUsedGlobal(alias);
  }

  // Replace an existing external declaration for the address point.
  if (entry) {
    auto existingVal = cast<llvm::GlobalValue>(entry);

    // FIXME: MC breaks when emitting alias references on some platforms
    // (rdar://problem/22450593 ). Work around this by referring to the aliasee
    // instead.
    llvm::Constant *aliasCast = alias->getAliasee();
    aliasCast = llvm::ConstantExpr::getBitCast(aliasCast,
                                               entry->getType());
    existingVal->replaceAllUsesWith(aliasCast);
    existingVal->eraseFromParent();
  }
  entry = alias;

  return alias;
}

/// Define the metadata for a type.
///
/// Some type metadata has information before the address point that the
/// public symbol for the metadata references. This function will rewrite any
/// existing external declaration to the address point as an alias into the
/// full metadata object.
llvm::GlobalValue *IRGenModule::defineTypeMetadata(CanType concreteType,
                                                   bool isPattern,
                                                   bool isConstant,
                                                   ConstantInitFuture init,
                                                   llvm::StringRef section) {
  assert(init);

  if (isPattern) {
    assert(isConstant && "Type metadata patterns must be constant");
    auto addr = getAddrOfTypeMetadataPattern(concreteType->getAnyNominal(),
                                             init, section);

    return cast<llvm::GlobalValue>(addr);
  }

  /// For concrete metadata, we want to use the initializer on the
  /// "full metadata", and define the "direct" address point as an alias.
  TypeMetadataAddress addrKind;
  unsigned adjustmentIndex;

  auto nominal = concreteType->getAnyNominal();

  // Native Swift class metadata has a destructor before the address point.
  // Foreign class metadata candidates do not, and neither does value type
  // metadata.
  if (nominal && isa<ClassDecl>(nominal) &&
      !requiresForeignTypeMetadata(nominal)) {
    addrKind = TypeMetadataAddress::FullMetadata;
    adjustmentIndex = MetadataAdjustmentIndex::Class;
  } else {
    addrKind = TypeMetadataAddress::FullMetadata;
    adjustmentIndex = MetadataAdjustmentIndex::ValueType;
  }

  auto entity = LinkEntity::forTypeMetadata(concreteType, addrKind);

  auto DbgTy = DebugTypeInfo::getMetadata(MetatypeType::get(concreteType),
    entity.getDefaultDeclarationType(*this)->getPointerTo(),
    Size(0), Alignment(1));

  // Define the variable.
  llvm::GlobalVariable *var = cast<llvm::GlobalVariable>(
      getAddrOfLLVMVariable(entity, init, DbgTy));

  var->setConstant(isConstant);
  if (!section.empty())
    var->setSection(section);

  LinkInfo link = LinkInfo::get(*this, entity, ForDefinition);
  if (link.isUsed())
    addUsedGlobal(var);

  // Keep type metadata around for all types.
  if (nominal)
    addRuntimeResolvableType(nominal);

  // Don't define the alias for foreign type metadata, since it's not ABI.
  if (nominal && requiresForeignTypeMetadata(nominal))
    return var;

  // For concrete metadata, declare the alias to its address point.
  auto directEntity = LinkEntity::forTypeMetadata(concreteType,
                                             TypeMetadataAddress::AddressPoint);

  llvm::Constant *addr = var;
  // Do an adjustment if necessary.
  if (adjustmentIndex) {
    llvm::Constant *indices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, adjustmentIndex)
    };
    addr = llvm::ConstantExpr::getInBoundsGetElementPtr(/*Ty=*/nullptr,
                                                        addr, indices);
  }
  addr = llvm::ConstantExpr::getBitCast(addr, TypeMetadataPtrTy);
  return defineAlias(directEntity, addr);
}

/// Fetch the declaration of the (possibly uninitialized) metadata for a type.
llvm::Constant *IRGenModule::getAddrOfTypeMetadata(CanType concreteType) {
  return getAddrOfTypeMetadata(concreteType,
                               SymbolReferenceKind::Absolute).getDirectValue();
}

ConstantReference IRGenModule::getAddrOfTypeMetadata(CanType concreteType,
                                               SymbolReferenceKind refKind) {
  assert(!isa<UnboundGenericType>(concreteType));

  auto nominal = concreteType->getAnyNominal();

  llvm::Type *defaultVarTy;
  unsigned adjustmentIndex;
  
  // Foreign classes reference the full metadata with a GEP.
  if (nominal && requiresForeignTypeMetadata(nominal)) {
    defaultVarTy = FullTypeMetadataStructTy;
    adjustmentIndex = MetadataAdjustmentIndex::ValueType;
  // The symbol for other nominal type metadata is generated at the address
  // point.
  } else if (nominal) {
    assert(!nominal->hasClangNode());

    defaultVarTy = TypeMetadataStructTy;
    adjustmentIndex = 0;
  } else {
    // FIXME: Non-nominal metadata provided by the C++ runtime is exported
    // with the address of the start of the full metadata object, since
    // Clang doesn't provide an easy way to emit symbols aliasing into the
    // middle of an object.
    defaultVarTy = FullTypeMetadataStructTy;
    adjustmentIndex = MetadataAdjustmentIndex::ValueType;
  }

  // If this is a use, and the type metadata is emitted lazily,
  // trigger lazy emission of the metadata.
  if (NominalTypeDecl *nominal = concreteType->getAnyNominal()) {
    IRGen.noteUseOfTypeMetadata(nominal);
  }

  Optional<LinkEntity> entity;
  DebugTypeInfo DbgTy;

  if (nominal && requiresForeignTypeMetadata(nominal)) {
    entity = LinkEntity::forTypeMetadata(concreteType,
                                         TypeMetadataAddress::FullMetadata);
    DbgTy = DebugTypeInfo::getMetadata(MetatypeType::get(concreteType),
                                       defaultVarTy->getPointerTo(), Size(0),
                                       Alignment(1));;
  } else {
    entity = LinkEntity::forTypeMetadata(concreteType,
                                         TypeMetadataAddress::AddressPoint);
    DbgTy = DebugTypeInfo::getMetadata(MetatypeType::get(concreteType),
                                       defaultVarTy->getPointerTo(), Size(0),
                                       Alignment(1));;
  }

  auto addr = getAddrOfLLVMVariable(*entity, ConstantInit(), DbgTy, refKind,
                                    defaultVarTy);
  if (auto *GV = dyn_cast<llvm::GlobalVariable>(addr.getValue()))
    GV->setComdat(nullptr);

  // FIXME: MC breaks when emitting alias references on some platforms
  // (rdar://problem/22450593 ). Work around this by referring to the aliasee
  // instead.
  if (auto alias = dyn_cast<llvm::GlobalAlias>(addr.getValue()))
    addr = ConstantReference(alias->getAliasee(), addr.isIndirect());

  // Adjust if necessary.
  if (adjustmentIndex) {
    llvm::Constant *indices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, adjustmentIndex)
    };
    addr = ConstantReference(
             llvm::ConstantExpr::getInBoundsGetElementPtr(
                                    /*Ty=*/nullptr, addr.getValue(), indices),
                             addr.isIndirect());
  }
  
  return addr;
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataPattern(NominalTypeDecl *D) {
  return getAddrOfTypeMetadataPattern(D, ConstantInit(), "");
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataPattern(NominalTypeDecl *D,
                                          ConstantInit init,
                                          StringRef section) {
  if (!init)
    IRGen.noteUseOfTypeMetadata(D);

  LinkEntity entity = LinkEntity::forTypeMetadataPattern(D);
  auto addr = getAddrOfLLVMVariable(entity, init, DebugTypeInfo());

  if (init) {
    auto var = cast<llvm::GlobalVariable>(addr);
    var->setConstant(true);
    if (!section.empty())
      var->setSection(section);

    // Keep type metadata around for all types.
    addRuntimeResolvableType(D);
  }

  return addr;
}

/// Returns the address of a class metadata base offset.
llvm::Constant *
IRGenModule::getAddrOfClassMetadataBounds(ClassDecl *D,
                                          ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forClassMetadataBaseOffset(D);
  return getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());
}

/// Return the address of a generic type's metadata instantiation cache.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataInstantiationCache(NominalTypeDecl *D,
                                                ForDefinition_t forDefinition) {
  auto entity = LinkEntity::forTypeMetadataInstantiationCache(D);
  return getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());
}

llvm::Function *
IRGenModule::getAddrOfTypeMetadataInstantiationFunction(NominalTypeDecl *D,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forTypeMetadataInstantiationFunction(D);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  // This function is used in two cases -- allocating generic type metadata,
  // and relocating non-generic resilient class metadata.
  llvm::FunctionType *fnType;

  if (D->isGenericContext()) {
    // MetadataInstantiator in ABI/Metadata.h
    llvm::Type *argTys[] = {
      /// Type descriptor.
      TypeContextDescriptorPtrTy,
      /// Generic arguments.
      Int8PtrPtrTy,
      /// Generic metadata pattern.
      Int8PtrTy
    };

    fnType = llvm::FunctionType::get(TypeMetadataPtrTy, argTys,
                                     /*isVarArg*/ false);
  } else {
    assert(isa<ClassDecl>(D));

    // MetadataRelocator in ABI/Metadata.h
    llvm::Type *argTys[] = {
      /// Type descriptor.
      TypeContextDescriptorPtrTy,
      /// Resilient metadata pattern.
      Int8PtrTy
    };

    fnType = llvm::FunctionType::get(TypeMetadataPtrTy, argTys,
                                     /*isVarArg*/ false);
  }

  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfTypeMetadataCompletionFunction(NominalTypeDecl *D,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forTypeMetadataCompletionFunction(D);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *argTys[] = {
    /// Type metadata.
    TypeMetadataPtrTy,
    /// Metadata completion context.
    Int8PtrTy,
    /// Generic metadata pattern.
    Int8PtrPtrTy
  };
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy,
                                        argTys, /*isVarArg*/ false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Return the address of a nominal type descriptor.
llvm::Constant *IRGenModule::getAddrOfTypeContextDescriptor(NominalTypeDecl *D,
                                              RequireMetadata_t requireMetadata,
                                              ConstantInit definition) {
  IRGen.noteUseOfTypeContextDescriptor(D, requireMetadata);

  auto entity = LinkEntity::forNominalTypeDescriptor(D);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfOpaqueTypeDescriptor(
                                               OpaqueTypeDecl *opaqueType,
                                               ConstantInit definition) {
  IRGen.noteUseOfOpaqueTypeDescriptor(opaqueType);
  auto entity = LinkEntity::forOpaqueTypeDescriptor(opaqueType);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::
getAddrOfReflectionBuiltinDescriptor(CanType type,
                                     ConstantInit definition) {
  auto entity = LinkEntity::forReflectionBuiltinDescriptor(type);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::
getAddrOfReflectionFieldDescriptor(CanType type,
                                   ConstantInit definition) {
  auto entity = LinkEntity::forReflectionFieldDescriptor(type);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::
getAddrOfReflectionAssociatedTypeDescriptor(const ProtocolConformance *c,
                                            ConstantInit definition) {
  auto entity = LinkEntity::forReflectionAssociatedTypeDescriptor(c);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

/// Return the address of a property descriptor.
llvm::Constant *IRGenModule::getAddrOfPropertyDescriptor(AbstractStorageDecl *D,
                                                      ConstantInit definition) {
  auto entity = LinkEntity::forPropertyDescriptor(D);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfProtocolDescriptor(ProtocolDecl *D,
                                                      ConstantInit definition) {
  if (D->isObjC()) {
    assert(!definition &&
           "cannot define an @objc protocol descriptor this way");
    return getAddrOfObjCProtocolRecord(D, NotForDefinition);
  }

  auto entity = LinkEntity::forProtocolDescriptor(D);
  return getAddrOfLLVMVariable(entity, definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfProtocolRequirementsBaseDescriptor(
                                                         ProtocolDecl *proto) {
  auto entity = LinkEntity::forProtocolRequirementsBaseDescriptor(proto);
  return getAddrOfLLVMVariable(entity, ConstantInit(),
                               DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineProtocolRequirementsBaseDescriptor(
                                                ProtocolDecl *proto,
                                                llvm::Constant *definition) {
  auto entity = LinkEntity::forProtocolRequirementsBaseDescriptor(proto);
  return defineAlias(entity, definition);
}

llvm::Constant *IRGenModule::getAddrOfAssociatedTypeDescriptor(
                                               AssociatedTypeDecl *assocType) {
  auto entity = LinkEntity::forAssociatedTypeDescriptor(assocType);
  return getAddrOfLLVMVariable(entity, ConstantInit(),
                               DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineAssociatedTypeDescriptor(
                                                 AssociatedTypeDecl *assocType,
                                                 llvm::Constant *definition) {
  auto entity = LinkEntity::forAssociatedTypeDescriptor(assocType);
  return defineAlias(entity, definition);
}

llvm::Constant *IRGenModule::getAddrOfAssociatedConformanceDescriptor(
                                          AssociatedConformance conformance) {
  auto entity = LinkEntity::forAssociatedConformanceDescriptor(conformance);
  return getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineAssociatedConformanceDescriptor(
                                            AssociatedConformance conformance,
                                            llvm::Constant *definition) {
  auto entity = LinkEntity::forAssociatedConformanceDescriptor(conformance);
  return defineAlias(entity, definition);
}

llvm::Constant *IRGenModule::getAddrOfBaseConformanceDescriptor(
                                                BaseConformance conformance) {
  auto entity = LinkEntity::forBaseConformanceDescriptor(conformance);
  return getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineBaseConformanceDescriptor(
                                            BaseConformance conformance,
                                            llvm::Constant *definition) {
  auto entity = LinkEntity::forBaseConformanceDescriptor(conformance);
  return defineAlias(entity, definition);
}

llvm::Constant *IRGenModule::getAddrOfProtocolConformanceDescriptor(
                                const RootProtocolConformance *conformance,
                                ConstantInit definition) {
  IRGen.addLazyWitnessTable(conformance);

  auto entity = LinkEntity::forProtocolConformanceDescriptor(conformance);
  return getAddrOfLLVMVariable(entity, definition,
                               DebugTypeInfo());
}

/// Fetch the declaration of the ivar initializer for the given class.
Optional<llvm::Function*> IRGenModule::getAddrOfIVarInitDestroy(
                            ClassDecl *cd,
                            bool isDestroyer,
                            bool isForeign,
                            ForDefinition_t forDefinition) {
  auto silRef = SILDeclRef(cd,
                           isDestroyer
                           ? SILDeclRef::Kind::IVarDestroyer
                           : SILDeclRef::Kind::IVarInitializer)
    .asForeign(isForeign);

  // Find the SILFunction for the ivar initializer or destroyer.
  if (auto silFn = getSILModule().lookUpFunction(silRef)) {
    return getAddrOfSILFunction(silFn, forDefinition);
  }

  return None;
}

/// Returns the address of a value-witness function.
llvm::Function *IRGenModule::getAddrOfValueWitness(CanType abstractType,
                                                   ValueWitness index,
                                                ForDefinition_t forDefinition) {
  // We shouldn't emit value witness symbols for generic type instances.
  assert(!isa<BoundGenericType>(abstractType) &&
         "emitting value witness for generic type instance?!");
  
  LinkEntity entity = LinkEntity::forValueWitness(abstractType, index);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getValueWitnessSignature(index);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Returns the address of a value-witness table.  If a definition
/// type is provided, the table is created with that type; the return
/// value will be an llvm::GlobalValue.  Otherwise, the result will
/// have type WitnessTablePtrTy.
llvm::Constant *
IRGenModule::getAddrOfValueWitnessTable(CanType concreteType,
                                        ConstantInit definition) {
  LinkEntity entity = LinkEntity::forValueWitnessTable(concreteType);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

static Address getAddrOfSimpleVariable(IRGenModule &IGM,
                             llvm::DenseMap<LinkEntity, llvm::Constant*> &cache,
                             LinkEntity entity,
                             ForDefinition_t forDefinition) {
  auto alignment = entity.getAlignment(IGM);
  auto type = entity.getDefaultDeclarationType(IGM);
  
  // Check whether it's already cached.
  llvm::Constant *&entry = cache[entity];
  if (entry) {
    auto existing = cast<llvm::GlobalValue>(entry);
    assert(alignment == Alignment(existing->getAlignment()));
    if (forDefinition) updateLinkageForDefinition(IGM, existing, entity);
    return Address(entry, alignment);
  }

  // Otherwise, we need to create it.
  LinkInfo link = LinkInfo::get(IGM, entity, forDefinition);
  auto addr = createVariable(IGM, link, type, alignment);

  entry = addr;
  return Address(addr, alignment);
}

/// getAddrOfFieldOffset - Get the address of the global variable
/// which contains an offset to apply to either an object (if direct)
/// or a metadata object in order to find an offset to apply to an
/// object (if indirect).
///
/// The result is always a GlobalValue.
Address IRGenModule::getAddrOfFieldOffset(VarDecl *var,
                                          ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forFieldOffset(var);
  return getAddrOfSimpleVariable(*this, GlobalVars, entity,
                                 forDefinition);
}

Address IRGenModule::getAddrOfEnumCase(EnumElementDecl *Case,
                                       ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forEnumCase(Case);
  auto addr = getAddrOfSimpleVariable(*this, GlobalVars, entity, forDefinition);

  auto *global = cast<llvm::GlobalVariable>(addr.getAddress());
  global->setConstant(true);

  return addr;
}

void IRGenModule::emitNestedTypeDecls(DeclRange members) {
  for (Decl *member : members) {
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::Extension:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::Param:
    case DeclKind::Module:
    case DeclKind::PrecedenceGroup:
      llvm_unreachable("decl not allowed in type context");

    case DeclKind::IfConfig:
    case DeclKind::PoundDiagnostic:
      continue;

    case DeclKind::Func:
    case DeclKind::Var:
    case DeclKind::Subscript:
      // Handled in SIL.
      continue;
        
    case DeclKind::PatternBinding:
    case DeclKind::Accessor:
    case DeclKind::Constructor:
    case DeclKind::Destructor:
    case DeclKind::EnumCase:
    case DeclKind::EnumElement:
    case DeclKind::MissingMember:
      // Skip non-type members.
      continue;

    case DeclKind::AssociatedType:
    case DeclKind::GenericTypeParam:
      // Do nothing.
      continue;

    case DeclKind::TypeAlias:
    case DeclKind::OpaqueType:
      // Do nothing.
      continue;

    case DeclKind::Enum:
      emitEnumDecl(cast<EnumDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    }
  }
}

static bool shouldEmitCategory(IRGenModule &IGM, ExtensionDecl *ext) {
  for (auto conformance : ext->getLocalConformances()) {
    if (conformance->getProtocol()->isObjC())
      return true;
  }

  for (auto member : ext->getMembers()) {
    if (auto func = dyn_cast<FuncDecl>(member)) {
      if (requiresObjCMethodDescriptor(func))
        return true;
    } else if (auto constructor = dyn_cast<ConstructorDecl>(member)) {
      if (requiresObjCMethodDescriptor(constructor))
        return true;
    } else if (auto var = dyn_cast<VarDecl>(member)) {
      if (requiresObjCPropertyDescriptor(IGM, var))
        return true;
    } else if (auto subscript = dyn_cast<SubscriptDecl>(member)) {
      if (requiresObjCSubscriptDescriptor(IGM, subscript))
        return true;
    }
  }

  return false;
}

void IRGenModule::emitExtension(ExtensionDecl *ext) {
  emitNestedTypeDecls(ext->getMembers());

  addLazyConformances(ext);

  // Generate a category if the extension either introduces a
  // conformance to an ObjC protocol or introduces a method
  // that requires an Objective-C entry point.
  ClassDecl *origClass = ext->getSelfClassDecl();
  if (!origClass)
    return;

  if (shouldEmitCategory(*this, ext)) {
    assert(origClass && !origClass->isForeign() &&
           "foreign types cannot have categories emitted");
    llvm::Constant *category = emitCategoryData(*this, ext);
    category = llvm::ConstantExpr::getBitCast(category, Int8PtrTy);

    auto *theClass = ext->getSelfClassDecl();

    // Categories on class stubs are added to a separate list.
    if (theClass->checkAncestry(AncestryFlags::ResilientOther))
      ObjCCategoriesOnStubs.push_back(category);
    else
      ObjCCategories.push_back(category);

    ObjCCategoryDecls.push_back(ext);
  }
}


/// Create an allocation on the stack.
Address IRGenFunction::createAlloca(llvm::Type *type,
                                    Alignment alignment,
                                    const llvm::Twine &name) {
  llvm::AllocaInst *alloca =
      new llvm::AllocaInst(type, IGM.DataLayout.getAllocaAddrSpace(), name,
                           AllocaIP);
  alloca->setAlignment(alignment.getValue());
  return Address(alloca, alignment);
}

/// Create an allocation of an array on the stack.
Address IRGenFunction::createAlloca(llvm::Type *type,
                                    llvm::Value *ArraySize,
                                    Alignment alignment,
                                    const llvm::Twine &name) {
  llvm::AllocaInst *alloca =
      new llvm::AllocaInst(type, IGM.DataLayout.getAllocaAddrSpace(), ArraySize,
                           alignment.getValue(), name, AllocaIP);
  return Address(alloca, alignment);
}

/// Allocate a fixed-size buffer on the stack.
Address IRGenFunction::createFixedSizeBufferAlloca(const llvm::Twine &name) {
  return createAlloca(IGM.getFixedBufferTy(),
                      getFixedBufferAlignment(IGM),
                      name);
}

/// Get or create a global string constant.
///
/// \returns an i8* with a null terminator; note that embedded nulls
///   are okay
///
/// FIXME: willBeRelativelyAddressed is only needed to work around an ld64 bug
/// resolving relative references to coalesceable symbols.
/// It should be removed when fixed. rdar://problem/22674524
llvm::Constant *IRGenModule::getAddrOfGlobalString(StringRef data,
                                               bool willBeRelativelyAddressed) {
  // Check whether this string already exists.
  auto &entry = GlobalStrings[data];
  if (entry.second) {
    // FIXME: Clear unnamed_addr if the global will be relative referenced
    // to work around an ld64 bug. rdar://problem/22674524
    if (willBeRelativelyAddressed)
      entry.first->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    return entry.second;
  }

  entry = createStringConstant(data, willBeRelativelyAddressed);
  return entry.second;
}

/// Get or create a global UTF-16 string constant.
///
/// \returns an i16* with a null terminator; note that embedded nulls
///   are okay
llvm::Constant *IRGenModule::getAddrOfGlobalUTF16String(StringRef utf8) {
  // Check whether this string already exists.
  auto &entry = GlobalUTF16Strings[utf8];
  if (entry) return entry;

  // If not, first transcode it to UTF16.
  SmallVector<llvm::UTF16, 128> buffer(utf8.size() + 1); // +1 for ending nulls.
  const llvm::UTF8 *fromPtr = (const llvm::UTF8 *) utf8.data();
  llvm::UTF16 *toPtr = &buffer[0];
  (void) ConvertUTF8toUTF16(&fromPtr, fromPtr + utf8.size(),
                            &toPtr, toPtr + utf8.size(),
                            llvm::strictConversion);

  // The length of the transcoded string in UTF-8 code points.
  size_t utf16Length = toPtr - &buffer[0];

  // Null-terminate the UTF-16 string.
  *toPtr = 0;
  ArrayRef<llvm::UTF16> utf16(&buffer[0], utf16Length + 1);

  auto init = llvm::ConstantDataArray::get(LLVMContext, utf16);
  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  // Drill down to make an i16*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(
      global->getValueType(), global, indices);

  // Cache and return.
  entry = address;
  return address;
}

/// Do we have to use resilient access patterns when working with this
/// declaration?
///
/// IRGen is primarily concerned with resilient handling of the following:
/// - For structs, a struct's size might change
/// - For enums, new cases can be added
/// - For classes, the superclass might change the size or number
///   of stored properties
bool IRGenModule::isResilient(NominalTypeDecl *D, ResilienceExpansion expansion) {
  if (expansion == ResilienceExpansion::Maximal &&
      Types.getLoweringMode() == TypeConverter::Mode::CompletelyFragile) {
    return false;
  }
  return D->isResilient(getSwiftModule(), expansion);
}

/// Do we have to use resilient access patterns when working with this
/// class?
///
/// For classes, this means that virtual method calls use dispatch thunks
/// rather than accessing metadata members directly.
bool IRGenModule::hasResilientMetadata(ClassDecl *D,
                                       ResilienceExpansion expansion) {
  if (expansion == ResilienceExpansion::Maximal &&
      Types.getLoweringMode() == TypeConverter::Mode::CompletelyFragile) {
    return false;
  }
  return D->hasResilientMetadata(getSwiftModule(), expansion);
}

// The most general resilience expansion where the given declaration is visible.
ResilienceExpansion
IRGenModule::getResilienceExpansionForAccess(NominalTypeDecl *decl) {
  if (decl->getModuleContext() == getSwiftModule() &&
      decl->getEffectiveAccess() < AccessLevel::Public)
    return ResilienceExpansion::Maximal;
  return ResilienceExpansion::Minimal;
}

// The most general resilience expansion which has knowledge of the declaration's
// layout. Calling isResilient() with this scope will always return false.
ResilienceExpansion
IRGenModule::getResilienceExpansionForLayout(NominalTypeDecl *decl) {
  if (Types.getLoweringMode() == TypeConverter::Mode::CompletelyFragile)
    return ResilienceExpansion::Minimal;

  if (isResilient(decl, ResilienceExpansion::Minimal))
    return ResilienceExpansion::Maximal;

  return getResilienceExpansionForAccess(decl);
}

// The most general resilience expansion which has knowledge of the global
// variable's layout.
ResilienceExpansion
IRGenModule::getResilienceExpansionForLayout(SILGlobalVariable *global) {
  if (hasPublicVisibility(global->getLinkage()))
    return ResilienceExpansion::Minimal;
  return ResilienceExpansion::Maximal;
}

llvm::Function *
IRGenModule::getAddrOfGenericWitnessTableInstantiationFunction(
                                      const NormalProtocolConformance *conf) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forGenericProtocolWitnessTableInstantiationFunction(conf);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto fnType = llvm::FunctionType::get(
      VoidTy, {WitnessTablePtrTy, TypeMetadataPtrTy, Int8PtrPtrTy},
      /*varargs*/ false);
  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the lazy witness table access function for a protocol conformance.
llvm::Function *
IRGenModule::getAddrOfWitnessTableLazyAccessFunction(
                                      const NormalProtocolConformance *conf,
                                              CanType conformingType,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity =
    LinkEntity::forProtocolWitnessTableLazyAccessFunction(conf, conformingType);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::FunctionType *fnType
    = llvm::FunctionType::get(WitnessTablePtrTy, false);

  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  ApplyIRLinkage({link.getLinkage(), link.getVisibility(), link.getDLLStorage()})
      .to(entry);
  return entry;
}

/// Get or create a witness table cache variable.  These are an
/// implementation detail of witness table lazy access functions.
llvm::Constant *
IRGenModule::getAddrOfWitnessTableLazyCacheVariable(
                                      const NormalProtocolConformance *conf,
                                              CanType conformingType,
                                              ForDefinition_t forDefinition) {
  assert(!conformingType->hasArchetype());
  LinkEntity entity =
    LinkEntity::forProtocolWitnessTableLazyCacheVariable(conf, conformingType);
  auto variable = getAddrOfLLVMVariable(entity,
                                        forDefinition,
                                        DebugTypeInfo());

  // Zero-initialize if we're asking for a definition.
  if (forDefinition) {
    cast<llvm::GlobalVariable>(variable)->setInitializer(
      llvm::ConstantPointerNull::get(WitnessTablePtrTy));
  }

  return variable;
}

/// Look up the address of a witness table.
///
/// This can only be used with non-dependent conformances.
llvm::Constant*
IRGenModule::getAddrOfWitnessTable(const RootProtocolConformance *conf,
                                   ConstantInit definition) {
  IRGen.addLazyWitnessTable(conf);

  auto entity = LinkEntity::forProtocolWitnessTable(conf);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

/// Look up the address of a witness table pattern.
///
/// This can only be used with dependent conformances from inside the
/// defining module.
llvm::Constant*
IRGenModule::getAddrOfWitnessTablePattern(const NormalProtocolConformance *conf,
                                          ConstantInit definition) {
  IRGen.addLazyWitnessTable(conf);

  auto entity = LinkEntity::forProtocolWitnessTablePattern(conf);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

llvm::Function *
IRGenModule::getAddrOfAssociatedTypeWitnessTableAccessFunction(
                                  const NormalProtocolConformance *conformance,
                                  const AssociatedConformance &association) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forAssociatedTypeWitnessTableAccessFunction(conformance,
                                                            association);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getAssociatedTypeWitnessTableAccessFunctionSignature();
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfDefaultAssociatedConformanceAccessor(
                                         AssociatedConformance requirement) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forDefaultAssociatedConformanceAccessor(requirement);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getAssociatedTypeWitnessTableAccessFunctionSignature();
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfContinuationPrototype(CanSILFunctionType fnType) {
  LinkEntity entity = LinkEntity::forCoroutineContinuationPrototype(fnType);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return entry;

  auto signature = Signature::forCoroutineContinuation(*this, fnType);
  LinkInfo link = LinkInfo::get(*this, entity, NotForDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Should we be defining the given helper function?
static llvm::Function *shouldDefineHelper(IRGenModule &IGM,
                                          llvm::Constant *fn,
                                          bool setIsNoInline) {
  auto *def = dyn_cast<llvm::Function>(fn);
  if (!def) return nullptr;
  if (!def->empty()) return nullptr;

  ApplyIRLinkage(IRLinkage::InternalLinkOnceODR).to(def);
  def->setDoesNotThrow();
  def->setCallingConv(IGM.DefaultCC);
  if (setIsNoInline)
    def->addFnAttr(llvm::Attribute::NoInline);
  return def;
}

/// Get or create a helper function with the given name and type, lazily
/// using the given generation function to fill in its body.
///
/// The helper function will be shared between translation units within the
/// current linkage unit, so choose the name carefully to ensure that it
/// does not collide with any other helper function.  In general, it should
/// be a Swift-specific C reserved name; that is, it should start with
//  "__swift".
llvm::Constant *
IRGenModule::getOrCreateHelperFunction(StringRef fnName, llvm::Type *resultTy,
                                       ArrayRef<llvm::Type*> paramTys,
                        llvm::function_ref<void(IRGenFunction &IGF)> generate,
                        bool setIsNoInline) {
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(resultTy, paramTys, false);

  llvm::Constant *fn =
      cast<llvm::Function>(Module.getOrInsertFunction(fnName, fnTy)
                               .getCallee()
                               ->stripPointerCasts());

  if (llvm::Function *def = shouldDefineHelper(*this, fn, setIsNoInline)) {
    IRGenFunction IGF(*this, def);
    if (DebugInfo)
      DebugInfo->emitArtificialFunction(IGF, def);
    generate(IGF);
  }

  return fn;
}
