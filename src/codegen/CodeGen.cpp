//===- CodeGen.cpp - AST -> LLVM IR ---------------------------------------===//
//
// Lowering (PLAN.md section 6):
//   integer -> i32, char -> i8, boolean -> i1 in registers / i8 in memory
//   program variables -> internal zero-initialized globals
//   and/or -> short-circuit (ISO leaves the evaluation order open)
//   div/mod -> runtime error on division by zero / overflow; ISO mode uses
//              ISO 7185 6.7.2.2 `mod` (divisor > 0, result >= 0)
//
//===----------------------------------------------------------------------===//

#include "pcc/CodeGen/CodeGen.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/AST/Type.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"

using namespace pcc;

namespace {

/// Declarations of the libpascalrt entry points (runtime/include/pascalrt.h).
struct Runtime {
  llvm::FunctionCallee Init, Fini, WriteStr, WriteChar, WriteInt, WriteBool,
      Writeln, ReadInt, ReadChar, Readln, RuntimeError;

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
      llvm::cast<llvm::Function>(F.getCallee())
          ->addFnAttr(llvm::Attribute::NoUnwind);
      return F;
    };
    Init = Decl("__pas_init", Void, {I32, Ptr});
    Fini = Decl("__pas_fini", Void, {});
    WriteStr = Decl("__pas_write_str", Void, {Ptr, I64, I32});
    WriteChar = Decl("__pas_write_char", Void, {I8, I32});
    WriteInt = Decl("__pas_write_int", Void, {I32, I32});
    WriteBool = Decl("__pas_write_bool", Void, {I32, I32});
    Writeln = Decl("__pas_writeln", Void, {});
    ReadInt = Decl("__pas_read_int", I32, {});
    ReadChar = Decl("__pas_read_char", I8, {});
    Readln = Decl("__pas_readln", Void, {});
    RuntimeError = Decl("__pas_runtime_error", Void, {Ptr});
    auto *RE = llvm::cast<llvm::Function>(RuntimeError.getCallee());
    RE->addFnAttr(llvm::Attribute::NoReturn);
    RE->addFnAttr(llvm::Attribute::Cold);
  }
};

class CodeGenModule : public StmtVisitor<CodeGenModule>,
                      public ExprVisitor<CodeGenModule, llvm::Value *> {
  llvm::Module &M;
  llvm::IRBuilder<> B;
  Runtime RT;
  const SourceManager &SM;
  const LangOptions &LangOpts;
  llvm::Function *CurFn = nullptr;
  llvm::DenseMap<VarDecl *, llvm::GlobalVariable *> Globals;

  //===--------------------------------------------------------------------===//
  // Types and storage
  //===--------------------------------------------------------------------===//

  /// In-memory representation of a Pascal type.
  llvm::Type *memType(Type *T) {
    switch (T->getKind()) {
    case Type::TK_Integer: return B.getInt32Ty();
    case Type::TK_Boolean: return B.getInt8Ty();
    case Type::TK_Char:    return B.getInt8Ty();
    default: llvm_unreachable("type has no storage representation yet");
    }
  }

  llvm::Value *emitLoad(VarDecl *V) {
    Type *T = V->getType();
    llvm::Value *Raw = B.CreateLoad(memType(T), Globals.lookup(V),
                                    V->getName().Name);
    return T->isBoolean() ? B.CreateTrunc(Raw, B.getInt1Ty()) : Raw;
  }

  void emitStore(VarDecl *V, llvm::Value *Val) {
    if (V->getType()->isBoolean())
      Val = B.CreateZExt(Val, B.getInt8Ty());
    B.CreateStore(Val, Globals.lookup(V));
  }

  void emitGlobals(Block &Blk) {
    for (VarDecl *V : Blk.getVars()) {
      llvm::Type *Ty = memType(V->getType());
      // ISO leaves variables undefined; zero-initializing makes programs
      // deterministic at no cost for globals.
      auto *G = new llvm::GlobalVariable(
          M, Ty, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
          llvm::Constant::getNullValue(Ty), V->getName().Name);
      Globals[V] = G;
    }
  }

  //===--------------------------------------------------------------------===//
  // Runtime checks
  //===--------------------------------------------------------------------===//

  std::string where(SourceLocation L) {
    PresumedLoc P = SM.getPresumedLoc(L);
    if (!P.isValid())
      return "";
    return (" at " + P.Filename + ":" + llvm::Twine(P.Line) + ":" +
            llvm::Twine(P.Column))
        .str();
  }

  /// if (Cond) __pas_runtime_error("<Msg> at file:line:col");
  void emitRuntimeCheck(llvm::Value *Cond, llvm::StringRef Msg,
                        SourceLocation L) {
    llvm::LLVMContext &C = M.getContext();
    auto *ErrBB = llvm::BasicBlock::Create(C, "rt.error", CurFn);
    auto *ContBB = llvm::BasicBlock::Create(C, "rt.ok", CurFn);
    llvm::MDBuilder MDB(C);
    B.CreateCondBr(Cond, ErrBB, ContBB, MDB.createUnlikelyBranchWeights());

    B.SetInsertPoint(ErrBB);
    llvm::Constant *Text =
        B.CreateGlobalString((Msg + where(L)).str(), ".rterr");
    B.CreateCall(RT.RuntimeError, {Text});
    B.CreateUnreachable();

    B.SetInsertPoint(ContBB);
  }

  //===--------------------------------------------------------------------===//
  // Operators
  //===--------------------------------------------------------------------===//

  llvm::Value *emitShortCircuit(BinaryExpr *E) {
    bool IsAnd = E->getOp() == BinaryOp::And;
    llvm::LLVMContext &C = M.getContext();
    llvm::Value *L = visitExpr(E->getLHS());
    llvm::BasicBlock *LHSEnd = B.GetInsertBlock();
    auto *RHSBB = llvm::BasicBlock::Create(C, IsAnd ? "and.rhs" : "or.rhs", CurFn);
    auto *EndBB = llvm::BasicBlock::Create(C, IsAnd ? "and.end" : "or.end", CurFn);
    if (IsAnd)
      B.CreateCondBr(L, RHSBB, EndBB);
    else
      B.CreateCondBr(L, EndBB, RHSBB);

    B.SetInsertPoint(RHSBB);
    llvm::Value *R = visitExpr(E->getRHS());
    llvm::BasicBlock *RHSEnd = B.GetInsertBlock();
    B.CreateBr(EndBB);

    B.SetInsertPoint(EndBB);
    llvm::PHINode *Phi = B.CreatePHI(B.getInt1Ty(), 2, IsAnd ? "and" : "or");
    Phi->addIncoming(B.getInt1(!IsAnd), LHSEnd);
    Phi->addIncoming(R, RHSEnd);
    return Phi;
  }

  // Checks are skipped when a constant divisor makes them provably pass.
  static bool isConstNonZero(llvm::Value *V) {
    auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
    return C && !C->isZero();
  }
  static bool isConstNotMinusOne(llvm::Value *V) {
    auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
    return C && !C->isMinusOne();
  }

  llvm::Value *emitDiv(llvm::Value *L, llvm::Value *R, SourceLocation Loc) {
    if (!isConstNonZero(R))
      emitRuntimeCheck(B.CreateICmpEQ(R, B.getInt32(0)), "division by zero",
                       Loc);
    // INT_MIN div -1 overflows (and traps on x86).
    if (!isConstNotMinusOne(R)) {
      llvm::Value *Overflow =
          B.CreateAnd(B.CreateICmpEQ(L, B.getInt32(INT32_MIN)),
                      B.CreateICmpEQ(R, B.getInt32(-1)));
      emitRuntimeCheck(Overflow, "integer overflow in 'div'", Loc);
    }
    return B.CreateSDiv(L, R, "div");
  }

  llvm::Value *emitMod(llvm::Value *L, llvm::Value *R, SourceLocation Loc) {
    if (LangOpts.Mode == LangMode::ISO) {
      // ISO 7185 6.7.2.2: error if j <= 0; result is i - k*j with
      // 0 <= result < j.
      auto *CR = llvm::dyn_cast<llvm::ConstantInt>(R);
      if (!CR || !CR->getValue().isStrictlyPositive())
        emitRuntimeCheck(B.CreateICmpSLE(R, B.getInt32(0)),
                         "'mod' by zero or a negative number", Loc);
      llvm::Value *Rem = B.CreateSRem(L, R);
      llvm::Value *Fixed = B.CreateAdd(Rem, R);
      return B.CreateSelect(B.CreateICmpSLT(Rem, B.getInt32(0)), Fixed, Rem,
                            "mod");
    }
    // Turbo/FPC: sign follows the dividend (like C %).
    if (!isConstNonZero(R))
      emitRuntimeCheck(B.CreateICmpEQ(R, B.getInt32(0)), "division by zero",
                       Loc);
    // x mod -1 == 0, but INT_MIN srem -1 traps on x86: divide by 1 instead.
    llvm::Value *SafeR = B.CreateSelect(B.CreateICmpEQ(R, B.getInt32(-1)),
                                        B.getInt32(1), R);
    return B.CreateSRem(L, SafeR, "mod");
  }

  llvm::Value *emitCompare(BinaryOp Op, Type *OperandTy, llvm::Value *L,
                           llvm::Value *R) {
    // integer is signed; char (0..255) and boolean (false < true) unsigned.
    bool Signed = OperandTy->isInteger();
    llvm::CmpInst::Predicate P;
    switch (Op) {
    case BinaryOp::Eq: P = llvm::CmpInst::ICMP_EQ; break;
    case BinaryOp::Ne: P = llvm::CmpInst::ICMP_NE; break;
    case BinaryOp::Lt: P = Signed ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_ULT; break;
    case BinaryOp::Le: P = Signed ? llvm::CmpInst::ICMP_SLE : llvm::CmpInst::ICMP_ULE; break;
    case BinaryOp::Gt: P = Signed ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_UGT; break;
    case BinaryOp::Ge: P = Signed ? llvm::CmpInst::ICMP_SGE : llvm::CmpInst::ICMP_UGE; break;
    default: llvm_unreachable("not a comparison");
    }
    return B.CreateICmp(P, L, R, "cmp");
  }

  //===--------------------------------------------------------------------===//
  // Built-in procedures
  //===--------------------------------------------------------------------===//

  void emitWriteArg(Expr *Arg) {
    Expr *Value = Arg;
    llvm::Value *Width = B.getInt32(0); // no field width
    if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
      Value = F->getValue();
      Width = visitExpr(F->getWidth());
    }

    Type *T = Value->getType();
    switch (T->getKind()) {
    case Type::TK_Integer:
      B.CreateCall(RT.WriteInt, {visitExpr(Value), Width});
      return;
    case Type::TK_Char:
      B.CreateCall(RT.WriteChar, {visitExpr(Value), Width});
      return;
    case Type::TK_Boolean:
      B.CreateCall(RT.WriteBool,
                   {B.CreateZExt(visitExpr(Value), B.getInt32Ty()), Width});
      return;
    case Type::TK_String: {
      llvm::StringRef S = llvm::cast<StringLiteralExpr>(Value)->getValue();
      B.CreateCall(RT.WriteStr, {B.CreateGlobalString(S, ".str"),
                                 B.getInt64(S.size()), Width});
      return;
    }
    case Type::TK_Error:
      llvm_unreachable("codegen on an invalid program");
    }
  }

  void emitReadArg(Expr *Arg) {
    auto *V = llvm::cast<VarDecl>(llvm::cast<NameRefExpr>(Arg)->getDecl());
    llvm::Value *Val = V->getType()->isInteger() ? B.CreateCall(RT.ReadInt)
                                                 : B.CreateCall(RT.ReadChar);
    emitStore(V, Val);
  }

public:
  CodeGenModule(llvm::Module &M, const SourceManager &SM,
                const LangOptions &LangOpts)
      : M(M), B(M.getContext()), RT(M), SM(SM), LangOpts(LangOpts) {}

  void emitProgram(ProgramDecl &P) {
    llvm::LLVMContext &C = M.getContext();
    auto *MainTy = llvm::FunctionType::get(
        B.getInt32Ty(), {B.getInt32Ty(), llvm::PointerType::getUnqual(C)},
        false);
    // Create main (and the runtime declarations, above) before the program's
    // globals, so a Pascal variable named e.g. `main` gets renamed, not main.
    CurFn = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                   "main", M);
    CurFn->getArg(0)->setName("argc");
    CurFn->getArg(1)->setName("argv");
    emitGlobals(*P.getBlock());

    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", CurFn));
    B.CreateCall(RT.Init, {CurFn->getArg(0), CurFn->getArg(1)});
    visitStmt(P.getBlock()->getBody());
    B.CreateCall(RT.Fini);
    B.CreateRet(B.getInt32(0));
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *) {}

  void visitCompoundStmt(CompoundStmt *S) {
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
  }

  void visitAssignStmt(AssignStmt *S) {
    auto *V = llvm::cast<VarDecl>(
        llvm::cast<NameRefExpr>(S->getTarget())->getDecl());
    emitStore(V, visitExpr(S->getValue()));
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    auto *BD = llvm::cast<BuiltinDecl>(S->getDecl());
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Write:
    case BuiltinKind::Writeln:
      for (Expr *Arg : S->getArgs())
        emitWriteArg(Arg);
      if (BD->getBuiltinKind() == BuiltinKind::Writeln)
        B.CreateCall(RT.Writeln);
      return;
    case BuiltinKind::Read:
    case BuiltinKind::Readln:
      for (Expr *Arg : S->getArgs())
        emitReadArg(Arg);
      if (BD->getBuiltinKind() == BuiltinKind::Readln)
        B.CreateCall(RT.Readln);
      return;
    default:
      llvm_unreachable("Sema only allows builtin procedures here");
    }
  }

  //===--------------------------------------------------------------------===//
  // Expressions
  //===--------------------------------------------------------------------===//

  llvm::Value *visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    return B.getInt32(static_cast<uint32_t>(E->getValue()));
  }

  llvm::Value *visitStringLiteralExpr(StringLiteralExpr *E) {
    // Multi-char strings only appear as write arguments (emitWriteArg).
    assert(E->isCharLiteral() && "string values need packed arrays (M8)");
    return B.getInt8(static_cast<uint8_t>(E->getValue()[0]));
  }

  llvm::Value *visitNameRefExpr(NameRefExpr *E) {
    Decl *D = E->getDecl();
    if (auto *V = llvm::dyn_cast<VarDecl>(D))
      return emitLoad(V);
    auto *C = llvm::cast<ConstDecl>(D);
    Type *T = C->getType();
    if (T->isBoolean())
      return B.getInt1(C->getValue() != 0);
    if (T->isChar())
      return B.getInt8(static_cast<uint8_t>(C->getValue()));
    return B.getInt32(static_cast<uint32_t>(C->getValue()));
  }

  llvm::Value *visitParenExpr(ParenExpr *E) { return visitExpr(E->getInner()); }

  llvm::Value *visitUnaryExpr(UnaryExpr *E) {
    llvm::Value *V = visitExpr(E->getOperand());
    switch (E->getOp()) {
    case UnaryOp::Plus:  return V;
    case UnaryOp::Minus: return B.CreateNeg(V, "neg");
    case UnaryOp::Not:   return B.CreateNot(V, "not");
    }
    llvm_unreachable("bad unary op");
  }

  llvm::Value *visitBinaryExpr(BinaryExpr *E) {
    BinaryOp Op = E->getOp();
    if (Op == BinaryOp::And || Op == BinaryOp::Or)
      return emitShortCircuit(E);

    llvm::Value *L = visitExpr(E->getLHS());
    llvm::Value *R = visitExpr(E->getRHS());
    switch (Op) {
    case BinaryOp::Add:    return B.CreateAdd(L, R, "add");
    case BinaryOp::Sub:    return B.CreateSub(L, R, "sub");
    case BinaryOp::Mul:    return B.CreateMul(L, R, "mul");
    case BinaryOp::IntDiv: return emitDiv(L, R, E->getOpLoc());
    case BinaryOp::Mod:    return emitMod(L, R, E->getOpLoc());
    case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
    case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
      return emitCompare(Op, E->getLHS()->getType(), L, R);
    default:
      llvm_unreachable("operator rejected by Sema");
    }
  }

  llvm::Value *visitCallExpr(CallExpr *E) {
    auto *BD = llvm::cast<BuiltinDecl>(E->getDecl());
    Expr *ArgE = E->getArgs()[0];
    llvm::Value *Arg = visitExpr(ArgE);
    // Range checks (chr(300), succ(maxint), ...) arrive with M7's {$R+}.
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Abs:
      return B.CreateBinaryIntrinsic(llvm::Intrinsic::abs, Arg, B.getFalse(),
                                     nullptr, "abs");
    case BuiltinKind::Sqr:
      return B.CreateMul(Arg, Arg, "sqr");
    case BuiltinKind::Odd:
      return B.CreateTrunc(Arg, B.getInt1Ty(), "odd");
    case BuiltinKind::Ord:
      return B.CreateZExt(Arg, B.getInt32Ty(), "ord"); // no-op for integer
    case BuiltinKind::Chr:
      return B.CreateTrunc(Arg, B.getInt8Ty(), "chr");
    case BuiltinKind::Succ:
      return B.CreateAdd(Arg, llvm::ConstantInt::get(Arg->getType(), 1), "succ");
    case BuiltinKind::Pred:
      return B.CreateSub(Arg, llvm::ConstantInt::get(Arg->getType(), 1), "pred");
    default:
      llvm_unreachable("not a builtin function");
    }
  }

  llvm::Value *visitFormattedExpr(FormattedExpr *) {
    llvm_unreachable("formatted expressions are handled by emitWriteArg");
  }
};

} // namespace

std::unique_ptr<llvm::Module>
pcc::generateModule(ProgramDecl &P, const SourceManager &SM,
                    const LangOptions &LangOpts, llvm::LLVMContext &Ctx,
                    llvm::TargetMachine &TM, llvm::StringRef SourceFileName) {
  auto M = std::make_unique<llvm::Module>(SourceFileName, Ctx);
  M->setSourceFileName(SourceFileName);
  M->setTargetTriple(TM.getTargetTriple().str());
  M->setDataLayout(TM.createDataLayout());
  CodeGenModule(*M, SM, LangOpts).emitProgram(P);
  return M;
}
