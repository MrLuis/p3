#include "p3/CodeGen/Backend.h"
#include "p3/Basic/Diagnostics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

using namespace p3;

void p3::initializeAllLLVMTargets() {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
}

static llvm::CodeGenOptLevel toCodeGenOptLevel(unsigned OptLevel) {
  switch (OptLevel) {
  case 0:  return llvm::CodeGenOptLevel::None;
  case 1:  return llvm::CodeGenOptLevel::Less;
  case 2:  return llvm::CodeGenOptLevel::Default;
  default: return llvm::CodeGenOptLevel::Aggressive;
  }
}

std::unique_ptr<llvm::TargetMachine>
p3::createTargetMachine(llvm::StringRef Triple, unsigned OptLevel,
                         llvm::StringRef CPU, llvm::StringRef Features,
                         DiagnosticsEngine &Diags) {
  std::string Err;
  llvm::Triple TheTriple(Triple.str());
  const llvm::Target *T = llvm::TargetRegistry::lookupTarget(TheTriple, Err);
  if (!T) {
    Diags.error("unsupported target '" + Triple + "': " + Err);
    return nullptr;
  }
  llvm::TargetOptions Options;
  // Each function and global in its own section, for the link-time garbage
  // collection (item 18).
  Options.FunctionSections = true;
  Options.DataSections = true;
  // PIC so the objects link into position-independent executables, which
  // is what clang/gcc produce by default on modern Linux and macOS.
  std::unique_ptr<llvm::TargetMachine> TM(T->createTargetMachine(
      TheTriple, CPU, Features, Options, llvm::Reloc::PIC_, std::nullopt,
      toCodeGenOptLevel(OptLevel)));
  if (!TM)
    Diags.error("could not create target machine for '" + Triple + "'");
  return TM;
}

void p3::linkRuntimeIR(llvm::Module &M, llvm::StringRef Path,
                        llvm::StringRef CPU, llvm::StringRef Features,
                        DiagnosticsEngine &Diags) {
  if (!llvm::sys::fs::exists(Path))
    return;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> RT = llvm::parseIRFile(Path, Err, M.getContext());
  if (!RT) {
    Diags.warning("cannot read the runtime's LLVM IR '" + Path + "' (" +
                  Err.getMessage() + "); runtime functions are not inlined");
    return;
  }
  // The file has no target of its own (utils/gen-runtime-ir.py).
  RT->setTargetTriple(M.getTargetTriple());
  RT->setDataLayout(M.getDataLayout());
  // The runtime IR is generic: give its functions the program's target so
  // that the parts that are inlined use the selected CPU and features.
  for (llvm::Function &F : *RT)
    if (!F.isDeclaration()) {
      if (!CPU.empty())
        F.addFnAttr("target-cpu", CPU);
      if (!Features.empty())
        F.addFnAttr("target-features", Features);
    }
  if (llvm::Linker::linkModules(M, std::move(RT)))
    Diags.warning("cannot link the runtime's LLVM IR '" + Path +
                  "'; runtime functions are not inlined");
}

static void markAvailableExternally(llvm::Module &M) {
  for (llvm::Function &F : M)
    if (!F.isDeclaration() && F.hasExternalLinkage())
      F.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
  for (llvm::GlobalVariable &G : M.globals())
    if (!G.isDeclaration() && G.hasExternalLinkage())
      G.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
}

void p3::linkUnitIR(llvm::Module &M, llvm::ArrayRef<std::string> ObjectPaths,
                     DiagnosticsEngine &Diags) {
  for (const std::string &Obj : ObjectPaths) {
    llvm::SmallString<256> Path(Obj);
    llvm::sys::path::replace_extension(Path, "ll");
    if (!llvm::sys::fs::exists(Path))
      continue;
    llvm::SMDiagnostic Err;
    std::unique_ptr<llvm::Module> U =
        llvm::parseIRFile(Path, Err, M.getContext());
    if (!U) {
      Diags.warning("cannot read the unit's LLVM IR '" + Path + "' (" +
                    Err.getMessage() + "); its routines are not inlined");
      continue;
    }
    U->setTargetTriple(M.getTargetTriple());
    U->setDataLayout(M.getDataLayout());
    // Importing a unit with mutable internal state would duplicate it (the
    // inlined code and the object would use different copies). A mutable
    // global that no code refers to cannot be observed: it does not block
    // the import (optimization2 item 6, first stage).
    bool MutableInternal = llvm::any_of(U->globals(), [](llvm::GlobalVariable &G) {
      return G.hasLocalLinkage() && !G.isConstant() && !G.use_empty();
    });
    if (MutableInternal)
      continue;
    markAvailableExternally(*U);
    if (llvm::Linker::linkModules(M, std::move(U)))
      Diags.warning("cannot link the unit's LLVM IR '" + Path +
                    "'; its routines are not inlined");
  }
}

bool p3::writeModuleIR(llvm::Module &M, llvm::StringRef Path,
                        DiagnosticsEngine &Diags) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    Diags.warning("cannot open '" + Path + "': " + EC.message());
    return false;
  }
  M.print(OS, nullptr);
  return true;
}

void p3::optimizeModule(llvm::Module &M, llvm::TargetMachine &TM,
                         unsigned OptLevel, unsigned SizeLevel) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PipelineTuningOptions PTO;
  PTO.SLPVectorization = SizeLevel > 0 || OptLevel >= 2;
  llvm::PassBuilder PB(&TM, PTO);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // (LLVM 23 folded OptimizationLevel's size levels out: Os and Oz map
  // by their own help text, -Os "like -O2, SLP vectorization on" and
  // -Oz "like -Os".)
  llvm::OptimizationLevel Level;
  if (SizeLevel == 1)
    Level = llvm::OptimizationLevel::O2;
  else if (SizeLevel == 2)
    Level = llvm::OptimizationLevel::O1;
  else
    switch (OptLevel) {
    case 0:  Level = llvm::OptimizationLevel::O0; break;
    case 1:  Level = llvm::OptimizationLevel::O1; break;
    case 2:  Level = llvm::OptimizationLevel::O2; break;
    default: Level = llvm::OptimizationLevel::O3; break;
    }
  llvm::ModulePassManager MPM =
      OptLevel == 0 && SizeLevel == 0 ? PB.buildO0DefaultPipeline(Level)
                                      : PB.buildPerModuleDefaultPipeline(Level);
  MPM.run(M, MAM);
}

bool p3::verifyModule(llvm::Module &M, DiagnosticsEngine &Diags) {
  std::string Msg;
  llvm::raw_string_ostream OS(Msg);
  if (!llvm::verifyModule(M, &OS))
    return true;
  Diags.fatal("internal compiler error: generated invalid LLVM IR:\n" +
              llvm::Twine(Msg));
  return false;
}

bool p3::emitOutput(llvm::Module &M, llvm::TargetMachine &TM, OutputKind Kind,
                     llvm::StringRef Path, DiagnosticsEngine &Diags) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC,
                          Kind == OutputKind::Object ? llvm::sys::fs::OF_None
                                                     : llvm::sys::fs::OF_Text);
  if (EC) {
    Diags.error("cannot open output file '" + Path + "': " + EC.message());
    return false;
  }

  if (Kind == OutputKind::LLVMIR) {
    M.print(OS, nullptr);
    return true;
  }

  llvm::legacy::PassManager PM;
  auto FileType = Kind == OutputKind::Object ? llvm::CodeGenFileType::ObjectFile
                                             : llvm::CodeGenFileType::AssemblyFile;
  if (TM.addPassesToEmitFile(PM, OS, nullptr, FileType)) {
    Diags.error("target '" + TM.getTargetTriple().str() +
                "' cannot emit this kind of file");
    return false;
  }
  PM.run(M);
  OS.flush();
  if (OS.has_error()) {
    Diags.error("error writing '" + Path + "': " + OS.error().message());
    OS.clear_error();
    return false;
  }
  return true;
}
