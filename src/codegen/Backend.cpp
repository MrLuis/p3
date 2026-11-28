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
#include "llvm/IR/Intrinsics.h"
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

static bool isVectorizablePureType(llvm::Type *T) {
  return T && (T->isIntegerTy(1) || T->isIntegerTy(8) || T->isIntegerTy(16) ||
               T->isIntegerTy(32) || T->isIntegerTy(64) || T->isFloatTy() ||
               T->isDoubleTy());
}

static bool isVectorizablePureInst(const llvm::Instruction &I) {
  if (!isVectorizablePureType(I.getType()))
    return false;
  if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
    switch (BO->getOpcode()) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
    case llvm::Instruction::FAdd:
    case llvm::Instruction::FSub:
    case llvm::Instruction::FMul:
    case llvm::Instruction::FDiv:
      return isVectorizablePureType(BO->getOperand(0)->getType());
    default:
      return false;
    }
  }
  if (auto *UO = llvm::dyn_cast<llvm::UnaryOperator>(&I))
    return UO->getOpcode() == llvm::Instruction::FNeg;
  if (auto *CI = llvm::dyn_cast<llvm::CastInst>(&I)) {
    switch (CI->getOpcode()) {
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::FPExt:
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::SExt:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::Trunc:
      return isVectorizablePureType(CI->getSrcTy());
    default:
      return false;
    }
  }
  if (auto *Cmp = llvm::dyn_cast<llvm::CmpInst>(&I))
    return isVectorizablePureType(Cmp->getOperand(0)->getType());
  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&I))
    return isVectorizablePureType(Sel->getTrueValue()->getType());
  return false;
}

/// Vectorizes counted `for` loops (step = 1) containing pure setup, a single
/// side-effect-free `while` loop, and post-while scalar integer reductions across
/// 4 lanes in lockstep (`<4 x T>` -> `2 x <2 x double>` NEON / AVX2), followed by
/// reusing the scalar loop as a 0..3 remainder tail.
static bool tryVectorizeLoopWithWhile(llvm::Loop *OuterL, llvm::LoopInfo &LI,
                                      llvm::ScalarEvolution &SE) {
  constexpr unsigned VF = 4;
  llvm::BasicBlock *OuterPre = OuterL->getLoopPreheader();
  llvm::BasicBlock *OuterHeader = OuterL->getHeader();
  llvm::BasicBlock *OuterLatch = OuterL->getLoopLatch();
  if (!OuterPre || !OuterHeader || !OuterLatch)
    return false;
  llvm::SmallVector<llvm::BasicBlock *, 4> OuterExits;
  OuterL->getExitBlocks(OuterExits);
  llvm::BasicBlock *OuterExit = nullptr;
  for (llvm::BasicBlock *Ex : OuterExits) {
    if (llvm::isa<llvm::UnreachableInst>(Ex->getTerminator()))
      continue;
    if (OuterExit && OuterExit != Ex)
      return false;
    OuterExit = Ex;
  }
  auto *LatchBr = llvm::dyn_cast<llvm::CondBrInst>(OuterLatch->getTerminator());
  if (!OuterExit || !LatchBr || OuterExit->getSinglePredecessor() != OuterLatch)
    return false;
  if (!((LatchBr->getSuccessor(0) == OuterExit &&
         LatchBr->getSuccessor(1) == OuterHeader) ||
        (LatchBr->getSuccessor(1) == OuterExit &&
         LatchBr->getSuccessor(0) == OuterHeader)))
    return false;
  if (OuterL->getSubLoops().size() != 1)
    return false;
  llvm::Loop *InnerL = OuterL->getSubLoops()[0];
  if (!InnerL->getSubLoops().empty())
    return false;

  const llvm::SCEV *OuterBTC = SE.getExitCount(OuterL, OuterLatch);
  if (llvm::isa<llvm::SCEVCouldNotCompute>(OuterBTC) ||
      !OuterBTC->getType()->isIntegerTy())
    return false;
  unsigned BTCWidth = OuterBTC->getType()->getIntegerBitWidth();
  if (BTCWidth > 64)
    return false;
  if (BTCWidth > 32) {
    auto *SC = llvm::dyn_cast<llvm::SCEVConstant>(OuterBTC);
    if (!SC || SC->getAPInt().isAllOnes())
      return false;
  }

  // Partition OuterHeader PHIs into one +1 induction variable and integer
  // accumulator reductions.
  llvm::PHINode *IV = nullptr;
  llvm::Value *IVInit = nullptr;
  llvm::SmallVector<llvm::PHINode *, 4> RedPHIs;
  llvm::SmallVector<llvm::Value *, 4> RedInits;
  llvm::SmallVector<llvm::Value *, 4> RedNexts;
  for (llvm::PHINode &PN : OuterHeader->phis()) {
    if (PN.getNumIncomingValues() != 2 || !PN.getType()->isIntegerTy())
      return false;
    unsigned BitW = PN.getType()->getIntegerBitWidth();
    if (BitW != 32 && BitW != 64)
      return false;
    llvm::Value *InitV = PN.getIncomingValueForBlock(OuterPre);
    llvm::Value *BackV = PN.getIncomingValueForBlock(OuterLatch);
    if (!InitV || !BackV)
      return false;
    bool MatchedIV = false;
    if (!IV) {
      if (const auto *AR = llvm::dyn_cast<llvm::SCEVAddRecExpr>(SE.getSCEV(&PN));
          AR && AR->getLoop() == OuterL && AR->isAffine()) {
        if (const auto *Step =
                llvm::dyn_cast<llvm::SCEVConstant>(AR->getStepRecurrence(SE));
            Step && Step->getAPInt().isOne()) {
          bool OnlyInside = true;
          for (llvm::User *U : PN.users()) {
            auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
            if (!UI || !OuterL->contains(UI)) {
              OnlyInside = false;
              break;
            }
          }
          if (OnlyInside) {
            IV = &PN;
            IVInit = InitV;
            MatchedIV = true;
          }
        }
      }
    }
    if (!MatchedIV) {
      auto *BI = llvm::dyn_cast<llvm::Instruction>(BackV);
      if (!BI || !OuterL->contains(BI))
        return false;
      RedPHIs.push_back(&PN);
      RedInits.push_back(InitV);
      RedNexts.push_back(BackV);
    }
  }
  if (!IV || RedPHIs.empty())
    return false;

  // Walk linear pure setup chain from OuterHeader to InnerL's header.
  llvm::BasicBlock *InnerHeader = InnerL->getHeader();
  llvm::BasicBlock *InnerLatch = InnerL->getLoopLatch();
  if (!InnerHeader || !InnerLatch)
    return false;
  llvm::SmallVector<llvm::BasicBlock *, 4> SetupBBs;
  llvm::SmallPtrSet<llvm::BasicBlock *, 8> OuterHandled;
  llvm::BasicBlock *Cur = OuterHeader;
  while (Cur != InnerHeader) {
    if (Cur == OuterLatch || InnerL->contains(Cur) ||
        !OuterHandled.insert(Cur).second)
      return false;
    if (Cur != OuterHeader && !Cur->phis().empty())
      return false;
    for (llvm::Instruction &I : *Cur) {
      if (llvm::isa<llvm::PHINode>(&I) || I.isTerminator())
        continue;
      if (!isVectorizablePureInst(I))
        return false;
      for (llvm::Value *Op : I.operands()) {
        auto *OI = llvm::dyn_cast<llvm::Instruction>(Op);
        if (OI && OuterL->contains(OI) &&
            !llvm::is_contained(RedPHIs, llvm::dyn_cast<llvm::PHINode>(OI))) {
          if (!OuterHandled.count(OI->getParent()))
            return false;
        } else if (llvm::is_contained(RedPHIs, llvm::dyn_cast<llvm::PHINode>(Op))) {
          return false;
        }
      }
    }
    auto *UTerm = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
    if (!UTerm)
      return false;
    SetupBBs.push_back(Cur);
    Cur = UTerm->getSuccessor();
  }
  if (SetupBBs.empty() || InnerL->getLoopPreheader() != SetupBBs.back())
    return false;

  // Walk linear inner `while` chain B_0 -> ... -> InnerLatch inside InnerL.
  struct InnerStep {
    llvm::BasicBlock *BB = nullptr;
    llvm::Value *Cond = nullptr;
    bool StayOnTrue = false;
    llvm::BasicBlock *ExitBB = nullptr;
  };
  llvm::SmallVector<InnerStep, 8> InnerSteps;
  llvm::SmallPtrSet<llvm::BasicBlock *, 8> InnerSeen;
  Cur = InnerHeader;
  while (true) {
    if (!InnerL->contains(Cur) || !InnerSeen.insert(Cur).second ||
        InnerSeen.size() > 8)
      return false;
    if (Cur == InnerHeader) {
      for (llvm::PHINode &PN : Cur->phis()) {
        if (PN.getNumIncomingValues() != 2 ||
            !isVectorizablePureType(PN.getType()))
          return false;
        if (!PN.getIncomingValueForBlock(SetupBBs.back()) ||
            !PN.getIncomingValueForBlock(InnerLatch))
          return false;
      }
    } else if (!Cur->phis().empty()) {
      return false;
    }
    for (llvm::Instruction &I : *Cur) {
      if (llvm::isa<llvm::PHINode>(&I) || I.isTerminator())
        continue;
      if (!isVectorizablePureInst(I))
        return false;
      for (llvm::Value *Op : I.operands())
        if (llvm::is_contained(RedPHIs, llvm::dyn_cast<llvm::PHINode>(Op)))
          return false;
    }
    if (auto *UTerm = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator())) {
      llvm::BasicBlock *Succ = UTerm->getSuccessor();
      InnerSteps.push_back({Cur, nullptr, false, nullptr});
      if (Succ == InnerHeader) {
        if (Cur != InnerLatch)
          return false;
        break;
      }
      Cur = Succ;
      continue;
    }
    auto *CTerm = llvm::dyn_cast<llvm::CondBrInst>(Cur->getTerminator());
    if (!CTerm)
      return false;
    llvm::BasicBlock *S0 = CTerm->getSuccessor(0), *S1 = CTerm->getSuccessor(1);
    bool In0 = InnerL->contains(S0), In1 = InnerL->contains(S1);
    if (In0 == In1)
      return false;
    llvm::BasicBlock *NextIn = In0 ? S0 : S1;
    llvm::BasicBlock *ExBB = In0 ? S1 : S0;
    if (!OuterL->contains(ExBB))
      return false;
    InnerSteps.push_back({Cur, CTerm->getCondition(), In0, ExBB});
    if (NextIn == InnerHeader) {
      if (Cur != InnerLatch)
        return false;
      break;
    }
    Cur = NextIn;
  }
  if (InnerSteps.size() != InnerL->getBlocks().size())
    return false;

  // Trace each inner exit edge through PostInner to OuterLatch first so we know
  // every ExitPath's LatchPred.
  struct ExitPath {
    unsigned StepIdx = 0;
    llvm::BasicBlock *LatchPred = nullptr;
    llvm::CallInst *TrapCall = nullptr;
  };
  llvm::SmallVector<ExitPath, 4> ExitPaths;
  llvm::SmallPtrSet<llvm::BasicBlock *, 16> PostInnerBBs;
  for (unsigned SI = 0; SI < InnerSteps.size(); ++SI) {
    llvm::BasicBlock *Ex = InnerSteps[SI].ExitBB;
    if (!Ex)
      continue;
    llvm::BasicBlock *Prev = InnerSteps[SI].BB;
    llvm::BasicBlock *Walk = Ex;
    llvm::CallInst *FoundTrap = nullptr;
    llvm::CallInst *OvCall = nullptr;
    unsigned Hops = 0;
    while (Walk != OuterLatch) {
      if (++Hops > 8 || !OuterL->contains(Walk) || InnerL->contains(Walk) ||
          Walk == OuterHeader)
        return false;
      if (!Walk->phis().empty())
        return false;
      PostInnerBBs.insert(Walk);
      for (llvm::Instruction &I : *Walk) {
        if (I.isTerminator())
          continue;
        if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(&I)) {
          if (II->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow ||
              II->getIntrinsicID() == llvm::Intrinsic::ssub_with_overflow) {
            if (OvCall)
              return false;
            OvCall = II;
            continue;
          }
          return false;
        }
        if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(&I)) {
          if (EV->getAggregateOperand() != OvCall || EV->getNumIndices() != 1 ||
              EV->getIndices()[0] > 1)
            return false;
          continue;
        }
        if (!isVectorizablePureInst(I))
          return false;
      }
      if (auto *UTerm = llvm::dyn_cast<llvm::UncondBrInst>(Walk->getTerminator())) {
        Prev = Walk;
        Walk = UTerm->getSuccessor();
        continue;
      }
      auto *CTerm = llvm::dyn_cast<llvm::CondBrInst>(Walk->getTerminator());
      if (!CTerm || !OvCall)
        return false;
      auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(CTerm->getCondition());
      if (!EV || EV->getAggregateOperand() != OvCall ||
          EV->getNumIndices() != 1 || EV->getIndices()[0] != 1)
        return false;
      llvm::BasicBlock *TrapBB = CTerm->getSuccessor(0);
      llvm::BasicBlock *OkBB = CTerm->getSuccessor(1);
      if (!llvm::isa<llvm::UnreachableInst>(TrapBB->getTerminator()))
        return false;
      if (TrapBB->size() > 3 || TrapBB->getSinglePredecessor() != Walk)
        return false;
      llvm::CallInst *TrapC = nullptr;
      for (llvm::Instruction &TI : *TrapBB) {
        if (TI.isTerminator())
          continue;
        auto *CI = llvm::dyn_cast<llvm::CallInst>(&TI);
        if (!CI || TrapC)
          return false;
        for (llvm::Value *Arg : CI->args())
          if (auto *AI = llvm::dyn_cast<llvm::Instruction>(Arg);
              AI && OuterL->contains(AI))
            return false;
        TrapC = CI;
      }
      if (!TrapC || OkBB->getSinglePredecessor() != Walk)
        return false;
      if (FoundTrap)
        return false;
      FoundTrap = TrapC;
      Prev = Walk;
      Walk = OkBB;
    }
    ExitPaths.push_back({SI, Prev, FoundTrap});
  }
  if (ExitPaths.empty())
    return false;

  // Validate OuterLatch instructions and identify any non-reduction live-out
  // PHIs (e.g. from LLVM merging exit edges in -fno-overflow-checks mode).
  for (llvm::Instruction &I : *OuterLatch) {
    if (llvm::isa<llvm::PHINode>(&I) || I.isTerminator())
      continue;
    if (!isVectorizablePureInst(I))
      return false;
  }

  llvm::SmallVector<llvm::PHINode *, 4> LatchLiveOutPHIs;
  llvm::SmallDenseMap<unsigned, unsigned, 8> StepToExitPath;
  for (size_t EI = 0; EI < ExitPaths.size(); ++EI)
    StepToExitPath[ExitPaths[EI].StepIdx] = EI;
  for (llvm::PHINode &PN : OuterLatch->phis()) {
    if (llvm::is_contained(RedNexts, static_cast<llvm::Value *>(&PN)))
      continue;
    if (!isVectorizablePureType(PN.getType()) ||
        PN.getNumIncomingValues() != ExitPaths.size())
      return false;
    for (const ExitPath &EP : ExitPaths) {
      llvm::Value *InV = PN.getIncomingValueForBlock(EP.LatchPred);
      if (!InV)
        return false;
      if (auto *AI = llvm::dyn_cast<llvm::Instruction>(InV))
        if (OuterL->contains(AI) && !OuterHandled.count(AI->getParent()) &&
            !InnerL->contains(AI))
          return false;
    }
    for (llvm::User *U : PN.users()) {
      auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
      if (!UI || UI->getParent() != OuterLatch ||
          llvm::isa<llvm::PHINode>(UI))
        return false;
    }
    LatchLiveOutPHIs.push_back(&PN);
  }

  // Inner values may only be used inside InnerL or via LatchLiveOutPHIs.
  for (const InnerStep &St : InnerSteps) {
    for (llvm::Instruction &I : *St.BB) {
      for (llvm::User *U : I.users()) {
        auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
        if (!UI)
          return false;
        if (InnerL->contains(UI))
          continue;
        auto *PN = llvm::dyn_cast<llvm::PHINode>(UI);
        if (!PN || !llvm::is_contained(LatchLiveOutPHIs, PN))
          return false;
      }
    }
  }

  struct RedExitDelta {
    bool IsIdentity = true;
    int64_t Delta = 0; // signed constant delta added to Red
    bool HasOv = false;
    llvm::CallInst *TrapCall = nullptr;
  };
  struct RedSpec {
    llvm::PHINode *PHI = nullptr;
    llvm::Value *Init = nullptr;
    llvm::Value *BackVal = nullptr;
    llvm::Value *DirectLatchDelta = nullptr;
    bool DirectIsSub = false;
    llvm::SmallVector<RedExitDelta, 4> PerExit;
    bool AnyOv = false;
  };
  auto isValidLatchDeltaExpr = [&](auto &Self, llvm::Value *V,
                                   llvm::PHINode *SkipRed) -> bool {
    if (OuterL->isLoopInvariant(V))
      return true;
    if (auto *PN = llvm::dyn_cast<llvm::PHINode>(V))
      return llvm::is_contained(LatchLiveOutPHIs, PN) || PN == IV;
    auto *I = llvm::dyn_cast<llvm::Instruction>(V);
    if (!I || V == SkipRed)
      return false;
    if (OuterHandled.count(I->getParent()))
      return true;
    if (I->getParent() != OuterLatch || !isVectorizablePureInst(*I))
      return false;
    for (llvm::Value *Op : I->operands())
      if (!Self(Self, Op, SkipRed))
        return false;
    return true;
  };
  llvm::SmallVector<RedSpec, 4> Specs;
  for (size_t RI = 0; RI < RedPHIs.size(); ++RI) {
    RedSpec Spec;
    Spec.PHI = RedPHIs[RI];
    Spec.Init = RedInits[RI];
    Spec.BackVal = RedNexts[RI];
    auto *BackPHI = llvm::dyn_cast<llvm::PHINode>(Spec.BackVal);
    if (!BackPHI) {
      auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Spec.BackVal);
      if (!BO || BO->getParent() != OuterLatch)
        return false;
      bool IsSub = BO->getOpcode() == llvm::Instruction::Sub;
      if (!IsSub && BO->getOpcode() != llvm::Instruction::Add)
        return false;
      llvm::Value *DeltaV = nullptr;
      if (BO->getOperand(0) == Spec.PHI)
        DeltaV = BO->getOperand(1);
      else if (!IsSub && BO->getOperand(1) == Spec.PHI)
        DeltaV = BO->getOperand(0);
      else
        return false;
      if (!isValidLatchDeltaExpr(isValidLatchDeltaExpr, DeltaV, Spec.PHI))
        return false;
      Spec.DirectLatchDelta = DeltaV;
      Spec.DirectIsSub = IsSub;
      Specs.push_back(std::move(Spec));
      continue;
    }
    if (BackPHI->getParent() != OuterLatch)
      return false;
    int Sign = 0; // +1 non-neg, -1 non-pos
    for (const ExitPath &EP : ExitPaths) {
      llvm::Value *V = BackPHI->getIncomingValueForBlock(EP.LatchPred);
      RedExitDelta D;
      if (V == Spec.PHI) {
        D.IsIdentity = true;
        D.Delta = 0;
      } else {
        bool IsSub = false;
        bool HasOv = false;
        llvm::Value *Other = nullptr;
        if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(V)) {
          auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(EV->getAggregateOperand());
          if (!II || EV->getNumIndices() != 1 || EV->getIndices()[0] != 0 ||
              !EP.TrapCall)
            return false;
          if (II->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow)
            IsSub = false;
          else if (II->getIntrinsicID() == llvm::Intrinsic::ssub_with_overflow)
            IsSub = true;
          else
            return false;
          HasOv = true;
          if (II->getArgOperand(0) == Spec.PHI)
            Other = II->getArgOperand(1);
          else if (!IsSub && II->getArgOperand(1) == Spec.PHI)
            Other = II->getArgOperand(0);
          else
            return false;
        } else if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
          if (BO->getOpcode() == llvm::Instruction::Add)
            IsSub = false;
          else if (BO->getOpcode() == llvm::Instruction::Sub)
            IsSub = true;
          else
            return false;
          if (BO->getOperand(0) == Spec.PHI)
            Other = BO->getOperand(1);
          else if (!IsSub && BO->getOperand(1) == Spec.PHI)
            Other = BO->getOperand(0);
          else
            return false;
        } else {
          return false;
        }
        auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Other);
        if (!CI || CI->getBitWidth() > 64)
          return false;
        int64_t CVal = CI->getSExtValue();
        if (CVal == INT64_MIN)
          return false;
        int64_t SignedD = IsSub ? -CVal : CVal;
        D.IsIdentity = SignedD == 0;
        D.Delta = SignedD;
        D.HasOv = HasOv;
        D.TrapCall = EP.TrapCall;
        if (HasOv)
          Spec.AnyOv = true;
        if (SignedD > 0) {
          if (Sign < 0)
            return false;
          Sign = 1;
        } else if (SignedD < 0) {
          if (Sign > 0)
            return false;
          Sign = -1;
        }
      }
      Spec.PerExit.push_back(D);
    }
    Specs.push_back(std::move(Spec));
  }

  // Only RedNexts may be used outside OuterL.
  for (llvm::BasicBlock *BB : OuterL->getBlocks()) {
    for (llvm::Instruction &I : *BB) {
      if (llvm::is_contained(RedNexts, static_cast<llvm::Value *>(&I)))
        continue;
      for (llvm::User *U : I.users()) {
        auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
        if (!UI || !OuterL->contains(UI))
          return false;
      }
    }
  }

  // Build the 4-wide lockstep loop!
  llvm::Function *F = OuterHeader->getParent();
  llvm::LLVMContext &Ctx = F->getContext();
  llvm::Type *I1 = llvm::Type::getInt1Ty(Ctx);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Ctx);
  auto *VecI1Ty = llvm::FixedVectorType::get(I1, VF);
  auto VecTy = [&](llvm::Type *T) {
    return llvm::FixedVectorType::get(T, VF);
  };

  auto *GuardBB = llvm::BasicBlock::Create(Ctx, "wvec.guard", F, OuterHeader);
  auto *VecHeaderBB = llvm::BasicBlock::Create(Ctx, "wvec.hdr", F, OuterHeader);
  auto *VecInnerBB = llvm::BasicBlock::Create(Ctx, "wvec.inner", F, OuterHeader);
  auto *VecPostBB = llvm::BasicBlock::Create(Ctx, "wvec.post", F, OuterHeader);
  auto *VecLatchBB = llvm::BasicBlock::Create(Ctx, "wvec.latch", F, OuterHeader);
  auto *TailCheckBB =
      llvm::BasicBlock::Create(Ctx, "wvec.tailchk", F, OuterHeader);
  auto *AfterLoopBB =
      llvm::BasicBlock::Create(Ctx, "wvec.after", F, OuterExit);

  // GuardBB: compute TC64, VecIters = TC64 / 4, VecDoneElems = TC64 & ~3.
  OuterPre->getTerminator()->replaceSuccessorWith(OuterHeader, GuardBB);
  llvm::IRBuilder<> B(GuardBB);
  B.CreateBr(VecHeaderBB);
  const llvm::SCEV *BTC64 = SE.getZeroExtendExpr(OuterBTC, I64);
  const llvm::SCEV *TC64Scev = SE.getAddExpr(BTC64, SE.getOne(I64));
  llvm::SCEVExpander Exp(SE, "wvec.tc");
  llvm::Value *TCVal =
      Exp.expandCodeFor(TC64Scev, I64, GuardBB->getTerminator());
  GuardBB->getTerminator()->eraseFromParent();
  B.SetInsertPoint(GuardBB);
  llvm::Value *VecIters = B.CreateLShr(TCVal, 2, "wvec.iters");
  llvm::Value *VecDoneElems = B.CreateAnd(
      TCVal, llvm::ConstantInt::get(I64, ~uint64_t(3)), "wvec.doneelems");
  llvm::Value *HasVec =
      B.CreateICmpUGT(TCVal, llvm::ConstantInt::get(I64, 3), "wvec.hasvec");
  B.CreateCondBr(HasVec, VecHeaderBB, TailCheckBB);

  // VecHeaderBB: setup IV_vec, reduction PHIs, and cloned SetupBBs.
  B.SetInsertPoint(VecHeaderBB);
  llvm::PHINode *VecK = B.CreatePHI(I64, 2, "wvec.k");
  VecK->addIncoming(llvm::ConstantInt::get(I64, 0), GuardBB);
  llvm::SmallVector<llvm::PHINode *, 4> VecRedPHIs;
  for (const RedSpec &Sp : Specs) {
    llvm::PHINode *RP =
        B.CreatePHI(Sp.PHI->getType(), 2, Sp.PHI->getName() + ".wvec");
    RP->addIncoming(Sp.Init, GuardBB);
    VecRedPHIs.push_back(RP);
  }

  llvm::DenseMap<llvm::Value *, llvm::Value *> VecMap;
  llvm::DenseMap<llvm::Value *, llvm::Value *> SplatCache;
  auto GetVec = [&](llvm::Value *Scalar) -> llvm::Value * {
    if (auto *V = VecMap.lookup(Scalar))
      return V;
    if (auto *V = SplatCache.lookup(Scalar))
      return V;
    llvm::Value *Sp = B.CreateVectorSplat(VF, Scalar, "wvec.splat");
    SplatCache[Scalar] = Sp;
    return Sp;
  };
  auto EmitVecInst = [&](llvm::Instruction &I, bool StripPoison) -> llvm::Value * {
    if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      llvm::Value *V = B.CreateBinOp(BO->getOpcode(), GetVec(BO->getOperand(0)),
                                     GetVec(BO->getOperand(1)), I.getName());
      if (auto *VI = llvm::dyn_cast<llvm::Instruction>(V)) {
        if (!StripPoison)
          VI->copyIRFlags(BO);
      }
      return V;
    }
    if (auto *UO = llvm::dyn_cast<llvm::UnaryOperator>(&I)) {
      llvm::Value *V = B.CreateUnOp(UO->getOpcode(), GetVec(UO->getOperand(0)),
                                    I.getName());
      if (auto *VI = llvm::dyn_cast<llvm::Instruction>(V); VI && !StripPoison)
        VI->copyIRFlags(UO);
      return V;
    }
    if (auto *CI = llvm::dyn_cast<llvm::CastInst>(&I)) {
      llvm::Value *V = B.CreateCast(CI->getOpcode(), GetVec(CI->getOperand(0)),
                                    VecTy(CI->getType()), I.getName());
      if (auto *VI = llvm::dyn_cast<llvm::Instruction>(V); VI && !StripPoison)
        VI->copyIRFlags(CI);
      return V;
    }
    if (auto *FC = llvm::dyn_cast<llvm::FCmpInst>(&I))
      return B.CreateFCmp(FC->getPredicate(), GetVec(FC->getOperand(0)),
                          GetVec(FC->getOperand(1)), I.getName());
    if (auto *IC = llvm::dyn_cast<llvm::ICmpInst>(&I))
      return B.CreateICmp(IC->getPredicate(), GetVec(IC->getOperand(0)),
                          GetVec(IC->getOperand(1)), I.getName());
    if (auto *SI = llvm::dyn_cast<llvm::SelectInst>(&I))
      return B.CreateSelect(GetVec(SI->getCondition()),
                            GetVec(SI->getTrueValue()),
                            GetVec(SI->getFalseValue()), I.getName());
    llvm_unreachable("unexpected instruction");
  };

  llvm::Value *KShift = B.CreateShl(VecK, 2, "wvec.k4");
  llvm::Value *KOff = B.CreateZExtOrTrunc(KShift, IV->getType(), "wvec.ivoff");
  llvm::Value *BaseIV = B.CreateAdd(IVInit, KOff, "wvec.ivbase");
  llvm::Value *SplatBaseIV = B.CreateVectorSplat(VF, BaseIV, "wvec.ivsplat");
  llvm::SmallVector<llvm::Constant *, 4> LaneIdxs;
  for (unsigned J = 0; J < VF; ++J)
    LaneIdxs.push_back(llvm::ConstantInt::get(IV->getType(), J));
  llvm::Value *VecIV = B.CreateAdd(
      SplatBaseIV, llvm::ConstantVector::get(LaneIdxs), "wvec.iv");
  VecMap[IV] = VecIV;

  for (llvm::BasicBlock *SBB : SetupBBs)
    for (llvm::Instruction &I : *SBB)
      if (!llvm::isa<llvm::PHINode>(&I) && !I.isTerminator())
        VecMap[&I] = EmitVecInst(I, /*StripPoison=*/false);
  B.CreateBr(VecInnerBB);

  // VecInnerBB: lockstep simulation across InnerSteps.
  B.SetInsertPoint(VecInnerBB);
  llvm::PHINode *ActivePHI = B.CreatePHI(VecI1Ty, 2, "wvec.act");
  ActivePHI->addIncoming(llvm::ConstantVector::getAllOnesValue(VecI1Ty),
                         VecHeaderBB);
  llvm::SmallVector<llvm::PHINode *, 4> TookExitPHIs(InnerSteps.size(), nullptr);
  for (unsigned SI = 0; SI < InnerSteps.size(); ++SI) {
    if (!InnerSteps[SI].ExitBB)
      continue;
    TookExitPHIs[SI] = B.CreatePHI(VecI1Ty, 2, "wvec.took");
    TookExitPHIs[SI]->addIncoming(llvm::Constant::getNullValue(VecI1Ty),
                                  VecHeaderBB);
  }
  llvm::SmallVector<std::pair<llvm::PHINode *, llvm::PHINode *>, 4> InnerPHIMap;
  for (llvm::PHINode &PN : InnerHeader->phis()) {
    llvm::PHINode *VP =
        B.CreatePHI(VecTy(PN.getType()), 2, PN.getName() + ".wvec");
    llvm::Value *InitScalar = PN.getIncomingValueForBlock(SetupBBs.back());
    llvm::Value *InitV = VecMap.lookup(InitScalar);
    if (!InitV)
      InitV = llvm::IRBuilder<>(VecHeaderBB->getTerminator())
                  .CreateVectorSplat(VF, InitScalar, "wvec.pin");
    VP->addIncoming(InitV, VecHeaderBB);
    VecMap[&PN] = VP;
    InnerPHIMap.push_back({&PN, VP});
  }

  llvm::SmallVector<llvm::PHINode *, 4> LiveOutCapPHIs;
  llvm::SmallVector<llvm::Value *, 4> CurCaps;
  for (llvm::PHINode *LO : LatchLiveOutPHIs) {
    llvm::Type *VT = VecTy(LO->getType());
    llvm::PHINode *CP = B.CreatePHI(VT, 2, LO->getName() + ".cap");
    CP->addIncoming(llvm::Constant::getNullValue(VT), VecHeaderBB);
    LiveOutCapPHIs.push_back(CP);
    CurCaps.push_back(CP);
  }

  llvm::Value *CurActive = ActivePHI;
  llvm::SmallVector<llvm::Value *, 4> TookExitNexts(InnerSteps.size(), nullptr);
  SplatCache.clear();
  for (unsigned SI = 0; SI < InnerSteps.size(); ++SI) {
    const InnerStep &St = InnerSteps[SI];
    for (llvm::Instruction &I : *St.BB)
      if (!llvm::isa<llvm::PHINode>(&I) && !I.isTerminator())
        VecMap[&I] = EmitVecInst(I, /*StripPoison=*/true);
    if (St.ExitBB) {
      llvm::Value *CondVec = GetVec(St.Cond);
      llvm::Value *StayVec =
          St.StayOnTrue ? CondVec : B.CreateNot(CondVec, "wvec.stay");
      llvm::Value *LeaveVec =
          St.StayOnTrue ? B.CreateNot(CondVec, "wvec.leave") : CondVec;
      llvm::Value *ExitedHere = B.CreateAnd(CurActive, LeaveVec, "wvec.ex");
      llvm::Value *TookNext =
          B.CreateOr(TookExitPHIs[SI], ExitedHere, "wvec.tookn");
      TookExitPHIs[SI]->addIncoming(TookNext, VecInnerBB);
      TookExitNexts[SI] = TookNext;
      if (!LatchLiveOutPHIs.empty()) {
        unsigned EI = StepToExitPath[SI];
        llvm::BasicBlock *LP = ExitPaths[EI].LatchPred;
        for (size_t LI = 0; LI < LatchLiveOutPHIs.size(); ++LI) {
          llvm::Value *InV = LatchLiveOutPHIs[LI]->getIncomingValueForBlock(LP);
          CurCaps[LI] = B.CreateSelect(ExitedHere, GetVec(InV), CurCaps[LI],
                                       "wvec.capn");
        }
      }
      CurActive = B.CreateAnd(CurActive, StayVec, "wvec.actn");
    }
  }
  ActivePHI->addIncoming(CurActive, VecInnerBB);
  for (size_t LI = 0; LI < LatchLiveOutPHIs.size(); ++LI)
    LiveOutCapPHIs[LI]->addIncoming(CurCaps[LI], VecInnerBB);
  for (auto &[OrigP, VP] : InnerPHIMap) {
    llvm::Value *BackScalar = OrigP->getIncomingValueForBlock(InnerLatch);
    VP->addIncoming(GetVec(BackScalar), VecInnerBB);
  }
  llvm::Value *AnyActive = B.CreateOrReduce(CurActive);
  B.CreateCondBr(AnyActive, VecInnerBB, VecPostBB);

  // VecPostBB -> VecLatchBB: reduce exit deltas per outer accumulator.
  B.SetInsertPoint(VecPostBB);
  llvm::BasicBlock *CurPostBB = VecPostBB;
  for (size_t LI = 0; LI < LatchLiveOutPHIs.size(); ++LI)
    VecMap[LatchLiveOutPHIs[LI]] = CurCaps[LI];
  auto BuildLatchVec = [&](auto &Self, llvm::Value *V) -> llvm::Value * {
    if (auto *Existing = VecMap.lookup(V))
      return Existing;
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(V);
        I && I->getParent() == OuterLatch) {
      for (llvm::Value *Op : I->operands())
        Self(Self, Op);
      llvm::Value *Res = EmitVecInst(*I, /*StripPoison=*/false);
      VecMap[V] = Res;
      return Res;
    }
    return GetVec(V);
  };
  llvm::SmallVector<llvm::Value *, 4> VecRedNexts;
  llvm::SmallDenseMap<llvm::CallInst *, llvm::BasicBlock *, 2> TrapMap;

  for (size_t RI = 0; RI < Specs.size(); ++RI) {
    const RedSpec &Sp = Specs[RI];
    llvm::Value *RedCur = VecRedPHIs[RI];
    llvm::Value *RedNext = nullptr;
    if (Sp.DirectLatchDelta) {
      llvm::Value *VecD = BuildLatchVec(BuildLatchVec, Sp.DirectLatchDelta);
      if (Sp.DirectIsSub)
        VecD = B.CreateNeg(VecD, "wvec.negd");
      llvm::Value *BatchDelta = B.CreateAddReduce(VecD);
      RedNext = B.CreateAdd(RedCur, BatchDelta, "wvec.redn");
      VecRedNexts.push_back(RedNext);
      continue;
    }
    llvm::Type *RTy = Sp.PHI->getType();
    llvm::Type *VecRTy = VecTy(RTy);
    llvm::Value *LaneDelta = llvm::Constant::getNullValue(VecRTy);
    llvm::CallInst *FirstOvTrap = nullptr;
    for (size_t EI = 0; EI < ExitPaths.size(); ++EI) {
      const RedExitDelta &D = Sp.PerExit[EI];
      if (D.IsIdentity)
        continue;
      if (D.HasOv && !FirstOvTrap)
        FirstOvTrap = D.TrapCall;
      llvm::Value *Mask = TookExitNexts[ExitPaths[EI].StepIdx];
      llvm::Value *DVec =
          B.CreateVectorSplat(VF, llvm::ConstantInt::get(RTy, D.Delta, true));
      LaneDelta = B.CreateSelect(Mask, DVec, LaneDelta, "wvec.ld");
    }
    llvm::Value *BatchDelta = B.CreateAddReduce(LaneDelta);
    if (Sp.AnyOv && FirstOvTrap) {
      llvm::BasicBlock *TrapBB = TrapMap.lookup(FirstOvTrap);
      if (!TrapBB) {
        TrapBB = llvm::BasicBlock::Create(Ctx, "wvec.trap", F, VecLatchBB);
        llvm::IRBuilder<> TB(TrapBB);
        TB.Insert(FirstOvTrap->clone());
        TB.CreateUnreachable();
        TrapMap[FirstOvTrap] = TrapBB;
      }
      llvm::BasicBlock *OkBB =
          llvm::BasicBlock::Create(Ctx, "wvec.ok", F, VecLatchBB);
      llvm::Value *Ov = B.CreateBinaryIntrinsic(
          llvm::Intrinsic::sadd_with_overflow, RedCur, BatchDelta, nullptr,
          "wvec.ov");
      llvm::Value *OvFlag = B.CreateExtractValue(Ov, 1, "wvec.ovf");
      RedNext = B.CreateExtractValue(Ov, 0, "wvec.redn");
      B.CreateCondBr(OvFlag, TrapBB, OkBB);
      CurPostBB = OkBB;
      B.SetInsertPoint(CurPostBB);
    } else {
      RedNext = B.CreateAdd(RedCur, BatchDelta, "wvec.redn");
    }
    VecRedNexts.push_back(RedNext);
  }
  B.CreateBr(VecLatchBB);

  B.SetInsertPoint(VecLatchBB);
  llvm::Value *VecKNext =
      B.CreateAdd(VecK, llvm::ConstantInt::get(I64, 1), "wvec.kn");
  VecK->addIncoming(VecKNext, VecLatchBB);
  for (size_t RI = 0; RI < Specs.size(); ++RI)
    VecRedPHIs[RI]->addIncoming(VecRedNexts[RI], VecLatchBB);
  llvm::Value *VecDone = B.CreateICmpEQ(VecKNext, VecIters, "wvec.done");
  B.CreateCondBr(VecDone, TailCheckBB, VecHeaderBB);

  // TailCheckBB -> OuterHeader (remainder 1..3) or AfterLoopBB (remainder 0).
  B.SetInsertPoint(TailCheckBB);
  llvm::Value *IVTailOff =
      B.CreateZExtOrTrunc(VecDoneElems, IV->getType(), "wvec.tailoff");
  llvm::Value *IVTailStart = B.CreateAdd(IVInit, IVTailOff, "wvec.tailiv");
  llvm::SmallVector<llvm::PHINode *, 4> RedTailStarts;
  for (size_t RI = 0; RI < Specs.size(); ++RI) {
    llvm::PHINode *RTS = B.CreatePHI(Specs[RI].PHI->getType(), 2,
                                     Specs[RI].PHI->getName() + ".tail");
    RTS->addIncoming(Specs[RI].Init, GuardBB);
    RTS->addIncoming(VecRedNexts[RI], VecLatchBB);
    RedTailStarts.push_back(RTS);
  }
  llvm::Value *HasTail = B.CreateICmpNE(VecDoneElems, TCVal, "wvec.hastail");
  B.CreateCondBr(HasTail, OuterHeader, AfterLoopBB);

  // Rewire OuterHeader preheader incoming edge -> TailCheckBB.
  IV->replaceIncomingBlockWith(OuterPre, TailCheckBB);
  IV->setIncomingValueForBlock(TailCheckBB, IVTailStart);
  for (size_t RI = 0; RI < Specs.size(); ++RI) {
    Specs[RI].PHI->replaceIncomingBlockWith(OuterPre, TailCheckBB);
    Specs[RI].PHI->setIncomingValueForBlock(TailCheckBB, RedTailStarts[RI]);
  }

  // Rewire OuterLatch -> AfterLoopBB -> OuterExit and merge FinalRed.
  OuterLatch->getTerminator()->replaceSuccessorWith(OuterExit, AfterLoopBB);
  B.SetInsertPoint(AfterLoopBB);
  llvm::SmallVector<llvm::PHINode *, 4> FinalReds;
  for (size_t RI = 0; RI < Specs.size(); ++RI) {
    llvm::PHINode *FR = B.CreatePHI(Specs[RI].PHI->getType(), 2,
                                    Specs[RI].PHI->getName() + ".final");
    FR->addIncoming(RedTailStarts[RI], TailCheckBB);
    FR->addIncoming(Specs[RI].BackVal, OuterLatch);
    FinalReds.push_back(FR);
  }
  B.CreateBr(OuterExit);

  for (llvm::PHINode &PN : OuterExit->phis())
    PN.replaceIncomingBlockWith(OuterLatch, AfterLoopBB);

  for (size_t RI = 0; RI < Specs.size(); ++RI) {
    llvm::Value *OldExt = Specs[RI].BackVal;
    llvm::PHINode *NewExt = FinalReds[RI];
    llvm::SmallVector<llvm::Use *, 4> ExtUses;
    for (llvm::Use &U : OldExt->uses()) {
      auto *UI = llvm::dyn_cast<llvm::Instruction>(U.getUser());
      if (UI && !OuterL->contains(UI) && UI != NewExt)
        ExtUses.push_back(&U);
    }
    for (llvm::Use *U : ExtUses)
      U->set(NewExt);
  }
  return true;
}

static bool vectorizeCountedLoopsWithWhile(llvm::Module &M,
                                           llvm::TargetMachine &TM) {
  bool ChangedAny = false;
  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    llvm::SmallPtrSet<llvm::BasicBlock *, 8> VectorizedHeaders;
    bool ChangedFn;
    do {
      ChangedFn = false;
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
        llvm::BasicBlock *Hdr = L->getHeader();
        if (VectorizedHeaders.count(Hdr))
          continue;
        if (tryVectorizeLoopWithWhile(L, LI, SE)) {
          VectorizedHeaders.insert(Hdr);
          ChangedFn = true;
          ChangedAny = true;
          break;
        }
      }
    } while (ChangedFn);
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
    if (vectorizeCountedLoopsWithWhile(M, TM))
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
