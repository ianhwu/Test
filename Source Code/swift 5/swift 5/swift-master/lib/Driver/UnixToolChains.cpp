//===------ UnixToolChains.cpp - Job invocations (non-Darwin Unix) --------===//
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

#include "ToolChains.h"

#include "swift/Basic/Dwarf.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Config.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "swift/Option/Options.h"
#include "swift/Option/SanitizerOptions.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Util.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;

std::string
toolchains::GenericUnix::sanitizerRuntimeLibName(StringRef Sanitizer,
                                                 bool shared) const {
  return (Twine("libclang_rt.") + Sanitizer + "-" +
          this->getTriple().getArchName() + ".a")
      .str();
}

ToolChain::InvocationInfo
toolchains::GenericUnix::constructInvocation(const InterpretJobAction &job,
                                             const JobContext &context) const {
  InvocationInfo II = ToolChain::constructInvocation(job, context);

  SmallVector<std::string, 4> runtimeLibraryPaths;
  getRuntimeLibraryPaths(runtimeLibraryPaths, context.Args, context.OI.SDKPath,
                         /*Shared=*/true);

  addPathEnvironmentVariableIfNeeded(II.ExtraEnvironment, "LD_LIBRARY_PATH",
                                     ":", options::OPT_L, context.Args,
                                     runtimeLibraryPaths);
  return II;
}

ToolChain::InvocationInfo toolchains::GenericUnix::constructInvocation(
    const AutolinkExtractJobAction &job, const JobContext &context) const {
  assert(context.Output.getPrimaryOutputType() == file_types::TY_AutolinkFile);

  InvocationInfo II{"swift-autolink-extract"};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);

  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}

std::string toolchains::GenericUnix::getDefaultLinker() const {
  switch (getTriple().getArch()) {
  case llvm::Triple::arm:
  case llvm::Triple::aarch64:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:
    // BFD linker has issues wrt relocation of the protocol conformance
    // section on these targets, it also generates COPY relocations for
    // final executables, as such, unless specified, we default to gold
    // linker.
    return "gold";
  case llvm::Triple::x86:
  case llvm::Triple::x86_64:
  case llvm::Triple::ppc64:
  case llvm::Triple::ppc64le:
  case llvm::Triple::systemz:
    // BFD linker has issues wrt relocations against protected symbols.
    return "gold";
  default:
    // Otherwise, use the default BFD linker.
    return "";
  }
}

std::string toolchains::GenericUnix::getTargetForLinker() const {
  return getTriple().str();
}

bool toolchains::GenericUnix::shouldProvideRPathToLinker() const {
  return true;
}

ToolChain::InvocationInfo
toolchains::GenericUnix::constructInvocation(const DynamicLinkJobAction &job,
                                             const JobContext &context) const {
  assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  ArgStringList Arguments;

  std::string Target = getTargetForLinker();
  if (!Target.empty()) {
    Arguments.push_back("-target");
    Arguments.push_back(context.Args.MakeArgString(Target));
  }

  switch (job.getKind()) {
  case LinkKind::None:
    llvm_unreachable("invalid link kind");
  case LinkKind::Executable:
    // Default case, nothing extra needed.
    break;
  case LinkKind::DynamicLibrary:
    Arguments.push_back("-shared");
    break;
  case LinkKind::StaticLibrary:
    llvm_unreachable("the dynamic linker cannot build static libraries");
  }

  // Select the linker to use.
  std::string Linker;
  if (const Arg *A = context.Args.getLastArg(options::OPT_use_ld)) {
    Linker = A->getValue();
  } else {
    Linker = getDefaultLinker();
  }
  if (!Linker.empty()) {
#if defined(__HAIKU__)
    // For now, passing -fuse-ld on Haiku doesn't work as swiftc doesn't
    // recognise it. Passing -use-ld= as the argument works fine.
    Arguments.push_back(context.Args.MakeArgString("-use-ld=" + Linker));
#else
    Arguments.push_back(context.Args.MakeArgString("-fuse-ld=" + Linker));
#endif
  }

  // Configure the toolchain.
  //
  // By default use the system `clang` to perform the link.  We use `clang` for
  // the driver here because we do not wish to select a particular C++ runtime.
  // Furthermore, until C++ interop is enabled, we cannot have a dependency on
  // C++ code from pure Swift code.  If linked libraries are C++ based, they
  // should properly link C++.  In the case of static linking, the user can
  // explicitly specify the C++ runtime to link against.  This is particularly
  // important for platforms like android where as it is a Linux platform, the
  // default C++ runtime is `libstdc++` which is unsupported on the target but
  // as the builds are usually cross-compiled from Linux, libstdc++ is going to
  // be present.  This results in linking the wrong version of libstdc++
  // generating invalid binaries.  It is also possible to use different C++
  // runtimes than the default C++ runtime for the platform (e.g. libc++ on
  // Windows rather than msvcprt).  When C++ interop is enabled, we will need to
  // surface this via a driver flag.  For now, opt for the simpler approach of
  // just using `clang` and avoid a dependency on the C++ runtime.
  const char *Clang = "clang";
  if (const Arg *A = context.Args.getLastArg(options::OPT_tools_directory)) {
    StringRef toolchainPath(A->getValue());

    // If there is a clang in the toolchain folder, use that instead.
    if (auto tool = llvm::sys::findProgramByName("clang", {toolchainPath})) {
      Clang = context.Args.MakeArgString(tool.get());
    }

    // Look for binutils in the toolchain folder.
    Arguments.push_back("-B");
    Arguments.push_back(context.Args.MakeArgString(A->getValue()));
  }

  if (getTriple().getOS() == llvm::Triple::Linux &&
      job.getKind() == LinkKind::Executable) {
    Arguments.push_back("-pie");
  }

  bool staticExecutable = false;
  bool staticStdlib = false;

  if (context.Args.hasFlag(options::OPT_static_executable,
                           options::OPT_no_static_executable, false)) {
    staticExecutable = true;
  } else if (context.Args.hasFlag(options::OPT_static_stdlib,
                                  options::OPT_no_static_stdlib, false)) {
    staticStdlib = true;
  }

  SmallVector<std::string, 4> RuntimeLibPaths;
  getRuntimeLibraryPaths(RuntimeLibPaths, context.Args, context.OI.SDKPath,
                         /*Shared=*/!(staticExecutable || staticStdlib));

  if (!(staticExecutable || staticStdlib) && shouldProvideRPathToLinker()) {
    // FIXME: We probably shouldn't be adding an rpath here unless we know
    //        ahead of time the standard library won't be copied.
    for (auto path : RuntimeLibPaths) {
      Arguments.push_back("-Xlinker");
      Arguments.push_back("-rpath");
      Arguments.push_back("-Xlinker");
      Arguments.push_back(context.Args.MakeArgString(path));
    }
  }

  SmallString<128> SharedResourceDirPath;
  getResourceDirPath(SharedResourceDirPath, context.Args, /*Shared=*/true);

  SmallString<128> swiftrtPath = SharedResourceDirPath;
  llvm::sys::path::append(swiftrtPath,
                          swift::getMajorArchitectureName(getTriple()));
  llvm::sys::path::append(swiftrtPath, "swiftrt.o");
  Arguments.push_back(context.Args.MakeArgString(swiftrtPath));

  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);

  for (const Arg *arg :
       context.Args.filtered(options::OPT_F, options::OPT_Fsystem)) {
    if (arg->getOption().matches(options::OPT_Fsystem))
      Arguments.push_back("-iframework");
    else
      Arguments.push_back(context.Args.MakeArgString(arg->getSpelling()));
    Arguments.push_back(arg->getValue());
  }

  if (!context.OI.SDKPath.empty()) {
    Arguments.push_back("--sysroot");
    Arguments.push_back(context.Args.MakeArgString(context.OI.SDKPath));
  }

  // Add any autolinking scripts to the arguments
  for (const Job *Cmd : context.Inputs) {
    auto &OutputInfo = Cmd->getOutput();
    if (OutputInfo.getPrimaryOutputType() == file_types::TY_AutolinkFile)
      Arguments.push_back(context.Args.MakeArgString(
          Twine("@") + OutputInfo.getPrimaryOutputFilename()));
  }

  // Add the runtime library link paths.
  for (auto path : RuntimeLibPaths) {
    Arguments.push_back("-L");
    Arguments.push_back(context.Args.MakeArgString(path));
  }

  // Link the standard library. In two paths, we do this using a .lnk file;
  // if we're going that route, we'll set `linkFilePath` to the path to that
  // file.
  SmallString<128> linkFilePath;
  getResourceDirPath(linkFilePath, context.Args, /*Shared=*/false);

  if (staticExecutable) {
    llvm::sys::path::append(linkFilePath, "static-executable-args.lnk");
  } else if (staticStdlib) {
    llvm::sys::path::append(linkFilePath, "static-stdlib-args.lnk");
  } else {
    linkFilePath.clear();
    Arguments.push_back("-lswiftCore");
  }

  if (!linkFilePath.empty()) {
    auto linkFile = linkFilePath.str();
    if (llvm::sys::fs::is_regular_file(linkFile)) {
      Arguments.push_back(context.Args.MakeArgString(Twine("@") + linkFile));
    } else {
      llvm::report_fatal_error(linkFile + " not found");
    }
  }

  // Explicitly pass the target to the linker
  Arguments.push_back(
      context.Args.MakeArgString("--target=" + getTriple().str()));

  // Delegate to Clang for sanitizers. It will figure out the correct linker
  // options.
  if (job.getKind() == LinkKind::Executable && context.OI.SelectedSanitizers) {
    Arguments.push_back(context.Args.MakeArgString(
        "-fsanitize=" + getSanitizerList(context.OI.SelectedSanitizers)));

    // The TSan runtime depends on the blocks runtime and libdispatch.
    if (context.OI.SelectedSanitizers & SanitizerKind::Thread) {
      Arguments.push_back("-lBlocksRuntime");
      Arguments.push_back("-ldispatch");
    }
  }

  if (context.Args.hasArg(options::OPT_profile_generate)) {
    SmallString<128> LibProfile(SharedResourceDirPath);
    llvm::sys::path::remove_filename(LibProfile); // remove platform name
    llvm::sys::path::append(LibProfile, "clang", "lib");

    llvm::sys::path::append(LibProfile, getTriple().getOSName(),
                            Twine("libclang_rt.profile-") +
                                getTriple().getArchName() + ".a");
    Arguments.push_back(context.Args.MakeArgString(LibProfile));
    Arguments.push_back(context.Args.MakeArgString(
        Twine("-u", llvm::getInstrProfRuntimeHookVarName())));
  }

  // Run clang++ in verbose mode if "-v" is set
  if (context.Args.hasArg(options::OPT_v)) {
    Arguments.push_back("-v");
  }

  // These custom arguments should be right before the object file at the end.
  context.Args.AddAllArgs(Arguments, options::OPT_linker_option_Group);
  context.Args.AddAllArgs(Arguments, options::OPT_Xlinker);
  context.Args.AddAllArgValues(Arguments, options::OPT_Xclang_linker);

  // This should be the last option, for convenience in checking output.
  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  InvocationInfo II{Clang, Arguments};
  II.allowsResponseFiles = true;

  return II;
}


ToolChain::InvocationInfo
toolchains::GenericUnix::constructInvocation(const StaticLinkJobAction &job,
                               const JobContext &context) const {
   assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  ArgStringList Arguments;

  // Configure the toolchain.
  const char *AR = "ar";
  Arguments.push_back("crs");

  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);

  InvocationInfo II{AR, Arguments};

  return II;
}

std::string toolchains::Android::getTargetForLinker() const {
  const llvm::Triple &T = getTriple();
  switch (T.getArch()) {
  default:
    // FIXME: we should just abort on an unsupported target
    return T.str();
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    // Current Android NDK versions only support ARMv7+.  Always assume ARMv7+
    // for the arm/thumb target.
    return "armv7-unknown-linux-androideabi";
  case llvm::Triple::aarch64:
    return "aarch64-unknown-linux-android";
  case llvm::Triple::x86:
    return "i686-unknown-linux-android";
  case llvm::Triple::x86_64:
    return "x86_64-unknown-linux-android";
  }
}

bool toolchains::Android::shouldProvideRPathToLinker() const { return false; }

std::string toolchains::Cygwin::getDefaultLinker() const {
  // Cygwin uses the default BFD linker, even on ARM.
  return "";
}

std::string toolchains::Cygwin::getTargetForLinker() const { return ""; }
