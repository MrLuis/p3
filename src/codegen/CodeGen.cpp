#include "pcc/CodeGen/CodeGen.h"
#include "pcc/AST/ASTVisitor.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

using namespace pcc;

namespace {

/// Declarations of the libpascalrt entry points (runtime/include/pascalrt.h).
struct Runtime {
  llvm::FunctionCallee Init, Fini, WriteStr, WriteChar, WriteInt, Writeln;

  explicit Runtime(llvm::Module &M) {
    llvm::LLVMContext &C = M.getContext();
    auto *Void = llvm::Type::getVoidTy(C);
    auto *I8 = llvm::Type::getInt8Ty(C);
    auto *I32 = llvm::Type::getInt32Ty(C);
    auto *I64 = llvm::Type::getInt64Ty(C);
    auto *Ptr = llvm::PointerType::getUnqual(C);

    auto Decl = [&](llvm::StringRef Name, llvm::Type *Ret,
                    llvm::ArrayRef<llvm::Type *> Params) {
      llvm::FunctionCallee F = M.getOrInsertFunction(
          Name, llvm::FunctionType::get(Ret, Params, /*isVarArg=*/false));
      cast<llvm::Function>(F.getCallee())
          ->addFnAttr(llvm::Attribute::NoUnwind);
      return F;
    };
    Init = Decl("__pas_init", Void, {I32, Ptr});
    Fini = Decl("__pas_fini", Void, {});
    WriteStr = Decl("__pas_write_str", Void, {Ptr, I64, I32});
    WriteChar = Decl("__pas_write_char", Void, {I8, I32});
    WriteInt = Decl("__pas_write_int", Void, {I32, I32});
    Writeln = Decl("__pas_writeln", Void, {});
  }
};

class CodeGenModule : public StmtVisitor<CodeGenModule> {
  llvm::Module &M;
  llvm::IRBuilder<> B;
  Runtime RT;

  llvm::ConstantInt *i32(int64_t V) { return B.getInt32(static_cast<uint32_t>(V)); }

  /// Emits one write argument. Sema guarantees it is a literal for now; M4
  /// dispatches on the expression's type instead.
  void emitWriteArg(Expr *Arg) {
    llvm::Value *NoWidth = i32(0);
    if (auto *IL = llvm::dyn_cast<IntegerLiteralExpr>(Arg)) {
      B.CreateCall(RT.WriteInt, {i32(static_cast<int64_t>(IL->getValue())), NoWidth});
      return;
    }
    auto *SL = llvm::cast<StringLiteralExpr>(Arg);
    llvm::StringRef S = SL->getValue();
    if (SL->isCharLiteral()) {
      B.CreateCall(RT.WriteChar, {B.getInt8(static_cast<uint8_t>(S[0])), NoWidth});
      return;
    }
    llvm::Constant *Str = B.CreateGlobalString(S, ".str");
    B.CreateCall(RT.WriteStr, {Str, B.getInt64(S.size()), NoWidth});
  }

public:
  explicit CodeGenModule(llvm::Module &M)
      : M(M), B(M.getContext()), RT(M) {}

  void emitProgram(ProgramDecl &P) {
    llvm::LLVMContext &C = M.getContext();
    auto *MainTy = llvm::FunctionType::get(
        B.getInt32Ty(), {B.getInt32Ty(), llvm::PointerType::getUnqual(C)},
        false);
    auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                        "main", M);
    Main->getArg(0)->setName("argc");
    Main->getArg(1)->setName("argv");

    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Main));
    B.CreateCall(RT.Init, {Main->getArg(0), Main->getArg(1)});
    visitStmt(P.getBlock()->getBody());
    B.CreateCall(RT.Fini);
    B.CreateRet(i32(0));
  }

  void visitEmptyStmt(EmptyStmt *) {}

  void visitCompoundStmt(CompoundStmt *S) {
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    // Sema allows only write / writeln for now.
    for (Expr *Arg : S->getArgs())
      emitWriteArg(Arg);
    if (S->getCallee().Name == "writeln")
      B.CreateCall(RT.Writeln);
  }
};

} // namespace

std::unique_ptr<llvm::Module>
pcc::generateModule(ProgramDecl &P, llvm::LLVMContext &Ctx,
                    llvm::TargetMachine &TM, llvm::StringRef SourceFileName) {
  auto M = std::make_unique<llvm::Module>(SourceFileName, Ctx);
  M->setSourceFileName(SourceFileName);
  M->setTargetTriple(TM.getTargetTriple().str());
  M->setDataLayout(TM.createDataLayout());
  CodeGenModule(*M).emitProgram(P);
  return M;
}
