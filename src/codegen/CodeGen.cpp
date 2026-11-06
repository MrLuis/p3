//===- CodeGen.cpp - AST -> LLVM IR ---------------------------------------===//
//
// Lowering (PLAN.md section 6):
//
//   integer -> i32   real -> double   char -> i8   enum -> i32
//   boolean -> i1 in registers / i8 in memory   subrange -> its host type
//   array   -> [N x T]        record -> struct (variants overlay a buffer)
//
//   program variables  -> internal globals @pas.<name>
//   procedure/function -> internal function @pas.<outer>.<name>
//   locals, parameters, function result -> fields of a per-call *frame*
//     struct. A nested routine receives a pointer to its parent's frame (the
//     static link, frame field 0) and walks the chain of static links to
//     reach variables of enclosing routines.
//   var parameters      -> pointer stored in the frame
//   value arrays/records -> passed by pointer, copied into the callee frame
//
//   and/or -> short-circuit;  for -> test the final value before stepping
//   case   -> switch; no matching label is a runtime error in ISO mode
//   runtime checks (CodeGenOptions) call __pas_runtime_error("... at f:l:c")
//
//===----------------------------------------------------------------------===//

#include "pcc/CodeGen/CodeGen.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/AST/Type.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

using namespace pcc;

namespace {

/// Declarations of the libpascalrt entry points (runtime/include/pascalrt.h)
/// and the C library functions we call.
struct Runtime {
  llvm::FunctionCallee Init, Fini, WriteStr, WriteChar, WriteInt, WriteBool,
      WriteReal, Writeln, ReadInt, ReadChar, ReadReal, Readln, RuntimeError,
      Memcmp, Atan;

  explicit Runtime(llvm::Module &M) {
    llvm::LLVMContext &C = M.getContext();
    auto *Void = llvm::Type::getVoidTy(C);
    auto *I8 = llvm::Type::getInt8Ty(C);
    auto *I32 = llvm::Type::getInt32Ty(C);
    auto *I64 = llvm::Type::getInt64Ty(C);
    auto *F64 = llvm::Type::getDoubleTy(C);
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
    WriteReal = Decl("__pas_write_real", Void, {F64, I32, I32});
    Writeln = Decl("__pas_writeln", Void, {});
    ReadInt = Decl("__pas_read_int", I32, {});
    ReadChar = Decl("__pas_read_char", I8, {});
    ReadReal = Decl("__pas_read_real", F64, {});
    Readln = Decl("__pas_readln", Void, {});
    RuntimeError = Decl("__pas_runtime_error", Void, {Ptr});
    auto *RE = llvm::cast<llvm::Function>(RuntimeError.getCallee());
    RE->addFnAttr(llvm::Attribute::NoReturn);
    RE->addFnAttr(llvm::Attribute::Cold);
    Memcmp = Decl("memcmp", I32, {Ptr, Ptr, I64});
    Atan = Decl("atan", F64, {F64});
  }
};

class CodeGenModule : public StmtVisitor<CodeGenModule>,
                      public ExprVisitor<CodeGenModule, llvm::Value *> {
  llvm::Module &M;
  const llvm::DataLayout &DL;
  llvm::IRBuilder<> B;
  Runtime RT;
  const SourceManager &SM;
  const LangOptions &LangOpts;
  const CodeGenOptions &CGOpts;

  // Current function being emitted.
  llvm::Function *CurFn = nullptr;
  RoutineDecl *CurRoutine = nullptr; ///< null while emitting main
  llvm::Value *CurFrame = nullptr;

  llvm::DenseMap<VarDecl *, llvm::GlobalVariable *> Globals;
  llvm::DenseMap<RoutineDecl *, llvm::Function *> Functions;
  llvm::DenseMap<RoutineDecl *, llvm::StructType *> FrameTypes;
  llvm::DenseMap<VarDecl *, unsigned> FrameSlots;
  llvm::DenseMap<Type *, llvm::Type *> LoweredTypes;
  llvm::DenseMap<FieldDecl *, uint64_t> FieldOffsets;
  llvm::DenseMap<WithContext *, llvm::Value *> WithAddrs;
  llvm::StringMap<llvm::GlobalVariable *> StringConstants;
  std::vector<RoutineDecl *> RoutinesToEmit;

  //===--------------------------------------------------------------------===//
  // Types
  //===--------------------------------------------------------------------===//

  /// In-memory representation of a Pascal type.
  llvm::Type *memType(Type *T) {
    if (llvm::Type *L = LoweredTypes.lookup(T))
      return L;
    llvm::Type *L = lowerType(T);
    LoweredTypes[T] = L;
    return L;
  }

  llvm::Type *lowerType(Type *T) {
    switch (T->getKind()) {
    case Type::TK_Integer: return B.getInt32Ty();
    case Type::TK_Real:    return B.getDoubleTy();
    case Type::TK_Boolean: return B.getInt8Ty();
    case Type::TK_Char:    return B.getInt8Ty();
    case Type::TK_Enum:    return B.getInt32Ty();
    case Type::TK_Subrange:
      return memType(llvm::cast<SubrangeType>(T)->getHost());
    case Type::TK_String:
      return llvm::ArrayType::get(
          B.getInt8Ty(), llvm::cast<StringLiteralType>(T)->getLength());
    case Type::TK_Array: {
      auto *A = llvm::cast<ArrayType>(T);
      OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
      return llvm::ArrayType::get(memType(A->getElementType()),
                                  static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1));
    }
    case Type::TK_Record: {
      auto *R = llvm::cast<RecordType>(T);
      std::string Name = T->getDeclName().empty()
                             ? std::string("record")
                             : ("record." + T->getDeclName()).str();
      return layoutFieldList(R->getFieldList(), Name, /*BaseOffset=*/0);
    }
    case Type::TK_Error:
      break;
    }
    llvm_unreachable("type has no storage representation");
  }

  /// Register representation: like memType, but booleans are i1.
  llvm::Type *regType(Type *T) {
    return T->getBase()->isBoolean() ? B.getInt1Ty() : memType(T);
  }

  /// Builds the struct for a record field list and records each field's
  /// byte offset from the start of the record. Variants are laid out as
  /// their own structs, all overlaying one storage buffer.
  llvm::StructType *layoutFieldList(FieldList *FL, const std::string &Name,
                                    uint64_t BaseOffset) {
    llvm::SmallVector<llvm::Type *, 8> Elems;
    llvm::SmallVector<FieldDecl *, 8> Fields;
    for (FieldDecl *F : FL->Fixed) {
      Elems.push_back(memType(F->getType()));
      Fields.push_back(F);
    }
    VariantPart *VP = FL->Variant;
    if (VP && VP->Tag) {
      Elems.push_back(memType(VP->Tag->getType()));
      Fields.push_back(VP->Tag);
    }

    // Lay out each variant first to size the overlay buffer. Their field
    // offsets are shifted once the buffer's own offset is known.
    llvm::SmallVector<Variant *, 4> Variants;
    uint64_t MaxSize = 0, MaxAlign = 1;
    if (VP) {
      for (Variant *V : VP->Variants) {
        llvm::StructType *VT = layoutFieldList(V->Fields, Name + ".variant", 0);
        Variants.push_back(V);
        MaxSize = std::max<uint64_t>(MaxSize, DL.getTypeAllocSize(VT));
        MaxAlign = std::max<uint64_t>(MaxAlign, DL.getABITypeAlign(VT).value());
      }
      llvm::Type *Chunk = B.getIntNTy(static_cast<unsigned>(MaxAlign * 8));
      Elems.push_back(
          llvm::ArrayType::get(Chunk, (MaxSize + MaxAlign - 1) / MaxAlign));
    }

    llvm::StructType *ST = llvm::StructType::create(M.getContext(), Elems, Name);
    const llvm::StructLayout *SL = DL.getStructLayout(ST);
    for (size_t I = 0; I < Fields.size(); ++I)
      FieldOffsets[Fields[I]] = BaseOffset + SL->getElementOffset(I);
    if (VP) {
      uint64_t BufferOffset = BaseOffset + SL->getElementOffset(Fields.size());
      for (Variant *V : Variants)
        shiftFieldOffsets(V->Fields, BufferOffset);
    }
    return ST;
  }

  void shiftFieldOffsets(FieldList *FL, uint64_t Delta) {
    for (FieldDecl *F : FL->Fixed)
      FieldOffsets[F] += Delta;
    if (VariantPart *VP = FL->Variant) {
      if (VP->Tag)
        FieldOffsets[VP->Tag] += Delta;
      for (Variant *V : VP->Variants)
        shiftFieldOffsets(V->Fields, Delta);
    }
  }

  uint64_t sizeOf(Type *T) { return DL.getTypeAllocSize(memType(T)); }

  //===--------------------------------------------------------------------===//
  // Scalars in memory
  //===--------------------------------------------------------------------===//

  llvm::Value *loadScalar(llvm::Value *Ptr, Type *T, const llvm::Twine &Name = "") {
    llvm::Value *Raw = B.CreateLoad(memType(T), Ptr, Name);
    return T->getBase()->isBoolean() ? B.CreateTrunc(Raw, B.getInt1Ty()) : Raw;
  }

  void storeScalar(llvm::Value *Ptr, llvm::Value *Val, Type *T) {
    if (T->getBase()->isBoolean())
      Val = B.CreateZExt(Val, B.getInt8Ty());
    B.CreateStore(Val, Ptr);
  }

  void copyAggregate(llvm::Value *Dst, llvm::Value *Src, Type *T) {
    B.CreateMemCpy(Dst, llvm::MaybeAlign(1), Src, llvm::MaybeAlign(1),
                   sizeOf(T));
  }

  /// Address of a private constant holding S (without a terminating NUL).
  llvm::GlobalVariable *getStringConstant(llvm::StringRef S) {
    llvm::GlobalVariable *&G = StringConstants[S];
    if (!G) {
      auto *Init = llvm::ConstantDataArray::getString(M.getContext(), S,
                                                      /*AddNull=*/false);
      G = new llvm::GlobalVariable(M, Init->getType(), true,
                                   llvm::GlobalValue::PrivateLinkage, Init,
                                   ".str");
      G->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    }
    return G;
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

  llvm::BasicBlock *newBlock(const llvm::Twine &Name) {
    return llvm::BasicBlock::Create(M.getContext(), Name, CurFn);
  }

  /// __pas_runtime_error("<Msg> at file:line:col"); unreachable.
  /// Callers must set a new insertion point afterwards.
  void emitRuntimeError(llvm::StringRef Msg, SourceLocation L) {
    llvm::Constant *Text = B.CreateGlobalString((Msg + where(L)).str(), ".rterr");
    B.CreateCall(RT.RuntimeError, {Text});
    B.CreateUnreachable();
  }

  /// if (Cond) __pas_runtime_error("<Msg> at file:line:col");
  void emitRuntimeCheck(llvm::Value *Cond, llvm::StringRef Msg,
                        SourceLocation L) {
    if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(Cond); C && C->isZero())
      return; // provably fine
    auto *ErrBB = newBlock("rt.error");
    auto *ContBB = newBlock("rt.ok");
    llvm::MDBuilder MDB(M.getContext());
    B.CreateCondBr(Cond, ErrBB, ContBB, MDB.createUnlikelyBranchWeights());
    B.SetInsertPoint(ErrBB);
    emitRuntimeError(Msg, L);
    B.SetInsertPoint(ContBB);
  }

  /// Ordinal values compare as signed if their host is integer, unsigned
  /// otherwise (char 0..255, boolean, enum 0..N-1).
  static bool isSignedOrdinal(Type *T) { return T->isIntegerLike(); }

  /// Widens an ordinal register value to i64 for range comparisons.
  llvm::Value *toI64(llvm::Value *V, Type *T) {
    return isSignedOrdinal(T) ? B.CreateSExt(V, B.getInt64Ty())
                              : B.CreateZExt(V, B.getInt64Ty());
  }

  /// Checks that ordinal value V (of type From) lies within Target's bounds.
  void emitRangeCheck(llvm::Value *V, Type *From, Type *Target,
                      SourceLocation L,
                      llvm::StringRef Msg = "value out of range") {
    if (!CGOpts.RangeChecks || !Target->isOrdinal())
      return;
    OrdinalBounds TB = getOrdinalBounds(Target);
    OrdinalBounds FB = getOrdinalBounds(From);
    if (FB.Lo >= TB.Lo && FB.Hi <= TB.Hi)
      return; // every value of From fits
    llvm::Value *X = toI64(V, From);
    llvm::Value *Bad = B.CreateOr(B.CreateICmpSLT(X, B.getInt64(TB.Lo)),
                                  B.CreateICmpSGT(X, B.getInt64(TB.Hi)));
    emitRuntimeCheck(Bad, Msg, L);
  }

  /// Converts a value for assignment / parameter passing: integer -> real,
  /// and range-checks ordinal values against the target type.
  llvm::Value *convertForAssign(llvm::Value *V, Type *From, Type *To,
                                SourceLocation L) {
    if (To->isReal() && !From->isReal())
      return B.CreateSIToFP(V, B.getDoubleTy(), "toreal");
    if (To->isOrdinal())
      emitRangeCheck(V, From, To, L);
    return V;
  }

  llvm::Value *toDouble(llvm::Value *V, Type *T) {
    return T->isReal() ? V : B.CreateSIToFP(V, B.getDoubleTy(), "toreal");
  }

  /// L op R with an overflow check (sadd/ssub/smul.with.overflow).
  llvm::Value *emitCheckedArith(llvm::Intrinsic::ID ID, llvm::Value *L,
                                llvm::Value *R, SourceLocation Loc,
                                const llvm::Twine &Name) {
    if (!CGOpts.OverflowChecks) {
      switch (ID) {
      case llvm::Intrinsic::sadd_with_overflow: return B.CreateAdd(L, R, Name);
      case llvm::Intrinsic::ssub_with_overflow: return B.CreateSub(L, R, Name);
      default: return B.CreateMul(L, R, Name);
      }
    }
    llvm::Value *Res = B.CreateBinaryIntrinsic(ID, L, R);
    emitRuntimeCheck(B.CreateExtractValue(Res, 1), "integer overflow", Loc);
    return B.CreateExtractValue(Res, 0, Name);
  }

  //===--------------------------------------------------------------------===//
  // Variables and frames
  //===--------------------------------------------------------------------===//

  static std::string mangle(RoutineDecl *R) {
    std::string Name = R->getName().Name.str();
    for (RoutineDecl *P = R->getParent(); P; P = P->getParent())
      Name = P->getName().Name.str() + "." + Name;
    return "pas." + Name;
  }

  /// Pointer to the frame of routine Target, reached from the current
  /// routine by following static links.
  llvm::Value *framePtrFor(RoutineDecl *Target) {
    llvm::Value *Frame = CurFrame;
    for (RoutineDecl *R = CurRoutine; R != Target; R = R->getParent()) {
      assert(R && R->hasStaticLink() && "target is not an enclosing routine");
      llvm::Value *LinkSlot = B.CreateStructGEP(FrameTypes[R], Frame, 0);
      Frame = B.CreateLoad(B.getPtrTy(), LinkSlot, "link");
    }
    return Frame;
  }

  llvm::Value *varAddress(VarDecl *V) {
    if (!V->getOwner())
      return Globals.lookup(V);
    RoutineDecl *Owner = V->getOwner();
    llvm::Value *Slot = B.CreateStructGEP(FrameTypes[Owner], framePtrFor(Owner),
                                          FrameSlots[V], V->getName().Name);
    if (V->isVarParam())
      return B.CreateLoad(B.getPtrTy(), Slot, V->getName().Name + ".ref");
    return Slot;
  }

  llvm::Value *fieldAddress(llvm::Value *RecordAddr, FieldDecl *F) {
    return B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), RecordAddr,
                                        FieldOffsets[F], F->getName().Name);
  }

  /// Address of a designator (variable, a[i], r.f, with-field) or of an
  /// aggregate constant (string literal / string constant).
  llvm::Value *emitAddress(Expr *E) {
    switch (E->getKind()) {
    case Expr::EK_NameRef: {
      auto *N = llvm::cast<NameRefExpr>(E);
      if (auto *V = llvm::dyn_cast<VarDecl>(N->getDecl()))
        return varAddress(V);
      if (auto *F = llvm::dyn_cast<FieldDecl>(N->getDecl()))
        return fieldAddress(WithAddrs.lookup(N->getWith()), F);
      auto *C = llvm::cast<ConstDecl>(N->getDecl());
      return getStringConstant(C->getValue().Str);
    }
    case Expr::EK_StringLiteral:
      return getStringConstant(llvm::cast<StringLiteralExpr>(E)->getValue());
    case Expr::EK_Paren:
      return emitAddress(llvm::cast<ParenExpr>(E)->getInner());
    case Expr::EK_Index: {
      auto *I = llvm::cast<IndexExpr>(E);
      auto *AT = llvm::cast<ArrayType>(I->getBase()->getType());
      llvm::Value *Base = emitAddress(I->getBase());
      llvm::Value *Idx = visitExpr(I->getIndex());
      Type *IdxTy = I->getIndex()->getType();
      emitRangeCheck(Idx, IdxTy, AT->getIndexType(),
                     I->getIndex()->getBeginLoc(), "array index out of bounds");
      OrdinalBounds Bd = getOrdinalBounds(AT->getIndexType());
      llvm::Value *Offset =
          B.CreateSub(toI64(Idx, IdxTy), B.getInt64(Bd.Lo), "idx");
      return B.CreateInBoundsGEP(memType(AT), Base, {B.getInt64(0), Offset});
    }
    case Expr::EK_Field: {
      auto *F = llvm::cast<FieldExpr>(E);
      return fieldAddress(emitAddress(F->getBase()), F->getFieldDecl());
    }
    default:
      llvm_unreachable("expression has no address");
    }
  }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  void emitGlobals(Block &Blk) {
    for (Decl *D : Blk.getDecls()) {
      auto *V = llvm::dyn_cast<VarDecl>(D);
      if (!V)
        continue;
      llvm::Type *Ty = memType(V->getType());
      // ISO leaves variables undefined; zero-initializing makes programs
      // deterministic at no cost for globals.
      Globals[V] = new llvm::GlobalVariable(
          M, Ty, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
          llvm::Constant::getNullValue(Ty), "pas." + V->getName().Name);
    }
  }

  /// Creates LLVM functions and frame types for every routine in Blk
  /// (recursively), and queues their bodies for emission.
  void declareRoutines(Block &Blk) {
    for (Decl *D : Blk.getDecls()) {
      auto *R = llvm::dyn_cast<RoutineDecl>(D);
      if (!R || R->isRedeclaration())
        continue; // the forward declaration is the canonical node

      // Frame: [static link], params, locals, [result].
      llvm::SmallVector<llvm::Type *, 8> Slots;
      if (R->hasStaticLink())
        Slots.push_back(B.getPtrTy());
      auto AddSlot = [&](VarDecl *V) {
        FrameSlots[V] = Slots.size();
        Slots.push_back(V->isVarParam() ? B.getPtrTy() : memType(V->getType()));
      };
      for (VarDecl *P : R->getParams())
        AddSlot(P);
      for (Decl *Local : R->getBody()->getDecls())
        if (auto *V = llvm::dyn_cast<VarDecl>(Local))
          AddSlot(V);
      if (R->isFunction())
        AddSlot(R->getResultVar());
      std::string Name = mangle(R);
      FrameTypes[R] = llvm::StructType::create(M.getContext(), Slots,
                                               "frame." + Name.substr(4));

      // Signature: [static link], params (var and aggregates by pointer).
      llvm::SmallVector<llvm::Type *, 8> ParamTys;
      if (R->hasStaticLink())
        ParamTys.push_back(B.getPtrTy());
      for (VarDecl *P : R->getParams())
        ParamTys.push_back(P->isVarParam() || P->getType()->isStructured()
                               ? B.getPtrTy()
                               : regType(P->getType()));
      llvm::Type *RetTy = R->isFunction() ? regType(R->getResultType())
                                          : B.getVoidTy();
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(RetTy, ParamTys, false),
          llvm::Function::InternalLinkage, Name, M);
      unsigned ArgNo = 0;
      if (R->hasStaticLink())
        F->getArg(ArgNo++)->setName("link");
      for (VarDecl *P : R->getParams())
        F->getArg(ArgNo++)->setName(P->getName().Name);
      Functions[R] = F;
      RoutinesToEmit.push_back(R);

      declareRoutines(*R->getBody());
    }
  }

  void emitRoutineBody(RoutineDecl *R) {
    llvm::Function *F = Functions[R];
    CurFn = F;
    CurRoutine = R;
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));

    llvm::StructType *FrameTy = FrameTypes[R];
    CurFrame = B.CreateAlloca(FrameTy, nullptr, "frame");
    // ISO leaves locals undefined; start from zero for determinism.
    B.CreateMemSet(CurFrame, B.getInt8(0), DL.getTypeAllocSize(FrameTy),
                   DL.getABITypeAlign(FrameTy));

    unsigned ArgNo = 0;
    if (R->hasStaticLink())
      B.CreateStore(F->getArg(ArgNo++), B.CreateStructGEP(FrameTy, CurFrame, 0));
    for (VarDecl *P : R->getParams()) {
      llvm::Value *Arg = F->getArg(ArgNo++);
      llvm::Value *Slot = B.CreateStructGEP(FrameTy, CurFrame, FrameSlots[P]);
      if (P->isVarParam())
        B.CreateStore(Arg, Slot);
      else if (P->getType()->isStructured())
        copyAggregate(Slot, Arg, P->getType()); // value semantics
      else
        storeScalar(Slot, Arg, P->getType());
    }

    visitStmt(R->getBody()->getBody());

    if (R->isFunction()) {
      VarDecl *Result = R->getResultVar();
      B.CreateRet(loadScalar(
          B.CreateStructGEP(FrameTy, CurFrame, FrameSlots[Result]),
          Result->getType(), "result"));
    } else {
      B.CreateRetVoid();
    }
  }

  //===--------------------------------------------------------------------===//
  // Calls
  //===--------------------------------------------------------------------===//

  llvm::Value *emitCall(RoutineDecl *R, llvm::ArrayRef<Expr *> Args) {
    llvm::SmallVector<llvm::Value *, 8> Values;
    if (R->hasStaticLink())
      Values.push_back(framePtrFor(R->getParent()));
    for (size_t I = 0; I < Args.size(); ++I) {
      VarDecl *P = R->getParams()[I];
      Expr *Arg = Args[I];
      if (P->isVarParam() || P->getType()->isStructured()) {
        Values.push_back(emitAddress(Arg));
        continue;
      }
      Values.push_back(convertForAssign(visitExpr(Arg), Arg->getType(),
                                        P->getType(), Arg->getBeginLoc()));
    }
    return B.CreateCall(Functions[R], Values);
  }

  //===--------------------------------------------------------------------===//
  // Operators
  //===--------------------------------------------------------------------===//

  llvm::Value *emitShortCircuit(BinaryExpr *E) {
    bool IsAnd = E->getOp() == BinaryOp::And;
    llvm::Value *L = visitExpr(E->getLHS());
    llvm::BasicBlock *LHSEnd = B.GetInsertBlock();
    auto *RHSBB = newBlock(IsAnd ? "and.rhs" : "or.rhs");
    auto *EndBB = newBlock(IsAnd ? "and.end" : "or.end");
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

  llvm::Value *compareInts(BinaryOp Op, llvm::Value *L, llvm::Value *R,
                           bool Signed) {
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

  llvm::Value *emitCompare(BinaryExpr *E) {
    BinaryOp Op = E->getOp();
    Type *LT = E->getLHS()->getType(), *RTy = E->getRHS()->getType();

    if (LT->isStringType()) {
      // Equal-length strings compare lexicographically (unsigned chars).
      llvm::Value *Cmp = B.CreateCall(
          RT.Memcmp, {emitAddress(E->getLHS()), emitAddress(E->getRHS()),
                      B.getInt64(LT->getStringLength())});
      return compareInts(Op, Cmp, B.getInt32(0), /*Signed=*/true);
    }
    if (LT->isReal() || RTy->isReal()) {
      llvm::Value *L = toDouble(visitExpr(E->getLHS()), LT);
      llvm::Value *R = toDouble(visitExpr(E->getRHS()), RTy);
      llvm::CmpInst::Predicate P;
      switch (Op) {
      case BinaryOp::Eq: P = llvm::CmpInst::FCMP_OEQ; break;
      case BinaryOp::Ne: P = llvm::CmpInst::FCMP_UNE; break;
      case BinaryOp::Lt: P = llvm::CmpInst::FCMP_OLT; break;
      case BinaryOp::Le: P = llvm::CmpInst::FCMP_OLE; break;
      case BinaryOp::Gt: P = llvm::CmpInst::FCMP_OGT; break;
      default:           P = llvm::CmpInst::FCMP_OGE; break;
      }
      return B.CreateFCmp(P, L, R, "cmp");
    }
    llvm::Value *L = visitExpr(E->getLHS());
    llvm::Value *R = visitExpr(E->getRHS());
    return compareInts(Op, L, R, isSignedOrdinal(LT));
  }

  //===--------------------------------------------------------------------===//
  // Built-in procedures and functions
  //===--------------------------------------------------------------------===//

  void emitWriteArg(Expr *Arg) {
    Expr *Value = Arg;
    llvm::Value *Width = B.getInt32(0); // no field width
    llvm::Value *Precision = B.getInt32(-1);
    if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
      Value = F->getValue();
      Width = visitExpr(F->getWidth());
      if (F->getPrecision())
        Precision = visitExpr(F->getPrecision());
    }

    Type *T = Value->getType();
    if (T->isStringType()) {
      B.CreateCall(RT.WriteStr, {emitAddress(Value),
                                 B.getInt64(T->getStringLength()), Width});
      return;
    }
    llvm::Value *V = visitExpr(Value);
    if (T->isReal())
      B.CreateCall(RT.WriteReal, {V, Width, Precision});
    else if (T->getBase()->isChar())
      B.CreateCall(RT.WriteChar, {V, Width});
    else if (T->getBase()->isBoolean())
      B.CreateCall(RT.WriteBool, {B.CreateZExt(V, B.getInt32Ty()), Width});
    else
      B.CreateCall(RT.WriteInt, {V, Width});
  }

  void emitReadArg(Expr *Arg) {
    Type *T = Arg->getType();
    llvm::Value *Addr = emitAddress(Arg);
    if (T->isReal()) {
      B.CreateStore(B.CreateCall(RT.ReadReal), Addr);
      return;
    }
    bool IsChar = T->getBase()->isChar();
    llvm::Value *V = B.CreateCall(IsChar ? RT.ReadChar : RT.ReadInt);
    emitRangeCheck(V, T->getBase(), T, Arg->getBeginLoc());
    B.CreateStore(V, Addr);
  }

  llvm::Value *emitBuiltinCall(CallExpr *E, BuiltinDecl *BD) {
    Expr *ArgE = E->getArgs()[0];
    Type *AT = ArgE->getType();
    llvm::Value *Arg = visitExpr(ArgE);
    SourceLocation Loc = E->getBeginLoc();
    auto *I32 = B.getInt32Ty();

    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Abs:
      if (AT->isReal())
        return B.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, Arg, nullptr, "abs");
      if (CGOpts.OverflowChecks)
        emitRuntimeCheck(B.CreateICmpEQ(Arg, B.getInt32(INT32_MIN)),
                         "integer overflow in 'abs'", Loc);
      return B.CreateBinaryIntrinsic(llvm::Intrinsic::abs, Arg, B.getFalse(),
                                     nullptr, "abs");
    case BuiltinKind::Sqr:
      if (AT->isReal())
        return B.CreateFMul(Arg, Arg, "sqr");
      return emitCheckedArith(llvm::Intrinsic::smul_with_overflow, Arg, Arg,
                              Loc, "sqr");
    case BuiltinKind::Odd:
      return B.CreateTrunc(Arg, B.getInt1Ty(), "odd");
    case BuiltinKind::Ord:
      return isSignedOrdinal(AT) ? B.CreateSExtOrTrunc(Arg, I32, "ord")
                                 : B.CreateZExtOrTrunc(Arg, I32, "ord");
    case BuiltinKind::Chr:
      emitRangeCheck(Arg, AT, E->getType(), Loc, "'chr' argument out of range");
      return B.CreateTrunc(Arg, B.getInt8Ty(), "chr");
    case BuiltinKind::Succ:
    case BuiltinKind::Pred: {
      bool IsSucc = BD->getBuiltinKind() == BuiltinKind::Succ;
      Type *Base = AT->getBase();
      OrdinalBounds Bd = getOrdinalBounds(Base);
      if (CGOpts.RangeChecks)
        emitRuntimeCheck(
            B.CreateICmpEQ(toI64(Arg, Base), B.getInt64(IsSucc ? Bd.Hi : Bd.Lo)),
            IsSucc ? "'succ' of the last value of its type"
                   : "'pred' of the first value of its type",
            Loc);
      llvm::Value *One = llvm::ConstantInt::get(Arg->getType(), 1);
      return IsSucc ? B.CreateAdd(Arg, One, "succ") : B.CreateSub(Arg, One, "pred");
    }
    case BuiltinKind::Sin:
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::sin, toDouble(Arg, AT));
    case BuiltinKind::Cos:
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::cos, toDouble(Arg, AT));
    case BuiltinKind::Exp:
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::exp, toDouble(Arg, AT));
    case BuiltinKind::Arctan:
      return B.CreateCall(RT.Atan, {toDouble(Arg, AT)});
    case BuiltinKind::Ln: {
      llvm::Value *X = toDouble(Arg, AT);
      if (CGOpts.RangeChecks)
        emitRuntimeCheck(
            B.CreateFCmpULE(X, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
            "'ln' of zero or a negative number", Loc);
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::log, X);
    }
    case BuiltinKind::Sqrt: {
      llvm::Value *X = toDouble(Arg, AT);
      if (CGOpts.RangeChecks)
        emitRuntimeCheck(
            B.CreateFCmpULT(X, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
            "'sqrt' of a negative number", Loc);
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, X);
    }
    case BuiltinKind::Trunc:
    case BuiltinKind::Round: {
      if (!AT->isReal())
        return Arg;
      bool IsRound = BD->getBuiltinKind() == BuiltinKind::Round;
      // ISO: round() rounds halves away from zero, like llvm.round.
      llvm::Value *X =
          IsRound ? B.CreateUnaryIntrinsic(llvm::Intrinsic::round, Arg) : Arg;
      if (CGOpts.RangeChecks) {
        auto *Lo = llvm::ConstantFP::get(B.getDoubleTy(), -2147483649.0);
        auto *Hi = llvm::ConstantFP::get(B.getDoubleTy(), 2147483648.0);
        // Also catches NaN (unordered comparisons are true).
        emitRuntimeCheck(B.CreateOr(B.CreateFCmpULE(X, Lo), B.CreateFCmpUGE(X, Hi)),
                         IsRound ? "'round' result does not fit in integer"
                                 : "'trunc' result does not fit in integer",
                         Loc);
      }
      return B.CreateFPToSI(X, I32, IsRound ? "round" : "trunc");
    }
    default:
      llvm_unreachable("not a builtin function");
    }
  }

public:
  CodeGenModule(llvm::Module &M, const SourceManager &SM,
                const LangOptions &LangOpts, const CodeGenOptions &CGOpts)
      : M(M), DL(M.getDataLayout()), B(M.getContext()), RT(M), SM(SM),
        LangOpts(LangOpts), CGOpts(CGOpts) {}

  void emitProgram(ProgramDecl &P) {
    llvm::LLVMContext &C = M.getContext();
    auto *MainTy = llvm::FunctionType::get(
        B.getInt32Ty(), {B.getInt32Ty(), llvm::PointerType::getUnqual(C)},
        false);
    llvm::Function *Main = llvm::Function::Create(
        MainTy, llvm::Function::ExternalLinkage, "main", M);
    Main->getArg(0)->setName("argc");
    Main->getArg(1)->setName("argv");

    Block &Blk = *P.getBlock();
    emitGlobals(Blk);
    declareRoutines(Blk);
    for (RoutineDecl *R : RoutinesToEmit)
      emitRoutineBody(R);

    CurFn = Main;
    CurRoutine = nullptr;
    CurFrame = nullptr;
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Main));
    B.CreateCall(RT.Init, {Main->getArg(0), Main->getArg(1)});
    visitStmt(Blk.getBody());
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
    Expr *Target = S->getTarget(), *Value = S->getValue();
    Type *TT = Target->getType();
    if (TT->isStructured()) {
      llvm::Value *Dst = emitAddress(Target);
      copyAggregate(Dst, emitAddress(Value), TT);
      return;
    }
    llvm::Value *V = convertForAssign(visitExpr(Value), Value->getType(), TT,
                                      Value->getBeginLoc());
    storeScalar(emitAddress(Target), V, TT);
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    if (auto *R = llvm::dyn_cast<RoutineDecl>(S->getDecl())) {
      emitCall(R, S->getArgs());
      return;
    }
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
      llvm_unreachable("Sema only allows procedures here");
    }
  }

  void visitIfStmt(IfStmt *S) {
    llvm::Value *Cond = visitExpr(S->getCond());
    auto *ThenBB = newBlock("if.then");
    auto *EndBB = newBlock("if.end");
    auto *ElseBB = S->getElse() ? newBlock("if.else") : EndBB;
    B.CreateCondBr(Cond, ThenBB, ElseBB);

    B.SetInsertPoint(ThenBB);
    visitStmt(S->getThen());
    B.CreateBr(EndBB);

    if (S->getElse()) {
      B.SetInsertPoint(ElseBB);
      visitStmt(S->getElse());
      B.CreateBr(EndBB);
    }
    B.SetInsertPoint(EndBB);
  }

  void visitWhileStmt(WhileStmt *S) {
    auto *CondBB = newBlock("while.cond");
    auto *BodyBB = newBlock("while.body");
    auto *EndBB = newBlock("while.end");
    B.CreateBr(CondBB);

    B.SetInsertPoint(CondBB);
    B.CreateCondBr(visitExpr(S->getCond()), BodyBB, EndBB);

    B.SetInsertPoint(BodyBB);
    visitStmt(S->getBody());
    B.CreateBr(CondBB);

    B.SetInsertPoint(EndBB);
  }

  void visitRepeatStmt(RepeatStmt *S) {
    auto *BodyBB = newBlock("repeat.body");
    auto *EndBB = newBlock("repeat.end");
    B.CreateBr(BodyBB);

    B.SetInsertPoint(BodyBB);
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
    B.CreateCondBr(visitExpr(S->getCond()), EndBB, BodyBB);

    B.SetInsertPoint(EndBB);
  }

  //   start = Init; final = Final            (each evaluated once)
  //   if !(start <= final) goto end          (>= for downto)
  //   [range-check start and final against the control variable's type]
  // loop:
  //   i = phi [start, pre], [next, inc]; Var := i
  //   Body
  //   if i == final goto end                 (test before stepping: no
  // inc:                                      overflow at maxint)
  //   next = i + 1 (- 1); goto loop
  void visitForStmt(ForStmt *S) {
    auto *V = llvm::cast<VarDecl>(S->getVar()->getDecl());
    Type *VarTy = V->getType();
    bool Signed = isSignedOrdinal(VarTy);
    bool Down = S->isDownto();

    llvm::Value *Start = visitExpr(S->getInit());
    llvm::Value *Final = visitExpr(S->getFinal());
    llvm::Value *Enter =
        Down ? (Signed ? B.CreateICmpSGE(Start, Final) : B.CreateICmpUGE(Start, Final))
             : (Signed ? B.CreateICmpSLE(Start, Final) : B.CreateICmpULE(Start, Final));
    auto *CheckBB = newBlock("for.check");
    auto *LoopBB = newBlock("for.body");
    auto *IncBB = newBlock("for.inc");
    auto *EndBB = newBlock("for.end");
    B.CreateCondBr(Enter, CheckBB, EndBB);

    // ISO 6.8.3.9: if the loop runs, both bounds must be assignment
    // compatible with the control variable.
    B.SetInsertPoint(CheckBB);
    emitRangeCheck(Start, S->getInit()->getType(), VarTy,
                   S->getInit()->getBeginLoc(), "'for' loop bound out of range");
    emitRangeCheck(Final, S->getFinal()->getType(), VarTy,
                   S->getFinal()->getBeginLoc(), "'for' loop bound out of range");
    llvm::BasicBlock *PreBB = B.GetInsertBlock();
    B.CreateBr(LoopBB);

    B.SetInsertPoint(LoopBB);
    llvm::PHINode *I = B.CreatePHI(Start->getType(), 2, V->getName().Name);
    I->addIncoming(Start, PreBB);
    storeScalar(varAddress(V), I, VarTy);
    visitStmt(S->getBody());
    B.CreateCondBr(B.CreateICmpEQ(I, Final, "for.done"), EndBB, IncBB);

    B.SetInsertPoint(IncBB);
    llvm::Value *One = llvm::ConstantInt::get(I->getType(), 1);
    llvm::Value *Next = Down ? B.CreateSub(I, One, "for.next")
                             : B.CreateAdd(I, One, "for.next");
    I->addIncoming(Next, IncBB);
    B.CreateBr(LoopBB);

    B.SetInsertPoint(EndBB);
  }

  void visitCaseStmt(CaseStmt *S) {
    llvm::Value *Sel = visitExpr(S->getSelector());
    auto *SelTy = llvm::cast<llvm::IntegerType>(Sel->getType());
    bool Signed = isSignedOrdinal(S->getSelector()->getType());
    auto *DefaultBB = newBlock("case.default");
    auto *EndBB = newBlock("case.end");
    llvm::SwitchInst *Switch = B.CreateSwitch(Sel, DefaultBB);

    // Ranges (TP) are expanded into switch cases when small; large ones are
    // tested with comparisons in the default block.
    constexpr int64_t MaxExpandedRange = 256;
    struct LargeRange { int64_t Lo, Hi; llvm::BasicBlock *Target; };
    llvm::SmallVector<LargeRange, 4> LargeRanges;

    auto Const = [&](int64_t V) {
      return llvm::ConstantInt::get(SelTy, static_cast<uint64_t>(V), Signed);
    };

    for (CaseArm *Arm : S->getArms()) {
      auto *ArmBB = newBlock("case.arm");
      for (CaseLabel *L : Arm->getLabels()) {
        if (L->HiValue - L->LoValue >= MaxExpandedRange) {
          LargeRanges.push_back({L->LoValue, L->HiValue, ArmBB});
          continue;
        }
        for (int64_t V = L->LoValue; V <= L->HiValue; ++V)
          Switch->addCase(Const(V), ArmBB);
      }
      B.SetInsertPoint(ArmBB);
      visitStmt(Arm->getBody());
      B.CreateBr(EndBB);
    }

    B.SetInsertPoint(DefaultBB);
    for (const LargeRange &R : LargeRanges) {
      llvm::Value *InRange = B.CreateAnd(
          Signed ? B.CreateICmpSGE(Sel, Const(R.Lo)) : B.CreateICmpUGE(Sel, Const(R.Lo)),
          Signed ? B.CreateICmpSLE(Sel, Const(R.Hi)) : B.CreateICmpULE(Sel, Const(R.Hi)));
      auto *NextBB = newBlock("case.range");
      B.CreateCondBr(InRange, R.Target, NextBB);
      B.SetInsertPoint(NextBB);
    }
    if (S->hasElse()) {
      for (Stmt *Child : S->getElseBody())
        visitStmt(Child);
      B.CreateBr(EndBB);
    } else if (LangOpts.Mode == LangMode::ISO) {
      // ISO 7185 6.8.3.5: it is an error if no label equals the selector.
      emitRuntimeError("no 'case' label matches the selector value",
                       S->getBeginLoc());
    } else {
      B.CreateBr(EndBB); // Turbo/FPC: do nothing
    }

    B.SetInsertPoint(EndBB);
  }

  void visitWithStmt(WithStmt *S) {
    // The record designators are evaluated once, on entry (ISO 6.8.3.10).
    for (WithContext *W : S->getRecords())
      WithAddrs[W] = emitAddress(W->Record);
    visitStmt(S->getBody());
  }

  //===--------------------------------------------------------------------===//
  // Expressions (scalar values in register representation)
  //===--------------------------------------------------------------------===//

  llvm::Value *visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    return B.getInt32(static_cast<uint32_t>(E->getValue()));
  }

  llvm::Value *visitRealLiteralExpr(RealLiteralExpr *E) {
    return llvm::ConstantFP::get(B.getDoubleTy(), E->getValue());
  }

  llvm::Value *visitStringLiteralExpr(StringLiteralExpr *E) {
    // Longer strings are aggregates and go through emitAddress.
    assert(E->isCharLiteral() && "string values are handled by address");
    return B.getInt8(static_cast<uint8_t>(E->getValue()[0]));
  }

  llvm::Value *visitNameRefExpr(NameRefExpr *E) {
    Decl *D = E->getDecl();
    if (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D))
      return loadScalar(emitAddress(E), E->getType(), E->getName().Name);
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D))
      return emitCall(R, {}); // parameterless function
    auto *C = llvm::cast<ConstDecl>(D);
    const ConstValue &V = C->getValue();
    if (V.K == ConstValue::Real)
      return llvm::ConstantFP::get(B.getDoubleTy(), V.RealVal);
    return llvm::ConstantInt::get(regType(C->getType()),
                                  static_cast<uint64_t>(V.Int), true);
  }

  llvm::Value *visitParenExpr(ParenExpr *E) { return visitExpr(E->getInner()); }

  llvm::Value *visitUnaryExpr(UnaryExpr *E) {
    llvm::Value *V = visitExpr(E->getOperand());
    switch (E->getOp()) {
    case UnaryOp::Plus:
      return V;
    case UnaryOp::Minus:
      if (E->getType()->isReal())
        return B.CreateFNeg(V, "neg");
      return emitCheckedArith(llvm::Intrinsic::ssub_with_overflow,
                              B.getInt32(0), V, E->getOpLoc(), "neg");
    case UnaryOp::Not:
      return B.CreateNot(V, "not");
    }
    llvm_unreachable("bad unary op");
  }

  llvm::Value *visitBinaryExpr(BinaryExpr *E) {
    BinaryOp Op = E->getOp();
    if (Op == BinaryOp::And || Op == BinaryOp::Or)
      return emitShortCircuit(E);
    if (isRelational(Op))
      return emitCompare(E);

    Type *LT = E->getLHS()->getType(), *RTy = E->getRHS()->getType();
    llvm::Value *L = visitExpr(E->getLHS());
    llvm::Value *R = visitExpr(E->getRHS());

    if (E->getType()->isReal()) {
      L = toDouble(L, LT);
      R = toDouble(R, RTy);
      switch (Op) {
      case BinaryOp::Add: return B.CreateFAdd(L, R, "add");
      case BinaryOp::Sub: return B.CreateFSub(L, R, "sub");
      case BinaryOp::Mul: return B.CreateFMul(L, R, "mul");
      case BinaryOp::RealDiv:
        emitRuntimeCheck(
            B.CreateFCmpOEQ(R, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
            "division by zero", E->getOpLoc());
        return B.CreateFDiv(L, R, "div");
      default: llvm_unreachable("not a real operator");
      }
    }

    SourceLocation Loc = E->getOpLoc();
    switch (Op) {
    case BinaryOp::Add:
      return emitCheckedArith(llvm::Intrinsic::sadd_with_overflow, L, R, Loc, "add");
    case BinaryOp::Sub:
      return emitCheckedArith(llvm::Intrinsic::ssub_with_overflow, L, R, Loc, "sub");
    case BinaryOp::Mul:
      return emitCheckedArith(llvm::Intrinsic::smul_with_overflow, L, R, Loc, "mul");
    case BinaryOp::IntDiv: return emitDiv(L, R, Loc);
    case BinaryOp::Mod:    return emitMod(L, R, Loc);
    default:
      llvm_unreachable("operator rejected by Sema");
    }
  }

  llvm::Value *visitCallExpr(CallExpr *E) {
    if (auto *R = llvm::dyn_cast<RoutineDecl>(E->getDecl()))
      return emitCall(R, E->getArgs());
    return emitBuiltinCall(E, llvm::cast<BuiltinDecl>(E->getDecl()));
  }

  llvm::Value *visitFormattedExpr(FormattedExpr *) {
    llvm_unreachable("formatted expressions are handled by emitWriteArg");
  }

  llvm::Value *visitIndexExpr(IndexExpr *E) {
    return loadScalar(emitAddress(E), E->getType());
  }

  llvm::Value *visitFieldExpr(FieldExpr *E) {
    return loadScalar(emitAddress(E), E->getType(), E->getField().Name);
  }
};

} // namespace

std::unique_ptr<llvm::Module>
pcc::generateModule(ProgramDecl &P, const SourceManager &SM,
                    const LangOptions &LangOpts, const CodeGenOptions &CGOpts,
                    llvm::LLVMContext &Ctx, llvm::TargetMachine &TM,
                    llvm::StringRef SourceFileName) {
  auto M = std::make_unique<llvm::Module>(SourceFileName, Ctx);
  M->setSourceFileName(SourceFileName);
  M->setTargetTriple(TM.getTargetTriple().str());
  M->setDataLayout(TM.createDataLayout());
  CodeGenModule(*M, SM, LangOpts, CGOpts).emitProgram(P);
  return M;
}
