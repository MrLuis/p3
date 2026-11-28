// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "p3/CodeGen/Backend.h"
#include "p3/Basic/Diagnostics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Instrumentation/InstrProfiling.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"

#include <optional>
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
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

void p3::internalizeFunctions(llvm::Module &M,
                              llvm::ArrayRef<std::string> Names) {
  for (const std::string &N : Names)
    if (llvm::Function *F = M.getFunction(N);
        F && F->hasAvailableExternallyLinkage())
      F->setLinkage(llvm::GlobalValue::InternalLinkage);
}

void p3::linkRuntimeIR(llvm::Module &M, llvm::StringRef Path,
                        llvm::StringRef CPU, llvm::StringRef Features,
                        DiagnosticsEngine &Diags, bool Internalize) {
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
  // (Internalize: the names of the runtime's functions, which become the
  // program's own internal copies once linked; -fno-internalize-runtime
  // keeps them available_externally.)
  std::vector<std::string> Names;
  for (llvm::Function &F : *RT)
    if (!F.isDeclaration()) {
      if (!CPU.empty())
        F.addFnAttr("target-cpu", CPU);
      if (!Features.empty())
        F.addFnAttr("target-features", Features);
      if (Internalize && F.hasAvailableExternallyLinkage())
        Names.push_back(F.getName().str());
    }
  if (llvm::Linker::linkModules(M, std::move(RT))) {
    Diags.warning("cannot link the runtime's LLVM IR '" + Path +
                  "'; runtime functions are not inlined");
    return;
  }
  internalizeFunctions(M, Names);
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

bool p3::linkUnitIRWhole(llvm::Module &M, llvm::ArrayRef<std::string> IRPaths,
                         DiagnosticsEngine &Diags) {
  for (const std::string &Path : IRPaths) {
    llvm::SMDiagnostic Err;
    std::unique_ptr<llvm::Module> U =
        llvm::parseIRFile(Path, Err, M.getContext());
    if (!U) {
      Diags.error("cannot read the unit's LLVM IR '" + Path + "' (" +
                  Err.getMessage() + ")");
      return false;
    }
    U->setTargetTriple(M.getTargetTriple());
    U->setDataLayout(M.getDataLayout());
    // Full definitions: the objects leave the link, so each unit's state
    // has one copy, the program's (no mutable-state exception here).
    if (llvm::Linker::linkModules(M, std::move(U))) {
      Diags.error("cannot link the unit's LLVM IR '" + Path + "'");
      return false;
    }
  }
  // The units' Pascal symbols (pas.<unit>..., see CodeGen's collectExports)
  // are now the program's own; C names (cdecl thunks, `external name`)
  // and main stay external.
  auto Own = [](llvm::GlobalValue &G) {
    if (!G.isDeclaration() && G.hasExternalLinkage() &&
        G.getName().starts_with("pas."))
      G.setLinkage(llvm::GlobalValue::InternalLinkage);
  };
  for (llvm::Function &F : M)
    Own(F);
  for (llvm::GlobalVariable &G : M.globals())
    Own(G);
  return true;
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

static bool promoteMainGlobals(llvm::Module &M) {
  llvm::Function *Main = M.getFunction("main");
  if (!Main || Main->isDeclaration())
    return false;

  const llvm::DataLayout &DL = M.getDataLayout();
  llvm::SmallVector<llvm::GlobalVariable *, 16> Candidates;
  for (llvm::GlobalVariable &GV : M.globals()) {
    if (!GV.hasLocalLinkage() || GV.isDeclaration() || GV.isConstant() ||
        !GV.hasInitializer() || GV.isThreadLocal() ||
        GV.isExternallyInitialized() || GV.hasSection())
      continue;
    if (!GV.getName().starts_with("pas."))
      continue;
    if (DL.getTypeAllocSize(GV.getValueType()) > 4096)
      continue;
    GV.removeDeadConstantUsers();
    if (GV.use_empty())
      continue;

    bool Safe = true;
    llvm::SmallVector<llvm::User *, 16> Work;
    llvm::SmallPtrSet<const llvm::User *, 32> Seen;
    for (llvm::User *U : GV.users())
      if (Seen.insert(U).second)
        Work.push_back(U);

    while (!Work.empty() && Safe) {
      llvm::User *U = Work.pop_back_val();
      if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(U)) {
        unsigned Op = CE->getOpcode();
        if (Op != llvm::Instruction::GetElementPtr &&
            Op != llvm::Instruction::BitCast &&
            Op != llvm::Instruction::AddrSpaceCast) {
          Safe = false;
          break;
        }
        for (llvm::User *CU : CE->users()) {
          if (!llvm::isa<llvm::Instruction>(CU)) {
            Safe = false;
            break;
          }
          if (Seen.insert(CU).second)
            Work.push_back(CU);
        }
        continue;
      }
      auto *I = llvm::dyn_cast<llvm::Instruction>(U);
      if (!I || I->getFunction() != Main) {
        Safe = false;
        break;
      }
      if (llvm::isa<llvm::LoadInst>(I))
        continue;
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(I)) {
        llvm::Value *Val = SI->getValueOperand();
        if (Val == &GV ||
            (llvm::isa<llvm::User>(Val) &&
             Seen.count(llvm::cast<llvm::User>(Val))))
          Safe = false;
        continue;
      }
      if (llvm::isa<llvm::GetElementPtrInst>(I) ||
          llvm::isa<llvm::BitCastInst>(I) ||
          llvm::isa<llvm::AddrSpaceCastInst>(I)) {
        for (llvm::User *IU : I->users())
          if (Seen.insert(IU).second)
            Work.push_back(IU);
        continue;
      }
      if (auto *CB = llvm::dyn_cast<llvm::CallBase>(I)) {
        if (CB->getCalledOperand() == &GV) {
          Safe = false;
          break;
        }
        for (unsigned ArgNo = 0; ArgNo < CB->arg_size(); ++ArgNo) {
          llvm::Value *Arg = CB->getArgOperand(ArgNo);
          if (Arg == &GV ||
              (llvm::isa<llvm::User>(Arg) &&
               Seen.count(llvm::cast<llvm::User>(Arg)))) {
            if (!CB->doesNotCapture(ArgNo)) {
              Safe = false;
              break;
            }
          }
        }
        continue;
      }
      Safe = false;
    }
    if (Safe)
      Candidates.push_back(&GV);
  }

  if (Candidates.empty())
    return false;

  llvm::BasicBlock &Entry = Main->getEntryBlock();
  auto AllocaIt = Entry.begin();
  while (AllocaIt != Entry.end() && llvm::isa<llvm::AllocaInst>(*AllocaIt))
    ++AllocaIt;
  llvm::Instruction *InsertAnchor =
      AllocaIt != Entry.end() ? &*AllocaIt : Entry.getTerminator();

  for (llvm::GlobalVariable *GV : Candidates) {
    // Expand any ConstantExpr users in Main into instructions before replacing GV.
    llvm::SmallVector<llvm::ConstantExpr *, 8> CEs;
    for (llvm::User *U : GV->users())
      if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(U))
        CEs.push_back(CE);
    while (!CEs.empty()) {
      llvm::ConstantExpr *CE = CEs.pop_back_val();
      llvm::SmallVector<llvm::Use *, 8> Uses;
      for (llvm::Use &U : CE->uses())
        Uses.push_back(&U);
      for (llvm::Use *U : Uses) {
        if (auto *I = llvm::dyn_cast<llvm::Instruction>(U->getUser())) {
          llvm::Instruction *Mat = CE->getAsInstruction();
          if (auto *PN = llvm::dyn_cast<llvm::PHINode>(I))
            Mat->insertBefore(
                PN->getIncomingBlock(*U)->getTerminator()->getIterator());
          else
            Mat->insertBefore(I->getIterator());
          U->set(Mat);
        }
      }
      if (CE->use_empty())
        CE->destroyConstant();
    }
    GV->removeDeadConstantUsers();

    llvm::Align Align = GV->getAlign().value_or(
        DL.getPrefTypeAlign(GV->getValueType()));
    auto *AI = new llvm::AllocaInst(GV->getValueType(), 0, nullptr, Align,
                                    GV->getName(), Entry.begin());
    auto *InitStore =
        new llvm::StoreInst(GV->getInitializer(), AI, InsertAnchor->getIterator());
    InitStore->setAlignment(AI->getAlign());
    GV->replaceAllUsesWith(AI);
    GV->clearMetadata();
    GV->eraseFromParent();
  }
  for (llvm::BasicBlock &BB : *Main)
    for (llvm::Instruction &I : BB)
      if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I))
        if (CI->getTailCallKind() == llvm::CallInst::TCK_Tail)
          CI->setTailCallKind(llvm::CallInst::TCK_None);
  return true;
}

static bool foldVectorRecurrences(llvm::Module &M, llvm::TargetMachine &TM) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB(&TM);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  bool ChangedAny = false;
  llvm::Type *I64 = llvm::Type::getInt64Ty(M.getContext());

  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    llvm::LoopSimplifyPass().run(F, FAM);
    auto &LI = FAM.getResult<llvm::LoopAnalysis>(F);
    auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(F);
    llvm::SmallVector<llvm::Loop *, 8> Worklist;
    for (llvm::Loop *L : LI)
      Worklist.push_back(L);
    while (!Worklist.empty()) {
      llvm::Loop *L = Worklist.pop_back_val();
      for (llvm::Loop *Sub : *L)
        Worklist.push_back(Sub);

      llvm::BasicBlock *Header = L->getHeader();
      llvm::BasicBlock *Latch = L->getLoopLatch();
      llvm::BasicBlock *Preheader = L->getLoopPreheader();
      if (!Header || !Latch || !Preheader || Header != Latch ||
          L->getBlocks().size() != 1 || L->getExitingBlock() != Latch ||
          !L->getUniqueExitBlock())
        continue;

      const llvm::SCEV *BTC = SE.getBackedgeTakenCount(L);
      if (llvm::isa<llvm::SCEVCouldNotCompute>(BTC) ||
          !BTC->getType()->isIntegerTy())
        continue;
      unsigned Width = BTC->getType()->getIntegerBitWidth();
      if (Width > 64)
        continue;
      if (Width > 32) {
        auto *SC = llvm::dyn_cast<llvm::SCEVConstant>(BTC);
        if (!SC || SC->getAPInt().isAllOnes())
          continue;
      }

      struct Candidate {
        llvm::PHINode *PN;
        llvm::BinaryOperator *BO;
        llvm::Value *Init;
        llvm::Value *Step;
        bool IsAdd;
      };
      llvm::SmallVector<Candidate, 4> Recs;

      for (llvm::PHINode &PN : Header->phis()) {
        auto *VTy = llvm::dyn_cast<llvm::FixedVectorType>(PN.getType());
        if (!VTy || !VTy->getElementType()->isIntegerTy())
          continue;
        if (PN.getNumIncomingValues() != 2)
          continue;
        llvm::Value *Init = PN.getIncomingValueForBlock(Preheader);
        llvm::Value *Back = PN.getIncomingValueForBlock(Latch);
        auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Back);
        if (!BO || BO->getParent() != Header)
          continue;
        bool IsAdd = BO->getOpcode() == llvm::Instruction::Add;
        bool IsSub = BO->getOpcode() == llvm::Instruction::Sub;
        if (!IsAdd && !IsSub)
          continue;
        llvm::Value *Step = nullptr;
        if (BO->getOperand(0) == &PN)
          Step = BO->getOperand(1);
        else if (IsAdd && BO->getOperand(1) == &PN)
          Step = BO->getOperand(0);
        if (!Step || !L->isLoopInvariant(Step))
          continue;

        bool Pure = true;
        for (llvm::User *U : PN.users()) {
          if (U != BO) {
            auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
            if (!UI || L->contains(UI)) {
              Pure = false;
              break;
            }
          }
        }
        for (llvm::User *U : BO->users()) {
          if (U != &PN) {
            auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
            if (!UI || L->contains(UI)) {
              Pure = false;
              break;
            }
          }
        }
        if (!Pure)
          continue;
        Recs.push_back({&PN, BO, Init, Step, IsAdd});
      }

      if (Recs.empty())
        continue;

      const llvm::SCEV *BTC64 = SE.getZeroExtendExpr(BTC, I64);
      const llvm::SCEV *TC64 = SE.getAddExpr(BTC64, SE.getOne(I64));
      llvm::SCEVExpander Exp(SE, "vec.tc");
      llvm::Value *TCVal =
          Exp.expandCodeFor(TC64, I64, Preheader->getTerminator());
      llvm::IRBuilder<> B(Preheader->getTerminator());

      for (const Candidate &C : Recs) {
        auto *VTy = llvm::cast<llvm::FixedVectorType>(C.PN->getType());
        llvm::Value *TCElem = B.CreateZExtOrTrunc(TCVal, VTy->getElementType());
        llvm::Value *TCSplat =
            B.CreateVectorSplat(VTy->getElementCount(), TCElem, "vec.tc");
        llvm::Value *Delta = B.CreateMul(C.Step, TCSplat, "vec.delta");
        llvm::Value *FinalVal =
            C.IsAdd ? B.CreateAdd(C.Init, Delta, "vec.fold")
                    : B.CreateSub(C.Init, Delta, "vec.fold");
        llvm::Value *PenultVal = nullptr;

        llvm::SmallVector<llvm::Use *, 4> OutBO, OutPN;
        for (llvm::Use &U : C.BO->uses())
          if (U.getUser() != C.PN)
            OutBO.push_back(&U);
        for (llvm::Use &U : C.PN->uses())
          if (U.getUser() != C.BO)
            OutPN.push_back(&U);
        for (llvm::Use *U : OutBO)
          U->set(FinalVal);
        if (!OutPN.empty()) {
          PenultVal = C.IsAdd ? B.CreateSub(FinalVal, C.Step, "vec.penult")
                              : B.CreateAdd(FinalVal, C.Step, "vec.penult");
          for (llvm::Use *U : OutPN)
            U->set(PenultVal);
        }
        ChangedAny = true;
      }
    }
  }
  return ChangedAny;
}

void p3::optimizeModule(llvm::Module &M, llvm::TargetMachine &TM,
                         unsigned OptLevel, unsigned SizeLevel,
                         const std::string &ProfileGen,
                         const std::string &ProfileUse) {
  auto RunPipeline = [&](bool WithPGO) {
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PipelineTuningOptions PTO;
    PTO.SLPVectorization = SizeLevel > 0 || OptLevel >= 2;
    std::optional<llvm::PGOOptions> PGOOpt;
    if (WithPGO) {
      if (!ProfileUse.empty())
        PGOOpt.emplace(ProfileUse, "", "", "", llvm::PGOOptions::IRUse);
      else if (!ProfileGen.empty())
        PGOOpt = llvm::PGOOptions("", "", "", "", llvm::PGOOptions::IRInstr);
    }
    llvm::PassBuilder PB(&TM, PTO, PGOOpt, /*PIC=*/nullptr);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

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
  };

  RunPipeline(true);
  if (OptLevel >= 2 && SizeLevel == 0 && ProfileGen.empty() &&
      ProfileUse.empty()) {
    if (promoteMainGlobals(M))
      RunPipeline(false);
    if (foldVectorRecurrences(M, TM))
      RunPipeline(false);
  }
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
