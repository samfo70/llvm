//===- lli.cpp - LLVM Interpreter / Dynamic compiler ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This utility provides a simple wrapper around the LLVM Execution Engines,
// which allow the direct execution of LLVM programs through a Just-In-Time
// compiler, or through an interpreter if no JIT is available for this platform.
//
//===----------------------------------------------------------------------===//

#include "RemoteJITUtils.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/CodeGen/CommandFlags.inc"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/OrcRemoteTargetClient.h"
#include "llvm/ExecutionEngine/OrcMCJITReplacement.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Memory.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Instrumentation.h"
#include <cerrno>

#ifdef __CYGWIN__
#include <cygwin/version.h>
#if defined(CYGWIN_VERSION_DLL_MAJOR) && CYGWIN_VERSION_DLL_MAJOR<1007
#define DO_NOTHING_ATEXIT 1
#endif
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#if (TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)
#include "ios_error.h"
#undef write
#include <stdio.h>
#undef exit
#define exit(a) { llvm_shutdown(); ios_exit(a); }
extern "C" {
extern const char* llvm_ios_progname;
// TODO: add these to the list of symbols, plus _dso_handle, plus __cxa_atexit
// Q = interpreter only?
// dso_handle *could* be the pointer for libstdc++?
// raw_fd_ostream standard_out(STDOUT_FILENO, false, false);
// raw_fd_ostream standard_err(STDERR_FILENO, false, true);

void llvm_ios_exit(int a) { llvm_shutdown(); ios_exit(a); }
void llvm_ios_abort(int a) { report_fatal_error("LLVM JIT compiled program raised SIGABRT"); }
int llvm_ios_putchar(char c) { return fputc(c, thread_stdout); }
int llvm_ios_getchar(void) { return fgetc(thread_stdin); }
wint_t llvm_ios_getwchar(void) { return fgetwc(thread_stdin); }
int llvm_ios_iswprint(wint_t a) { return 1; }
int llvm_ios_printf (const char *format, ...) {
	va_list arg;
	int done;

	va_start (arg, format);
	done = vfprintf (thread_stdout, format, arg);
	va_end (arg);

	return done;
}
int llvm_ios_scanf (const char *format, ...) {
    int             count;
    va_list ap;
    
    fflush(thread_stdout);
    va_start (ap, format);
    count = vfscanf (thread_stdin, format, ap);
    va_end (ap);
    return (count);
}
ssize_t llvm_ios_write(int fildes, const void *buf, size_t nbyte) {
	if (fildes == STDOUT_FILENO) return write(fileno(thread_stdout), buf, nbyte); 
	if (fildes == STDERR_FILENO) return write(fileno(thread_stderr), buf, nbyte); 
	return write(fildes, buf, nbyte); 
}
int llvm_ios_puts(const char *s) {
	// puts adds a newline at the end.
	int returnValue = fputs(s, thread_stdout);
	fputc('\n', thread_stdout); 
	return returnValue;
}
int llvm_ios_fputs(const char* s, FILE *stream) {
	if (fileno(stream) == STDOUT_FILENO) return fputs(s, thread_stdout); 
	if (fileno(stream) == STDERR_FILENO) return fputs(s, thread_stderr); 
	return fputs(s, stream);
}
int llvm_ios_fputc(int c, FILE *stream) {
	if (fileno(stream) == STDOUT_FILENO) return fputc(c, thread_stdout); 
	if (fileno(stream) == STDERR_FILENO) return fputc(c, thread_stderr); 
	return fputc(c, stream);
}
int llvm_ios_putw(int w, FILE *stream) {
	if (fileno(stream) == STDOUT_FILENO) return putw(w, thread_stdout); 
	if (fileno(stream) == STDERR_FILENO) return putw(w, thread_stderr); 
	return putw(w, stream);
}
pid_t llvm_ios_fork(void) { return 0; } // Always go through the child branch
pid_t llvm_ios_waitpid(pid_t pid, int *stat_loc, int options) {
	pthread_join(ios_getLastThreadId(), NULL); // best we can do
	if (stat_loc) *stat_loc = W_EXITCODE(ios_getCommandStatus(), 0); 
	return 0; 
}
// 
void llvm_ios_vwarn(const char *fmt, va_list args)
{
	fputs(llvm_ios_progname, thread_stderr);
	if (fmt != NULL)
	{
		fputs(": ", thread_stderr);
		vfprintf(thread_stderr, fmt, args);
	}
	fputs(": ", thread_stderr);
	fputs(strerror(errno), thread_stderr);
	putc('\n', thread_stderr);
}

void llvm_ios_vwarnx(const char *fmt, va_list args)
{
	fputs(llvm_ios_progname, thread_stderr);
	fputs(": ", thread_stderr);
	if (fmt != NULL)
		vfprintf(thread_stderr, fmt, args);
	putc('\n', thread_stderr);
}
// void err(int eval, const char *fmt, ...);
void llvm_ios_err(int eval, const char *fmt, ...) {
	va_list argptr;
	va_start(argptr, fmt);
	llvm_ios_vwarn(fmt, argptr);
	va_end(argptr);
	llvm_ios_exit(eval);
}
//	 void errx(int eval, const char *fmt, ...);
void llvm_ios_errx(int eval, const char *fmt, ...) {
	va_list argptr;
	va_start(argptr, fmt);
	llvm_ios_vwarnx(fmt, argptr);
	va_end(argptr);
	llvm_ios_exit(eval);
}
//   void warn(const char *fmt, ...);
void llvm_ios_warn(const char *fmt, ...) {
	va_list argptr;
	va_start(argptr, fmt);
	llvm_ios_vwarn(fmt, argptr);
	va_end(argptr);
}
//   void warnx(const char *fmt, ...);
void llvm_ios_warnx(const char *fmt, ...) {
	va_list argptr;
	va_start(argptr, fmt);
	llvm_ios_vwarnx(fmt, argptr);
	va_end(argptr);
}
}
#endif
#endif

using namespace llvm;

#define DEBUG_TYPE "lli"

namespace {

  enum class JITKind { MCJIT, OrcMCJITReplacement, OrcLazy };

  cl::opt<std::string>
  InputFile(cl::desc("<input bitcode>"), cl::Positional, cl::init("-"));

  cl::list<std::string>
  InputArgv(cl::ConsumeAfter, cl::desc("<program arguments>..."));

  cl::opt<bool> ForceInterpreter("force-interpreter",
                                 cl::desc("Force interpretation: disable JIT"),
#if (TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)
                                 // force use of interpreter on iOS:
                                 // JIT compiler works inside of Xcode, not outside.
                                 cl::init(true));
#else
                                 cl::init(false));
#endif

  cl::opt<JITKind> UseJITKind(
      "jit-kind", cl::desc("Choose underlying JIT kind."),
      cl::init(JITKind::MCJIT),
      cl::values(clEnumValN(JITKind::MCJIT, "mcjit", "MCJIT"),
                 clEnumValN(JITKind::OrcMCJITReplacement, "orc-mcjit",
                            "Orc-based MCJIT replacement "
                            "(deprecated)"),
                 clEnumValN(JITKind::OrcLazy, "orc-lazy",
                            "Orc-based lazy JIT.")));

  cl::opt<unsigned>
  LazyJITCompileThreads("compile-threads",
                        cl::desc("Choose the number of compile threads "
                                 "(jit-kind=orc-lazy only)"),
                        cl::init(0));

  cl::list<std::string>
  ThreadEntryPoints("thread-entry",
                    cl::desc("calls the given entry-point on a new thread "
                             "(jit-kind=orc-lazy only)"));

  cl::opt<bool> PerModuleLazy(
      "per-module-lazy",
      cl::desc("Performs lazy compilation on whole module boundaries "
               "rather than individual functions"),
      cl::init(false));

  cl::list<std::string>
      JITDylibs("jd",
                cl::desc("Specifies the JITDylib to be used for any subsequent "
                         "-extra-module arguments."));

  // The MCJIT supports building for a target address space separate from
  // the JIT compilation process. Use a forked process and a copying
  // memory manager with IPC to execute using this functionality.
  cl::opt<bool> RemoteMCJIT("remote-mcjit",
    cl::desc("Execute MCJIT'ed code in a separate process."),
    cl::init(false));

  // Manually specify the child process for remote execution. This overrides
  // the simulated remote execution that allocates address space for child
  // execution. The child process will be executed and will communicate with
  // lli via stdin/stdout pipes.
  cl::opt<std::string>
  ChildExecPath("mcjit-remote-process",
                cl::desc("Specify the filename of the process to launch "
                         "for remote MCJIT execution.  If none is specified,"
                         "\n\tremote execution will be simulated in-process."),
                cl::value_desc("filename"), cl::init(""));

  // Determine optimization level.
  cl::opt<char>
  OptLevel("O",
           cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                    "(default = '-O2')"),
           cl::Prefix,
           cl::ZeroOrMore,
           cl::init(' '));

  cl::opt<std::string>
  TargetTriple("mtriple", cl::desc("Override target triple for module"));

  cl::opt<std::string>
  EntryFunc("entry-function",
            cl::desc("Specify the entry function (default = 'main') "
                     "of the executable"),
            cl::value_desc("function"),
            cl::init("main"));

  cl::list<std::string>
  ExtraModules("extra-module",
         cl::desc("Extra modules to be loaded"),
         cl::value_desc("input bitcode"));

  cl::list<std::string>
  ExtraObjects("extra-object",
         cl::desc("Extra object files to be loaded"),
         cl::value_desc("input object"));

  cl::list<std::string>
  ExtraArchives("extra-archive",
         cl::desc("Extra archive files to be loaded"),
         cl::value_desc("input archive"));

  cl::opt<bool>
  EnableCacheManager("enable-cache-manager",
        cl::desc("Use cache manager to save/load modules"),
        cl::init(false));

  cl::opt<std::string>
  ObjectCacheDir("object-cache-dir",
                  cl::desc("Directory to store cached object files "
                           "(must be user writable)"),
                  cl::init(""));

  cl::opt<std::string>
  FakeArgv0("fake-argv0",
            cl::desc("Override the 'argv[0]' value passed into the executing"
                     " program"), cl::value_desc("executable"));

  cl::opt<bool>
  DisableCoreFiles("disable-core-files", cl::Hidden,
                   cl::desc("Disable emission of core files if possible"));

  cl::opt<bool>
  NoLazyCompilation("disable-lazy-compilation",
                  cl::desc("Disable JIT lazy compilation"),
                  cl::init(false));

  cl::opt<bool>
  GenerateSoftFloatCalls("soft-float",
    cl::desc("Generate software floating point library calls"),
    cl::init(false));

  enum class DumpKind {
    NoDump,
    DumpFuncsToStdOut,
    DumpModsToStdOut,
    DumpModsToDisk
  };

  cl::opt<DumpKind> OrcDumpKind(
      "orc-lazy-debug", cl::desc("Debug dumping for the orc-lazy JIT."),
      cl::init(DumpKind::NoDump),
      cl::values(clEnumValN(DumpKind::NoDump, "no-dump",
                            "Don't dump anything."),
                 clEnumValN(DumpKind::DumpFuncsToStdOut, "funcs-to-stdout",
                            "Dump function names to stdout."),
                 clEnumValN(DumpKind::DumpModsToStdOut, "mods-to-stdout",
                            "Dump modules to stdout."),
                 clEnumValN(DumpKind::DumpModsToDisk, "mods-to-disk",
                            "Dump modules to the current "
                            "working directory. (WARNING: "
                            "will overwrite existing files).")),
      cl::Hidden);

  ExitOnError ExitOnErr;
}

//===----------------------------------------------------------------------===//
// Object cache
//
// This object cache implementation writes cached objects to disk to the
// directory specified by CacheDir, using a filename provided in the module
// descriptor. The cache tries to load a saved object using that path if the
// file exists. CacheDir defaults to "", in which case objects are cached
// alongside their originating bitcodes.
//
class LLIObjectCache : public ObjectCache {
public:
  LLIObjectCache(const std::string& CacheDir) : CacheDir(CacheDir) {
    // Add trailing '/' to cache dir if necessary.
    if (!this->CacheDir.empty() &&
        this->CacheDir[this->CacheDir.size() - 1] != '/')
      this->CacheDir += '/';
  }
  ~LLIObjectCache() override {}

  void notifyObjectCompiled(const Module *M, MemoryBufferRef Obj) override {
    const std::string &ModuleID = M->getModuleIdentifier();
    std::string CacheName;
    if (!getCacheFilename(ModuleID, CacheName))
      return;
    if (!CacheDir.empty()) { // Create user-defined cache dir.
      SmallString<128> dir(sys::path::parent_path(CacheName));
      sys::fs::create_directories(Twine(dir));
    }
    std::error_code EC;
    raw_fd_ostream outfile(CacheName, EC, sys::fs::OF_None);
    outfile.write(Obj.getBufferStart(), Obj.getBufferSize());
    outfile.close();
  }

  std::unique_ptr<MemoryBuffer> getObject(const Module* M) override {
    const std::string &ModuleID = M->getModuleIdentifier();
    std::string CacheName;
    if (!getCacheFilename(ModuleID, CacheName))
      return nullptr;
    // Load the object from the cache filename
    ErrorOr<std::unique_ptr<MemoryBuffer>> IRObjectBuffer =
        MemoryBuffer::getFile(CacheName, -1, false);
    // If the file isn't there, that's OK.
    if (!IRObjectBuffer)
      return nullptr;
    // MCJIT will want to write into this buffer, and we don't want that
    // because the file has probably just been mmapped.  Instead we make
    // a copy.  The filed-based buffer will be released when it goes
    // out of scope.
    return MemoryBuffer::getMemBufferCopy(IRObjectBuffer.get()->getBuffer());
  }

private:
  std::string CacheDir;

  bool getCacheFilename(const std::string &ModID, std::string &CacheName) {
    std::string Prefix("file:");
    size_t PrefixLength = Prefix.length();
    if (ModID.substr(0, PrefixLength) != Prefix)
      return false;
        std::string CacheSubdir = ModID.substr(PrefixLength);
#if defined(_WIN32)
        // Transform "X:\foo" => "/X\foo" for convenience.
        if (isalpha(CacheSubdir[0]) && CacheSubdir[1] == ':') {
          CacheSubdir[1] = CacheSubdir[0];
          CacheSubdir[0] = '/';
        }
#endif
    CacheName = CacheDir + CacheSubdir;
    size_t pos = CacheName.rfind('.');
    CacheName.replace(pos, CacheName.length() - pos, ".o");
    return true;
  }
};

// On Mingw and Cygwin, an external symbol named '__main' is called from the
// generated 'main' function to allow static initialization.  To avoid linking
// problems with remote targets (because lli's remote target support does not
// currently handle external linking) we add a secondary module which defines
// an empty '__main' function.
static void addCygMingExtraModule(ExecutionEngine &EE, LLVMContext &Context,
                                  StringRef TargetTripleStr) {
  IRBuilder<> Builder(Context);
  Triple TargetTriple(TargetTripleStr);

  // Create a new module.
  std::unique_ptr<Module> M = std::make_unique<Module>("CygMingHelper", Context);
  M->setTargetTriple(TargetTripleStr);

  // Create an empty function named "__main".
  Type *ReturnTy;
  if (TargetTriple.isArch64Bit())
    ReturnTy = Type::getInt64Ty(Context);
  else
    ReturnTy = Type::getInt32Ty(Context);
  Function *Result =
      Function::Create(FunctionType::get(ReturnTy, {}, false),
                       GlobalValue::ExternalLinkage, "__main", M.get());

  BasicBlock *BB = BasicBlock::Create(Context, "__main", Result);
  Builder.SetInsertPoint(BB);
  Value *ReturnVal = ConstantInt::get(ReturnTy, 0);
  Builder.CreateRet(ReturnVal);

  // Add this new module to the ExecutionEngine.
  EE.addModule(std::move(M));
}

CodeGenOpt::Level getOptLevel() {
  switch (OptLevel) {
  default:
    WithColor::error(errs(), "lli") << "invalid optimization level.\n";
    exit(1);
  case '0': return CodeGenOpt::None;
  case '1': return CodeGenOpt::Less;
  case ' ':
  case '2': return CodeGenOpt::Default;
  case '3': return CodeGenOpt::Aggressive;
  }
  llvm_unreachable("Unrecognized opt level.");
}

LLVM_ATTRIBUTE_NORETURN
static void reportError(SMDiagnostic Err, const char *ProgName) {
  Err.print(ProgName, errs());
  exit(1);
}

int runOrcLazyJIT(const char *ProgName);
void disallowOrcOptions();

//===----------------------------------------------------------------------===//
// main Driver function
//
int main(int argc, char **argv, char * const *envp) {
  InitLLVM X(argc, argv);

  if (argc > 1)
    ExitOnErr.setBanner(std::string(argv[0]) + ": ");

  // If we have a native target, initialize it to ensure it is linked in and
  // usable by the JIT.
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  cl::ParseCommandLineOptions(argc, argv,
                              "llvm interpreter & dynamic compiler\n");

  // If the user doesn't want core files, disable them.
  if (DisableCoreFiles)
    sys::Process::PreventCoreFiles();

  if (UseJITKind == JITKind::OrcLazy)
    return runOrcLazyJIT(argv[0]);
  else
    disallowOrcOptions();

  LLVMContext Context;

  // Load the bitcode...
  SMDiagnostic Err;
  std::unique_ptr<Module> Owner = parseIRFile(InputFile, Err, Context);
  Module *Mod = Owner.get();
  if (!Mod)
    reportError(Err, argv[0]);

  if (EnableCacheManager) {
    std::string CacheName("file:");
    CacheName.append(InputFile);
    Mod->setModuleIdentifier(CacheName);
  }

  // If not jitting lazily, load the whole bitcode file eagerly too.
  if (NoLazyCompilation) {
    // Use *argv instead of argv[0] to work around a wrong GCC warning.
    ExitOnError ExitOnErr(std::string(*argv) +
                          ": bitcode didn't read correctly: ");
    ExitOnErr(Mod->materializeAll());
  }

  std::string ErrorMsg;
  EngineBuilder builder(std::move(Owner));
  builder.setMArch(MArch);
  builder.setMCPU(getCPUStr());
  builder.setMAttrs(getFeatureList());
  if (RelocModel.getNumOccurrences())
    builder.setRelocationModel(RelocModel);
  if (CMModel.getNumOccurrences())
    builder.setCodeModel(CMModel);
  builder.setErrorStr(&ErrorMsg);
  builder.setEngineKind(ForceInterpreter
                        ? EngineKind::Interpreter
                        : EngineKind::JIT);
  builder.setUseOrcMCJITReplacement(AcknowledgeORCv1Deprecation,
                                    UseJITKind == JITKind::OrcMCJITReplacement);

  // If we are supposed to override the target triple, do so now.
  if (!TargetTriple.empty())
    Mod->setTargetTriple(Triple::normalize(TargetTriple));

  // Enable MCJIT if desired.
  RTDyldMemoryManager *RTDyldMM = nullptr;
  if (!ForceInterpreter) {
    if (RemoteMCJIT)
      RTDyldMM = new ForwardingMemoryManager();
    else
      RTDyldMM = new SectionMemoryManager();

    // Deliberately construct a temp std::unique_ptr to pass in. Do not null out
    // RTDyldMM: We still use it below, even though we don't own it.
    builder.setMCJITMemoryManager(
      std::unique_ptr<RTDyldMemoryManager>(RTDyldMM));
  } else if (RemoteMCJIT) {
    WithColor::error(errs(), argv[0])
        << "remote process execution does not work with the interpreter.\n";
    exit(1);
  }

  builder.setOptLevel(getOptLevel());

  TargetOptions Options = InitTargetOptionsFromCodeGenFlags();
  if (FloatABIForCalls != FloatABI::Default)
    Options.FloatABIType = FloatABIForCalls;

  builder.setTargetOptions(Options);
#if (TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)
	  // For ios_system, add symbols that override the existing ones:
	  // This needs to be done *before* the engine creation:
	  // This way, we act on both interpreter and JIT:
	  sys::DynamicLibrary::AddSymbol("stdin", &thread_stdin);
	  sys::DynamicLibrary::AddSymbol("stdout", &thread_stdout);
	  sys::DynamicLibrary::AddSymbol("stderr", &thread_stderr);
	  sys::DynamicLibrary::AddSymbol("__stdinp", &thread_stdin);
	  sys::DynamicLibrary::AddSymbol("__stdoutp", &thread_stdout);
	  sys::DynamicLibrary::AddSymbol("__stderrp", &thread_stderr);
	  sys::DynamicLibrary::AddSymbol("thread_stdin", &thread_stdin);
	  sys::DynamicLibrary::AddSymbol("thread_stdout", &thread_stdout);
	  sys::DynamicLibrary::AddSymbol("thread_stderr", &thread_stderr);
	  // External functions defined in ios_system:
	  sys::DynamicLibrary::AddSymbol("system", (void*)&ios_system);
	  sys::DynamicLibrary::AddSymbol("popen", (void*)&ios_popen);
	  sys::DynamicLibrary::AddSymbol("pclose", (void*)&fclose);
	  sys::DynamicLibrary::AddSymbol("isatty", (void*)&ios_isatty);
	  sys::DynamicLibrary::AddSymbol("dup2", (void*)&ios_dup2);
	  sys::DynamicLibrary::AddSymbol("execv", (void*)&ios_execv);
	  sys::DynamicLibrary::AddSymbol("execvp", (void*)&ios_execv);
	  sys::DynamicLibrary::AddSymbol("execve", (void*)&ios_execve);
	  // External functions defined locally:
	  sys::DynamicLibrary::AddSymbol("exit", (void*)&llvm_ios_exit);
	  sys::DynamicLibrary::AddSymbol("_exit", (void*)&llvm_ios_exit);
	  sys::DynamicLibrary::AddSymbol("abort", (void*)&llvm_ios_abort);
	  sys::DynamicLibrary::AddSymbol("putchar", (void*)&llvm_ios_putchar);
	  sys::DynamicLibrary::AddSymbol("getchar", (void*)&llvm_ios_getchar);
	  sys::DynamicLibrary::AddSymbol("getwchar", (void*)&llvm_ios_getwchar);
	  sys::DynamicLibrary::AddSymbol("iswprint", (void*)&llvm_ios_iswprint);
	  // scanf, printf, write: redirect to right stream
	  sys::DynamicLibrary::AddSymbol("printf", (void*)&llvm_ios_printf);
	  sys::DynamicLibrary::AddSymbol("scanf", (void*)&llvm_ios_scanf);
	  sys::DynamicLibrary::AddSymbol("write", (void*)&llvm_ios_write);
	  sys::DynamicLibrary::AddSymbol("puts", (void*)&llvm_ios_puts);
	  sys::DynamicLibrary::AddSymbol("fputs", (void*)&llvm_ios_fputs);
	  sys::DynamicLibrary::AddSymbol("fputc", (void*)&llvm_ios_fputc);
	  sys::DynamicLibrary::AddSymbol("putw", (void*)&llvm_ios_putw);
	  // fork, waitpid: minimal service here:
	  sys::DynamicLibrary::AddSymbol("fork", (void*)&llvm_ios_fork);
	  sys::DynamicLibrary::AddSymbol("waitpid", (void*)&llvm_ios_waitpid);
	  // err, errx, warnx, warn (because they call exit)
	  llvm_ios_progname = InputFile.c_str(); 
	  sys::DynamicLibrary::AddSymbol("err", (void*)&llvm_ios_err);
	  sys::DynamicLibrary::AddSymbol("errx", (void*)&llvm_ios_errx);
	  sys::DynamicLibrary::AddSymbol("warn", (void*)&llvm_ios_warn);
	  sys::DynamicLibrary::AddSymbol("warnx", (void*)&llvm_ios_warnx);
	  sys::DynamicLibrary::AddSymbol("vwarn", (void*)&llvm_ios_vwarn);
	  sys::DynamicLibrary::AddSymbol("vwarnx", (void*)&llvm_ios_vwarnx);
	  // External C++ symbols (todo)
	  sys::DynamicLibrary::AddSymbol("__dso_handle", NULL);
	  // sys::DynamicLibrary::AddSymbol("_ZSt4cout", &standard_out);
	  // sys::DynamicLibrary::AddSymbol("_ZSt4cerr", &standard_err);
#endif  

  std::unique_ptr<ExecutionEngine> EE(builder.create());
  if (!EE) {
    if (!ErrorMsg.empty())
      WithColor::error(errs(), argv[0])
          << "error creating EE: " << ErrorMsg << "\n";
    else
      WithColor::error(errs(), argv[0]) << "unknown error creating EE!\n";
    exit(1);
  }

  std::unique_ptr<LLIObjectCache> CacheManager;
  if (EnableCacheManager) {
    CacheManager.reset(new LLIObjectCache(ObjectCacheDir));
    EE->setObjectCache(CacheManager.get());
  }

  // Load any additional modules specified on the command line.
  for (unsigned i = 0, e = ExtraModules.size(); i != e; ++i) {
    std::unique_ptr<Module> XMod = parseIRFile(ExtraModules[i], Err, Context);
    if (!XMod)
      reportError(Err, argv[0]);
    if (EnableCacheManager) {
      std::string CacheName("file:");
      CacheName.append(ExtraModules[i]);
      XMod->setModuleIdentifier(CacheName);
    }
    EE->addModule(std::move(XMod));
  }

  for (unsigned i = 0, e = ExtraObjects.size(); i != e; ++i) {
    Expected<object::OwningBinary<object::ObjectFile>> Obj =
        object::ObjectFile::createObjectFile(ExtraObjects[i]);
    if (!Obj) {
      // TODO: Actually report errors helpfully.
      consumeError(Obj.takeError());
      reportError(Err, argv[0]);
    }
    object::OwningBinary<object::ObjectFile> &O = Obj.get();
    EE->addObjectFile(std::move(O));
  }

  for (unsigned i = 0, e = ExtraArchives.size(); i != e; ++i) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> ArBufOrErr =
        MemoryBuffer::getFileOrSTDIN(ExtraArchives[i]);
    if (!ArBufOrErr)
      reportError(Err, argv[0]);
    std::unique_ptr<MemoryBuffer> &ArBuf = ArBufOrErr.get();

    Expected<std::unique_ptr<object::Archive>> ArOrErr =
        object::Archive::create(ArBuf->getMemBufferRef());
    if (!ArOrErr) {
      std::string Buf;
      raw_string_ostream OS(Buf);
      logAllUnhandledErrors(ArOrErr.takeError(), OS);
      OS.flush();
      errs() << Buf;
      exit(1);
    }
    std::unique_ptr<object::Archive> &Ar = ArOrErr.get();

    object::OwningBinary<object::Archive> OB(std::move(Ar), std::move(ArBuf));

    EE->addArchive(std::move(OB));
  }

  // If the target is Cygwin/MingW and we are generating remote code, we
  // need an extra module to help out with linking.
  if (RemoteMCJIT && Triple(Mod->getTargetTriple()).isOSCygMing()) {
    addCygMingExtraModule(*EE, Context, Mod->getTargetTriple());
  }

  // The following functions have no effect if their respective profiling
  // support wasn't enabled in the build configuration.
  EE->RegisterJITEventListener(
                JITEventListener::createOProfileJITEventListener());
  EE->RegisterJITEventListener(
                JITEventListener::createIntelJITEventListener());
  if (!RemoteMCJIT)
    EE->RegisterJITEventListener(
                JITEventListener::createPerfJITEventListener());

  if (!NoLazyCompilation && RemoteMCJIT) {
    WithColor::warning(errs(), argv[0])
        << "remote mcjit does not support lazy compilation\n";
    NoLazyCompilation = true;
  }
  EE->DisableLazyCompilation(NoLazyCompilation);

  // If the user specifically requested an argv[0] to pass into the program,
  // do it now.
  if (!FakeArgv0.empty()) {
    InputFile = static_cast<std::string>(FakeArgv0);
  } else {
    // Otherwise, if there is a .bc suffix on the executable strip it off, it
    // might confuse the program.
    if (StringRef(InputFile).endswith(".bc"))
      InputFile.erase(InputFile.length() - 3);
  }

  // Add the module's name to the start of the vector of arguments to main().
  InputArgv.insert(InputArgv.begin(), InputFile);

  // Call the main function from M as if its signature were:
  //   int main (int argc, char **argv, const char **envp)
  // using the contents of Args to determine argc & argv, and the contents of
  // EnvVars to determine envp.
  //
  Function *EntryFn = Mod->getFunction(EntryFunc);
  if (!EntryFn) {
    WithColor::error(errs(), argv[0])
        << '\'' << EntryFunc << "\' function not found in module.\n";
    return -1;
  }

  // Reset errno to zero on entry to main.
  errno = 0;

  int Result = -1;

  // Sanity check use of remote-jit: LLI currently only supports use of the
  // remote JIT on Unix platforms.
  if (RemoteMCJIT) {
#ifndef LLVM_ON_UNIX
    WithColor::warning(errs(), argv[0])
        << "host does not support external remote targets.\n";
    WithColor::note() << "defaulting to local execution\n";
    return -1;
#else
    if (ChildExecPath.empty()) {
      WithColor::error(errs(), argv[0])
          << "-remote-mcjit requires -mcjit-remote-process.\n";
      exit(1);
    } else if (!sys::fs::can_execute(ChildExecPath)) {
      WithColor::error(errs(), argv[0])
          << "unable to find usable child executable: '" << ChildExecPath
          << "'\n";
      return -1;
    }
#endif
  }

  if (!RemoteMCJIT) {
    // If the program doesn't explicitly call exit, we will need the Exit
    // function later on to make an explicit call, so get the function now.
#if (TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)
	  // on iOS, normally, ForceInterpreter = true, but if your run the JIT you need this:
	  if (!ForceInterpreter) {
		  FunctionCallee Exit = Mod->getOrInsertFunction("llvm_ios_exit", Type::getVoidTy(Context),
                                                      Type::getInt32Ty(Context));
	  } else 
#endif
		  FunctionCallee Exit = Mod->getOrInsertFunction(
				  "exit", Type::getVoidTy(Context), Type::getInt32Ty(Context));

    // Run static constructors.
    if (!ForceInterpreter) {
      // Give MCJIT a chance to apply relocations and set page permissions.
      EE->finalizeObject();
    }
    EE->runStaticConstructorsDestructors(false);

    // Trigger compilation separately so code regions that need to be
    // invalidated will be known.
    (void)EE->getPointerToFunction(EntryFn);
    // Clear instruction cache before code will be executed.
    if (RTDyldMM)
      static_cast<SectionMemoryManager*>(RTDyldMM)->invalidateInstructionCache();

    // Run main.
    Result = EE->runFunctionAsMain(EntryFn, InputArgv, envp);

    // Run static destructors.
    EE->runStaticConstructorsDestructors(true);

    // If the program didn't call exit explicitly, we should call it now.
    // This ensures that any atexit handlers get called correctly.
    if (Function *ExitF =
            dyn_cast<Function>(Exit.getCallee()->stripPointerCasts())) {
      if (ExitF->getFunctionType() == Exit.getFunctionType()) {
        std::vector<GenericValue> Args;
        GenericValue ResultGV;
        ResultGV.IntVal = APInt(32, Result);
        Args.push_back(ResultGV);
        EE->runFunction(ExitF, Args);
        WithColor::error(errs(), argv[0])
            << "exit(" << Result << ") returned!\n";
        abort();
      }
    }
    WithColor::error(errs(), argv[0]) << "exit defined with wrong prototype!\n";
    abort();
  } else {
    // else == "if (RemoteMCJIT)"

    // Remote target MCJIT doesn't (yet) support static constructors. No reason
    // it couldn't. This is a limitation of the LLI implementation, not the
    // MCJIT itself. FIXME.

    // Lanch the remote process and get a channel to it.
    std::unique_ptr<FDRawChannel> C = launchRemote();
    if (!C) {
      WithColor::error(errs(), argv[0]) << "failed to launch remote JIT.\n";
      exit(1);
    }

    // Create a remote target client running over the channel.
    llvm::orc::ExecutionSession ES;
    ES.setErrorReporter([&](Error Err) { ExitOnErr(std::move(Err)); });
    typedef orc::remote::OrcRemoteTargetClient MyRemote;
    auto R = ExitOnErr(MyRemote::Create(*C, ES));

    // Create a remote memory manager.
    auto RemoteMM = ExitOnErr(R->createRemoteMemoryManager());

    // Forward MCJIT's memory manager calls to the remote memory manager.
    static_cast<ForwardingMemoryManager*>(RTDyldMM)->setMemMgr(
      std::move(RemoteMM));

    // Forward MCJIT's symbol resolution calls to the remote.
    static_cast<ForwardingMemoryManager *>(RTDyldMM)->setResolver(
        orc::createLambdaResolver(
            AcknowledgeORCv1Deprecation,
            [](const std::string &Name) { return nullptr; },
            [&](const std::string &Name) {
              if (auto Addr = ExitOnErr(R->getSymbolAddress(Name)))
                return JITSymbol(Addr, JITSymbolFlags::Exported);
              return JITSymbol(nullptr);
            }));

    // Grab the target address of the JIT'd main function on the remote and call
    // it.
    // FIXME: argv and envp handling.
    JITTargetAddress Entry = EE->getFunctionAddress(EntryFn->getName().str());
    EE->finalizeObject();
    LLVM_DEBUG(dbgs() << "Executing '" << EntryFn->getName() << "' at 0x"
                      << format("%llx", Entry) << "\n");
    Result = ExitOnErr(R->callIntVoid(Entry));

    // Like static constructors, the remote target MCJIT support doesn't handle
    // this yet. It could. FIXME.

    // Delete the EE - we need to tear it down *before* we terminate the session
    // with the remote, otherwise it'll crash when it tries to release resources
    // on a remote that has already been disconnected.
    EE.reset();

    // Signal the remote target that we're done JITing.
    ExitOnErr(R->terminateSession());
  }

  return Result;
}

static std::function<void(Module &)> createDebugDumper() {
  switch (OrcDumpKind) {
  case DumpKind::NoDump:
    return [](Module &M) {};

  case DumpKind::DumpFuncsToStdOut:
    return [](Module &M) {
      printf("[ ");

      for (const auto &F : M) {
        if (F.isDeclaration())
          continue;

        if (F.hasName()) {
          std::string Name(F.getName());
          printf("%s ", Name.c_str());
        } else
          printf("<anon> ");
      }

      printf("]\n");
    };

  case DumpKind::DumpModsToStdOut:
    return [](Module &M) {
      outs() << "----- Module Start -----\n" << M << "----- Module End -----\n";
    };

  case DumpKind::DumpModsToDisk:
    return [](Module &M) {
      std::error_code EC;
      raw_fd_ostream Out(M.getModuleIdentifier() + ".ll", EC, sys::fs::OF_Text);
      if (EC) {
        errs() << "Couldn't open " << M.getModuleIdentifier()
               << " for dumping.\nError:" << EC.message() << "\n";
        exit(1);
      }
      Out << M;
    };
  }
  llvm_unreachable("Unknown DumpKind");
}

static void exitOnLazyCallThroughFailure() { exit(1); }

int runOrcLazyJIT(const char *ProgName) {
  // Start setting up the JIT environment.

  // Parse the main module.
  orc::ThreadSafeContext TSCtx(std::make_unique<LLVMContext>());
  SMDiagnostic Err;
  auto MainModule = parseIRFile(InputFile, Err, *TSCtx.getContext());
  if (!MainModule)
    reportError(Err, ProgName);

  const auto &TT = MainModule->getTargetTriple();
  orc::LLLazyJITBuilder Builder;

  Builder.setJITTargetMachineBuilder(
      TT.empty() ? ExitOnErr(orc::JITTargetMachineBuilder::detectHost())
                 : orc::JITTargetMachineBuilder(Triple(TT)));

  if (!MArch.empty())
    Builder.getJITTargetMachineBuilder()->getTargetTriple().setArchName(MArch);

  Builder.getJITTargetMachineBuilder()
      ->setCPU(getCPUStr())
      .addFeatures(getFeatureList())
      .setRelocationModel(RelocModel.getNumOccurrences()
                              ? Optional<Reloc::Model>(RelocModel)
                              : None)
      .setCodeModel(CMModel.getNumOccurrences()
                        ? Optional<CodeModel::Model>(CMModel)
                        : None);

  Builder.setLazyCompileFailureAddr(
      pointerToJITTargetAddress(exitOnLazyCallThroughFailure));
  Builder.setNumCompileThreads(LazyJITCompileThreads);

  auto J = ExitOnErr(Builder.create());

  if (PerModuleLazy)
    J->setPartitionFunction(orc::CompileOnDemandLayer::compileWholeModule);

  auto Dump = createDebugDumper();

  J->setLazyCompileTransform([&](orc::ThreadSafeModule TSM,
                                 const orc::MaterializationResponsibility &R) {
    TSM.withModuleDo([&](Module &M) {
      if (verifyModule(M, &dbgs())) {
        dbgs() << "Bad module: " << &M << "\n";
        exit(1);
      }
      Dump(M);
    });
    return TSM;
  });
  J->getMainJITDylib().addGenerator(
      ExitOnErr(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          J->getDataLayout().getGlobalPrefix())));

  orc::MangleAndInterner Mangle(J->getExecutionSession(), J->getDataLayout());
  orc::LocalCXXRuntimeOverrides CXXRuntimeOverrides;
  ExitOnErr(CXXRuntimeOverrides.enable(J->getMainJITDylib(), Mangle));

  // Add the main module.
  ExitOnErr(
      J->addLazyIRModule(orc::ThreadSafeModule(std::move(MainModule), TSCtx)));

  // Create JITDylibs and add any extra modules.
  {
    // Create JITDylibs, keep a map from argument index to dylib. We will use
    // -extra-module argument indexes to determine what dylib to use for each
    // -extra-module.
    std::map<unsigned, orc::JITDylib *> IdxToDylib;
    IdxToDylib[0] = &J->getMainJITDylib();
    for (auto JDItr = JITDylibs.begin(), JDEnd = JITDylibs.end();
         JDItr != JDEnd; ++JDItr) {
      orc::JITDylib *JD = J->getJITDylibByName(*JDItr);
      if (!JD)
        JD = &J->createJITDylib(*JDItr);
      IdxToDylib[JITDylibs.getPosition(JDItr - JITDylibs.begin())] = JD;
    }

    for (auto EMItr = ExtraModules.begin(), EMEnd = ExtraModules.end();
         EMItr != EMEnd; ++EMItr) {
      auto M = parseIRFile(*EMItr, Err, *TSCtx.getContext());
      if (!M)
        reportError(Err, ProgName);

      auto EMIdx = ExtraModules.getPosition(EMItr - ExtraModules.begin());
      assert(EMIdx != 0 && "ExtraModule should have index > 0");
      auto JDItr = std::prev(IdxToDylib.lower_bound(EMIdx));
      auto &JD = *JDItr->second;
      ExitOnErr(
          J->addLazyIRModule(JD, orc::ThreadSafeModule(std::move(M), TSCtx)));
    }

    for (auto EAItr = ExtraArchives.begin(), EAEnd = ExtraArchives.end();
         EAItr != EAEnd; ++EAItr) {
      auto EAIdx = ExtraArchives.getPosition(EAItr - ExtraArchives.begin());
      assert(EAIdx != 0 && "ExtraArchive should have index > 0");
      auto JDItr = std::prev(IdxToDylib.lower_bound(EAIdx));
      auto &JD = *JDItr->second;
      JD.addGenerator(ExitOnErr(orc::StaticLibraryDefinitionGenerator::Load(
          J->getObjLinkingLayer(), EAItr->c_str())));
    }
  }

  // Add the objects.
  for (auto &ObjPath : ExtraObjects) {
    auto Obj = ExitOnErr(errorOrToExpected(MemoryBuffer::getFile(ObjPath)));
    ExitOnErr(J->addObjectFile(std::move(Obj)));
  }

  // Generate a argument string.
  std::vector<std::string> Args;
  Args.push_back(InputFile);
  for (auto &Arg : InputArgv)
    Args.push_back(Arg);

  // Run any static constructors.
  ExitOnErr(J->runConstructors());

  // Run any -thread-entry points.
  std::vector<std::thread> AltEntryThreads;
  for (auto &ThreadEntryPoint : ThreadEntryPoints) {
    auto EntryPointSym = ExitOnErr(J->lookup(ThreadEntryPoint));
    typedef void (*EntryPointPtr)();
    auto EntryPoint =
      reinterpret_cast<EntryPointPtr>(static_cast<uintptr_t>(EntryPointSym.getAddress()));
    AltEntryThreads.push_back(std::thread([EntryPoint]() { EntryPoint(); }));
  }

  // Run main.
  auto MainSym = ExitOnErr(J->lookup("main"));
  typedef int (*MainFnPtr)(int, const char *[]);
  std::vector<const char *> ArgV;
  for (auto &Arg : Args)
    ArgV.push_back(Arg.c_str());
  ArgV.push_back(nullptr);

  int ArgC = ArgV.size() - 1;
  auto Main =
      reinterpret_cast<MainFnPtr>(static_cast<uintptr_t>(MainSym.getAddress()));
  auto Result = Main(ArgC, (const char **)ArgV.data());

  // Wait for -entry-point threads.
  for (auto &AltEntryThread : AltEntryThreads)
    AltEntryThread.join();

  // Run destructors.
  ExitOnErr(J->runDestructors());
  CXXRuntimeOverrides.runDestructors();

  return Result;
}

void disallowOrcOptions() {
  // Make sure nobody used an orc-lazy specific option accidentally.

  if (LazyJITCompileThreads != 0) {
    errs() << "-compile-threads requires -jit-kind=orc-lazy\n";
    exit(1);
  }

  if (!ThreadEntryPoints.empty()) {
    errs() << "-thread-entry requires -jit-kind=orc-lazy\n";
    exit(1);
  }

  if (PerModuleLazy) {
    errs() << "-per-module-lazy requires -jit-kind=orc-lazy\n";
    exit(1);
  }
}

std::unique_ptr<FDRawChannel> launchRemote() {
	// TODO. The usual.
#ifndef LLVM_ON_UNIX
  llvm_unreachable("launchRemote not supported on non-Unix platforms");
#else
  int PipeFD[2][2];
  pid_t ChildPID;

  // Create two pipes.
  if (pipe(PipeFD[0]) != 0 || pipe(PipeFD[1]) != 0)
    perror("Error creating pipe: ");

  ChildPID = fork();

  if (ChildPID == 0) {
    // In the child...

    // Close the parent ends of the pipes
    close(PipeFD[0][1]);
    close(PipeFD[1][0]);


    // Execute the child process.
    std::unique_ptr<char[]> ChildPath, ChildIn, ChildOut;
    {
      ChildPath.reset(new char[ChildExecPath.size() + 1]);
      std::copy(ChildExecPath.begin(), ChildExecPath.end(), &ChildPath[0]);
      ChildPath[ChildExecPath.size()] = '\0';
      std::string ChildInStr = utostr(PipeFD[0][0]);
      ChildIn.reset(new char[ChildInStr.size() + 1]);
      std::copy(ChildInStr.begin(), ChildInStr.end(), &ChildIn[0]);
      ChildIn[ChildInStr.size()] = '\0';
      std::string ChildOutStr = utostr(PipeFD[1][1]);
      ChildOut.reset(new char[ChildOutStr.size() + 1]);
      std::copy(ChildOutStr.begin(), ChildOutStr.end(), &ChildOut[0]);
      ChildOut[ChildOutStr.size()] = '\0';
    }

    char * const args[] = { &ChildPath[0], &ChildIn[0], &ChildOut[0], nullptr };
    int rc = execv(ChildExecPath.c_str(), args);
    if (rc != 0)
      perror("Error executing child process: ");
    llvm_unreachable("Error executing child process");
  }
  // else we're the parent...

  // Close the child ends of the pipes
  close(PipeFD[0][0]);
  close(PipeFD[1][1]);

  // Return an RPC channel connected to our end of the pipes.
  return std::make_unique<FDRawChannel>(PipeFD[1][0], PipeFD[0][1]);
#endif
}
