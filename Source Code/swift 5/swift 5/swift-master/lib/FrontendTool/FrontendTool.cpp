//===--- FrontendTool.cpp - Swift Compiler Frontend -----------------------===//
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
///
/// \file
/// This is the entry point to the swift -frontend functionality, which
/// implements the core compiler functionality along with a number of additional
/// tools for demonstration and testing purposes.
///
/// This is separate from the rest of libFrontend to reduce the dependencies
/// required by that library.
///
//===----------------------------------------------------------------------===//

#include "swift/FrontendTool/FrontendTool.h"
#include "ImportedModules.h"
#include "ReferenceDependencies.h"
#include "TBD.h"

#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExperimentalDependencies.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ReferencedNameTracker.h"
#include "swift/AST/TypeRefinementContext.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/Edit.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/JSONSerialization.h"
#include "swift/Basic/LLVMContext.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/Timer.h"
#include "swift/Basic/UUID.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Frontend/SerializedDiagnosticConsumer.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/Immediate/Immediate.h"
#include "swift/Index/IndexRecord.h"
#include "swift/Option/Options.h"
#include "swift/Migrator/FixitFilter.h"
#include "swift/Migrator/Migrator.h"
#include "swift/PrintAsObjC/PrintAsObjC.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Syntax/Serialization/SyntaxSerialization.h"
#include "swift/Syntax/SyntaxNodes.h"
#include "swift/TBDGen/TBDGen.h"

#include "clang/AST/ASTContext.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Option/Option.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Target/TargetMachine.h"

#include <memory>
#include <unordered_set>
#include <utility>

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#else
#include <io.h>
#endif

using namespace swift;

static std::string displayName(StringRef MainExecutablePath) {
  std::string Name = llvm::sys::path::stem(MainExecutablePath);
  Name += " -frontend";
  return Name;
}

StringRef
swift::frontend::utils::escapeForMake(StringRef raw,
                                      llvm::SmallVectorImpl<char> &buffer) {
  buffer.clear();

  // The escaping rules for GNU make are complicated due to the various
  // subsitutions and use of the tab in the leading position for recipes.
  // Various symbols have significance in different contexts.  It is not
  // possible to correctly quote all characters in Make (as of 3.7).  Match
  // gcc and clang's behaviour for the escaping which covers only a subset of
  // characters.
  for (unsigned I = 0, E = raw.size(); I != E; ++I) {
    switch (raw[I]) {
    case '#': // Handle '#' the broken GCC way
      buffer.push_back('\\');
      break;

    case ' ':
      for (unsigned J = I; J && raw[J - 1] == '\\'; --J)
        buffer.push_back('\\');
      buffer.push_back('\\');
      break;

    case '$': // $ is escaped by $
      buffer.push_back('$');
      break;
    }
    buffer.push_back(raw[I]);
  }
  buffer.push_back('\0');

  return buffer.data();
}

/// Emits a Make-style dependencies file.
static bool emitMakeDependenciesIfNeeded(DiagnosticEngine &diags,
                                         DependencyTracker *depTracker,
                                         const FrontendOptions &opts,
                                         const InputFile &input) {
  const std::string &dependenciesFilePath = input.dependenciesFilePath();
  if (dependenciesFilePath.empty())
    return false;

  std::error_code EC;
  llvm::raw_fd_ostream out(dependenciesFilePath, EC, llvm::sys::fs::F_None);

  if (out.has_error() || EC) {
    diags.diagnose(SourceLoc(), diag::error_opening_output,
                   dependenciesFilePath, EC.message());
    out.clear_error();
    return true;
  }

  llvm::SmallString<256> buffer;

  // FIXME: Xcode can't currently handle multiple targets in a single
  // dependency line.
  opts.forAllOutputPaths(input, [&](const StringRef targetName) {
    out << swift::frontend::utils::escapeForMake(targetName, buffer) << " :";
    // First include all other files in the module. Make-style dependencies
    // need to be conservative!
    for (auto const &path :
         reversePathSortedFilenames(opts.InputsAndOutputs.getInputFilenames()))
      out << ' ' << swift::frontend::utils::escapeForMake(path, buffer);
    // Then print dependencies we've picked up during compilation.
    for (auto const &path :
           reversePathSortedFilenames(depTracker->getDependencies()))
      out << ' ' << swift::frontend::utils::escapeForMake(path, buffer);
    out << '\n';
  });

  return false;
}

static bool emitMakeDependenciesIfNeeded(DiagnosticEngine &diags,
                                         DependencyTracker *depTracker,
                                         const FrontendOptions &opts) {
  return opts.InputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &f) -> bool {
        return emitMakeDependenciesIfNeeded(diags, depTracker, opts, f);
      });
}

namespace {
struct SwiftModuleTraceInfo {
  Identifier Name;
  std::string Path;
  bool IsImportedDirectly;
  bool SupportsLibraryEvolution;
};

struct LoadedModuleTraceFormat {
  static const unsigned CurrentVersion = 2;
  unsigned Version;
  Identifier Name;
  std::string Arch;
  std::vector<SwiftModuleTraceInfo> SwiftModules;
};
}

namespace swift {
namespace json {
template <> struct ObjectTraits<SwiftModuleTraceInfo> {
  static void mapping(Output &out, SwiftModuleTraceInfo &contents) {
    StringRef name = contents.Name.str();
    out.mapRequired("name", name);
    out.mapRequired("path", contents.Path);
    out.mapRequired("isImportedDirectly", contents.IsImportedDirectly);
    out.mapRequired("supportsLibraryEvolution",
                    contents.SupportsLibraryEvolution);
  }
};

// Version notes:
// 1. Keys: name, arch, swiftmodules
// 2. New keys: version, swiftmodulesDetailedInfo
template <> struct ObjectTraits<LoadedModuleTraceFormat> {
  static void mapping(Output &out, LoadedModuleTraceFormat &contents) {
    out.mapRequired("version", contents.Version);

    StringRef name = contents.Name.str();
    out.mapRequired("name", name);

    out.mapRequired("arch", contents.Arch);

    // The 'swiftmodules' key is kept for backwards compatibility.
    std::vector<std::string> moduleNames;
    for (auto &m : contents.SwiftModules)
      moduleNames.push_back(m.Path);
    out.mapRequired("swiftmodules", moduleNames);

    out.mapRequired("swiftmodulesDetailedInfo", contents.SwiftModules);
  }
};
}
}

/// Compute the per-module information to be recorded in the trace file.
//
// The most interesting/tricky thing here is _which_ paths get recorded in
// the trace file as dependencies. It depends on how the module was synthesized.
// The key points are:
//
// 1. Paths to swiftmodules in the module cache or in the prebuilt cache are not
//    recorded - Precondition: the corresponding path to the swiftinterface must
//    already be present as a key in pathToModuleDecl.
// 2. swiftmodules next to a swiftinterface are saved if they are up-to-date.
//
// FIXME: Use the VFS instead of handling paths directly. We are particularly
// sloppy about handling relative paths in the dependency tracker.
static void computeSwiftModuleTraceInfo(
    const SmallPtrSetImpl<ModuleDecl *> &importedModules,
    const llvm::DenseMap<StringRef, ModuleDecl *> &pathToModuleDecl,
    const DependencyTracker &depTracker,
    StringRef prebuiltCachePath,
    std::vector<SwiftModuleTraceInfo> &traceInfo) {

  SmallString<256> buffer;

  std::string errMsg;
  llvm::raw_string_ostream err(errMsg);

  // FIXME: Use PrettyStackTrace instead.
  auto errorUnexpectedPath =
      [&pathToModuleDecl](llvm::raw_string_ostream &errStream) {
    errStream << "The module <-> path mapping we have is:\n";
    for (auto &m: pathToModuleDecl)
      errStream << m.second->getName() << " <-> " << m.first << '\n';
    llvm::report_fatal_error(errStream.str());
  };

  using namespace llvm::sys;

  auto computeAdjacentInterfacePath = [](SmallVectorImpl<char> &modPath) {
    auto swiftInterfaceExt =
      file_types::getExtension(file_types::TY_SwiftModuleInterfaceFile);
    path::replace_extension(modPath, swiftInterfaceExt);
  };

  for (auto &depPath : depTracker.getDependencies()) {

    // Decide if this is a swiftmodule based on the extension of the raw
    // dependency path, as the true file may have a different one.
    // For example, this might happen when the canonicalized path points to
    // a Content Addressed Storage (CAS) location.
    auto moduleFileType =
      file_types::lookupTypeForExtension(path::extension(depPath));
    auto isSwiftmodule =
      moduleFileType == file_types::TY_SwiftModuleFile;
    auto isSwiftinterface =
      moduleFileType == file_types::TY_SwiftModuleInterfaceFile;

    if (!(isSwiftmodule || isSwiftinterface))
      continue;

    auto dep = pathToModuleDecl.find(depPath);
    if (dep != pathToModuleDecl.end()) {
      // Great, we recognize the path! Check if the file is still around.

      ModuleDecl *depMod = dep->second;
      if(depMod->isResilient() && !isSwiftinterface) {
        // FIXME: Ideally, we would check that the swiftmodule has a
        // swiftinterface next to it. Tracked by rdar://problem/56351399.
      }

      // FIXME: Better error handling
      StringRef realDepPath
        = fs::real_path(depPath, buffer, /*expand_tile*/true)
        ? StringRef(depPath) // Couldn't find the canonical path, assume
                             // this is good enough.
        : buffer.str();

      traceInfo.push_back({
        /*Name=*/
        depMod->getName(),
        /*Path=*/
        realDepPath,
        // TODO: There is an edge case which is not handled here.
        // When we build a framework using -import-underlying-module, or an
        // app/test using -import-objc-header, we should look at the direct
        // imports of the bridging modules, and mark those as our direct
        // imports.
        /*IsImportedDirectly=*/
        importedModules.find(depMod) != importedModules.end(),
        /*SupportsLibraryEvolution=*/
        depMod->isResilient()
      });
      buffer.clear();

      continue;
    }

    // If the depTracker had an interface, that means that we must've
    // built a swiftmodule from that interface, so we should have that
    // filename available.
    if (isSwiftinterface) {
      err << "Unexpected path for swiftinterface file:\n" << depPath << "\n";
      errorUnexpectedPath(err);
    }

    // Skip cached modules in the prebuilt cache. We will add the corresponding
    // swiftinterface from the SDK directly, but this isn't checked. :-/
    //
    // FIXME: This is incorrect if both paths are not relative w.r.t. to the
    // same root.
    if (StringRef(depPath).startswith(prebuiltCachePath))
      continue;

    // If we have a swiftmodule next to an interface, that interface path will
    // be saved (not checked), so don't save the path to this swiftmodule.
    SmallString<256> moduleAdjacentInterfacePath(depPath);
    computeAdjacentInterfacePath(moduleAdjacentInterfacePath);
    if (pathToModuleDecl.find(moduleAdjacentInterfacePath)
        != pathToModuleDecl.end())
      continue;

    // FIXME: The behavior of fs::exists for relative paths is undocumented.
    // Use something else instead?
    if (fs::exists(moduleAdjacentInterfacePath)) {
      // This should be an error but it is not because of funkiness around
      // compatible modules such as us having both armv7s.swiftinterface
      // and armv7.swiftinterface in the dependency tracker.
      continue;
    }
    buffer.clear();

    // We might land here when we have a arm.swiftmodule in the cache path
    // which added a dependency on a arm.swiftinterface (which was not loaded).
  }

  // Almost a re-implementation of reversePathSortedFilenames :(.
  std::sort(
    traceInfo.begin(), traceInfo.end(),
    [](const SwiftModuleTraceInfo &m1, const SwiftModuleTraceInfo &m2) -> bool {
      return std::lexicographical_compare(
        m1.Path.rbegin(), m1.Path.rend(),
        m2.Path.rbegin(), m2.Path.rend());
  });
}

static bool emitLoadedModuleTraceIfNeeded(ModuleDecl *mainModule,
                                          DependencyTracker *depTracker,
                                          StringRef prebuiltCachePath,
                                          StringRef loadedModuleTracePath) {
  ASTContext &ctxt = mainModule->getASTContext();
  assert(!ctxt.hadError()
         && "We should've already exited earlier if there was an error.");

  if (loadedModuleTracePath.empty())
    return false;
  std::error_code EC;
  llvm::raw_fd_ostream out(loadedModuleTracePath, EC, llvm::sys::fs::F_Append);

  if (out.has_error() || EC) {
    ctxt.Diags.diagnose(SourceLoc(), diag::error_opening_output,
                        loadedModuleTracePath, EC.message());
    out.clear_error();
    return true;
  }

  ModuleDecl::ImportFilter filter = ModuleDecl::ImportFilterKind::Public;
  filter |= ModuleDecl::ImportFilterKind::Private;
  filter |= ModuleDecl::ImportFilterKind::ImplementationOnly;
  SmallVector<ModuleDecl::ImportedModule, 8> imports;
  mainModule->getImportedModules(imports, filter);

  SmallPtrSet<ModuleDecl *, 8> importedModules;
  for (std::pair<ModuleDecl::AccessPathTy, ModuleDecl *> &import : imports)
    importedModules.insert(import.second);

  llvm::DenseMap<StringRef, ModuleDecl *> pathToModuleDecl;
  for (auto &module : ctxt.LoadedModules) {
    ModuleDecl *loadedDecl = module.second;
    if (!loadedDecl)
      llvm::report_fatal_error("Expected loaded modules to be non-null.");
    if (loadedDecl == mainModule)
      continue;
    if (loadedDecl->getModuleFilename().empty())
      llvm::report_fatal_error(
        "Don't know how to handle modules with empty names."
        " One potential reason for getting an empty module name might"
        " be that the module could not be deserialized correctly.");
    pathToModuleDecl.insert(
      std::make_pair(loadedDecl->getModuleFilename(), loadedDecl));
  }

  std::vector<SwiftModuleTraceInfo> swiftModules;
  computeSwiftModuleTraceInfo(importedModules, pathToModuleDecl, *depTracker,
                              prebuiltCachePath, swiftModules);

  LoadedModuleTraceFormat trace = {
      /*version=*/LoadedModuleTraceFormat::CurrentVersion,
      /*name=*/mainModule->getName(),
      /*arch=*/ctxt.LangOpts.Target.getArchName(),
      swiftModules
  };

  // raw_fd_ostream is unbuffered, and we may have multiple processes writing,
  // so first write to memory and then dump the buffer to the trace file.
  std::string stringBuffer;
  {
    llvm::raw_string_ostream memoryBuffer(stringBuffer);
    json::Output jsonOutput(memoryBuffer, /*UserInfo=*/{},
                            /*PrettyPrint=*/false);
    json::jsonize(jsonOutput, trace, /*Required=*/true);
  }
  stringBuffer += "\n";
  out << stringBuffer;

  return true;
}

static bool
emitLoadedModuleTraceForAllPrimariesIfNeeded(ModuleDecl *mainModule,
                                             DependencyTracker *depTracker,
                                             const FrontendOptions &opts) {
  return opts.InputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &input) -> bool {
        return emitLoadedModuleTraceIfNeeded(
          mainModule, depTracker, opts.PrebuiltModuleCachePath,
          input.loadedModuleTracePath());
      });
}

/// Gets an output stream for the provided output filename, or diagnoses to the
/// provided AST Context and returns null if there was an error getting the
/// stream.
static std::unique_ptr<llvm::raw_fd_ostream>
getFileOutputStream(StringRef OutputFilename, ASTContext &Ctx) {
  std::error_code errorCode;
  auto os = llvm::make_unique<llvm::raw_fd_ostream>(
              OutputFilename, errorCode, llvm::sys::fs::F_None);
  if (errorCode) {
    Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_output,
                       OutputFilename, errorCode.message());
    return nullptr;
  }
  return os;
}

/// Writes the Syntax tree to the given file
static bool emitSyntax(SourceFile *SF, LangOptions &LangOpts,
                       SourceManager &SM, StringRef OutputFilename) {
  auto bufferID = SF->getBufferID();
  assert(bufferID && "frontend should have a buffer ID "
         "for the main source file");
  (void)bufferID;

  auto os = getFileOutputStream(OutputFilename, SF->getASTContext());
  if (!os) return true;

  json::Output jsonOut(*os, /*UserInfo=*/{}, /*PrettyPrint=*/false);
  auto Root = SF->getSyntaxRoot().getRaw();
  jsonOut << *Root;
  *os << "\n";
  return false;
}

/// Writes SIL out to the given file.
static bool writeSIL(SILModule &SM, ModuleDecl *M, bool EmitVerboseSIL,
                     StringRef OutputFilename, bool SortSIL) {
  auto OS = getFileOutputStream(OutputFilename, M->getASTContext());
  if (!OS) return true;
  SM.print(*OS, EmitVerboseSIL, M, SortSIL);

  return M->getASTContext().hadError();
}

static bool writeSIL(SILModule &SM, const PrimarySpecificPaths &PSPs,
                     CompilerInstance &Instance,
                     CompilerInvocation &Invocation) {
  const FrontendOptions &opts = Invocation.getFrontendOptions();
  return writeSIL(SM, Instance.getMainModule(), opts.EmitVerboseSIL,
                  PSPs.OutputFilename, opts.EmitSortedSIL);
}

/// Prints the Objective-C "generated header" interface for \p M to \p
/// outputPath.
///
/// ...unless \p outputPath is empty, in which case it does nothing.
///
/// \returns true if there were any errors
///
/// \see swift::printAsObjC
static bool printAsObjCIfNeeded(StringRef outputPath, ModuleDecl *M,
                                StringRef bridgingHeader, bool moduleIsPublic) {
  if (outputPath.empty())
    return false;
  return withOutputFile(M->getDiags(), outputPath,
                        [&](raw_ostream &out) -> bool {
    auto requiredAccess = moduleIsPublic ? AccessLevel::Public
                                         : AccessLevel::Internal;
    return printAsObjC(out, M, bridgingHeader, requiredAccess);
  });
}

/// Prints the stable module interface for \p M to \p outputPath.
///
/// ...unless \p outputPath is empty, in which case it does nothing.
///
/// \returns true if there were any errors
///
/// \see swift::emitSwiftInterface
static bool
printModuleInterfaceIfNeeded(StringRef outputPath,
                             ModuleInterfaceOptions const &Opts,
                             LangOptions const &LangOpts,
                             ModuleDecl *M) {
  if (outputPath.empty())
    return false;

  DiagnosticEngine &diags = M->getDiags();
  if (!LangOpts.isSwiftVersionAtLeast(5)) {
    assert(LangOpts.isSwiftVersionAtLeast(4));
    diags.diagnose(SourceLoc(),
                   diag::warn_unsupported_module_interface_swift_version,
                   LangOpts.isSwiftVersionAtLeast(4, 2) ? "4.2" : "4");
  }
  if (M->getResilienceStrategy() != ResilienceStrategy::Resilient) {
    diags.diagnose(SourceLoc(),
                   diag::warn_unsupported_module_interface_library_evolution);
  }
  return withOutputFile(diags, outputPath,
                        [M, Opts](raw_ostream &out) -> bool {
    return swift::emitSwiftInterface(out, Opts, M);
  });
}

/// Returns the OutputKind for the given Action.
static IRGenOutputKind getOutputKind(FrontendOptions::ActionType Action) {
  switch (Action) {
  case FrontendOptions::ActionType::EmitIR:
    return IRGenOutputKind::LLVMAssembly;
  case FrontendOptions::ActionType::EmitBC:
    return IRGenOutputKind::LLVMBitcode;
  case FrontendOptions::ActionType::EmitAssembly:
    return IRGenOutputKind::NativeAssembly;
  case FrontendOptions::ActionType::EmitObject:
    return IRGenOutputKind::ObjectFile;
  case FrontendOptions::ActionType::Immediate:
    return IRGenOutputKind::Module;
  default:
    llvm_unreachable("Unknown ActionType which requires IRGen");
    return IRGenOutputKind::ObjectFile;
  }
}

namespace {

/// If there is an error with fixits it writes the fixits as edits in json
/// format.
class JSONFixitWriter
  : public DiagnosticConsumer, public migrator::FixitFilter {
  std::string FixitsOutputPath;
  std::unique_ptr<llvm::raw_ostream> OSPtr;
  bool FixitAll;
  std::vector<SingleEdit> AllEdits;

public:
  JSONFixitWriter(std::string fixitsOutputPath,
                  const DiagnosticOptions &DiagOpts)
    : FixitsOutputPath(fixitsOutputPath),
      FixitAll(DiagOpts.FixitCodeForAllDiagnostics) {}

private:
  void handleDiagnostic(SourceManager &SM,
                        const DiagnosticInfo &Info) override {
    if (!(FixitAll || shouldTakeFixit(Info)))
      return;
    for (const auto &Fix : Info.FixIts) {
      AllEdits.push_back({SM, Fix.getRange(), Fix.getText()});
    }
  }

  bool finishProcessing() override {
    std::error_code EC;
    std::unique_ptr<llvm::raw_fd_ostream> OS;
    OS.reset(new llvm::raw_fd_ostream(FixitsOutputPath,
                                      EC,
                                      llvm::sys::fs::F_None));
    if (EC) {
      // Create a temporary diagnostics engine to print the error to stderr.
      SourceManager dummyMgr;
      DiagnosticEngine DE(dummyMgr);
      PrintingDiagnosticConsumer PDC;
      DE.addConsumer(PDC);
      DE.diagnose(SourceLoc(), diag::cannot_open_file,
                  FixitsOutputPath, EC.message());
      return true;
    }

    swift::writeEditsInJson(llvm::makeArrayRef(AllEdits), *OS);
    return false;
  }
};

} // anonymous namespace

// This is a separate function so that it shows up in stack traces.
LLVM_ATTRIBUTE_NOINLINE
static void debugFailWithAssertion() {
  // Per the user's request, this assertion should always fail in
  // builds with assertions enabled.

  // This should not be converted to llvm_unreachable, as those are
  // treated as optimization hints in builds where they turn into
  // __builtin_unreachable().
  assert((0) && "This is an assertion!");
}

// This is a separate function so that it shows up in stack traces.
LLVM_ATTRIBUTE_NOINLINE
static void debugFailWithCrash() {
  LLVM_BUILTIN_TRAP;
}

/// \return true on error.
static bool emitIndexDataIfNeeded(SourceFile *PrimarySourceFile,
                                  const CompilerInvocation &Invocation,
                                  CompilerInstance &Instance);

static void countStatsOfSourceFile(UnifiedStatsReporter &Stats,
                                   CompilerInstance &Instance,
                                   SourceFile *SF) {
  auto &C = Stats.getFrontendCounters();
  auto &SM = Instance.getSourceMgr();
  C.NumDecls += SF->Decls.size();
  C.NumLocalTypeDecls += SF->LocalTypeDecls.size();
  C.NumObjCMethods += SF->ObjCMethods.size();
  C.NumInfixOperators += SF->InfixOperators.size();
  C.NumPostfixOperators += SF->PostfixOperators.size();
  C.NumPrefixOperators += SF->PrefixOperators.size();
  C.NumPrecedenceGroups += SF->PrecedenceGroups.size();

  auto bufID = SF->getBufferID();
  if (bufID.hasValue()) {
    C.NumSourceLines +=
      SM.getEntireTextForBuffer(bufID.getValue()).count('\n');
  }
}

static void countStatsPostSema(UnifiedStatsReporter &Stats,
                               CompilerInstance& Instance) {
  auto &C = Stats.getFrontendCounters();
  auto &SM = Instance.getSourceMgr();
  C.NumSourceBuffers = SM.getLLVMSourceMgr().getNumBuffers();
  C.NumLinkLibraries = Instance.getLinkLibraries().size();

  auto const &AST = Instance.getASTContext();
  C.NumLoadedModules = AST.LoadedModules.size();

  if (auto *D = Instance.getDependencyTracker()) {
    C.NumDependencies = D->getDependencies().size();
  }

  for (auto SF : Instance.getPrimarySourceFiles()) {
    if (auto *R = SF->getReferencedNameTracker()) {
      C.NumReferencedTopLevelNames += R->getTopLevelNames().size();
      C.NumReferencedDynamicNames += R->getDynamicLookupNames().size();
      C.NumReferencedMemberNames += R->getUsedMembers().size();
    }
  }

  if (!Instance.getPrimarySourceFiles().empty()) {
    for (auto SF : Instance.getPrimarySourceFiles())
      countStatsOfSourceFile(Stats, Instance, SF);
  } else if (auto *M = Instance.getMainModule()) {
    // No primary source file, but a main module; this is WMO-mode
    for (auto *F : M->getFiles()) {
      if (auto *SF = dyn_cast<SourceFile>(F)) {
        countStatsOfSourceFile(Stats, Instance, SF);
      }
    }
  }
}

static void countStatsPostSILGen(UnifiedStatsReporter &Stats,
                                 const SILModule& Module) {
  auto &C = Stats.getFrontendCounters();
  // FIXME: calculate these in constant time, via the dense maps.
  C.NumSILGenFunctions += Module.getFunctionList().size();
  C.NumSILGenVtables += Module.getVTableList().size();
  C.NumSILGenWitnessTables += Module.getWitnessTableList().size();
  C.NumSILGenDefaultWitnessTables += Module.getDefaultWitnessTableList().size();
  C.NumSILGenGlobalVariables += Module.getSILGlobalList().size();
}

static std::unique_ptr<llvm::raw_fd_ostream>
createOptRecordFile(StringRef Filename, DiagnosticEngine &DE) {
  if (Filename.empty())
    return nullptr;

  std::error_code EC;
  auto File = llvm::make_unique<llvm::raw_fd_ostream>(Filename, EC,
                                                      llvm::sys::fs::F_None);
  if (EC) {
    DE.diagnose(SourceLoc(), diag::cannot_open_file, Filename, EC.message());
    return nullptr;
  }
  return File;
}

static bool precompileBridgingHeader(CompilerInvocation &Invocation,
                                     CompilerInstance &Instance) {
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  auto &ImporterOpts = Invocation.getClangImporterOptions();
  auto &PCHOutDir = ImporterOpts.PrecompiledHeaderOutputDir;
  if (!PCHOutDir.empty()) {
    ImporterOpts.BridgingHeader =
        Invocation.getFrontendOptions()
            .InputsAndOutputs.getFilenameOfFirstInput();
    // Create or validate a persistent PCH.
    auto SwiftPCHHash = Invocation.getPCHHash();
    auto PCH = clangImporter->getOrCreatePCH(ImporterOpts, SwiftPCHHash);
    return !PCH.hasValue();
  }
  return clangImporter->emitBridgingPCH(
      Invocation.getFrontendOptions()
          .InputsAndOutputs.getFilenameOfFirstInput(),
      Invocation.getFrontendOptions()
          .InputsAndOutputs.getSingleOutputFilename());
}

static bool precompileClangModule(CompilerInvocation &Invocation,
                                  CompilerInstance &Instance) {
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  return clangImporter->emitPrecompiledModule(
      Invocation.getFrontendOptions()
          .InputsAndOutputs.getFilenameOfFirstInput(),
      Invocation.getFrontendOptions().ModuleName,
      Invocation.getFrontendOptions()
          .InputsAndOutputs.getSingleOutputFilename());
}

static bool dumpPrecompiledClangModule(CompilerInvocation &Invocation,
                                       CompilerInstance &Instance) {
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  return clangImporter->dumpPrecompiledModule(
      Invocation.getFrontendOptions()
          .InputsAndOutputs.getFilenameOfFirstInput(),
      Invocation.getFrontendOptions()
          .InputsAndOutputs.getSingleOutputFilename());
}

static bool buildModuleFromInterface(CompilerInvocation &Invocation,
                                     CompilerInstance &Instance) {
  const FrontendOptions &FEOpts = Invocation.getFrontendOptions();
  assert(FEOpts.InputsAndOutputs.hasSingleInput());
  StringRef InputPath = FEOpts.InputsAndOutputs.getFilenameOfFirstInput();
  StringRef PrebuiltCachePath = FEOpts.PrebuiltModuleCachePath;
  return ModuleInterfaceLoader::buildSwiftModuleFromSwiftInterface(
      Instance.getSourceMgr(), Instance.getDiags(),
      Invocation.getSearchPathOptions(), Invocation.getLangOptions(),
      Invocation.getClangModuleCachePath(),
      PrebuiltCachePath, Invocation.getModuleName(), InputPath,
      Invocation.getOutputFilename(),
      FEOpts.SerializeModuleInterfaceDependencyHashes,
      FEOpts.TrackSystemDeps, FEOpts.RemarkOnRebuildFromModuleInterface);
}

static bool compileLLVMIR(CompilerInvocation &Invocation,
                          CompilerInstance &Instance,
                          UnifiedStatsReporter *Stats) {
  auto &LLVMContext = getGlobalLLVMContext();

  // Load in bitcode file.
  assert(Invocation.getFrontendOptions().InputsAndOutputs.hasSingleInput() &&
         "We expect a single input for bitcode input!");
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      swift::vfs::getFileOrSTDIN(Instance.getFileSystem(),
                                 Invocation.getFrontendOptions()
                                   .InputsAndOutputs.getFilenameOfFirstInput());

  if (!FileBufOrErr) {
    Instance.getASTContext().Diags.diagnose(
        SourceLoc(), diag::error_open_input_file,
        Invocation.getFrontendOptions()
            .InputsAndOutputs.getFilenameOfFirstInput(),
        FileBufOrErr.getError().message());
    return true;
  }
  llvm::MemoryBuffer *MainFile = FileBufOrErr.get().get();

  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> Module =
      llvm::parseIR(MainFile->getMemBufferRef(), Err, LLVMContext);
  if (!Module) {
    // TODO: Translate from the diagnostic info to the SourceManager location
    // if available.
    Instance.getASTContext().Diags.diagnose(
        SourceLoc(), diag::error_parse_input_file,
        Invocation.getFrontendOptions()
            .InputsAndOutputs.getFilenameOfFirstInput(),
        Err.getMessage());
    return true;
  }
  IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();
  // TODO: remove once the frontend understands what action it should perform
  IRGenOpts.OutputKind =
      getOutputKind(Invocation.getFrontendOptions().RequestedAction);

  return performLLVM(IRGenOpts, Instance.getASTContext(), Module.get(),
                     Invocation.getFrontendOptions()
                         .InputsAndOutputs.getSingleOutputFilename(),
                     Stats);
}

static void verifyGenericSignaturesIfNeeded(CompilerInvocation &Invocation,
                                            ASTContext &Context) {
  auto verifyGenericSignaturesInModule =
      Invocation.getFrontendOptions().VerifyGenericSignaturesInModule;
  if (verifyGenericSignaturesInModule.empty())
    return;
  if (auto module = Context.getModuleByName(verifyGenericSignaturesInModule))
    GenericSignatureBuilder::verifyGenericSignaturesInModule(module);
}

static void dumpAndPrintScopeMap(CompilerInvocation &Invocation,
                                 CompilerInstance &Instance, SourceFile *SF) {
  // Not const because may require reexpansion
  ASTScope &scope = SF->getScope();

  if (Invocation.getFrontendOptions().DumpScopeMapLocations.empty()) {
    llvm::errs() << "***Complete scope map***\n";
    scope.buildFullyExpandedTree();
    scope.print(llvm::errs());
    return;
  }
  // Probe each of the locations, and dump what we find.
  for (auto lineColumn :
       Invocation.getFrontendOptions().DumpScopeMapLocations) {
    scope.buildFullyExpandedTree();
    scope.dumpOneScopeMapLocation(lineColumn);
  }
}

static SourceFile *getPrimaryOrMainSourceFile(CompilerInvocation &Invocation,
                                              CompilerInstance &Instance) {
  SourceFile *SF = Instance.getPrimarySourceFile();
  if (!SF) {
    SourceFileKind Kind = Invocation.getSourceFileKind();
    SF = &Instance.getMainModule()->getMainSourceFile(Kind);
  }
  return SF;
}

/// Dumps the AST of all available primary source files. If corresponding output
/// files were specified, use them; otherwise, dump the AST to stdout.
static void dumpAST(CompilerInvocation &Invocation,
                    CompilerInstance &Instance) {
  // FIXME: WMO doesn't use the notion of primary files, so this doesn't do the
  // right thing. Perhaps it'd be best to ignore WMO when dumping the AST, just
  // like WMO ignores `-incremental`.

  auto primaryFiles = Instance.getPrimarySourceFiles();
  if (!primaryFiles.empty()) {
    for (SourceFile *sourceFile: primaryFiles) {
      auto PSPs = Instance.getPrimarySpecificPathsForSourceFile(*sourceFile);
      auto OutputFilename = PSPs.OutputFilename;
      auto OS = getFileOutputStream(OutputFilename, Instance.getASTContext());
      sourceFile->dump(*OS);
    }
  } else {
    // Some invocations don't have primary files. In that case, we default to
    // looking for the main file and dumping it to `stdout`.
    getPrimaryOrMainSourceFile(Invocation, Instance)->dump(llvm::outs());
  }
}

/// We may have been told to dump the AST (either after parsing or
/// type-checking, which is already differentiated in
/// CompilerInstance::performSema()), so dump or print the main source file and
/// return.

static Optional<bool> dumpASTIfNeeded(CompilerInvocation &Invocation,
                                      CompilerInstance &Instance) {
  FrontendOptions &opts = Invocation.getFrontendOptions();
  FrontendOptions::ActionType Action = opts.RequestedAction;
  ASTContext &Context = Instance.getASTContext();
  switch (Action) {
  default:
    return None;

  case FrontendOptions::ActionType::PrintAST:
    getPrimaryOrMainSourceFile(Invocation, Instance)
        ->print(llvm::outs(), PrintOptions::printEverything());
    break;

  case FrontendOptions::ActionType::DumpScopeMaps:
    dumpAndPrintScopeMap(Invocation, Instance,
                         getPrimaryOrMainSourceFile(Invocation, Instance));
    break;

  case FrontendOptions::ActionType::DumpTypeRefinementContexts:
    getPrimaryOrMainSourceFile(Invocation, Instance)
        ->getTypeRefinementContext()
        ->dump(llvm::errs(), Context.SourceMgr);
    break;

  case FrontendOptions::ActionType::DumpInterfaceHash:
    getPrimaryOrMainSourceFile(Invocation, Instance)
        ->dumpInterfaceHash(llvm::errs());
    break;

  case FrontendOptions::ActionType::EmitSyntax:
    emitSyntax(getPrimaryOrMainSourceFile(Invocation, Instance),
               Invocation.getLangOptions(), Instance.getSourceMgr(),
               opts.InputsAndOutputs.getSingleOutputFilename());
    break;

  case FrontendOptions::ActionType::DumpParse:
  case FrontendOptions::ActionType::DumpAST:
    dumpAST(Invocation, Instance);
    break;

  case FrontendOptions::ActionType::EmitImportedModules:
    emitImportedModules(Context, Instance.getMainModule(), opts);
    break;
  }
  return Context.hadError();
}

static void emitReferenceDependenciesForAllPrimaryInputsIfNeeded(
    CompilerInvocation &Invocation, CompilerInstance &Instance) {
  if (Invocation.getFrontendOptions()
          .InputsAndOutputs.hasReferenceDependenciesPath() &&
      Instance.getPrimarySourceFiles().empty()) {
    Instance.getASTContext().Diags.diagnose(
        SourceLoc(), diag::emit_reference_dependencies_without_primary_file);
    return;
  }
  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &referenceDependenciesFilePath =
        Invocation.getReferenceDependenciesFilePathForPrimary(
            SF->getFilename());
    if (!referenceDependenciesFilePath.empty()) {
      if (Invocation.getLangOptions().EnableExperimentalDependencies)
        (void)experimental_dependencies::emitReferenceDependencies(
            Instance.getASTContext().Diags, SF,
            *Instance.getDependencyTracker(), referenceDependenciesFilePath);
      else
        (void)emitReferenceDependencies(Instance.getASTContext().Diags, SF,
                                        *Instance.getDependencyTracker(),
                                        referenceDependenciesFilePath);
    }
  }
}
static void
emitSwiftRangesForAllPrimaryInputsIfNeeded(CompilerInvocation &Invocation,
                                           CompilerInstance &Instance) {
  if (Invocation.getFrontendOptions().InputsAndOutputs.hasSwiftRangesPath() &&
      Instance.getPrimarySourceFiles().empty()) {
    Instance.getASTContext().Diags.diagnose(
        SourceLoc(), diag::emit_swift_ranges_without_primary_file);
    return;
  }
  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &swiftRangesFilePath =
        Invocation.getSwiftRangesFilePathForPrimary(SF->getFilename());
    if (!swiftRangesFilePath.empty()) {
      (void)Instance.emitSwiftRanges(Instance.getASTContext().Diags, SF,
                                     swiftRangesFilePath);
    }
  }
}
static void
emitCompiledSourceForAllPrimaryInputsIfNeeded(CompilerInvocation &Invocation,
                                              CompilerInstance &Instance) {
  if (Invocation.getFrontendOptions()
          .InputsAndOutputs.hasCompiledSourcePath() &&
      Instance.getPrimarySourceFiles().empty()) {
    Instance.getASTContext().Diags.diagnose(
        SourceLoc(), diag::emit_compiled_source_without_primary_file);
    return;
  }
  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &compiledSourceFilePath =
        Invocation.getCompiledSourceFilePathForPrimary(SF->getFilename());
    if (!compiledSourceFilePath.empty()) {
      (void)Instance.emitCompiledSource(Instance.getASTContext().Diags, SF,
                                        compiledSourceFilePath);
    }
  }
}

static bool writeTBDIfNeeded(CompilerInvocation &Invocation,
                             CompilerInstance &Instance) {
  const auto &frontendOpts = Invocation.getFrontendOptions();
  const auto &tbdOpts = Invocation.getTBDGenOptions();
  if (!frontendOpts.InputsAndOutputs.hasTBDPath())
    return false;

  if (!frontendOpts.InputsAndOutputs.isWholeModule()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_only_supported_in_whole_module);
    return false;
  }

  const std::string &TBDPath = Invocation.getTBDPathForWholeModule();

  return writeTBD(Instance.getMainModule(), TBDPath, tbdOpts);
}

static bool performCompileStepsPostSILGen(
    CompilerInstance &Instance, CompilerInvocation &Invocation,
    std::unique_ptr<SILModule> SM, bool astGuaranteedToCorrespondToSIL,
    ModuleOrSourceFile MSF, const PrimarySpecificPaths &PSPs,
    bool moduleIsPublic, int &ReturnValue, FrontendObserver *observer,
    UnifiedStatsReporter *Stats);

static bool
performCompileStepsPostSema(CompilerInvocation &Invocation,
                            CompilerInstance &Instance,
                            bool moduleIsPublic, int &ReturnValue,
                            FrontendObserver *observer,
                            UnifiedStatsReporter *Stats) {
  auto mod = Instance.getMainModule();
  if (auto SM = Instance.takeSILModule()) {
    const PrimarySpecificPaths PSPs =
        Instance.getPrimarySpecificPathsForAtMostOnePrimary();
    return performCompileStepsPostSILGen(Instance, Invocation, std::move(SM),
                                         /*ASTGuaranteedToCorrespondToSIL=*/false,
                                         mod, PSPs, moduleIsPublic,
                                         ReturnValue, observer, Stats);
  }

  SILOptions &SILOpts = Invocation.getSILOptions();
  FrontendOptions &opts = Invocation.getFrontendOptions();
  auto fileIsSIB = [](const FileUnit *File) -> bool {
    auto SASTF = dyn_cast<SerializedASTFile>(File);
    return SASTF && SASTF->isSIB();
  };

  if (!opts.InputsAndOutputs.hasPrimaryInputs()) {
    // If there are no primary inputs the compiler is in WMO mode and builds one
    // SILModule for the entire module.
    auto SM = performSILGeneration(mod, Instance.getSILTypes(), SILOpts);
    const PrimarySpecificPaths PSPs =
        Instance.getPrimarySpecificPathsForWholeModuleOptimizationMode();
    bool astGuaranteedToCorrespondToSIL =
        llvm::none_of(mod->getFiles(), fileIsSIB);
    return performCompileStepsPostSILGen(Instance, Invocation, std::move(SM),
                                         astGuaranteedToCorrespondToSIL,
                                         mod, PSPs, moduleIsPublic,
                                         ReturnValue, observer, Stats);
  }
  // If there are primary source files, build a separate SILModule for
  // each source file, and run the remaining SILOpt-Serialize-IRGen-LLVM
  // once for each such input.
  if (!Instance.getPrimarySourceFiles().empty()) {
    bool result = false;
    for (auto *PrimaryFile : Instance.getPrimarySourceFiles()) {
      auto SM = performSILGeneration(*PrimaryFile, Instance.getSILTypes(), SILOpts);
      const PrimarySpecificPaths PSPs =
          Instance.getPrimarySpecificPathsForSourceFile(*PrimaryFile);
      result |= performCompileStepsPostSILGen(Instance, Invocation, std::move(SM),
                                              /*ASTGuaranteedToCorrespondToSIL*/true,
                                              PrimaryFile, PSPs, moduleIsPublic,
                                              ReturnValue, observer, Stats);
    }

    return result;
  }

  // If there are primary inputs but no primary _source files_, there might be
  // a primary serialized input.
  bool result = false;
  for (FileUnit *fileUnit : mod->getFiles()) {
    if (auto SASTF = dyn_cast<SerializedASTFile>(fileUnit))
      if (Invocation.getFrontendOptions().InputsAndOutputs.isInputPrimary(
              SASTF->getFilename())) {
        auto SM = performSILGeneration(*SASTF, Instance.getSILTypes(), SILOpts);
        const PrimarySpecificPaths &PSPs =
            Instance.getPrimarySpecificPathsForPrimary(SASTF->getFilename());
        result |= performCompileStepsPostSILGen(Instance, Invocation, std::move(SM),
                                                !fileIsSIB(SASTF),
                                                mod, PSPs, moduleIsPublic,
                                                ReturnValue, observer, Stats);
      }
  }

  return result;
}

/// Emits index data for all primary inputs, or the main module.
static bool
emitIndexData(CompilerInvocation &Invocation, CompilerInstance &Instance) {
  bool hadEmitIndexDataError = false;
  if (Instance.getPrimarySourceFiles().empty())
    return emitIndexDataIfNeeded(nullptr, Invocation, Instance);
  for (SourceFile *SF : Instance.getPrimarySourceFiles())
    hadEmitIndexDataError = emitIndexDataIfNeeded(SF, Invocation, Instance) ||
                            hadEmitIndexDataError;
  return hadEmitIndexDataError;
}

/// Emits all "one-per-module" supplementary outputs that don't depend on
/// anything past type-checking.
///
/// These are extracted out so that they can be invoked early when using
/// `-typecheck`, but skipped for any mode that runs SIL diagnostics if there's
/// an error found there (to get those diagnostics back to the user faster).
static bool emitAnyWholeModulePostTypeCheckSupplementaryOutputs(
    CompilerInstance &Instance, CompilerInvocation &Invocation,
    bool moduleIsPublic) {
  const FrontendOptions &opts = Invocation.getFrontendOptions();

  // Record whether we failed to emit any of these outputs, but keep going; one
  // failure does not mean skipping the rest.
  bool hadAnyError = false;

  if (opts.InputsAndOutputs.hasObjCHeaderOutputPath()) {
    hadAnyError |= printAsObjCIfNeeded(
        Invocation.getObjCHeaderOutputPathForAtMostOnePrimary(),
        Instance.getMainModule(), opts.ImplicitObjCHeaderPath, moduleIsPublic);
  }

  if (opts.InputsAndOutputs.hasModuleInterfaceOutputPath()) {
    hadAnyError |= printModuleInterfaceIfNeeded(
        Invocation.getModuleInterfaceOutputPathForWholeModule(),
        Invocation.getModuleInterfaceOptions(),
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }

  {
    hadAnyError |= writeTBDIfNeeded(Invocation, Instance);
  }

  return hadAnyError;
}

/// Performs the compile requested by the user.
/// \param Instance Will be reset after performIRGeneration when the verifier
///                 mode is NoVerify and there were no errors.
/// \returns true on error
static bool performCompile(CompilerInstance &Instance,
                           CompilerInvocation &Invocation,
                           ArrayRef<const char *> Args,
                           int &ReturnValue,
                           FrontendObserver *observer,
                           UnifiedStatsReporter *Stats) {
  FrontendOptions opts = Invocation.getFrontendOptions();
  FrontendOptions::ActionType Action = opts.RequestedAction;

  if (Action == FrontendOptions::ActionType::EmitSyntax) {
    Instance.getASTContext().LangOpts.BuildSyntaxTree = true;
    Instance.getASTContext().LangOpts.VerifySyntaxTree = true;
  }

  // We've been asked to precompile a bridging header or module; we want to
  // avoid touching any other inputs and just parse, emit and exit.
  if (Action == FrontendOptions::ActionType::EmitPCH)
    return precompileBridgingHeader(Invocation, Instance);
  if (Action == FrontendOptions::ActionType::EmitPCM)
    return precompileClangModule(Invocation, Instance);
  if (Action == FrontendOptions::ActionType::DumpPCM)
    return dumpPrecompiledClangModule(Invocation, Instance);

  if (Action == FrontendOptions::ActionType::CompileModuleFromInterface)
    return buildModuleFromInterface(Invocation, Instance);

  if (Invocation.getInputKind() == InputFileKind::LLVM)
    return compileLLVMIR(Invocation, Instance, Stats);

  if (FrontendOptions::shouldActionOnlyParse(Action)) {
    bool ParseDelayedDeclListsOnEnd =
      Action == FrontendOptions::ActionType::DumpParse ||
      Invocation.getDiagnosticOptions().VerifyMode != DiagnosticOptions::NoVerify;
    Instance.performParseOnly(/*EvaluateConditionals*/
                    Action == FrontendOptions::ActionType::EmitImportedModules,
                              ParseDelayedDeclListsOnEnd);
  } else if (Action == FrontendOptions::ActionType::ResolveImports) {
    Instance.performParseAndResolveImportsOnly();
  } else {
    Instance.performSema();
  }

  ASTContext &Context = Instance.getASTContext();
  if (Action == FrontendOptions::ActionType::Parse)
    return Context.hadError();

  (void)emitMakeDependenciesIfNeeded(Context.Diags,
                                     Instance.getDependencyTracker(), opts);

  if (Action == FrontendOptions::ActionType::ResolveImports)
    return Context.hadError();

  if (observer)
    observer->performedSemanticAnalysis(Instance);

  if (Stats)
    countStatsPostSema(*Stats, Instance);

  {
    FrontendOptions::DebugCrashMode CrashMode = opts.CrashMode;
    if (CrashMode == FrontendOptions::DebugCrashMode::AssertAfterParse)
      debugFailWithAssertion();
    else if (CrashMode == FrontendOptions::DebugCrashMode::CrashAfterParse)
      debugFailWithCrash();
  }

  verifyGenericSignaturesIfNeeded(Invocation, Context);

  (void)migrator::updateCodeAndEmitRemapIfNeeded(&Instance, Invocation);

  if (Action == FrontendOptions::ActionType::REPL) {
    runREPL(Instance, ProcessCmdLine(Args.begin(), Args.end()),
            Invocation.getParseStdlib());
    return Context.hadError();
  }

  if (auto r = dumpASTIfNeeded(Invocation, Instance))
    return *r;

  // If we were asked to print Clang stats, do so.
  if (opts.PrintClangStats && Context.getClangModuleLoader())
    Context.getClangModuleLoader()->printStatistics();

  emitReferenceDependenciesForAllPrimaryInputsIfNeeded(Invocation, Instance);
  emitSwiftRangesForAllPrimaryInputsIfNeeded(Invocation, Instance);
  emitCompiledSourceForAllPrimaryInputsIfNeeded(Invocation, Instance);

  if (Context.hadError()) {
    //  Emit the index store data even if there were compiler errors.
    (void)emitIndexData(Invocation, Instance);
    return true;
  }

  (void)emitLoadedModuleTraceForAllPrimariesIfNeeded(
      Instance.getMainModule(), Instance.getDependencyTracker(), opts);

  // FIXME: This is still a lousy approximation of whether the module file will
  // be externally consumed.
  bool moduleIsPublic =
      !Instance.getMainModule()->hasEntryPoint() &&
      opts.ImplicitObjCHeaderPath.empty() &&
      !Context.LangOpts.EnableAppExtensionRestrictions;

  // We've just been told to perform a typecheck, so we can return now.
  if (Action == FrontendOptions::ActionType::Typecheck) {
    if (emitIndexData(Invocation, Instance))
      return true;
    // FIXME: Whole-module outputs with a non-whole-module -typecheck ought to
    // be disallowed, but the driver implements -index-file mode by generating a
    // regular whole-module frontend command line and modifying it to index just
    // one file (by making it a primary) instead of all of them. If that
    // invocation also has flags to emit whole-module supplementary outputs, the
    // compiler can crash trying to access information for non-type-checked
    // declarations in the non-primary files. For now, prevent those crashes by
    // guarding the emission of whole-module supplementary outputs.
    if (opts.InputsAndOutputs.isWholeModule()) {
      if (emitAnyWholeModulePostTypeCheckSupplementaryOutputs(Instance,
                                                              Invocation,
                                                              moduleIsPublic)) {
        return true;
      }
    }
    return false;
  }

  assert(FrontendOptions::doesActionGenerateSIL(Action) &&
         "All actions not requiring SILGen must have been handled!");

  return performCompileStepsPostSema(Invocation, Instance, moduleIsPublic,
                                     ReturnValue, observer, Stats);
}

/// Get the main source file's private discriminator and attach it to
/// the compile unit's flags.
static void setPrivateDiscriminatorIfNeeded(IRGenOptions &IRGenOpts,
                                            ModuleOrSourceFile MSF) {
  if (IRGenOpts.DebugInfoLevel == IRGenDebugInfoLevel::None ||
      !MSF.is<SourceFile *>())
    return;
  Identifier PD = MSF.get<SourceFile *>()->getPrivateDiscriminator();
  if (!PD.empty()) {
    if (!IRGenOpts.DebugFlags.empty())
      IRGenOpts.DebugFlags += " ";
    IRGenOpts.DebugFlags += ("-private-discriminator " + PD.str()).str();
  }
}

static bool serializeSIB(SILModule *SM, const PrimarySpecificPaths &PSPs,
                         ASTContext &Context, ModuleOrSourceFile MSF) {
  const std::string &moduleOutputPath =
      PSPs.SupplementaryOutputs.ModuleOutputPath;
  assert(!moduleOutputPath.empty() && "must have an output path");

  SerializationOptions serializationOpts;
  serializationOpts.OutputPath = moduleOutputPath.c_str();
  serializationOpts.SerializeAllSIL = true;
  serializationOpts.IsSIB = true;

  serialize(MSF, serializationOpts, SM);
  return Context.hadError();
}

static void generateIR(IRGenOptions &IRGenOpts, std::unique_ptr<SILModule> SM,
                       const PrimarySpecificPaths &PSPs,
                       StringRef OutputFilename, ModuleOrSourceFile MSF,
                       std::unique_ptr<llvm::Module> &IRModule,
                       llvm::GlobalVariable *&HashGlobal,
                       ArrayRef<std::string> parallelOutputFilenames) {
  // FIXME: We shouldn't need to use the global context here, but
  // something is persisting across calls to performIRGeneration.
  auto &LLVMContext = getGlobalLLVMContext();
  IRModule = MSF.is<SourceFile *>()
                 ? performIRGeneration(IRGenOpts, *MSF.get<SourceFile *>(),
                                       std::move(SM), OutputFilename, PSPs,
                                       LLVMContext, &HashGlobal)
                 : performIRGeneration(IRGenOpts, MSF.get<ModuleDecl *>(),
                                       std::move(SM), OutputFilename, PSPs,
                                       LLVMContext, parallelOutputFilenames,
                                       &HashGlobal);
}

static bool processCommandLineAndRunImmediately(CompilerInvocation &Invocation,
                                                CompilerInstance &Instance,
                                                std::unique_ptr<SILModule> SM,
                                                ModuleOrSourceFile MSF,
                                                FrontendObserver *observer,
                                                int &ReturnValue) {
  FrontendOptions &opts = Invocation.getFrontendOptions();
  assert(!MSF.is<SourceFile *>() && "-i doesn't work in -primary-file mode");
  IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();
  IRGenOpts.UseJIT = true;
  IRGenOpts.DebugInfoLevel = IRGenDebugInfoLevel::Normal;
  IRGenOpts.DebugInfoFormat = IRGenDebugInfoFormat::DWARF;
  const ProcessCmdLine &CmdLine =
      ProcessCmdLine(opts.ImmediateArgv.begin(), opts.ImmediateArgv.end());
  Instance.setSILModule(std::move(SM));

  ReturnValue =
      RunImmediately(Instance, CmdLine, IRGenOpts, Invocation.getSILOptions());
  return Instance.getASTContext().hadError();
}

static bool validateTBDIfNeeded(CompilerInvocation &Invocation,
                                ModuleOrSourceFile MSF,
                                bool astGuaranteedToCorrespondToSIL,
                                llvm::Module &IRModule) {
  if (!astGuaranteedToCorrespondToSIL ||
      !inputFileKindCanHaveTBDValidated(Invocation.getInputKind()))
    return false;

  const auto &frontendOpts = Invocation.getFrontendOptions();
  auto mode = frontendOpts.ValidateTBDAgainstIR;
  // Ensure all cases are covered by using a switch here.
  switch (mode) {
  case FrontendOptions::TBDValidationMode::Default:
#ifndef NDEBUG
    // When a debug compiler is targeting an apple platform, we do some
    // validation by default.
    if (Invocation.getLangOptions().Target.getVendor() == llvm::Triple::Apple) {
      mode = FrontendOptions::TBDValidationMode::MissingFromTBD;
      break;
    }
#endif
    // Otherwise, the default is to do nothing.
    LLVM_FALLTHROUGH;
  case FrontendOptions::TBDValidationMode::None:
    return false;
  case FrontendOptions::TBDValidationMode::All:
  case FrontendOptions::TBDValidationMode::MissingFromTBD:
    break;
  }

  const bool allSymbols = mode == FrontendOptions::TBDValidationMode::All;
  return MSF.is<SourceFile *>()
             ? validateTBD(MSF.get<SourceFile *>(), IRModule,
                           Invocation.getTBDGenOptions(), allSymbols)
             : validateTBD(MSF.get<ModuleDecl *>(), IRModule,
                           Invocation.getTBDGenOptions(), allSymbols);
}

static bool generateCode(CompilerInvocation &Invocation,
                         CompilerInstance &Instance, StringRef OutputFilename,
                         llvm::Module *IRModule,
                         llvm::GlobalVariable *HashGlobal,
                         UnifiedStatsReporter *Stats) {
  std::unique_ptr<llvm::TargetMachine> TargetMachine = createTargetMachine(
      Invocation.getIRGenOptions(), Instance.getASTContext());
  version::Version EffectiveLanguageVersion =
      Instance.getASTContext().LangOpts.EffectiveLanguageVersion;

  if (!Stats) {
    // Free up some compiler resources now that we have an IRModule.
    Instance.freeSILModule();

    // If there are multiple primary inputs it is too soon to free
    // the ASTContext, etc.. OTOH, if this compilation generates code for > 1
    // primary input, then freeing it after processing the last primary is
    // unlikely to reduce the peak heap size. So, only optimize the
    // single-primary-case (or WMO).
    if (!Invocation.getFrontendOptions()
             .InputsAndOutputs.hasMultiplePrimaryInputs())
      Instance.freeASTContext();
  }

  // Now that we have a single IR Module, hand it over to performLLVM.
  return performLLVM(Invocation.getIRGenOptions(), &Instance.getDiags(),
                     nullptr, HashGlobal, IRModule, TargetMachine.get(),
                     EffectiveLanguageVersion, OutputFilename, Stats);
}

static bool performCompileStepsPostSILGen(
    CompilerInstance &Instance, CompilerInvocation &Invocation,
    std::unique_ptr<SILModule> SM, bool astGuaranteedToCorrespondToSIL,
    ModuleOrSourceFile MSF, const PrimarySpecificPaths &PSPs,
    bool moduleIsPublic, int &ReturnValue, FrontendObserver *observer,
    UnifiedStatsReporter *Stats) {

  FrontendOptions opts = Invocation.getFrontendOptions();
  FrontendOptions::ActionType Action = opts.RequestedAction;
  ASTContext &Context = Instance.getASTContext();
  SILOptions &SILOpts = Invocation.getSILOptions();
  IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();

  Optional<BufferIndirectlyCausingDiagnosticRAII> ricd;
  if (auto *SF = MSF.dyn_cast<SourceFile *>())
    ricd.emplace(*SF);

  if (observer)
    observer->performedSILGeneration(*SM);

  if (Stats)
    countStatsPostSILGen(*Stats, *SM);

  // We've been told to emit SIL after SILGen, so write it now.
  if (Action == FrontendOptions::ActionType::EmitSILGen) {
    return writeSIL(*SM, PSPs, Instance, Invocation);
  }

  if (Action == FrontendOptions::ActionType::EmitSIBGen) {
    serializeSIB(SM.get(), PSPs, Instance.getASTContext(), MSF);
    return Context.hadError();
  }

  std::unique_ptr<llvm::raw_fd_ostream> OptRecordFile =
      createOptRecordFile(SILOpts.OptRecordFile, Instance.getDiags());
  if (OptRecordFile) {
    auto Output =
        llvm::make_unique<llvm::yaml::Output>(*OptRecordFile,
                                              &Instance.getSourceMgr());
    SM->setOptRecordStream(std::move(Output), std::move(OptRecordFile));
  }

  // This is the action to be used to serialize SILModule.
  // It may be invoked multiple times, but it will perform
  // serialization only once. The serialization may either happen
  // after high-level optimizations or after all optimizations are
  // done, depending on the compiler setting.

  auto SerializeSILModuleAction = [&]() {
    const SupplementaryOutputPaths &outs = PSPs.SupplementaryOutputs;
    if (outs.ModuleOutputPath.empty())
      return;

    SerializationOptions serializationOpts =
        Invocation.computeSerializationOptions(outs, moduleIsPublic);
    serialize(MSF, serializationOpts, SM.get());
  };

  // Set the serialization action, so that the SIL module
  // can be serialized at any moment, e.g. during the optimization pipeline.
  SM->setSerializeSILAction(SerializeSILModuleAction);

  // Perform optimizations and mandatory/diagnostic passes.
  if (Instance.performSILProcessing(SM.get(), Stats))
    return true;

  if (observer)
    observer->performedSILProcessing(*SM);

  emitAnyWholeModulePostTypeCheckSupplementaryOutputs(Instance, Invocation,
                                                      moduleIsPublic);

  setPrivateDiscriminatorIfNeeded(IRGenOpts, MSF);

  if (Action == FrontendOptions::ActionType::EmitSIB)
    return serializeSIB(SM.get(), PSPs, Instance.getASTContext(), MSF);

  {
    if (PSPs.haveModuleOrModuleDocOutputPaths()) {
      if (Action == FrontendOptions::ActionType::MergeModules ||
          Action == FrontendOptions::ActionType::EmitModuleOnly) {
        // What if MSF is a module?
        // emitIndexDataIfNeeded already handles that case;
        // it'll index everything.
        return emitIndexDataIfNeeded(MSF.dyn_cast<SourceFile *>(), Invocation,
                                     Instance) ||
               Context.hadError();
      }
    }
  }

  assert(Action >= FrontendOptions::ActionType::EmitSIL &&
         "All actions not requiring SILPasses must have been handled!");

  // We've been told to write canonical SIL, so write it now.
  if (Action == FrontendOptions::ActionType::EmitSIL)
    return writeSIL(*SM, PSPs, Instance, Invocation);

  assert(Action >= FrontendOptions::ActionType::Immediate &&
         "All actions not requiring IRGen must have been handled!");
  assert(Action != FrontendOptions::ActionType::REPL &&
         "REPL mode must be handled immediately after Instance->performSema()");

  // Check if we had any errors; if we did, don't proceed to IRGen.
  if (Context.hadError())
    return true;

  runSILLoweringPasses(*SM);

  // TODO: at this point we need to flush any the _tracing and profiling_
  // in the UnifiedStatsReporter, because the those subsystems of the USR
  // retain _pointers into_ the SILModule, and the SILModule's lifecycle is
  // not presently such that it will outlive the USR (indeed, as it's
  // destroyed on a separate thread, this fact isn't even _deterministic_
  // after this point). If future plans require the USR tracing or
  // profiling entities after this point, more rearranging will be required.
  if (Stats)
    Stats->flushTracesAndProfiles();

  if (Action == FrontendOptions::ActionType::DumpTypeInfo)
    return performDumpTypeInfo(IRGenOpts, *SM, getGlobalLLVMContext());

  // TODO: remove once the frontend understands what action it should perform
  IRGenOpts.OutputKind = getOutputKind(Action);
  if (Action == FrontendOptions::ActionType::Immediate)
    return processCommandLineAndRunImmediately(
        Invocation, Instance, std::move(SM), MSF, observer, ReturnValue);

  StringRef OutputFilename = PSPs.OutputFilename;
  std::vector<std::string> ParallelOutputFilenames =
    Invocation.getFrontendOptions().InputsAndOutputs.copyOutputFilenames();
  std::unique_ptr<llvm::Module> IRModule;
  llvm::GlobalVariable *HashGlobal;
  generateIR(
      IRGenOpts, std::move(SM), PSPs, OutputFilename, MSF, IRModule, HashGlobal,
      ParallelOutputFilenames);

  // Walk the AST for indexing after IR generation. Walking it before seems
  // to cause miscompilation issues.
  if (emitIndexDataIfNeeded(MSF.dyn_cast<SourceFile *>(), Invocation, Instance))
    return true;

  // Just because we had an AST error it doesn't mean we can't performLLVM.
  bool HadError = Instance.getASTContext().hadError();

  // If the AST Context has no errors but no IRModule is available,
  // parallelIRGen happened correctly, since parallel IRGen produces multiple
  // modules.
  if (!IRModule)
    return HadError;

  if (validateTBDIfNeeded(Invocation, MSF, astGuaranteedToCorrespondToSIL,
                          *IRModule))
    return true;

  return generateCode(Invocation, Instance, OutputFilename, IRModule.get(),
                      HashGlobal, Stats) ||
         HadError;
}

static bool emitIndexDataIfNeeded(SourceFile *PrimarySourceFile,
                                  const CompilerInvocation &Invocation,
                                  CompilerInstance &Instance) {
  const FrontendOptions &opts = Invocation.getFrontendOptions();

  if (opts.IndexStorePath.empty())
    return false;

  // FIXME: provide index unit token(s) explicitly and only use output file
  // paths as a fallback.

  bool isDebugCompilation;
  switch (Invocation.getSILOptions().OptMode) {
    case OptimizationMode::NotSet:
    case OptimizationMode::NoOptimization:
      isDebugCompilation = true;
      break;
    case OptimizationMode::ForSpeed:
    case OptimizationMode::ForSize:
      isDebugCompilation = false;
      break;
  }

  if (PrimarySourceFile) {
    const PrimarySpecificPaths &PSPs =
        opts.InputsAndOutputs.getPrimarySpecificPathsForPrimary(
            PrimarySourceFile->getFilename());
    if (index::indexAndRecord(PrimarySourceFile, PSPs.OutputFilename,
                              opts.IndexStorePath, opts.IndexSystemModules,
                              isDebugCompilation, Invocation.getTargetTriple(),
                              *Instance.getDependencyTracker())) {
      return true;
    }
  } else {
    std::string moduleToken =
        Invocation.getModuleOutputPathForAtMostOnePrimary();
    if (moduleToken.empty())
      moduleToken = opts.InputsAndOutputs.getSingleOutputFilename();

    if (index::indexAndRecord(Instance.getMainModule(), opts.InputsAndOutputs.copyOutputFilenames(),
                              moduleToken, opts.IndexStorePath,
                              opts.IndexSystemModules,
                              isDebugCompilation, Invocation.getTargetTriple(),
                              *Instance.getDependencyTracker())) {
      return true;
    }
  }

  return false;
}

/// Returns true if an error occurred.
static bool dumpAPI(ModuleDecl *Mod, StringRef OutDir) {
  using namespace llvm::sys;

  auto getOutPath = [&](SourceFile *SF) -> std::string {
    SmallString<256> Path = OutDir;
    StringRef Filename = SF->getFilename();
    path::append(Path, path::filename(Filename));
    return Path.str();
  };

  std::unordered_set<std::string> Filenames;

  auto dumpFile = [&](SourceFile *SF) -> bool {
    SmallString<512> TempBuf;
    llvm::raw_svector_ostream TempOS(TempBuf);

    PrintOptions PO = PrintOptions::printInterface();
    PO.PrintOriginalSourceText = true;
    PO.Indent = 2;
    PO.PrintAccess = false;
    PO.SkipUnderscoredStdlibProtocols = true;
    SF->print(TempOS, PO);
    if (TempOS.str().trim().empty())
      return false; // nothing to show.

    std::string OutPath = getOutPath(SF);
    bool WasInserted = Filenames.insert(OutPath).second;
    if (!WasInserted) {
      llvm::errs() << "multiple source files ended up with the same dump API "
                      "filename to write to: " << OutPath << '\n';
      return true;
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS(OutPath, EC, fs::FA_Read | fs::FA_Write);
    if (EC) {
      llvm::errs() << "error opening file '" << OutPath << "': "
                   << EC.message() << '\n';
      return true;
    }

    OS << TempOS.str();
    return false;
  };

  std::error_code EC = fs::create_directories(OutDir);
  if (EC) {
    llvm::errs() << "error creating directory '" << OutDir << "': "
                 << EC.message() << '\n';
    return true;
  }

  for (auto *FU : Mod->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(FU))
      if (dumpFile(SF))
        return true;
  }

  return false;
}

static StringRef
silOptModeArgStr(OptimizationMode mode) {
  switch (mode) {
 case OptimizationMode::ForSpeed:
   return "O";
 case OptimizationMode::ForSize:
   return "Osize";
 default:
   return "Onone";
  }
}

static std::unique_ptr<UnifiedStatsReporter>
computeStatsReporter(const CompilerInvocation &Invocation, CompilerInstance *Instance) {
  const std::string &StatsOutputDir =
      Invocation.getFrontendOptions().StatsOutputDir;
  std::unique_ptr<UnifiedStatsReporter> StatsReporter;
  if (StatsOutputDir.empty())
    return std::unique_ptr<UnifiedStatsReporter>();

  auto &FEOpts = Invocation.getFrontendOptions();
  auto &LangOpts = Invocation.getLangOptions();
  auto &SILOpts = Invocation.getSILOptions();
  std::string InputName =
      FEOpts.InputsAndOutputs.getStatsFileMangledInputName();
  StringRef OptType = silOptModeArgStr(SILOpts.OptMode);
  const std::string &OutFile =
      FEOpts.InputsAndOutputs.lastInputProducingOutput().outputFilename();
  StringRef OutputType = llvm::sys::path::extension(OutFile);
  std::string TripleName = LangOpts.Target.normalize();
  auto Trace = Invocation.getFrontendOptions().TraceStats;
  auto ProfileEvents = Invocation.getFrontendOptions().ProfileEvents;
  auto ProfileEntities = Invocation.getFrontendOptions().ProfileEntities;
  SourceManager *SM = &Instance->getSourceMgr();
  clang::SourceManager *CSM = nullptr;
  if (auto *clangImporter = static_cast<ClangImporter *>(
          Instance->getASTContext().getClangModuleLoader())) {
    CSM = &clangImporter->getClangASTContext().getSourceManager();
  }
  return llvm::make_unique<UnifiedStatsReporter>(
      "swift-frontend", FEOpts.ModuleName, InputName, TripleName, OutputType,
      OptType, StatsOutputDir, SM, CSM, Trace,
      ProfileEvents, ProfileEntities);
}

/// Creates a diagnostic consumer that handles dispatching diagnostics to
/// multiple output files, based on the supplementary output paths specified by
/// \p inputsAndOutputs.
///
/// If no output files are needed, returns null.
static std::unique_ptr<DiagnosticConsumer>
createDispatchingDiagnosticConsumerIfNeeded(
    const FrontendInputsAndOutputs &inputsAndOutputs,
    llvm::function_ref<std::unique_ptr<DiagnosticConsumer>(const InputFile &)>
        maybeCreateConsumerForDiagnosticsFrom) {

  // The "4" here is somewhat arbitrary. In practice we're going to have one
  // sub-consumer for each diagnostic file we're trying to output, which (again
  // in practice) is going to be 1 in WMO mode and equal to the number of
  // primary inputs in batch mode. That in turn is going to be "the number of
  // files we need to recompile in this build, divided by the number of jobs".
  // So a value of "4" here means that there would be no heap allocation on a
  // clean build of a module with up to 32 files on an 8-core machine, if the
  // user doesn't customize anything.
  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 4> subconsumers;

  inputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &input) -> bool {
        if (auto consumer = maybeCreateConsumerForDiagnosticsFrom(input))
          subconsumers.emplace_back(input.file(), std::move(consumer));
        return false;
      });
  // For batch mode, the compiler must sometimes swallow diagnostics pertaining
  // to non-primary files in order to avoid Xcode showing the same diagnostic
  // multiple times. So, create a diagnostic "eater" for those non-primary
  // files.
  //
  // This routine gets called in cases where no primary subconsumers are created.
  // Don't bother to create non-primary subconsumers if there aren't any primary
  // ones.
  //
  // To avoid introducing bugs into WMO or single-file modes, test for multiple
  // primaries.
  if (!subconsumers.empty() && inputsAndOutputs.hasMultiplePrimaryInputs()) {
    inputsAndOutputs.forEachNonPrimaryInput(
        [&](const InputFile &input) -> bool {
          subconsumers.emplace_back(input.file(), nullptr);
          return false;
        });
  }

  return FileSpecificDiagnosticConsumer::consolidateSubconsumers(subconsumers);
}

/// Creates a diagnostic consumer that handles serializing diagnostics, based on
/// the supplementary output paths specified by \p inputsAndOutputs.
///
/// The returned consumer will handle producing multiple serialized diagnostics
/// files if necessary, by using sub-consumers for each file and dispatching to
/// the right one.
///
/// If no serialized diagnostics are being produced, returns null.
static std::unique_ptr<DiagnosticConsumer>
createSerializedDiagnosticConsumerIfNeeded(
    const FrontendInputsAndOutputs &inputsAndOutputs) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      inputsAndOutputs,
      [](const InputFile &input) -> std::unique_ptr<DiagnosticConsumer> {
    std::string serializedDiagnosticsPath = input.serializedDiagnosticsPath();
    if (serializedDiagnosticsPath.empty())
      return nullptr;
    return serialized_diagnostics::createConsumer(serializedDiagnosticsPath);
  });
}

/// Creates a diagnostic consumer that handles serializing diagnostics, based on
/// the supplementary output paths specified in \p options.
///
/// The returned consumer will handle producing multiple serialized diagnostics
/// files if necessary, by using sub-consumers for each file and dispatching to
/// the right one.
///
/// If no serialized diagnostics are being produced, returns null.
static std::unique_ptr<DiagnosticConsumer>
createJSONFixItDiagnosticConsumerIfNeeded(
    const CompilerInvocation &invocation) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      invocation.getFrontendOptions().InputsAndOutputs,
      [&](const InputFile &input) -> std::unique_ptr<DiagnosticConsumer> {
    std::string fixItsOutputPath = input.fixItsOutputPath();
    if (fixItsOutputPath.empty())
      return nullptr;
    return llvm::make_unique<JSONFixitWriter>(
        fixItsOutputPath, invocation.getDiagnosticOptions());
  });
}


int swift::performFrontend(ArrayRef<const char *> Args,
                           const char *Argv0, void *MainAddr,
                           FrontendObserver *observer) {
  INITIALIZE_LLVM();

  PrintingDiagnosticConsumer PDC;

  // Hopefully we won't trigger any LLVM-level fatal errors, but if we do try
  // to route them through our usual textual diagnostics before crashing.
  //
  // Unfortunately it's not really safe to do anything else, since very
  // low-level operations in LLVM can trigger fatal errors.
  auto diagnoseFatalError = [&PDC](const std::string &reason, bool shouldCrash){
    static const std::string *recursiveFatalError = nullptr;
    if (recursiveFatalError) {
      // Report the /original/ error through LLVM's default handler, not
      // whatever we encountered.
      llvm::remove_fatal_error_handler();
      llvm::report_fatal_error(*recursiveFatalError, shouldCrash);
    }
    recursiveFatalError = &reason;

    SourceManager dummyMgr;

    DiagnosticInfo errorInfo(
        DiagID(0), SourceLoc(), DiagnosticKind::Error,
        "fatal error encountered during compilation; please file a bug report "
        "with your project and the crash log",
        {}, SourceLoc(), {}, {}, {}, false);
    DiagnosticInfo noteInfo(DiagID(0), SourceLoc(), DiagnosticKind::Note,
                            reason, {}, SourceLoc(), {}, {}, {}, false);
    PDC.handleDiagnostic(dummyMgr, errorInfo);
    PDC.handleDiagnostic(dummyMgr, noteInfo);
    if (shouldCrash)
      abort();
  };
  llvm::ScopedFatalErrorHandler handler([](void *rawCallback,
                                           const std::string &reason,
                                           bool shouldCrash) {
    auto *callback = static_cast<decltype(&diagnoseFatalError)>(rawCallback);
    (*callback)(reason, shouldCrash);
  }, &diagnoseFatalError);

  std::unique_ptr<CompilerInstance> Instance =
    llvm::make_unique<CompilerInstance>();
  Instance->addDiagnosticConsumer(&PDC);

  struct FinishDiagProcessingCheckRAII {
    bool CalledFinishDiagProcessing = false;
    ~FinishDiagProcessingCheckRAII() {
      assert(CalledFinishDiagProcessing && "returned from the function "
        "without calling finishDiagProcessing");
    }
  } FinishDiagProcessingCheckRAII;

  auto finishDiagProcessing = [&](int retValue) -> int {
    FinishDiagProcessingCheckRAII.CalledFinishDiagProcessing = true;
    bool err = Instance->getDiags().finishProcessing();
    return retValue ? retValue : err;
  };

  if (Args.empty()) {
    Instance->getDiags().diagnose(SourceLoc(), diag::error_no_frontend_args);
    return finishDiagProcessing(1);
  }

  CompilerInvocation Invocation;
  std::string MainExecutablePath = llvm::sys::fs::getMainExecutable(Argv0,
                                                                    MainAddr);
  Invocation.setMainExecutablePath(MainExecutablePath);

  SmallString<128> workingDirectory;
  llvm::sys::fs::current_path(workingDirectory);

  // Parse arguments.
  SmallVector<std::unique_ptr<llvm::MemoryBuffer>, 4> configurationFileBuffers;
  if (Invocation.parseArgs(Args, Instance->getDiags(),
                           &configurationFileBuffers, workingDirectory)) {
    return finishDiagProcessing(1);
  }

  // Make an array of PrettyStackTrace objects to dump the configuration files
  // we used to parse the arguments. These are RAII objects, so they and the
  // buffers they refer to must be kept alive in order to be useful. (That is,
  // we want them to be alive for the entire rest of performFrontend.)
  //
  // This can't be a SmallVector or similar because PrettyStackTraces can't be
  // moved (or copied)...and it can't be an array of non-optionals because
  // PrettyStackTraces can't be default-constructed. So we end up with a
  // dynamically-sized array of optional PrettyStackTraces, which get
  // initialized by iterating over the buffers we collected above.
  auto configurationFileStackTraces =
      llvm::make_unique<Optional<PrettyStackTraceFileContents>[]>(
        configurationFileBuffers.size());
  for_each(configurationFileBuffers.begin(), configurationFileBuffers.end(),
           &configurationFileStackTraces[0],
           [](const std::unique_ptr<llvm::MemoryBuffer> &buffer,
              Optional<PrettyStackTraceFileContents> &trace) {
    trace.emplace(*buffer);
  });

  // Setting DWARF Version depend on platform
  IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();
  IRGenOpts.DWARFVersion = swift::DWARFVersion;

  // The compiler invocation is now fully configured; notify our observer.
  if (observer) {
    observer->parsedArgs(Invocation);
  }

  if (Invocation.getFrontendOptions().PrintHelp ||
      Invocation.getFrontendOptions().PrintHelpHidden) {
    unsigned IncludedFlagsBitmask = options::FrontendOption;
    unsigned ExcludedFlagsBitmask =
      Invocation.getFrontendOptions().PrintHelpHidden ? 0 :
                                                        llvm::opt::HelpHidden;
    std::unique_ptr<llvm::opt::OptTable> Options(createSwiftOptTable());
    Options->PrintHelp(llvm::outs(), displayName(MainExecutablePath).c_str(),
                       "Swift frontend", IncludedFlagsBitmask,
                       ExcludedFlagsBitmask, /*ShowAllAliases*/false);
    return finishDiagProcessing(0);
  }

  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::NoneAction) {
    Instance->getDiags().diagnose(SourceLoc(),
                                  diag::error_missing_frontend_action);
    return finishDiagProcessing(1);
  }

  // Because the serialized diagnostics consumer is initialized here,
  // diagnostics emitted above, within CompilerInvocation::parseArgs, are never
  // serialized. This is a non-issue because, in nearly all cases, frontend
  // arguments are generated by the driver, not directly by a user. The driver
  // is responsible for emitting diagnostics for its own errors. See SR-2683
  // for details.
  std::unique_ptr<DiagnosticConsumer> SerializedConsumerDispatcher =
      createSerializedDiagnosticConsumerIfNeeded(
        Invocation.getFrontendOptions().InputsAndOutputs);
  if (SerializedConsumerDispatcher)
    Instance->addDiagnosticConsumer(SerializedConsumerDispatcher.get());

  std::unique_ptr<DiagnosticConsumer> FixItsConsumer =
      createJSONFixItDiagnosticConsumerIfNeeded(Invocation);
  if (FixItsConsumer)
    Instance->addDiagnosticConsumer(FixItsConsumer.get());

  if (Invocation.getDiagnosticOptions().UseColor)
    PDC.forceColors();

  if (Invocation.getFrontendOptions().DebugTimeCompilation)
    SharedTimer::enableCompilationTimers();

  if (Invocation.getFrontendOptions().PrintStats) {
    llvm::EnableStatistics();
  }

  const DiagnosticOptions &diagOpts = Invocation.getDiagnosticOptions();
  if (diagOpts.VerifyMode != DiagnosticOptions::NoVerify) {
    enableDiagnosticVerifier(Instance->getSourceMgr());
  }

  if (Invocation.getFrontendOptions()
          .InputsAndOutputs.hasDependencyTrackerPath() ||
      !Invocation.getFrontendOptions().IndexStorePath.empty() ||
      Invocation.getFrontendOptions().TrackSystemDeps) {
    // Note that we're tracking dependencies even when we don't need to write
    // them directly; in particular, -track-system-dependencies affects how
    // module interfaces get loaded, and so we need to be consistently tracking
    // system dependencies throughout the compiler.
    Instance->createDependencyTracker(
        Invocation.getFrontendOptions().TrackSystemDeps);
  }

  if (Instance->setup(Invocation)) {
    return finishDiagProcessing(1);
  }

  std::unique_ptr<UnifiedStatsReporter> StatsReporter =
      computeStatsReporter(Invocation, Instance.get());
  if (StatsReporter) {
    // Install stats-reporter somewhere visible for subsystems that
    // need to bump counters as they work, rather than measure
    // accumulated work on completion (mostly: TypeChecker).
    Instance->getASTContext().setStatsReporter(StatsReporter.get());
  }

  // The compiler instance has been configured; notify our observer.
  if (observer) {
    observer->configuredCompiler(*Instance);
  }

  int ReturnValue = 0;
  bool HadError =
    performCompile(*Instance, Invocation, Args, ReturnValue, observer,
                   StatsReporter.get());

  if (!HadError) {
    Mangle::printManglingStats();
  }

  if (!HadError && !Invocation.getFrontendOptions().DumpAPIPath.empty()) {
    HadError = dumpAPI(Instance->getMainModule(),
                       Invocation.getFrontendOptions().DumpAPIPath);
  }

  if (diagOpts.VerifyMode != DiagnosticOptions::NoVerify) {
    HadError = verifyDiagnostics(
        Instance->getSourceMgr(),
        Instance->getInputBufferIDs(),
        diagOpts.VerifyMode == DiagnosticOptions::VerifyAndApplyFixes,
        diagOpts.VerifyIgnoreUnknown);

    DiagnosticEngine &diags = Instance->getDiags();
    if (diags.hasFatalErrorOccurred() &&
        !Invocation.getDiagnosticOptions().ShowDiagnosticsAfterFatalError) {
      diags.resetHadAnyError();
      diags.diagnose(SourceLoc(), diag::verify_encountered_fatal);
      HadError = true;
    }
  }

  auto r = finishDiagProcessing(HadError ? 1 : ReturnValue);
  if (StatsReporter)
    StatsReporter->noteCurrentProcessExitStatus(r);
  return r;
}

void FrontendObserver::parsedArgs(CompilerInvocation &invocation) {}
void FrontendObserver::configuredCompiler(CompilerInstance &instance) {}
void FrontendObserver::performedSemanticAnalysis(CompilerInstance &instance) {}
void FrontendObserver::performedSILGeneration(SILModule &module) {}
void FrontendObserver::performedSILProcessing(SILModule &module) {}
