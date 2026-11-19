//===- CodeGen.cpp - AST -> LLVM IR ---------------------------------------===//
//
// Lowering (PLAN.md section 6):
//
//   integer -> i32 (int64 -> i64, cardinal -> i32, byte/shortint -> i8,
//   word/smallint -> i16)   real -> double   char -> i8   enum -> i32
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
//   objects (objfpc) -> a pointer to a heap block: [VMT pointer, ancestors'
//     fields, own fields]. A class's VMT is a constant [4 + N x ptr]:
//     parent VMT, class name (length-prefixed), instance size, interface
//     table, then the N virtual methods. TObject's VMT and destructor are
//     in the runtime. An interface (CORBA style) value is {object, method
//     table}: the class's table for the interface, found through the
//     object's VMT ([count, iid, table, iid, table, ...]; an interface's
//     iid is a global of its own).
//     Methods take Self after the static link; a class method's Self is a
//     class reference (a VMT). Class variables are globals.
//
//   and/or -> short-circuit;  for -> test the final value before stepping
//   case   -> switch; no matching label is a runtime error in ISO mode
//   runtime checks (CodeGenOptions) call __pas_runtime_error("... at f:l:c")
//
//===----------------------------------------------------------------------===//

#include "p3/CodeGen/CodeGen.h"
#include "p3/AST/ASTVisitor.h"
#include "p3/AST/Type.h"
#include "p3/Basic/Directives.h"
#include "p3/Basic/LangOptions.h"
#include "p3/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

using namespace p3;

namespace {

/// Does the runtime function Name only read the memory reachable from its
/// pointer arguments, and always return? (Item 9: these are facts LLVM
/// cannot infer across the library boundary.)
static bool runtimeIsPureQuery(llvm::StringRef Name) {
  return Name == "__pas_str_compare" || Name == "__pas_str_pos" ||
         Name == "__pas_astr_compare" || Name == "__pas_astr_pos" ||
         Name == "__pas_astr_length" || Name == "__pas_astr_bad_index" ||
         Name == "__pas_dyn_length" || Name == "__pas_dyn_bad_index" ||
         Name == "__pas_aoc_bad_index" || Name == "__pas_class_is" ||
         Name == "__pas_class_inherits" || Name == "__pas_intf_lookup" ||
         Name == "memcmp";
}

/// Does the runtime function Name return a new allocation (or NULL), never
/// a pointer into an existing object?
static bool runtimeReturnsFresh(llvm::StringRef Name) {
  return Name == "__pas_new" || Name == "__pas_astr_from_chars" ||
         Name == "__pas_astr_from_pstr" || Name == "__pas_astr_from_char" ||
         Name == "__pas_astr_from_cstr" || Name == "__pas_astr_join" ||
         Name == "__pas_dyn_new" || Name == "__pas_dyn_copy" ||
         Name == "__pas_format" || Name == "__pas_tmpl_add";
}

/// The pointer parameters of the runtime function Name that it only reads,
/// and never captures.
static bool runtimeReadsParam(llvm::StringRef Name, unsigned I) {
  if (Name == "__pas_str_compare" || Name == "__pas_str_pos" ||
      Name == "__pas_astr_compare" || Name == "__pas_astr_pos" ||
      Name == "__pas_class_is" || Name == "__pas_class_inherits" ||
      Name == "__pas_intf_lookup" || Name == "memcmp")
    return I <= 1;
  if (Name == "__pas_astr_length" || Name == "__pas_astr_bad_index" ||
      Name == "__pas_dyn_length" || Name == "__pas_dyn_bad_index" ||
      Name == "__pas_aoc_bad_index" || Name == "__pas_astr_from_chars" ||
      Name == "__pas_astr_from_pstr" || Name == "__pas_astr_from_cstr" ||
      Name == "__pas_astr_pchar" || Name == "__pas_dyn_copy" ||
      Name == "__pas_dyn_new" || Name == "__pas_astr_copy")
    return I == 0;
  if (Name == "__pas_tmpl_add")
    return I <= 1;
  if (Name == "__pas_write_enum")
    return I == 1; // the names table (not the file it writes to)
  if (Name == "__pas_str_assign" || Name == "__pas_str_copy")
    return I == 2 || I == 1;
  if (Name == "__pas_str_concat")
    return I == 1 || I == 2;
  if (Name == "__pas_str_append")
    return I == 2;
  if (Name == "__pas_str_insert")
    return I == 0;
  if (Name == "__pas_write_str" || Name == "__pas_write_pstr" ||
      Name == "__pas_write_cstr" || Name == "__pas_write_astr")
    return I == 1;
  if (Name == "__pas_astr_to_pstr")
    return I == 2;
  if (Name == "__pas_astr_join")
    return I == 0 || I == 1 || I == 3;
  if (Name == "__pas_format")
    return I == 0 || I == 1;
  if (Name == "__pas_format_pstr")
    return I == 1 || I == 2;
  if (Name == "__pas_intf_as" || Name == "__pas_class_as")
    return I <= 2;
  return false;
}

/// A runtime destination that it only writes, and that cannot alias a
/// source of the call (item 4).
static bool runtimeWritesParam(llvm::StringRef Name, unsigned I) {
  return (Name == "__pas_read_elem" && I == 1) ||
         (Name == "__pas_read_str" && I == 1) ||
         (Name == "__pas_paramstr" && I == 0);
}

/// Adds to the runtime function F the attributes that are true of it: its
/// memory effects and return, plus those of its parameters (item 9).
static void addRuntimeAttrs(llvm::Function *F) {
  llvm::LLVMContext &C = F->getContext();
  llvm::StringRef Name = F->getName();
  if (runtimeIsPureQuery(Name)) {
    F->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        C, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    F->addFnAttr(llvm::Attribute::WillReturn);
  } else if (Name == "__pas_astr_pchar") {
    F->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        C, llvm::MemoryEffects::none()));
    F->addFnAttr(llvm::Attribute::WillReturn);
  }
  if (runtimeReturnsFresh(Name))
    F->addRetAttr(llvm::Attribute::NoAlias);
  if (Name == "__pas_abstract_error") {
    F->addFnAttr(llvm::Attribute::Cold);
    F->addFnAttr(llvm::Attribute::NoReturn);
  }
  if (Name == "__pas_new") { // calloc(1, size), and returns zeroed memory
    llvm::AttrBuilder B(C);
    B.addAllocKindAttr(llvm::AllocFnKind::Alloc |
                       llvm::AllocFnKind::Zeroed);
    B.addAllocSizeAttr(0, std::nullopt);
    F->addFnAttrs(B);
  } else if (Name == "__pas_dyn_new") { // calloc(1, header + count * size)
    llvm::AttrBuilder B(C);
    B.addAllocKindAttr(llvm::AllocFnKind::Alloc |
                       llvm::AllocFnKind::Zeroed);
    F->addFnAttrs(B);
  } else if (Name == "__pas_realloc") {
    llvm::AttrBuilder B(C);
    B.addAllocKindAttr(llvm::AllocFnKind::Realloc);
    B.addAllocSizeAttr(1, std::nullopt);
    F->addFnAttrs(B);
  }
  for (unsigned I = 0; I < F->arg_size(); ++I) {
    if (!F->getArg(I)->getType()->isPointerTy())
      continue;
    llvm::AttrBuilder B(C);
    bool Any = false;
    if (runtimeReadsParam(Name, I)) {
      B.addAttribute(llvm::Attribute::NoCapture);
      B.addAttribute(llvm::Attribute::ReadOnly);
      Any = true;
    } else if (runtimeWritesParam(Name, I)) {
      B.addAttribute(llvm::Attribute::NoCapture);
      B.addAttribute(llvm::Attribute::WriteOnly);
      Any = true;
    }
    if (Any)
      F->addParamAttrs(I, B);
  }
}

/// Declarations of the libpascalrt entry points (runtime/include/pascalrt.h)
/// and the C library functions we call.
struct Runtime {
  llvm::FunctionCallee Init, Fini, WriteBegin, WriteEnd, WriteDiscard,
      WriteStr, WriteChar, WriteInt, WriteBool, WriteReal, Writeln,
      ReadInt, ReadInt64, ReadChar, ReadReal, Readln,
      RuntimeError, RuntimeErrorAt,
      Memcmp, Atan, New, Dispose, FileNew, FileFree, ProgramParam, Reset,
      Rewrite, Get, Put, Page, Eof, Eoln, Buffer, ReadElem, WriteElem,
      SetJmp, LongJmp, StrAssign, StrConcat, StrCompare, StrCopy, StrPos,
      StrDelete, StrInsert, WritePStr, ReadStr, Free, Realloc, Halt,
      ParamCount, ParamStr;

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
      auto *Fn = llvm::cast<llvm::Function>(F.getCallee());
      Fn->addFnAttr(llvm::Attribute::NoUnwind);
      addRuntimeAttrs(Fn);
      return F;
    };
    Init = Decl("__pas_init", Void, {I32, Ptr});
    Fini = Decl("__pas_fini", Void, {});
    // Text I/O: the first argument is the PasFile*.
    WriteBegin = Decl("__pas_write_begin", Void, {Ptr});
    WriteEnd = Decl("__pas_write_end", Void, {Ptr});
    WriteDiscard = Decl("__pas_write_discard", Void, {Ptr});
    WriteStr = Decl("__pas_write_str", Void, {Ptr, Ptr, I64, I32});
    WriteChar = Decl("__pas_write_char", Void, {Ptr, I8, I32});
    WriteInt = Decl("__pas_write_int", Void, {Ptr, I64, I32});
    WriteBool = Decl("__pas_write_bool", Void, {Ptr, I32, I32});
    WriteReal = Decl("__pas_write_real", Void, {Ptr, F64, I32, I32});
    Writeln = Decl("__pas_writeln", Void, {Ptr});
    ReadInt = Decl("__pas_read_int", I32, {Ptr});
    ReadInt64 = Decl("__pas_read_int64", I64, {Ptr});
    ReadChar = Decl("__pas_read_char", I8, {Ptr});
    ReadReal = Decl("__pas_read_real", F64, {Ptr});
    Readln = Decl("__pas_readln", Void, {Ptr});
    // Files.
    FileNew = Decl("__pas_file_new", Ptr, {I64, I32, Ptr});
    FileFree = Decl("__pas_file_free", Void, {Ptr});
    ProgramParam = Decl("__pas_program_param", Ptr, {I32, Ptr});
    Reset = Decl("__pas_reset", Void, {Ptr});
    Rewrite = Decl("__pas_rewrite", Void, {Ptr});
    Get = Decl("__pas_get", Void, {Ptr});
    Put = Decl("__pas_put", Void, {Ptr});
    Page = Decl("__pas_page", Void, {Ptr});
    Eof = Decl("__pas_eof", I32, {Ptr});
    Eoln = Decl("__pas_eoln", I32, {Ptr});
    Buffer = Decl("__pas_buffer", Ptr, {Ptr});
    ReadElem = Decl("__pas_read_elem", Void, {Ptr, Ptr});
    WriteElem = Decl("__pas_write_elem", Void, {Ptr, Ptr});
    RuntimeError = Decl("__pas_runtime_error", Void, {Ptr});
    auto *RE = llvm::cast<llvm::Function>(RuntimeError.getCallee());
    RE->addFnAttr(llvm::Attribute::NoReturn);
    RE->addFnAttr(llvm::Attribute::Cold);
    RuntimeErrorAt = Decl("__pas_runtime_error_at", Void, {Ptr, Ptr});
    auto *REA = llvm::cast<llvm::Function>(RuntimeErrorAt.getCallee());
    REA->addFnAttr(llvm::Attribute::NoReturn);
    REA->addFnAttr(llvm::Attribute::Cold);
    Memcmp = Decl("memcmp", I32, {Ptr, Ptr, I64});
    New = Decl("__pas_new", Ptr, {I64});
    Dispose = Decl("__pas_dispose", Void, {Ptr});
    Atan = Decl("atan", F64, {F64});
    Free = Decl("free", Void, {Ptr});
    Realloc = Decl("__pas_realloc", Ptr, {Ptr, I64});
    Halt = Decl("__pas_halt", Void, {I32});
    llvm::cast<llvm::Function>(Halt.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
    ParamCount = Decl("__pas_paramcount", I32, {});
    ParamStr = Decl("__pas_paramstr", Void, {Ptr, I32});
    // Short strings (Turbo/FPC).
    StrAssign = Decl("__pas_str_assign", Void, {Ptr, I32, Ptr});
    StrConcat = Decl("__pas_str_concat", Void, {Ptr, Ptr, Ptr});
    StrCompare = Decl("__pas_str_compare", I32, {Ptr, Ptr});
    StrCopy = Decl("__pas_str_copy", Void, {Ptr, Ptr, I32, I32});
    StrPos = Decl("__pas_str_pos", I32, {Ptr, Ptr});
    StrDelete = Decl("__pas_str_delete", Void, {Ptr, I32, I32});
    StrInsert = Decl("__pas_str_insert", Void, {Ptr, Ptr, I32, I32});
    WritePStr = Decl("__pas_write_pstr", Void, {Ptr, Ptr, I32});
    ReadStr = Decl("__pas_read_str", Void, {Ptr, Ptr, I32});
    // Non-local goto. The underscore variants do not save the signal mask.
    SetJmp = Decl("_setjmp", I32, {Ptr});
    llvm::cast<llvm::Function>(SetJmp.getCallee())
        ->addFnAttr(llvm::Attribute::ReturnsTwice);
    LongJmp = Decl("_longjmp", Void, {Ptr, I32});
    llvm::cast<llvm::Function>(LongJmp.getCallee())
        ->addFnAttr(llvm::Attribute::NoReturn);
  }
};

using VarSet = llvm::DenseSet<VarDecl *>;

/// Finds the locals that have to stay in the frame when a routine has a try
/// statement: a write inside a protected region is visible after a _longjmp
/// only if the variable is in memory. A variable that is only read, or is
/// overwritten before any read after the landing, can stay in an alloca.
/// The analysis is deliberately conservative around calls (an argument of an
/// unknown call may be a var parameter) and loops.
class PinAnalysis {
  RoutineDecl *R;
  VarSet Pins;

  /// The variable whose storage an assignment to E writes, if local: x,
  /// r.f and a[i] write x, r and a (not the index).
  void addTarget(Expr *E, VarSet &Out) {
    if (!E)
      return;
    if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
      addTarget(P->getInner(), Out);
      return;
    }
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
      if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl())) {
        Out.insert(V->getAbsoluteRoot() ? V->getAbsoluteRoot() : V);
      } else if (N->getWith()) {
        addTarget(N->getWith()->Record, Out);
      }
      return;
    }
    if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
      addTarget(F->getBase(), Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
      addTarget(I->getBase(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
      // The pointee changes. Keep the pointer in memory too: if LLVM keeps it
      // in a register, it may forward a field through it across the setjmp.
      addTarget(D->getBase(), Out);
      return;
    }
  }

  /// Calls that can write through a var/out parameter. A routine's parameter
  /// list says which arguments; builtins are handled by their known effects;
  /// an unknown call gets the conservative answer (every argument).
  void addCallWrites(Decl *D, llvm::ArrayRef<Expr *> Args, VarSet &Out) {
    auto Mark = [&](size_t I) {
      if (I < Args.size())
        addTarget(Args[I], Out);
    };
    if (auto *B = llvm::dyn_cast_or_null<BuiltinDecl>(D)) {
      switch (B->getBuiltinKind()) {
      case BuiltinKind::Write:
      case BuiltinKind::Writeln:
      case BuiltinKind::Halt:
      case BuiltinKind::Break:
      case BuiltinKind::Continue:
        return;
      case BuiltinKind::Exit:
        if (!Args.empty() && R->isFunction())
          Out.insert(R->getResultVar());
        return;
      case BuiltinKind::Inc:
      case BuiltinKind::Dec:
      case BuiltinKind::SetLength:
      case BuiltinKind::Delete:
      case BuiltinKind::Insert:
      case BuiltinKind::GetMem:
      case BuiltinKind::FreeMem:
      case BuiltinKind::ReallocMem:
      case BuiltinKind::FillChar:
      case BuiltinKind::New:
      case BuiltinKind::Dispose:
      case BuiltinKind::Assign:
      case BuiltinKind::AssignFile:
      case BuiltinKind::Reset:
      case BuiltinKind::Rewrite:
      case BuiltinKind::Get:
      case BuiltinKind::Put:
      case BuiltinKind::Page:
      case BuiltinKind::Close:
      case BuiltinKind::CloseFile:
      case BuiltinKind::Flush:
      case BuiltinKind::Seek:
        Mark(0);
        return;
      case BuiltinKind::Move:
        Mark(1);
        return;
      case BuiltinKind::BlockRead:
      case BuiltinKind::BlockWrite:
        for (size_t I = 1; I < Args.size(); ++I)
          Mark(I);
        return;
      case BuiltinKind::Pack:
      case BuiltinKind::Unpack:
        for (size_t I = 0; I < Args.size(); ++I)
          Mark(I);
        return;
      default:
        for (size_t I = 0; I < Args.size(); ++I)
          Mark(I);
        return;
      }
    }
    if (auto *RD = llvm::dyn_cast_or_null<RoutineDecl>(D)) {
      auto Params = RD->getParams();
      for (size_t I = 0; I < Args.size() && I < Params.size(); ++I)
        if (Params[I]->isVarParam() || Params[I]->isOutParam())
          addTarget(Args[I], Out);
      return;
    }
    // A procedural value or an unknown declaration.
    for (size_t I = 0; I < Args.size(); ++I)
      Mark(I);
  }

  void addWrittenExpr(Expr *E, VarSet &Out) {
    if (!E)
      return;
    if (auto *C = llvm::dyn_cast<CallExpr>(E)) {
      addCallWrites(C->getDecl(), C->getArgs(), Out);
      if (C->getWith())
        addTarget(C->getWith()->Record, Out);
      addTarget(C->getCalleeValue(), Out);
      addWrittenExpr(C->getCalleeValue(), Out);
      for (Expr *A : C->getArgs())
        addWrittenExpr(A, Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<IndirectCallExpr>(E)) {
      for (Expr *A : C->getArgs())
        addTarget(A, Out);
      addTarget(C->getCallee(), Out);
      addWrittenExpr(C->getCallee(), Out);
      for (Expr *A : C->getArgs())
        addWrittenExpr(A, Out);
      return;
    }
    if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
      addWrittenExpr(P->getInner(), Out);
      return;
    }
    if (auto *U = llvm::dyn_cast<UnaryExpr>(E)) {
      addWrittenExpr(U->getOperand(), Out);
      return;
    }
    if (auto *B = llvm::dyn_cast<BinaryExpr>(E)) {
      addWrittenExpr(B->getLHS(), Out);
      addWrittenExpr(B->getRHS(), Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<FormattedExpr>(E)) {
      addWrittenExpr(F->getValue(), Out);
      addWrittenExpr(F->getWidth(), Out);
      addWrittenExpr(F->getPrecision(), Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
      addWrittenExpr(I->getBase(), Out);
      addWrittenExpr(I->getIndex(), Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
      addWrittenExpr(F->getBase(), Out);
      return;
    }
    if (auto *A = llvm::dyn_cast<AddrOfExpr>(E)) {
      // The address escapes: a call inside the protected code may write it.
      addTarget(A->getOperand(), Out);
      addWrittenExpr(A->getOperand(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
      addWrittenExpr(D->getBase(), Out);
      return;
    }
    if (auto *S = llvm::dyn_cast<SetCtorExpr>(E)) {
      for (const SetElement &El : S->getElements()) {
        addWrittenExpr(El.Lo, Out);
        addWrittenExpr(El.Hi, Out);
      }
      return;
    }
    if (auto *A = llvm::dyn_cast<AggregateExpr>(E)) {
      for (AggregateElement *El : A->getElements())
        addWrittenExpr(El->Value, Out);
      return;
    }
    if (auto *T = llvm::dyn_cast<TemplateExpr>(E)) {
      for (Expr *P : T->getParts())
        addWrittenExpr(P, Out);
      return;
    }
    if (auto *T = llvm::dyn_cast<TupleExpr>(E)) {
      for (Expr *X : T->getElements())
        addWrittenExpr(X, Out);
      return;
    }
    if (auto *N = llvm::dyn_cast<NamedArgExpr>(E)) {
      addWrittenExpr(N->getValue(), Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<CondExpr>(E)) {
      addWrittenExpr(C->getCond(), Out);
      addWrittenExpr(C->getThen(), Out);
      addWrittenExpr(C->getElse(), Out);
      return;
    }
    if (auto *A = llvm::dyn_cast<AwaitExpr>(E)) {
      addWrittenExpr(A->getOperand(), Out);
      return;
    }
    if (auto *S = llvm::dyn_cast<SpawnExpr>(E)) {
      addWrittenExpr(S->getCall(), Out);
      return;
    }
    if (auto *S = llvm::dyn_cast<SafeNavExpr>(E)) {
      addWrittenExpr(S->getBase(), Out);
      addWrittenExpr(S->getAccess(), Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<CoalesceExpr>(E)) {
      addWrittenExpr(C->getLeft(), Out);
      addWrittenExpr(C->getRight(), Out);
      return;
    }
    if (auto *M = llvm::dyn_cast<MoveExpr>(E)) {
      addWrittenExpr(M->getOperand(), Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<InheritedExpr>(E)) {
      for (Expr *A : I->getImplicitArgs())
        addWrittenExpr(A, Out);
      return;
    }
  }

  void addWrittenList(llvm::ArrayRef<Stmt *> Body, VarSet &Out) {
    for (Stmt *S : Body)
      addWrittenStmt(S, Out);
  }

  void addWrittenStmt(Stmt *S, VarSet &Out) {
    if (!S)
      return;
    if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
      addWrittenList(C->getBody(), Out);
      return;
    }
    if (auto *A = llvm::dyn_cast<AssignStmt>(S)) {
      addTarget(A->getTarget(), Out);
      addWrittenExpr(A->getValue(), Out);
      return;
    }
    if (auto *P = llvm::dyn_cast<ProcCallStmt>(S)) {
      addCallWrites(P->getDecl(), P->getArgs(), Out);
      if (P->getWith())
        addTarget(P->getWith()->Record, Out);
      addTarget(P->getCalleeValue(), Out);
      addWrittenExpr(P->getCalleeValue(), Out);
      for (Expr *A : P->getArgs())
        addWrittenExpr(A, Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IndirectCallStmt>(S)) {
      addWrittenExpr(I->getCall(), Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IfStmt>(S)) {
      addWrittenExpr(I->getCond(), Out);
      addWrittenStmt(I->getThen(), Out);
      addWrittenStmt(I->getElse(), Out);
      return;
    }
    if (auto *W = llvm::dyn_cast<WhileStmt>(S)) {
      addWrittenExpr(W->getCond(), Out);
      addWrittenStmt(W->getBody(), Out);
      return;
    }
    if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
      addWrittenList(R->getBody(), Out);
      addWrittenExpr(R->getCond(), Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<ForStmt>(S)) {
      addTarget(F->getVar(), Out);
      addWrittenExpr(F->getInit(), Out);
      addWrittenExpr(F->getFinal(), Out);
      addWrittenStmt(F->getBody(), Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<ForInStmt>(S)) {
      addTarget(F->getVar(), Out);
      if (F->getElementVar())
        Out.insert(F->getElementVar());
      if (F->getCollectionVar())
        Out.insert(F->getCollectionVar());
      if (F->getEnumeratorVar())
        Out.insert(F->getEnumeratorVar());
      addWrittenExpr(F->getCollection(), Out);
      if (F->getStep())
        addWrittenStmt(F->getStep(), Out);
      addWrittenExpr(F->getGetEnumerator(), Out);
      addWrittenExpr(F->getMoveNext(), Out);
      addWrittenStmt(F->getBody(), Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
      addWrittenExpr(C->getSelector(), Out);
      for (CaseArm *A : C->getArms()) {
        for (CaseLabel *L : A->getLabels()) {
          addWrittenExpr(L->Lo, Out);
          addWrittenExpr(L->Hi, Out);
        }
        addWrittenStmt(A->getBody(), Out);
      }
      addWrittenList(C->getElseBody(), Out);
      return;
    }
    if (auto *W = llvm::dyn_cast<WithStmt>(S)) {
      for (WithContext *C : W->getRecords())
        addWrittenExpr(C->Record, Out);
      addWrittenStmt(W->getBody(), Out);
      return;
    }
    if (auto *L = llvm::dyn_cast<LabeledStmt>(S)) {
      addWrittenStmt(L->getBody(), Out);
      return;
    }
    if (auto *T = llvm::dyn_cast<TryStmt>(S)) {
      addWrittenList(T->getBody(), Out);
      for (ExceptHandler *H : T->getHandlers())
        addWrittenStmt(H->getBody(), Out);
      addWrittenList(T->getExceptBody(), Out);
      return;
    }
    if (auto *R = llvm::dyn_cast<RaiseStmt>(S)) {
      addWrittenExpr(R->getObject(), Out);
      addWrittenExpr(R->getAddress(), Out);
      addWrittenExpr(R->getFrame(), Out);
      return;
    }
    if (auto *V = llvm::dyn_cast<VarDeclStmt>(S)) {
      for (VarDecl *D : V->getVars())
        Out.insert(D);
      addWrittenExpr(V->getInit(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DestructureStmt>(S)) {
      for (VarDecl *V : D->getVars())
        Out.insert(V);
      for (Expr *T : D->getTargets())
        addTarget(T, Out);
      addWrittenExpr(D->getValue(), Out);
      return;
    }
    if (auto *E = llvm::dyn_cast<ExprStmt>(S)) {
      addWrittenExpr(E->getExpr(), Out);
      return;
    }
    if (auto *S2 = llvm::dyn_cast<ScopeStmt>(S)) {
      addWrittenExpr(S2->getToken(), Out);
      addWrittenList(S2->getBody(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DetachedStmt>(S)) {
      addWrittenExpr(D->getHandler(), Out);
      addWrittenExpr(D->getCall(), Out);
      return;
    }
    if (auto *Y = llvm::dyn_cast<YieldStmt>(S)) {
      addWrittenExpr(Y->getValue(), Out);
      return;
    }
  }

  /// The variables an assignment target reads to compute its address: a[i]
  /// reads a and i, p^ reads p, r.f reads r, but x := e does not read x.
  static void addTargetReads(Expr *E, VarSet &Out) {
    if (!E)
      return;
    if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
      addTargetReads(P->getInner(), Out);
      return;
    }
    if (llvm::isa<NameRefExpr>(E))
      return;
    if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
      addUsed(F->getBase(), Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
      addUsed(I->getBase(), Out);
      addUsed(I->getIndex(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
      addUsed(D->getBase(), Out);
      return;
    }
  }

  static void addUsed(Expr *E, VarSet &Out) {
    if (!E)
      return;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
      if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl())) {
        Out.insert(V->getAbsoluteRoot() ? V->getAbsoluteRoot() : V);
      } else if (N->getWith()) {
        addUsed(N->getWith()->Record, Out);
      }
      return;
    }
    if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
      addUsed(P->getInner(), Out);
      return;
    }
    if (auto *U = llvm::dyn_cast<UnaryExpr>(E)) {
      addUsed(U->getOperand(), Out);
      return;
    }
    if (auto *B = llvm::dyn_cast<BinaryExpr>(E)) {
      addUsed(B->getLHS(), Out);
      addUsed(B->getRHS(), Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<CallExpr>(E)) {
      if (C->getWith())
        addUsed(C->getWith()->Record, Out);
      addUsed(C->getCalleeValue(), Out);
      for (Expr *A : C->getArgs())
        addUsed(A, Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<FormattedExpr>(E)) {
      addUsed(F->getValue(), Out);
      addUsed(F->getWidth(), Out);
      addUsed(F->getPrecision(), Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
      addUsed(I->getBase(), Out);
      addUsed(I->getIndex(), Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
      addUsed(F->getBase(), Out);
      return;
    }
    if (auto *A = llvm::dyn_cast<AddrOfExpr>(E)) {
      addUsed(A->getOperand(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
      addUsed(D->getBase(), Out);
      return;
    }
    if (auto *S = llvm::dyn_cast<SetCtorExpr>(E)) {
      for (const SetElement &El : S->getElements()) {
        addUsed(El.Lo, Out);
        addUsed(El.Hi, Out);
      }
      return;
    }
    if (auto *A = llvm::dyn_cast<AggregateExpr>(E)) {
      for (AggregateElement *El : A->getElements())
        addUsed(El->Value, Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<IndirectCallExpr>(E)) {
      addUsed(C->getCallee(), Out);
      for (Expr *A : C->getArgs())
        addUsed(A, Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<InheritedExpr>(E)) {
      for (Expr *A : I->getImplicitArgs())
        addUsed(A, Out);
      return;
    }
    if (auto *M = llvm::dyn_cast<MoveExpr>(E)) {
      addUsed(M->getOperand(), Out);
      return;
    }
    if (auto *T = llvm::dyn_cast<TemplateExpr>(E)) {
      for (Expr *P : T->getParts())
        addUsed(P, Out);
      return;
    }
    if (auto *T = llvm::dyn_cast<TupleExpr>(E)) {
      for (Expr *X : T->getElements())
        addUsed(X, Out);
      return;
    }
    if (auto *N = llvm::dyn_cast<NamedArgExpr>(E)) {
      addUsed(N->getValue(), Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<CondExpr>(E)) {
      addUsed(C->getCond(), Out);
      addUsed(C->getThen(), Out);
      addUsed(C->getElse(), Out);
      return;
    }
    if (auto *A = llvm::dyn_cast<AwaitExpr>(E)) {
      addUsed(A->getOperand(), Out);
      return;
    }
    if (auto *S = llvm::dyn_cast<SpawnExpr>(E)) {
      addUsed(S->getCall(), Out);
      return;
    }
    if (auto *S = llvm::dyn_cast<SafeNavExpr>(E)) {
      addUsed(S->getBase(), Out);
      addUsed(S->getAccess(), Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<CoalesceExpr>(E)) {
      addUsed(C->getLeft(), Out);
      addUsed(C->getRight(), Out);
      return;
    }
  }

  static void addUsedList(llvm::ArrayRef<Stmt *> Body, VarSet &Out) {
    for (Stmt *S : Body)
      addUsedStmt(S, Out);
  }

  static void addUsedStmt(Stmt *S, VarSet &Out) {
    if (!S)
      return;
    if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
      addUsedList(C->getBody(), Out);
      return;
    }
    if (auto *P = llvm::dyn_cast<ProcCallStmt>(S)) {
      if (P->getWith())
        addUsed(P->getWith()->Record, Out);
      addUsed(P->getCalleeValue(), Out);
      for (Expr *A : P->getArgs())
        addUsed(A, Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IndirectCallStmt>(S)) {
      addUsed(I->getCall(), Out);
      return;
    }
    if (auto *A = llvm::dyn_cast<AssignStmt>(S)) {
      if (A->isCompound())
        addUsed(A->getTarget(), Out);
      else
        addTargetReads(A->getTarget(), Out);
      addUsed(A->getValue(), Out);
      return;
    }
    if (auto *I = llvm::dyn_cast<IfStmt>(S)) {
      addUsed(I->getCond(), Out);
      addUsedStmt(I->getThen(), Out);
      addUsedStmt(I->getElse(), Out);
      return;
    }
    if (auto *W = llvm::dyn_cast<WhileStmt>(S)) {
      addUsed(W->getCond(), Out);
      addUsedStmt(W->getBody(), Out);
      return;
    }
    if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
      addUsedList(R->getBody(), Out);
      addUsed(R->getCond(), Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<ForStmt>(S)) {
      addUsed(F->getVar(), Out);
      addUsed(F->getInit(), Out);
      addUsed(F->getFinal(), Out);
      addUsedStmt(F->getBody(), Out);
      return;
    }
    if (auto *F = llvm::dyn_cast<ForInStmt>(S)) {
      addUsed(F->getVar(), Out);
      addUsed(F->getCollection(), Out);
      addUsedStmt(F->getBody(), Out);
      return;
    }
    if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
      addUsed(C->getSelector(), Out);
      for (CaseArm *A : C->getArms()) {
        for (CaseLabel *L : A->getLabels()) {
          addUsed(L->Lo, Out);
          addUsed(L->Hi, Out);
        }
        addUsedStmt(A->getBody(), Out);
      }
      addUsedList(C->getElseBody(), Out);
      return;
    }
    if (auto *W = llvm::dyn_cast<WithStmt>(S)) {
      for (WithContext *C : W->getRecords())
        addUsed(C->Record, Out);
      addUsedStmt(W->getBody(), Out);
      return;
    }
    if (auto *L = llvm::dyn_cast<LabeledStmt>(S)) {
      addUsedStmt(L->getBody(), Out);
      return;
    }
    if (auto *T = llvm::dyn_cast<TryStmt>(S)) {
      addUsedList(T->getBody(), Out);
      for (ExceptHandler *H : T->getHandlers())
        addUsedStmt(H->getBody(), Out);
      addUsedList(T->getExceptBody(), Out);
      return;
    }
    if (auto *R = llvm::dyn_cast<RaiseStmt>(S)) {
      addUsed(R->getObject(), Out);
      addUsed(R->getAddress(), Out);
      addUsed(R->getFrame(), Out);
      return;
    }
    if (auto *V = llvm::dyn_cast<VarDeclStmt>(S)) {
      addUsed(V->getInit(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DestructureStmt>(S)) {
      for (Expr *T : D->getTargets())
        addUsed(T, Out);
      addUsed(D->getValue(), Out);
      return;
    }
    if (auto *E = llvm::dyn_cast<ExprStmt>(S)) {
      addUsed(E->getExpr(), Out);
      return;
    }
    if (auto *S2 = llvm::dyn_cast<ScopeStmt>(S)) {
      addUsed(S2->getToken(), Out);
      addUsedList(S2->getBody(), Out);
      return;
    }
    if (auto *D = llvm::dyn_cast<DetachedStmt>(S)) {
      addUsed(D->getHandler(), Out);
      addUsed(D->getCall(), Out);
      return;
    }
    if (auto *Y = llvm::dyn_cast<YieldStmt>(S)) {
      addUsed(Y->getValue(), Out);
      return;
    }
  }

  void pinList(llvm::ArrayRef<Stmt *> Body, VarSet After) {
    for (auto It = Body.rbegin(); It != Body.rend(); ++It) {
      pinStmt(*It, After);
      addUsedStmt(*It, After);
    }
  }

  void pinStmt(Stmt *S, const VarSet &After) {
    if (!S)
      return;
    if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
      pinList(C->getBody(), After);
      return;
    }
    if (auto *T = llvm::dyn_cast<TryStmt>(S)) {
      VarSet Landing = After;
      for (ExceptHandler *H : T->getHandlers())
        addUsedStmt(H->getBody(), Landing);
      addUsedList(T->getExceptBody(), Landing);
      VarSet Written;
      addWrittenList(T->getBody(), Written);
      for (VarDecl *V : Written)
        if (Landing.count(V))
          Pins.insert(V);
      pinList(T->getBody(), Landing);
      for (ExceptHandler *H : T->getHandlers())
        pinStmt(H->getBody(), After);
      pinList(T->getExceptBody(), After);
      return;
    }
    if (auto *I = llvm::dyn_cast<IfStmt>(S)) {
      pinStmt(I->getThen(), After);
      pinStmt(I->getElse(), After);
      return;
    }
    if (auto *W = llvm::dyn_cast<WhileStmt>(S)) {
      VarSet LoopAfter = After;
      addUsed(W->getCond(), LoopAfter);
      addUsedStmt(W->getBody(), LoopAfter);
      pinStmt(W->getBody(), LoopAfter);
      return;
    }
    if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
      VarSet LoopAfter = After;
      addUsedList(R->getBody(), LoopAfter);
      addUsed(R->getCond(), LoopAfter);
      pinList(R->getBody(), LoopAfter);
      return;
    }
    if (auto *F = llvm::dyn_cast<ForStmt>(S)) {
      VarSet LoopAfter = After;
      addUsed(F->getVar(), LoopAfter);
      addUsed(F->getInit(), LoopAfter);
      addUsed(F->getFinal(), LoopAfter);
      addUsedStmt(F->getBody(), LoopAfter);
      pinStmt(F->getBody(), LoopAfter);
      return;
    }
    if (auto *F = llvm::dyn_cast<ForInStmt>(S)) {
      VarSet LoopAfter = After;
      addUsed(F->getVar(), LoopAfter);
      addUsed(F->getCollection(), LoopAfter);
      addUsedStmt(F->getBody(), LoopAfter);
      pinStmt(F->getBody(), LoopAfter);
      return;
    }
    if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
      for (CaseArm *A : C->getArms())
        pinStmt(A->getBody(), After);
      pinList(C->getElseBody(), After);
      return;
    }
    if (auto *W = llvm::dyn_cast<WithStmt>(S)) {
      pinStmt(W->getBody(), After);
      return;
    }
    if (auto *L = llvm::dyn_cast<LabeledStmt>(S)) {
      pinStmt(L->getBody(), After);
      return;
    }
    if (auto *S2 = llvm::dyn_cast<ScopeStmt>(S)) {
      pinList(S2->getBody(), After);
      return;
    }
  }

public:
  explicit PinAnalysis(RoutineDecl *R) : R(R) {}

  VarSet run() {
    VarSet After;
    if (R->isFunction())
      After.insert(R->getResultVar());
    // Owned variables are freed by an implicit try-finally around the body;
    // its landing reads them, and any write to one happens inside it.
    bool Owned = false;
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
        // A file local is closed on the way out, after the landing.
        if (V->getType() && containsFile(V->getType()))
          After.insert(V);
        if (V->isOwned()) {
          After.insert(V);
          Owned = true;
        }
      }
    if (Owned) {
      VarSet Written;
      addWrittenStmt(R->getBody()->getBody(), Written);
      for (VarDecl *V : Written)
        if (After.count(V))
          Pins.insert(V);
    }
    pinStmt(R->getBody()->getBody(), After);
    return Pins;
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
  llvm::Value *CurPin = nullptr; ///< see pinInMemory
  /// The runtime's functions (declared here), and the functions that
  /// cannot raise an exception: see mayRaise.
  llvm::DenseSet<const llvm::Function *> RuntimeFns, NoRaiseFns;
  /// Are temporaries holding managed values pinned when they are made
  /// (see pinInMemory)? In a routine without try statements they are
  /// pinned later, if its body turns out to need protection.
  bool PinTemps = true;

  llvm::DenseMap<VarDecl *, llvm::GlobalVariable *> Globals;
  llvm::DenseMap<RoutineDecl *, llvm::Function *> Functions;
  llvm::DenseMap<RoutineDecl *, llvm::FunctionCallee> ExternalFns;
  llvm::DenseMap<RoutineDecl *, llvm::StructType *> FrameTypes;
  llvm::DenseMap<VarDecl *, unsigned> FrameSlots;
  /// The current routine's variables that are not in its frame record:
  /// their allocas.
  llvm::DenseMap<VarDecl *, llvm::Value *> LocalAddrs;
  /// Routines whose frame record starts with their static link (the
  /// others use their `link` argument).
  llvm::DenseSet<RoutineDecl *> LinkInFrame;
  llvm::DenseMap<Type *, llvm::Type *> LoweredTypes;
  llvm::DenseMap<FieldDecl *, uint64_t> FieldOffsets;
  /// The bit offsets of the ordinal fields of bitpacked records.
  llvm::DenseMap<FieldDecl *, uint64_t> FieldBitOffsets;
  /// Classes (objfpc): instance layouts (used only for offsets and sizes)
  /// and VMTs, in the order the classes were declared.
  llvm::DenseMap<ClassType *, llvm::StructType *> ClassLayouts;
  llvm::DenseMap<ClassType *, llvm::GlobalVariable *> VMTs;
  std::vector<ClassType *> VMTsToDefine;
  /// Every class declared in this module (item 11: the whole-program class
  /// hierarchy), and those with an interface visible to separately
  /// compiled units.
  llvm::SmallVector<ClassType *, 16> AllClasses;
  llvm::DenseSet<ClassType *> ClassSet, ExportedClasses;
  llvm::DenseMap<WithContext *, llvm::Value *> WithAddrs;
  llvm::StringMap<llvm::GlobalVariable *> StringConstants;
  /// Item 13: runtime-error messages and the location records and file
  /// strings the checks share.
  llvm::StringMap<llvm::Constant *> ErrorMessages, FileStrings;
  llvm::StringMap<llvm::GlobalVariable *> LocationRecords;
  /// The names of an enumeration's values (for write), by type.
  llvm::DenseMap<EnumType *, llvm::GlobalVariable *> EnumNameTables;
  llvm::StringMap<llvm::GlobalVariable *> AnsiConstants;
  std::vector<RoutineDecl *> RoutinesToEmit;
  llvm::DenseMap<LabelDecl *, llvm::BasicBlock *> LabelBlocks;
  /// Enclosing loops (Turbo/FPC): the targets of continue and break, and
  /// the number of Cleanups around the loop.
  struct LoopTargets {
    llvm::BasicBlock *Continue, *Break;
    size_t Cleanups;
  };
  llvm::SmallVector<LoopTargets, 4> Loops;
  llvm::BasicBlock *ExitBlock = nullptr; ///< target of `exit`

  /// Exceptions (objfpc): the try statements and running handlers around
  /// the statement being emitted, innermost last. exit, break and continue
  /// leaving them pop their PasExcFrames (emitJump):
  ///   TryBody  the protected statements of try..except: pop Frame
  ///   Handler  an exception handler: pop Frame (the handler's), free Object
  ///   Finally  the protected statements of try..finally: pop Frame, then
  ///            run the finally part with State = 2 + k, after which it
  ///            continues at Exits[k] (State 0: normal end, 1: exception)
  struct Cleanup {
    enum Kind { TryBody, Handler, Finally } K;
    llvm::Value *Frame;
    llvm::Value *Object = nullptr;
    llvm::Value *State = nullptr;
    llvm::BasicBlock *FinallyBB = nullptr;
    llvm::SmallVector<llvm::BasicBlock *, 2> Exits;
    Cleanup(Kind K, llvm::Value *Frame, llvm::Value *Object = nullptr,
            llvm::Value *State = nullptr, llvm::BasicBlock *FinallyBB = nullptr)
        : K(K), Frame(Frame), Object(Object), State(State),
          FinallyBB(FinallyBB) {}
  };
  std::vector<Cleanup> Cleanups;
  /// The try statements' frames of the running exception handlers,
  /// innermost last: a bare `raise` raises the exception of the last.
  llvm::SmallVector<llvm::Value *, 2> HandledFrames;
  /// `on E: TFoo do`: the variable E's storage.
  llvm::DenseMap<VarDecl *, llvm::Value *> HandlerVars;

  /// Reference counting (COM interfaces). A function's COM interface result
  /// is held in a temporary until the statement ends (LiveTemps: the
  /// current statements', innermost last). A routine releases the
  /// references in its value parameters, local variables and temporaries
  /// (RoutineTemps) when it ends, also when an exception leaves it.
  llvm::SmallVector<llvm::Value *, 4> LiveTemps;
  /// The short-string temporaries made by the current statement, and their
  /// sizes: lifetime.start at creation, lifetime.end when it ends (item 5).
  llvm::SmallVector<std::pair<llvm::Value *, uint64_t>, 8> StringTemps;
  llvm::SmallVector<llvm::Value *, 4> RoutineTemps;
  llvm::DenseMap<llvm::Value *, Type *> RecordTemps; ///< record temporaries' types
  /// Conservative integer ranges (optimization2.md item 1): active for-loop
  /// control variables, and values computed from them and constants.
  struct ValueRange {
    bool Known = false;
    int64_t Lo = 0, Hi = 0;
  };
  llvm::DenseMap<VarDecl *, ValueRange> VarRanges;
  llvm::DenseMap<const llvm::Value *, ValueRange> ValueRanges;
  /// The exact class of an object pointer when it is a direct constructor
  /// result (optimization2 item 10).
  llvm::DenseMap<const llvm::Value *, ClassType *> KnownClass;
  /// A direct call whose ansistring result is moved into the assignment
  /// target: no temporary holds it (NoHoldAnsiResult), and the call that
  /// returned it took that path (TookAnsiResult).
  bool NoHoldAnsiResult = false;
  bool TookAnsiResult = false;
  /// The unwind edge of the innermost eligible table-based try body, or
  /// null. emitCallOrInvoke makes a call an invoke when this is set.
  llvm::BasicBlock *TableUnwind = nullptr;
  /// Classes whose cleanup functions (releasing their own fields' COM
  /// interfaces) are to be emitted.
  std::vector<std::pair<ClassType *, llvm::Function *>> CleanupsToEmit;
  /// Dynamic arrays: the PasDynInfo of each array type, and the functions
  /// that release or count an element, to be emitted.
  llvm::DenseMap<Type *, llvm::GlobalVariable *> DynInfos;
  struct DynOp {
    Type *Elem;
    llvm::Function *F;
    bool IsFini;
  };
  std::vector<DynOp> DynOpsToEmit;

  // Debug information (-g).
  std::unique_ptr<llvm::DIBuilder> DIB;
  llvm::DICompileUnit *CU = nullptr;
  llvm::DIFile *DIUnitFile = nullptr;
  llvm::DISubprogram *CurSP = nullptr;
  llvm::DenseMap<Type *, llvm::DIType *> DITypes;
  llvm::DenseMap<RoutineDecl *, llvm::DISubprogram *> Subprograms;
  llvm::StringMap<llvm::DIFile *> DIFiles;
  llvm::DenseMap<std::pair<llvm::DISubprogram *, llvm::DIFile *>,
                 llvm::DILexicalBlockFile *> FileScopes;

  llvm::DIFile *diFileFor(SourceLocation L) {
    PresumedLoc P = SM.getPresumedLoc(L);
    if (!P.isValid())
      return DIUnitFile;
    llvm::DIFile *&F = DIFiles[P.Filename];
    if (!F) {
      llvm::SmallString<256> Abs(P.Filename);
      llvm::sys::fs::make_absolute(Abs);
      F = DIB->createFile(llvm::sys::path::filename(Abs),
                          llvm::sys::path::parent_path(Abs));
    }
    return F;
  }
  /// Separate units (S11): the symbols of units' interface variables and
  /// routines, and those of units compiled separately (declared only).
  llvm::DenseMap<Decl *, std::string> ExportNames;
  llvm::DenseSet<Decl *> ImportedDecls;

  void collectExports(UnitDecl &U, bool Imported) {
    for (Decl *D : withNested(U.getInterface()->getDecls())) {
      if (ClassType *C = definedClass(D)) {
        // A class: its VMT and its methods.
        ExportedClasses.insert(C);
        std::string Prefix =
            ("pas." + U.getName().Name + "$" + C->getDeclName().lower()).str();
        ExportNames[D] = Prefix + "$vmt";
        if (Imported)
          ImportedDecls.insert(D);
        for (Decl *M : C->getMembers()) {
          if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // a class variable
            ExportNames[V] = Prefix + "." + V->getName().Name.str();
            if (Imported)
              ImportedDecls.insert(V);
            continue;
          }
          // (a generic method: each module compiles the specializations
          // it uses)
          if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
              R && !R->isAbstract() && !R->isGenericMethod()) {
            ExportNames[R] = Prefix + "." + R->getName().Name.str() +
                             overloadSuffix(R);
            if (Imported)
              ImportedDecls.insert(R);
          }
        }
        continue;
      }
      if (ClassType *MT = advancedRecord(D)) { // an advanced record: methods
        std::string Prefix =
            ("pas." + U.getName().Name + "$" + MT->getDeclName().lower()).str();
        for (Decl *M : MT->getMembers()) {
          if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // a class variable
            ExportNames[V] = Prefix + "." + V->getName().Name.str();
            if (Imported)
              ImportedDecls.insert(V);
            continue;
          }
          if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
            ExportNames[R] = Prefix + "." + R->getName().Name.str() +
                             overloadSuffix(R);
            if (Imported)
              ImportedDecls.insert(R);
          }
        }
        continue;
      }
      if (definedInterface(D)) { // an interface: its iid
        ExportNames[D] = ("pas." + U.getName().Name + "$" +
                          D->getName().Name + "$iid").str();
        if (Imported)
          ImportedDecls.insert(D);
        continue;
      }
      auto *R = llvm::dyn_cast<RoutineDecl>(D);
      if (!llvm::isa<VarDecl>(D) && !(R && !R->isExternal() && !R->isGeneric()))
        continue;
      if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->isExternal())
        continue; // a C variable keeps its symbol
      ExportNames[D] = ("pas." + U.getName().Name + "$" + D->getName().Name).str() +
                       (R ? overloadSuffix(R) : std::string());
      if (Imported)
        ImportedDecls.insert(D);
    }
  }

  static std::string initName(UnitDecl &U) {
    return ("pas." + U.getName().Name + "$$init").str();
  }

  /// A unit-level or program variable's global (or a class variable's,
  /// pas.<class>.<name>).
  llvm::GlobalVariable *createGlobal(VarDecl *V, llvm::Type *Ty,
                                     llvm::Constant *Init) {
    auto It = ExportNames.find(V);
    if (It == ExportNames.end())
      return new llvm::GlobalVariable(
          M, Ty, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage, Init,
          V->getVarClass() ? "pas." + V->getVarClass()->getDeclName().lower() +
                                 "." + V->getName().Name.str()
                           : "pas." + V->getName().Name.str());
    bool Imported = ImportedDecls.count(V);
    auto *G = new llvm::GlobalVariable(M, Ty, /*isConstant=*/false,
                                       llvm::GlobalValue::ExternalLinkage,
                                       Imported ? nullptr : Init, It->second);
    return G;
  }

  llvm::DenseMap<RoutineDecl *, unsigned> JmpBufSlots; ///< frame slot
  llvm::GlobalVariable *MainJmpBuf = nullptr;

  /// Generous for every supported libc (glibc: 200-312 bytes, macOS: 192).
  llvm::Type *jmpBufType() { return llvm::ArrayType::get(B.getInt64Ty(), 64); }

  /// The interface D defines (not a forward declaration), or null.
  static InterfaceType *definedInterface(Decl *D) {
    auto *TD = llvm::dyn_cast<TypeDecl>(D);
    auto *IS = TD ? llvm::dyn_cast_or_null<InterfaceTypeSpec>(TD->getTypeSpec())
                  : nullptr;
    if (!IS || IS->isForward())
      return nullptr;
    return llvm::dyn_cast_or_null<InterfaceType>(TD->getType());
  }

  /// The class D defines (a type declaration `TFoo = class ... end`, not a
  /// forward one), or null.
  /// The member table of the advanced record D declares (or the type
  /// helper D declares), or null.
  static ClassType *advancedRecord(Decl *D) {
    auto *TD = llvm::dyn_cast<TypeDecl>(D);
    if (TD && llvm::isa_and_nonnull<HelperTypeSpec>(TD->getTypeSpec()))
      return llvm::dyn_cast_or_null<ClassType>(TD->getType());
    auto *RT = TD ? llvm::dyn_cast_or_null<RecordType>(TD->getType()) : nullptr;
    return RT ? RT->getMemberTable() : nullptr;
  }

  /// Does E name a type (TFoo, TOuter.TInner)?
  static bool namesType(Expr *E) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
      return llvm::isa_and_nonnull<TypeDecl>(N->getDecl());
    auto *F = llvm::dyn_cast<FieldExpr>(E);
    return F && llvm::isa_and_nonnull<TypeDecl>(F->getMember());
  }

  /// Decls, each followed by the types declared in it when it is a class
  /// or an advanced record (recursively).
  static llvm::SmallVector<Decl *, 16> withNested(llvm::ArrayRef<Decl *> Decls) {
    llvm::SmallVector<Decl *, 16> Out;
    std::function<void(Decl *)> Add = [&](Decl *D) {
      Out.push_back(D);
      ClassType *C = definedClass(D);
      if (!C)
        C = advancedRecord(D);
      if (C)
        for (Decl *M : C->getMembers())
          if (llvm::isa<TypeDecl>(M))
            Add(M);
    };
    for (Decl *D : Decls)
      Add(D);
    return Out;
  }

  static ClassType *definedClass(Decl *D) {
    auto *TD = llvm::dyn_cast<TypeDecl>(D);
    auto *CS = TD ? llvm::dyn_cast_or_null<ClassTypeSpec>(TD->getTypeSpec())
                  : nullptr;
    if (!CS || CS->isForward())
      return nullptr;
    return llvm::dyn_cast_or_null<ClassType>(TD->getType());
  }

  static bool hasNonLocalLabels(Block &Blk) {
    for (Decl *D : Blk.getDecls())
      if (auto *L = llvm::dyn_cast<LabelDecl>(D); L && L->isNonLocal())
        return true;
    return false;
  }

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
    case Type::TK_Integer:
      return B.getIntNTy(llvm::cast<IntegerType>(T)->getBits());
    case Type::TK_Real:    return B.getDoubleTy();
    case Type::TK_Boolean: return B.getInt8Ty();
    case Type::TK_Char:    return B.getInt8Ty();
    case Type::TK_WideChar: return B.getInt16Ty();
    case Type::TK_Enum:    return B.getInt32Ty();
    case Type::TK_Subrange:
      return memType(llvm::cast<SubrangeType>(T)->getHost());
    case Type::TK_String:
      return llvm::ArrayType::get(
          B.getInt8Ty(), llvm::cast<StringLiteralType>(T)->getLength());
    case Type::TK_Array: {
      auto *A = llvm::cast<ArrayType>(T);
      OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
      uint64_t N = static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1);
      if (A->hasBitElements()) // the elements' bits, in whole bytes
        return llvm::ArrayType::get(
            B.getInt8Ty(), (N * bitPackedWidth(A->getElementType()) + 7) / 8);
      return llvm::ArrayType::get(memType(A->getElementType()), N);
    }
    case Type::TK_Record: {
      auto *R = llvm::cast<RecordType>(T);
      std::string Name = T->getDeclName().empty()
                             ? std::string("record")
                             : ("record." + T->getDeclName()).str();
      if (R->isBitPacked()) {
        uint64_t Bits = layoutBitPacked(R->getFieldList(), 0);
        return llvm::StructType::create(
            M.getContext(), {llvm::ArrayType::get(B.getInt8Ty(), (Bits + 7) / 8)},
            Name);
      }
      if (unsigned N = R->getFieldAlignment()) {
        // Free Pascal's layout under {$packrecords N} (packed: 1), as the
        // record's bytes in units of its alignment (which LLVM then gives
        // it as a field or an element).
        uint64_t End = 0, Align = 1;
        layoutAligned(R->getFieldList(), N, End, Align);
        uint64_t Size = (End + Align - 1) / Align * Align;
        return llvm::StructType::create(
            M.getContext(),
            {llvm::ArrayType::get(B.getIntNTy(static_cast<unsigned>(Align * 8)),
                                  Size / Align)},
            Name);
      }
      return layoutFieldList(R->getFieldList(), Name, /*BaseOffset=*/0);
    }
    case Type::TK_Pointer:
    case Type::TK_Nil:
    case Type::TK_File: // a PasFile* owned by the runtime
      return B.getPtrTy();
    case Type::TK_Set:
      return setType(T);
    case Type::TK_ShortString: // length byte + chars
      return llvm::ArrayType::get(
          B.getInt8Ty(), llvm::cast<ShortStringType>(T)->getMaxLength() + 1);
    case Type::TK_Routine: // {function, static link}
      return llvm::StructType::get(B.getPtrTy(), B.getPtrTy());
    case Type::TK_Interface: // {object, method table}
      return llvm::StructType::get(B.getPtrTy(), B.getPtrTy());
    case Type::TK_DynArray: // its elements, or null when empty
    case Type::TK_OpenArray: // its {data, count} pair
    case Type::TK_ArrayOfConst: // its header slot
    case Type::TK_AnsiString: // its characters, or null when empty
    case Type::TK_ClassRef: // a reference to the class's VMT
    case Type::TK_Class: // a reference to the object
    case Type::TK_Task:  // a counted reference to the task
    case Type::TK_Token: // a counted reference to the token
    case Type::TK_Iterator: // a counted reference to the generator
      return B.getPtrTy();
    case Type::TK_Error:
    case Type::TK_Untyped: // (untyped parameters are passed by reference)
      break;
    }
    llvm_unreachable("type has no storage representation");
  }

  /// Sets are bit masks: bit i is set iff ord(i) is a member. 32, 64 or
  /// 256 bits, by the element type's highest value (SetType::
  /// getStorageBits; optimization.md, item 6).
  static unsigned setBits(Type *T) {
    return llvm::cast<SetType>(T)->getStorageBits();
  }
  llvm::IntegerType *setType(Type *T) { return B.getIntNTy(setBits(T)); }
  /// The set value V of type From as a value of the set type To (the
  /// members that do not fit are dropped).
  llvm::Value *convertSet(llvm::Value *V, Type *From, Type *To) {
    (void)From;
    return B.CreateZExtOrTrunc(V, setType(To));
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

  /// The alignment of a value of type T (a field's natural alignment).
  uint64_t alignOf(Type *T) { return DL.getABITypeAlign(memType(T)).value(); }

  /// Lays out the fields of FL from byte End as Free Pascal does under
  /// {$packrecords N}: each field at a multiple of the smaller of its
  /// alignment and N; a variant part (each variant from the same offset) at
  /// a multiple of the smaller of N and 16. End becomes the end of the
  /// last field; Align the largest alignment used.
  void layoutAligned(FieldList *FL, uint64_t N, uint64_t &End,
                     uint64_t &Align) {
    auto Place = [&](FieldDecl *F) {
      uint64_t A = std::min(alignOf(F->getType()), N);
      End = (End + A - 1) / A * A;
      FieldOffsets[F] = End;
      End += sizeOf(F->getType());
      Align = std::max(Align, A);
    };
    for (FieldDecl *F : FL->Fixed)
      Place(F);
    VariantPart *VP = FL->Variant;
    if (!VP)
      return;
    if (VP->Tag)
      Place(VP->Tag);
    uint64_t VA = std::min<uint64_t>(N, 16);
    uint64_t Start = (End + VA - 1) / VA * VA, Max = Start;
    for (Variant *V : VP->Variants) {
      uint64_t E = Start;
      layoutAligned(V->Fields, N, E, Align);
      Max = std::max(Max, E);
    }
    End = Max;
  }

  /// Lays out the fields of a bitpacked record from bit Start, as Free
  /// Pascal does: ordinals take their bitPackedWidth bits, one after
  /// another; other fields and the variant part start at a byte. Returns
  /// the bit after the last field.
  uint64_t layoutBitPacked(FieldList *FL, uint64_t Start) {
    uint64_t Bit = Start;
    auto Place = [&](FieldDecl *F) {
      if (F->getType()->isOrdinal()) {
        FieldBitOffsets[F] = Bit;
        Bit += bitPackedWidth(F->getType());
        return;
      }
      Bit = (Bit + 7) / 8 * 8;
      FieldOffsets[F] = Bit / 8;
      Bit += sizeOf(F->getType()) * 8;
    };
    for (FieldDecl *F : FL->Fixed)
      Place(F);
    if (VariantPart *VP = FL->Variant) {
      if (VP->Tag)
        Place(VP->Tag);
      uint64_t VarStart = (Bit + 7) / 8 * 8, End = VarStart;
      for (Variant *V : VP->Variants)
        End = std::max(End, (layoutBitPacked(V->Fields, VarStart) + 7) / 8 * 8);
      Bit = End;
    }
    return Bit;
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

  /// What pointer arithmetic on a PT counts in: its pointee (bytes, for an
  /// untyped pointer).
  llvm::Type *pointeeMemType(PointerType *PT) {
    return PT->isUntyped() || !PT->getPointee() ? B.getInt8Ty()
                                                : memType(PT->getPointee());
  }

  /// The layout of an object of class C: VMT pointer, then all fields.
  /// Records each field's offset; a prefix of fields has the same offsets
  /// in every descendant.
  llvm::StructType *classLayout(ClassType *C) {
    if (llvm::StructType *ST = ClassLayouts.lookup(C))
      return ST;
    llvm::SmallVector<llvm::Type *, 8> Elems{B.getPtrTy()};
    for (FieldDecl *F : C->getAllFields())
      Elems.push_back(memType(F->getType()));
    llvm::StructType *ST = llvm::StructType::create(
        M.getContext(), Elems, ("class." + C->getDeclName()).str());
    const llvm::StructLayout *SL = DL.getStructLayout(ST);
    for (size_t I = 0; I < C->getAllFields().size(); ++I)
      FieldOffsets[C->getAllFields()[I]] = SL->getElementOffset(I + 1);
    ClassLayouts[C] = ST;
    return ST;
  }

  uint64_t instanceSize(ClassType *C) {
    return DL.getTypeAllocSize(classLayout(C));
  }

  //===--------------------------------------------------------------------===//
  // Scalars in memory
  //===--------------------------------------------------------------------===//

  /// The addresses (in the current function) of fields and elements that
  /// a field alignment ({$packrecords}, packed) leaves less aligned than
  /// their types: the alignment they have. Loads, stores and copies
  /// through them say so.
  llvm::DenseMap<llvm::Value *, uint64_t> AddrAligns;

  /// The alignment of the value of type T at address P.
  uint64_t addrAlign(llvm::Value *P, Type *T) {
    auto It = AddrAligns.find(P);
    return It != AddrAligns.end() ? It->second : alignOf(T);
  }

  /// Notes that the component of type T at Addr, Offset bytes into a value
  /// aligned to BaseAlign, may be less aligned than T.
  void noteComponentAlign(llvm::Value *Addr, uint64_t BaseAlign,
                          uint64_t Offset, Type *T) {
    uint64_t A = Offset ? std::min(BaseAlign, Offset & (~Offset + 1))
                        : BaseAlign;
    if (A < alignOf(T))
      AddrAligns[Addr] = A;
  }

  llvm::Value *loadScalar(llvm::Value *Ptr, Type *T, const llvm::Twine &Name = "") {
    llvm::LoadInst *Raw = B.CreateLoad(memType(T), Ptr, Name);
    if (auto It = AddrAligns.find(Ptr); It != AddrAligns.end())
      Raw->setAlignment(llvm::Align(It->second));
    return T->getBase()->isBoolean() ? B.CreateTrunc(Raw, B.getInt1Ty()) : Raw;
  }

  void storeScalar(llvm::Value *Ptr, llvm::Value *Val, Type *T) {
    if (T->getBase()->isBoolean())
      Val = B.CreateZExt(Val, B.getInt8Ty());
    llvm::StoreInst *S = B.CreateStore(Val, Ptr);
    if (auto It = AddrAligns.find(Ptr); It != AddrAligns.end())
      S->setAlignment(llvm::Align(It->second));
  }

  /// Copies the array, record or string T: both are aligned as T is (or as
  /// AddrAligns says).
  void copyAggregate(llvm::Value *Dst, llvm::Value *Src, Type *T) {
    B.CreateMemCpy(Dst, llvm::Align(addrAlign(Dst, T)), Src,
                   llvm::Align(addrAlign(Src, T)), sizeOf(T));
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

  /// The .rterr global of a message (shared by every check that uses it).
  llvm::Constant *errorMessage(llvm::StringRef Msg) {
    llvm::Constant *&Text = ErrorMessages[Msg];
    if (!Text)
      Text = B.CreateGlobalString(Msg, ".rterr");
    return Text;
  }

  /// The .file global of a source file name (one per file, item 13).
  llvm::Constant *fileString(llvm::StringRef File) {
    llvm::Constant *&Text = FileStrings[File];
    if (!Text)
      Text = B.CreateGlobalString(File, ".file");
    return Text;
  }

  /// The pooled { file, line, column } record of L, or null (item 13).
  llvm::Constant *locationRecord(SourceLocation L) {
    llvm::Type *Ptr = B.getPtrTy();
    PresumedLoc P = SM.getPresumedLoc(L);
    if (!P.isValid())
      return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(Ptr));
    std::string Key =
        (P.Filename + ":" + llvm::Twine(P.Line) + ":" + llvm::Twine(P.Column))
            .str();
    llvm::GlobalVariable *&G = LocationRecords[Key];
    if (!G) {
      auto *Ty = llvm::StructType::get(M.getContext(), {Ptr, B.getInt32Ty(),
                                                        B.getInt32Ty()});
      llvm::Constant *Init = llvm::ConstantStruct::get(
          Ty, {fileString(P.Filename), B.getInt32(P.Line),
               B.getInt32(P.Column)});
      G = new llvm::GlobalVariable(M, Ty, /*isConstant=*/true,
                                   llvm::GlobalValue::PrivateLinkage, Init,
                                   ".loc");
      G->setAlignment(llvm::Align(8));
    }
    return G;
  }

  /// __pas_runtime_error_at("<Msg>", &loc); unreachable. Callers must set
  /// a new insertion point afterwards.
  void emitRuntimeError(llvm::StringRef Msg, SourceLocation L) {
    B.CreateCall(RT.RuntimeErrorAt, {errorMessage(Msg), locationRecord(L)});
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

  /// Are range / overflow checks on at L? (command line, then {$R±}/{$Q±})
  bool rangeChecks(SourceLocation L) const {
    return !CGOpts.Directives
               ? CGOpts.RangeChecks
               : CGOpts.Directives->isEnabled(CheckDirectives::Range, L,
                                              CGOpts.RangeChecks);
  }
  /// In an unchecked block at L: integer values converted to a narrower
  /// integer type wrap instead of being range-checked.
  bool wraps(SourceLocation L) const {
    return CGOpts.Directives &&
           CGOpts.Directives->isEnabled(CheckDirectives::Wrap, L, false);
  }
  /// In a checked block or expression at L (Wrap explicitly off): integer
  /// values converted to a narrower integer type are checked, whatever
  /// {$R} says.
  bool checksConversions(SourceLocation L) const {
    return CGOpts.Directives && CGOpts.Directives->stateAt(
                                    CheckDirectives::Wrap, L) == CheckDirectives::Off;
  }
  bool overflowChecks(SourceLocation L) const {
    return !CGOpts.Directives
               ? CGOpts.OverflowChecks
               : CGOpts.Directives->isEnabled(CheckDirectives::Overflow, L,
                                              CGOpts.OverflowChecks);
  }

  /// Ordinal values compare as signed if their host is a signed integer
  /// type, unsigned otherwise (byte, word, cardinal, char, boolean, enum).
  static bool isSignedOrdinal(Type *T) { return T->isSignedIntegerLike(); }

  /// Converts an ordinal register value of type From to the register type of
  /// To (sign- or zero-extending by From, or truncating). No range check.
  llvm::Value *convertOrdinal(llvm::Value *V, Type *From, Type *To) {
    llvm::Type *Dst = regType(To);
    if (V->getType() == Dst)
      return V;
    if (From->getBase()->isWideChar() && To->getBase()->isChar())
      return wideToChar(V);
    if (Dst->isIntegerTy(1))
      return B.CreateICmpNE(V, llvm::ConstantInt::get(V->getType(), 0));
    return isSignedOrdinal(From) ? B.CreateSExtOrTrunc(V, Dst)
                                 : B.CreateZExtOrTrunc(V, Dst);
  }

  /// A widechar's code as a char, as Free Pascal converts it: '?' above
  /// 255.
  llvm::Value *wideToChar(llvm::Value *V) {
    llvm::Value *Fits = B.CreateICmpULE(V, B.getInt16(255));
    return B.CreateSelect(Fits, B.CreateTrunc(V, B.getInt8Ty()), B.getInt8('?'),
                          "tochar");
  }

  /// A char or widechar expression's value as a char.
  llvm::Value *emitCharValue(Expr *E) {
    llvm::Value *V = visitExpr(E);
    return E->getType()->getBase()->isWideChar() ? wideToChar(V) : V;
  }

  /// An integer expression as i32 (field widths, string positions).
  llvm::Value *emitI32(Expr *E) {
    return B.CreateTrunc(toI64(visitExpr(E), E->getType()), B.getInt32Ty());
  }

  static unsigned intBits(Type *T) {
    return llvm::cast<IntegerType>(T->getBase())->getBits();
  }

  /// Widens an ordinal register value to i64 for range comparisons.
  llvm::Value *toI64(llvm::Value *V, Type *T) {
    return isSignedOrdinal(T) ? B.CreateSExt(V, B.getInt64Ty())
                              : B.CreateZExt(V, B.getInt64Ty());
  }

  /// The range of an already emitted integer value: a literal, a load of an
  /// active loop variable, or a result noted by the arithmetic below.
  ValueRange rangeOfValue(const llvm::Value *V) const {
    auto It = ValueRanges.find(V);
    if (It != ValueRanges.end())
      return It->second;
    if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(V))
      if (C->getBitWidth() <= 64)
        return {true, C->getSExtValue(), C->getSExtValue()};
    return {};
  }

  void noteRange(const llvm::Value *V, ValueRange R) {
    if (R.Known)
      ValueRanges[V] = R;
  }

  /// The bounds of a signed integer type of Bits bits.
  static bool signedRange(unsigned Bits, ValueRange &R) {
    if (Bits == 0 || Bits > 64)
      return false;
    if (Bits == 64) {
      R = {true, INT64_MIN, INT64_MAX};
      return true;
    }
    R = {true, -(int64_t(1) << (Bits - 1)), (int64_t(1) << (Bits - 1)) - 1};
    return true;
  }

  static bool inInt64(__int128 V) {
    return V >= INT64_MIN && V <= INT64_MAX;
  }

  /// The range of A op B (add, sub or mul), when it fits int64.
  static bool combineRange(BinaryOp Op, ValueRange A, ValueRange B,
                           ValueRange &R) {
    if (!A.Known || !B.Known)
      return false;
    __int128 Lo, Hi;
    switch (Op) {
    case BinaryOp::Add:
      Lo = (__int128)A.Lo + B.Lo;
      Hi = (__int128)A.Hi + B.Hi;
      break;
    case BinaryOp::Sub:
      Lo = (__int128)A.Lo - B.Hi;
      Hi = (__int128)A.Hi - B.Lo;
      break;
    case BinaryOp::Mul: {
      __int128 P1 = (__int128)A.Lo * B.Lo, P2 = (__int128)A.Lo * B.Hi;
      __int128 P3 = (__int128)A.Hi * B.Lo, P4 = (__int128)A.Hi * B.Hi;
      Lo = std::min(std::min(P1, P2), std::min(P3, P4));
      Hi = std::max(std::max(P1, P2), std::max(P3, P4));
      break;
    }
    default: return false;
    }
    if (!inInt64(Lo) || !inInt64(Hi))
      return false;
    R = {true, (int64_t)Lo, (int64_t)Hi};
    return true;
  }

  /// X < Lo or X > Hi, for an i64 X.
  ///
  /// Throughout this file, instructions are created one statement at a
  /// time, never as two arguments of one call: C++ leaves the order of
  /// argument evaluation unspecified, and the IR must not depend on the
  /// compiler that built p3 (the self-hosted compiler writes the same IR,
  /// SELFHOST.md S10).
  llvm::Value *outsideRange(llvm::Value *X, int64_t Lo, int64_t Hi) {
    // One unsigned comparison: (uint64)(X - Lo) > (uint64)(Hi - Lo). The
    // subtraction wraps, so X < Lo becomes a huge offset.
    llvm::Value *Off = B.CreateSub(X, B.getInt64(Lo));
    llvm::Value *Span = B.CreateSub(B.getInt64(Hi), B.getInt64(Lo));
    return B.CreateICmpUGT(Off, Span);
  }

  /// Checks that ordinal value V (of type From) lies within Target's bounds.
  void emitRangeCheck(llvm::Value *V, Type *From, Type *Target,
                      SourceLocation L,
                      llvm::StringRef Msg = "value out of range") {
    // An integer converted to an integer type (not a subrange): wraps in an
    // unchecked block, is checked (EIntOverflow) in a checked one whatever
    // {$R} says.
    bool IntConv = llvm::isa<IntegerType>(Target) && From->isIntegerLike();
    bool Checked = IntConv && checksConversions(L);
    if ((!rangeChecks(L) && !Checked) || !Target->isOrdinal())
      return;
    if (IntConv && wraps(L))
      return;
    auto Fail = [&](llvm::Value *Cond) {
      if (Checked)
        emitOverflowCheck(Cond, "integer overflow", L);
      else
        emitRuntimeCheck(Cond, Msg, L);
    };
    OrdinalBounds TB = getOrdinalBounds(Target);
    OrdinalBounds FB = getOrdinalBounds(From);
    if (isSignedOrdinal(From) && isSignedOrdinal(Target)) {
      ValueRange R = rangeOfValue(V);
      if (R.Known && R.Lo >= TB.Lo && R.Hi <= TB.Hi)
        return;
    }
    bool FromQWord = isQWord(From->getBase()), ToQWord = isQWord(Target);
    if (FromQWord) { // its values, unsigned: those above Target's
      if (ToQWord)
        return;
      llvm::Value *X = toI64(V, From);
      llvm::Value *Above = B.CreateICmpUGT(X, B.getInt64(TB.Hi));
      if (TB.Lo > 0) {
        llvm::Value *Below = B.CreateICmpULT(X, B.getInt64(TB.Lo));
        Above = B.CreateOr(Below, Above);
      }
      Fail(Above);
      return;
    }
    // (to qword, TB is 0..INT64_MAX: the negative values are caught)
    if (FB.Lo >= TB.Lo && FB.Hi <= TB.Hi)
      return; // every value of From fits
    llvm::Value *X = toI64(V, From);
    Fail(outsideRange(X, TB.Lo, TB.Hi));
  }

  /// Converts a value for assignment / parameter passing: integer -> real,
  /// and range-checks ordinal values (and set members) against the target.
  llvm::Value *convertForAssign(llvm::Value *V, Type *From, Type *To,
                                SourceLocation L) {
    if (auto *I = llvm::dyn_cast<InterfaceType>(To)) {
      if (From->isNil())
        return llvm::Constant::getNullValue(memType(To));
      if (From->isClass())
        return toInterface(V, I);
      return V; // an interface (the same or a descendant)
    }
    if (To->isReal() && !From->isReal())
      return toDouble(V, From);
    if (To->isOrdinal() && From->isOrdinal()) {
      if (From->getBase()->isWideChar() && To->getBase()->isChar()) {
        V = wideToChar(V); // then checked as a char
        From = To->getBase();
      }
      emitRangeCheck(V, From, To, L);
      return convertOrdinal(V, From, To);
    }
    if (auto *TS = llvm::dyn_cast<SetType>(To); TS && rangeChecks(L)) {
      OrdinalBounds TB = getOrdinalBounds(TS->getElementType());
      auto *FS = llvm::cast<SetType>(From);
      OrdinalBounds FB = FS->isEmptySet()
                             ? TB
                             : getOrdinalBounds(FS->getElementType()->getBase());
      if (FS->isEmptySet() || (FB.Lo >= TB.Lo && FB.Hi <= TB.Hi))
        return convertSet(V, From, To);
      // (in From's representation: a member beyond it cannot be there)
      unsigned Bits = setBits(From);
      llvm::Value *Outside =
          B.CreateAnd(V, B.CreateNot(constRangeMask(TB.Lo, TB.Hi, Bits)));
      emitRuntimeCheck(
          B.CreateICmpNE(Outside, llvm::ConstantInt::get(B.getIntNTy(Bits), 0)),
          "set member out of range", L);
    }
    if (To->isSet())
      return convertSet(V, From, To);
    return V;
  }

  //===--------------------------------------------------------------------===//
  // Short strings (Turbo/FPC): values are pointers to length-prefixed bytes
  //===--------------------------------------------------------------------===//

  llvm::GlobalVariable *getPStrConstant(llvm::StringRef S) {
    std::string Bytes(1, static_cast<char>(std::min<size_t>(S.size(), 255)));
    Bytes += S.take_front(255).str();
    return getStringConstant(Bytes);
  }

  /// A string constant as an ansistring: static characters after a header
  /// {count -1, length}; null when empty.
  llvm::Constant *getAnsiConstant(llvm::StringRef S) {
    if (S.empty())
      return llvm::ConstantPointerNull::get(B.getPtrTy());
    llvm::GlobalVariable *&G = AnsiConstants[S];
    if (!G) {
      std::string Bytes(8, '\xff');
      uint64_t N = S.size();
      for (int I = 0; I < 8; ++I)
        Bytes.push_back(static_cast<char>(N >> (8 * I)));
      Bytes += S.str();
      Bytes.push_back('\0');
      auto *Init = llvm::ConstantDataArray::getString(M.getContext(), Bytes,
                                                      /*AddNull=*/false);
      G = new llvm::GlobalVariable(M, Init->getType(), true,
                                   llvm::GlobalValue::PrivateLinkage, Init,
                                   ".astr");
      G->setAlignment(llvm::Align(8));
    }
    return llvm::ConstantExpr::getInBoundsGetElementPtr(B.getInt8Ty(), G,
                                                        B.getInt64(16));
  }

  /// A call of an ansistring routine of the runtime on Args.
  llvm::Value *astrCall(llvm::StringRef Name, llvm::Type *Ret,
                        llvm::ArrayRef<llvm::Value *> Args) {
    llvm::SmallVector<llvm::Type *, 4> Params;
    for (llvm::Value *A : Args)
      Params.push_back(A->getType());
    return B.CreateCall(classRuntime(Name, Ret, Params), Args);
  }

  /// Any string-like expression, or a pchar, as an ansistring (borrowed: a
  /// new one is held until the statement ends).
  llvm::Value *emitAnsi(Expr *E) {
    Type *T = E->getType();
    if (T->isAnsiString())
      return visitExpr(E);
    if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(E))
      return getAnsiConstant(SL->getValue());
    if (auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(E)))
      if (auto *C = llvm::dyn_cast<ConstDecl>(N->getDecl());
          C && C->getValue().K == ConstValue::String)
        return getAnsiConstant(C->getValue().Str);
    llvm::Type *Ptr = B.getPtrTy();
    if (T->isCharLike())
      return holdResult(astrCall("__pas_astr_from_char", Ptr, {emitCharValue(E)}));
    if (T->isPointer())
      return holdResult(astrCall("__pas_astr_from_cstr", Ptr, {visitExpr(E)}));
    return holdResult(astrCall("__pas_astr_from_pstr", Ptr, {emitPStr(E)}));
  }

  static bool isMethodPointerType(Type *T) {
    auto *RT = llvm::dyn_cast<RoutineType>(T);
    return RT && RT->isOfObject();
  }

  /// slice(a, n): the first n elements of a, as an open array.
  llvm::Value *emitSlice(CallExpr *C) {
    Expr *A = C->getArgs()[0], *NE = C->getArgs()[1];
    llvm::Value *Data = emitAddress(A);
    llvm::Value *Count;
    if (auto *AT = llvm::dyn_cast<ArrayType>(A->getType())) {
      OrdinalBounds Bd = getOrdinalBounds(AT->getIndexType());
      Count = B.getInt64(Bd.Hi - Bd.Lo + 1);
    } else {
      Count = openCount(openDescriptor(A));
    }
    llvm::Value *N = toI64(visitExpr(NE), NE->getType());
    if (rangeChecks(NE->getBeginLoc()))
      emitRuntimeCheck(B.CreateICmpUGT(N, Count), "slice length out of bounds",
                       NE->getBeginLoc());
    return newOpenDescriptor(Data, N);
  }

  /// The descriptor {data, count} of the open array parameter E.
  llvm::Value *openDescriptor(Expr *E) {
    auto *V = llvm::cast<VarDecl>(llvm::cast<NameRefExpr>(E)->getDecl());
    return B.CreateLoad(B.getPtrTy(), varAddress(V), "open");
  }

  llvm::Value *openCount(llvm::Value *Desc) {
    return B.CreateLoad(B.getInt64Ty(),
                        B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Desc, 1),
                        "count");
  }

  llvm::Value *newOpenDescriptor(llvm::Value *Data, llvm::Value *Count) {
    llvm::Value *Desc =
        createTemp(llvm::ArrayType::get(B.getInt64Ty(), 2), "open");
    B.CreateStore(Data, Desc);
    B.CreateStore(Count, B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Desc, 1));
    return Desc;
  }

  /// The open array passed for Arg: another one, [...] built in a
  /// temporary, an array, or a string constant.
  llvm::Value *emitOpenArrayArg(Expr *Arg, OpenArrayType *OA) {
    Type *T = Arg->getType();
    if (auto *S = llvm::dyn_cast<SetCtorExpr>(Arg)) // typed as the parameter
      return emitOpenArrayCtor(S, OA);
    if (auto *C = llvm::dyn_cast<CallExpr>(Arg)) // no function returns one
      return emitSlice(C);
    if (T == OA)
      return openDescriptor(Arg);
    if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
      OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
      llvm::Value *Data = emitAddress(Arg);
      return newOpenDescriptor(Data, B.getInt64(Bd.Hi - Bd.Lo + 1));
    }
    if (T->isDynArray()) {
      llvm::Value *Data = visitExpr(Arg);
      return newOpenDescriptor(Data, dynLength(Data));
    }
    if (T->isStringLiteral())
      return newOpenDescriptor(emitAddress(Arg), B.getInt64(T->getStringLength()));
    llvm::Value *C = createTemp(B.getInt8Ty(), "chr"); // a char constant
    B.CreateStore(visitExpr(Arg), C);
    return newOpenDescriptor(C, B.getInt64(1));
  }

  llvm::Value *emitOpenArrayCtor(SetCtorExpr *S, OpenArrayType *OA) {
    Type *Elem = OA->getElementType();
    auto Els = S->getElements();
    if (Els.empty())
      return newOpenDescriptor(llvm::ConstantPointerNull::get(B.getPtrTy()),
                               B.getInt64(0));
    llvm::Type *ArrTy = llvm::ArrayType::get(memType(Elem), Els.size());
    llvm::Value *Arr = createTemp(ArrTy, "elems");
    for (size_t I = 0; I < Els.size(); ++I) {
      Expr *E = Els[I].Lo;
      llvm::Value *Slot =
          B.CreateInBoundsGEP(ArrTy, Arr, {B.getInt64(0), B.getInt64(I)});
      // Borrowed: the callee counts what it keeps.
      if (auto *SS = llvm::dyn_cast<ShortStringType>(Elem))
        emitStringAssign(Slot, SS, E);
      else if (Elem->isAnsiString())
        B.CreateStore(emitAnsi(E), Slot);
      else if (Elem->isStructured())
        copyAggregate(Slot, emitAddress(E), Elem);
      else if (llvm::Constant *C = cStringFor(Elem, E))
        B.CreateStore(C, Slot);
      else
        storeScalar(Slot, convertForAssign(visitExpr(E), E->getType(), Elem,
                                           E->getBeginLoc()),
                    Elem);
    }
    return newOpenDescriptor(Arr, B.getInt64(Els.size()));
  }

  /// A value parameter's own copy of the open array in frame slot Slot.
  void copyOpenArray(llvm::Value *Slot, OpenArrayType *OA) {
    llvm::Type *ET = memType(OA->getElementType());
    llvm::Value *Desc = B.CreateLoad(B.getPtrTy(), Slot, "open");
    llvm::Value *Data = B.CreateLoad(B.getPtrTy(), Desc, "data");
    llvm::Value *N = openCount(Desc);
    llvm::Value *Copy = B.CreateAlloca(ET, N, "copy");
    B.CreateMemCpy(Copy, llvm::MaybeAlign(1), Data, llvm::MaybeAlign(1),
                   B.CreateMul(N, B.getInt64(sizeOf(OA->getElementType()))));
    B.CreateStore(newOpenDescriptor(Copy, N), Slot);
  }

  /// Runs Fn on the address of each element of the open array in slot Slot.
  void forEachOpenElement(llvm::Value *Slot, OpenArrayType *OA,
                          llvm::function_ref<void(llvm::Value *)> Fn,
                          llvm::StringRef What) {
    llvm::Value *Desc = B.CreateLoad(B.getPtrTy(), Slot, "open");
    llvm::Value *Data = B.CreateLoad(B.getPtrTy(), Desc, "data");
    llvm::Value *N = openCount(Desc);
    llvm::BasicBlock *Pre = B.GetInsertBlock();
    auto *Loop = newBlock(What + ".loop");
    auto *Done = newBlock(What + ".done");
    B.CreateCondBr(B.CreateICmpEQ(N, B.getInt64(0)), Done, Loop);
    B.SetInsertPoint(Loop);
    llvm::PHINode *I = B.CreatePHI(B.getInt64Ty(), 2, "i");
    I->addIncoming(B.getInt64(0), Pre);
    Fn(B.CreateInBoundsGEP(memType(OA->getElementType()), Data, I));
    llvm::Value *Next = B.CreateAdd(I, B.getInt64(1));
    I->addIncoming(Next, B.GetInsertBlock());
    B.CreateCondBr(B.CreateICmpEQ(Next, N), Done, Loop);
    B.SetInsertPoint(Done);
  }

  /// The array of const passed for Arg: another array of const, or [...]
  /// built in a temporary: a header slot {count}, then {VType, value}.
  llvm::Value *emitConstArray(Expr *Arg) {
    auto *S = llvm::dyn_cast<SetCtorExpr>(Arg);
    if (!S)
      return visitExpr(Arg);
    auto Els = S->getElements();
    llvm::Value *Tmp = createTemp(
        llvm::ArrayType::get(B.getInt64Ty(), 2 * (Els.size() + 1)), "args");
    B.CreateStore(B.getInt64(Els.size()), Tmp);
    uint64_t Slot = 2;
    for (const SetElement &El : Els) {
      int64_t Kind;
      llvm::Value *V = emitConstArrayValue(El.Lo, Kind);
      B.CreateStore(B.getInt64(Kind),
                    B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Tmp, Slot));
      B.CreateStore(V, B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Tmp, Slot + 1));
      Slot += 2;
    }
    return Tmp;
  }

  /// An element's value as stored in its TVarRec (Kind: its VType).
  llvm::Value *emitConstArrayValue(Expr *E, int64_t &Kind) {
    Type *T = E->getType();
    if (T->isStringLiteral() || T->isAnsiString()) {
      Kind = 11; // vtAnsiString
      return emitAnsi(E);
    }
    if (T->isShortString() || T->isStringType()) {
      Kind = 4; // vtString
      return emitPStr(E);
    }
    if (T->isReal() || T->isIntegerLike()) {
      llvm::Value *V = visitExpr(E);
      if (T->isIntegerLike() && intBits(T) != 64) {
        Kind = 0; // vtInteger
        return toI64(V, T);
      }
      Kind = T->isReal() ? 3 : 16; // vtExtended, vtInt64: by reference
      llvm::Value *Tmp = createTemp(V->getType(), "arg");
      B.CreateStore(V, Tmp);
      return Tmp;
    }
    if (T->getBase()->isChar() || T->getBase()->isBoolean()) {
      Kind = T->getBase()->isChar() ? 2 : 1; // vtChar, vtBoolean
      return B.CreateZExt(visitExpr(E), B.getInt64Ty());
    }
    Kind = T->isClass() ? 7 : T->isClassRef() ? 8 : 5; // vtObject, vtClass, vtPointer
    if (auto *P = llvm::dyn_cast<PointerType>(T);
        P && !P->isUntyped() && P->getPointee() && P->getPointee()->isChar())
      Kind = 6; // vtPChar
    return visitExpr(E);
  }

  llvm::Value *newStringTemp(unsigned MaxLen = 255) {
    llvm::Value *Tmp =
        createTemp(llvm::ArrayType::get(B.getInt8Ty(), MaxLen + 1), "str");
    uint64_t Size = MaxLen + 1;
    B.CreateLifetimeStart(Tmp, B.getInt64(Size));
    StringTemps.push_back({Tmp, Size});
    return Tmp;
  }

  /// Ends the lifetime of the short-string temporaries made since Mark.
  void releaseStringTemps(size_t Mark) {
    for (size_t I = Mark; I < StringTemps.size(); ++I)
      B.CreateLifetimeEnd(StringTemps[I].first,
                          B.getInt64(StringTemps[I].second));
    StringTemps.resize(Mark);
  }

  /// Any string-like expression as a pointer to a length-prefixed string.
  llvm::Value *emitPStr(Expr *E) {
    Type *T = E->getType();
    if (T->isShortString())
      return visitExpr(E); // designators yield their address, results a temp
    if (T->isAnsiString()) {
      llvm::Value *S = visitExpr(E);
      llvm::Value *Tmp = newStringTemp();
      astrCall("__pas_astr_to_pstr", B.getVoidTy(), {Tmp, B.getInt32(255), S});
      return Tmp;
    }
    if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(E))
      return getPStrConstant(SL->getValue());
    if (auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(E)))
      if (auto *C = llvm::dyn_cast<ConstDecl>(N->getDecl());
          C && C->getValue().K == ConstValue::String)
        return getPStrConstant(C->getValue().Str);
    if (T->isCharLike()) {
      llvm::Value *Tmp = newStringTemp(1);
      B.CreateStore(B.getInt8(1), Tmp);
      llvm::Value *C = emitCharValue(E);
      B.CreateStore(C, B.CreateConstGEP1_64(B.getInt8Ty(), Tmp, 1));
      return Tmp;
    }
    // packed array [1..n] of char
    uint64_t N = T->getStringLength();
    llvm::Value *Tmp = newStringTemp(static_cast<unsigned>(std::min<uint64_t>(N, 255)));
    B.CreateStore(B.getInt8(static_cast<uint8_t>(std::min<uint64_t>(N, 255))), Tmp);
    llvm::Value *Dst = B.CreateConstGEP1_64(B.getInt8Ty(), Tmp, 1);
    llvm::Value *Src = emitAddress(E);
    B.CreateMemCpy(Dst, llvm::MaybeAlign(1), Src, llvm::MaybeAlign(1),
                   std::min<uint64_t>(N, 255));
    return Tmp;
  }

  void emitStringAssign(llvm::Value *Dst, ShortStringType *T, Expr *Value) {
    B.CreateCall(RT.StrAssign,
                 {Dst, B.getInt32(T->getMaxLength()), emitPStr(Value)});
  }

  llvm::Value *emitStringFunction(CallExpr *E, BuiltinKind K) {
    auto Args = E->getArgs();
    switch (K) {
    case BuiltinKind::Length: {
      Type *T = Args[0]->getType();
      if (T->isOpenArray())
        return B.CreateTrunc(openCount(openDescriptor(Args[0])), B.getInt32Ty());
      if (T->isDynArray())
        return B.CreateTrunc(dynLength(visitExpr(Args[0])), B.getInt32Ty());
      if (T->isArrayOfConst())
        return B.CreateTrunc(B.CreateLoad(B.getInt64Ty(), visitExpr(Args[0])),
                             B.getInt32Ty());
      if (T->isAnsiString())
        return astrCall("__pas_astr_length", B.getInt32Ty(), {visitExpr(Args[0])});
      if (T->isShortString())
        return B.CreateZExt(B.CreateLoad(B.getInt8Ty(), emitPStr(Args[0])),
                            B.getInt32Ty(), "length");
      if (T->isCharLike()) {
        visitExpr(Args[0]);
        return B.getInt32(1);
      }
      return B.getInt32(static_cast<uint32_t>(T->getStringLength()));
    }
    case BuiltinKind::UpCase: {
      llvm::Value *C = visitExpr(Args[0]); // a char or a widechar
      auto Ch = [&](uint64_t X) { return llvm::ConstantInt::get(C->getType(), X); };
      llvm::Value *GeA = B.CreateICmpUGE(C, Ch('a'));
      llvm::Value *LeZ = B.CreateICmpULE(C, Ch('z'));
      llvm::Value *IsLower = B.CreateAnd(GeA, LeZ);
      return B.CreateSelect(IsLower, B.CreateSub(C, Ch(32)), C, "upcase");
    }
    case BuiltinKind::Pos:
      if (Args[0]->getType()->isAnsiString() || Args[1]->getType()->isAnsiString()) {
        llvm::Value *Sub = emitAnsi(Args[0]);
        llvm::Value *S = emitAnsi(Args[1]);
        return astrCall("__pas_astr_pos", B.getInt32Ty(), {Sub, S});
      }
      return B.CreateCall(RT.StrPos, {emitPStr(Args[0]), emitPStr(Args[1])});
    case BuiltinKind::Copy: {
      if (Args[0]->getType()->isDynArray()) {
        llvm::Value *A = visitExpr(Args[0]);
        llvm::Value *Start = Args.size() > 1
                                 ? toI64(visitExpr(Args[1]), Args[1]->getType())
                                 : B.getInt64(0);
        llvm::Value *Count = Args.size() > 2
                                 ? toI64(visitExpr(Args[2]), Args[2]->getType())
                                 : B.getInt64(INT64_MAX);
        return holdResult(astrCall("__pas_dyn_copy", B.getPtrTy(), {A, Start, Count}),
                          Args[0]->getType());
      }
      if (Args[0]->getType()->isAnsiString()) {
        llvm::Value *S = visitExpr(Args[0]);
        llvm::Value *Index = emitI32(Args[1]);
        llvm::Value *Count = emitI32(Args[2]);
        return holdResult(astrCall("__pas_astr_copy", B.getPtrTy(), {S, Index, Count}));
      }
      llvm::Value *Tmp = newStringTemp();
      B.CreateCall(RT.StrCopy, {Tmp, emitPStr(Args[0]), emitI32(Args[1]),
                                emitI32(Args[2])});
      return Tmp;
    }
    default: { // Concat
      if (E->getType()->isAnsiString()) {
        llvm::Value *R = emitAnsi(Args[0]);
        for (Expr *A : Args.drop_front()) {
          llvm::Value *S = emitAnsi(A);
          R = holdResult(astrCall("__pas_astr_concat", B.getPtrTy(), {R, S}));
        }
        return R;
      }
      llvm::Value *Tmp = newStringTemp();
      B.CreateCall(RT.StrAssign, {Tmp, B.getInt32(255), emitPStr(Args[0])});
      for (Expr *A : Args.drop_front())
        B.CreateCall(RT.StrConcat, {Tmp, Tmp, emitPStr(A)});
      return Tmp;
    }
    }
  }

  //===--------------------------------------------------------------------===//
  // Sets
  //===--------------------------------------------------------------------===//

  /// The members Lo..Hi (within 0..255) as a set value of Bits bits.
  llvm::Constant *constRangeMask(int64_t Lo, int64_t Hi, unsigned Bits) {
    llvm::APInt M = llvm::APInt::getBitsSet(256, static_cast<unsigned>(Lo),
                                            static_cast<unsigned>(Hi) + 1);
    return llvm::ConstantInt::get(B.getIntNTy(Bits), M.trunc(Bits));
  }

  /// Ordinal value -> bit index of a set of Bits bits, checking 0..255
  /// under range checks. (A set constructor's representation holds every
  /// value of its elements' type that is in 0..255.)
  llvm::Value *setIndex(llvm::Value *V, Type *T, SourceLocation L,
                        unsigned Bits) {
    llvm::Value *X = toI64(V, T);
    OrdinalBounds Bd = getOrdinalBounds(T);
    if (rangeChecks(L) && (Bd.Lo < 0 || Bd.Hi > 255))
      emitRuntimeCheck(B.CreateICmpUGT(X, B.getInt64(255)),
                       "set element out of range 0..255", L);
    llvm::Value *Masked = B.CreateAnd(X, B.getInt64(Bits - 1));
    return B.CreateZExtOrTrunc(Masked, B.getIntNTy(Bits));
  }

  llvm::Value *emitSetCtor(SetCtorExpr *E) {
    unsigned Bits = setBits(E->getType());
    llvm::IntegerType *Ty = B.getIntNTy(Bits);
    llvm::Value *Result = llvm::ConstantInt::get(Ty, 0);
    llvm::Value *One = llvm::ConstantInt::get(Ty, 1);
    llvm::Value *AllOnes = llvm::ConstantInt::getAllOnesValue(Ty);
    for (const SetElement &El : E->getElements()) {
      Type *LT = El.Lo->getType();
      llvm::Value *Lo = visitExpr(El.Lo);
      if (!El.Hi) {
        Result = B.CreateOr(
            Result, B.CreateShl(One, setIndex(Lo, LT, El.Lo->getBeginLoc(), Bits)));
        continue;
      }
      llvm::Value *Hi = visitExpr(El.Hi);
      Type *HT = El.Hi->getType();
      // Bits Lo..Hi, or nothing if Lo > Hi.
      llvm::Value *Empty = isSignedOrdinal(LT) ? B.CreateICmpSGT(Lo, Hi)
                                               : B.CreateICmpUGT(Lo, Hi);
      llvm::Value *LoI = setIndex(Lo, LT, El.Lo->getBeginLoc(), Bits);
      llvm::Value *HiI = setIndex(Hi, HT, El.Hi->getBeginLoc(), Bits);
      llvm::Value *IsTop =
          B.CreateICmpEQ(HiI, llvm::ConstantInt::get(Ty, Bits - 1));
      llvm::Value *Below = B.CreateSub(B.CreateShl(One, B.CreateAdd(HiI, One)), One);
      llvm::Value *HiMask = B.CreateSelect(IsTop, AllOnes, Below);
      llvm::Value *LoMask = B.CreateNot(B.CreateSub(B.CreateShl(One, LoI), One));
      llvm::Value *Range = B.CreateSelect(
          Empty, llvm::ConstantInt::get(Ty, 0), B.CreateAnd(HiMask, LoMask));
      Result = B.CreateOr(Result, Range);
    }
    return Result;
  }

  /// x in [a, b..c, ...] without building the set (optimization.md, item
  /// 6). The members given by constants are tested together: one value or
  /// one range by comparisons; values at most 63 apart by a bit of a
  /// 64-bit mask; up to four ranges by comparisons; else the bit of the
  /// 256-bit set. Each other element is a comparison (a range: two). All
  /// elements are evaluated, in order, after x.
  llvm::Value *emitInCtor(llvm::Value *X, SetCtorExpr *S) {
    llvm::APInt Members(256, 0);
    llvm::Value *Result = nullptr;
    auto Or = [&](llvm::Value *V) {
      Result = Result ? B.CreateOr(Result, V, "in") : V;
    };
    // An element as an i64, checked like a set member (0..255).
    auto Element = [&](Expr *El) {
      Type *T = El->getType();
      llvm::Value *V = toI64(visitExpr(El), T);
      OrdinalBounds Bd = getOrdinalBounds(T);
      if (!llvm::isa<llvm::ConstantInt>(V) && rangeChecks(El->getBeginLoc()) &&
          (Bd.Lo < 0 || Bd.Hi > 255))
        emitRuntimeCheck(B.CreateICmpUGT(V, B.getInt64(255)),
                         "set element out of range 0..255", El->getBeginLoc());
      return V;
    };
    for (const SetElement &El : S->getElements()) {
      llvm::Value *Lo = Element(El.Lo);
      llvm::Value *Hi = El.Hi ? Element(El.Hi) : Lo;
      auto *CLo = llvm::dyn_cast<llvm::ConstantInt>(Lo);
      auto *CHi = llvm::dyn_cast<llvm::ConstantInt>(Hi);
      if (CLo && CHi) {
        int64_t L = std::max<int64_t>(CLo->getSExtValue(), 0);
        int64_t H = std::min<int64_t>(CHi->getSExtValue(), 255);
        if (L <= H)
          Members.setBits(static_cast<unsigned>(L), static_cast<unsigned>(H) + 1);
        continue;
      }
      if (!El.Hi) {
        Or(B.CreateICmpEQ(X, Lo, "in"));
        continue;
      }
      llvm::Value *AtLeast = B.CreateICmpSGE(X, Lo);
      llvm::Value *AtMost = B.CreateICmpSLE(X, Hi);
      Or(B.CreateAnd(AtLeast, AtMost, "in"));
    }
    if (!Members.isZero()) {
      // The constant members: their ranges, lowest first.
      llvm::SmallVector<std::pair<int64_t, int64_t>, 8> Ranges;
      for (unsigned I = 0; I < 256; ++I) {
        if (!Members[I])
          continue;
        unsigned J = I;
        while (J + 1 < 256 && Members[J + 1])
          ++J;
        Ranges.push_back({I, J});
        I = J;
      }
      int64_t Min = Ranges.front().first, Max = Ranges.back().second;
      auto InRange = [&](int64_t L, int64_t H) -> llvm::Value * {
        if (L == H)
          return B.CreateICmpEQ(X, B.getInt64(L), "in");
        llvm::Value *AtLeast = B.CreateICmpSGE(X, B.getInt64(L));
        llvm::Value *AtMost = B.CreateICmpSLE(X, B.getInt64(H));
        return B.CreateAnd(AtLeast, AtMost, "in");
      };
      if (Ranges.size() == 1) {
        Or(InRange(Min, Max));
      } else if (Max - Min < 64) {
        llvm::Value *Idx = B.CreateSub(X, B.getInt64(Min));
        llvm::Value *Inside = B.CreateICmpULE(Idx, B.getInt64(Max - Min));
        uint64_t Mask = Members.lshr(static_cast<unsigned>(Min)).getLoBits(64)
                            .getZExtValue();
        llvm::Value *Bit = B.CreateTrunc(
            B.CreateLShr(B.getInt64(Mask), B.CreateAnd(Idx, B.getInt64(63))),
            B.getInt1Ty());
        Or(B.CreateAnd(Inside, Bit, "in"));
      } else if (Ranges.size() <= 4) {
        for (auto [L, H] : Ranges)
          Or(InRange(L, H));
      } else {
        llvm::Value *Inside = B.CreateICmpULE(X, B.getInt64(255));
        llvm::Value *Idx =
            B.CreateZExt(B.CreateAnd(X, B.getInt64(255)), B.getIntNTy(256));
        llvm::Value *Bit = B.CreateTrunc(
            B.CreateLShr(llvm::ConstantInt::get(B.getIntNTy(256), Members), Idx),
            B.getInt1Ty());
        Or(B.CreateAnd(Inside, Bit, "in"));
      }
    }
    if (!Result)
      return B.getFalse(); // x in []
    return Result;
  }

  /// x in s  ==  x is within s's representation and bit x of s is set
  llvm::Value *emitIn(BinaryExpr *E) {
    Type *XT = E->getLHS()->getType();
    llvm::Value *X = toI64(visitExpr(E->getLHS()), XT);
    if (auto *S = llvm::dyn_cast<SetCtorExpr>(E->getRHS()))
      return emitInCtor(X, S);
    llvm::Value *S = visitExpr(E->getRHS());
    unsigned Bits = setBits(E->getRHS()->getType());
    llvm::Value *InRange = B.CreateICmpULE(X, B.getInt64(Bits - 1));
    llvm::Value *Masked = B.CreateAnd(X, B.getInt64(Bits - 1));
    llvm::Value *Idx = B.CreateZExtOrTrunc(Masked, S->getType());
    llvm::Value *Bit = B.CreateTrunc(B.CreateLShr(S, Idx), B.getInt1Ty());
    return B.CreateAnd(InRange, Bit, "in");
  }

  llvm::Value *toDouble(llvm::Value *V, Type *T) {
    if (T->isReal())
      return V;
    return isSignedOrdinal(T) ? B.CreateSIToFP(V, B.getDoubleTy(), "toreal")
                              : B.CreateUIToFP(V, B.getDoubleTy(), "toreal");
  }

  /// L op R (Add, Sub or Mul) in a signed or unsigned integer type, with
  /// an overflow check if enabled.
  llvm::Value *emitCheckedArith(BinaryOp Op, bool Signed, llvm::Value *L,
                                llvm::Value *R, SourceLocation Loc,
                                const llvm::Twine &Name) {
    llvm::Intrinsic::ID ID;
    switch (Op) {
    case BinaryOp::Add:
      ID = Signed ? llvm::Intrinsic::sadd_with_overflow
                  : llvm::Intrinsic::uadd_with_overflow;
      break;
    case BinaryOp::Sub:
      ID = Signed ? llvm::Intrinsic::ssub_with_overflow
                  : llvm::Intrinsic::usub_with_overflow;
      break;
    default:
      ID = Signed ? llvm::Intrinsic::smul_with_overflow
                  : llvm::Intrinsic::umul_with_overflow;
      break;
    }
    return emitCheckedArith(ID, L, R, Loc, Name);
  }

  /// L op R with an overflow check (s/u add/sub/mul.with.overflow).
  llvm::Value *emitCheckedArith(llvm::Intrinsic::ID ID, llvm::Value *L,
                                llvm::Value *R, SourceLocation Loc,
                                const llvm::Twine &Name) {
    BinaryOp Op = ID == llvm::Intrinsic::sadd_with_overflow ||
                          ID == llvm::Intrinsic::uadd_with_overflow
                      ? BinaryOp::Add
                  : ID == llvm::Intrinsic::ssub_with_overflow ||
                            ID == llvm::Intrinsic::usub_with_overflow
                      ? BinaryOp::Sub
                      : BinaryOp::Mul;
    bool Signed = ID == llvm::Intrinsic::sadd_with_overflow ||
                  ID == llvm::Intrinsic::ssub_with_overflow ||
                  ID == llvm::Intrinsic::smul_with_overflow;
    ValueRange ResultRange;
    // (subtraction is left alone for now: it is a separate range rule)
    bool Safe = Signed && Op != BinaryOp::Sub &&
                combineRange(Op, rangeOfValue(L), rangeOfValue(R), ResultRange);
    if (Safe) {
      auto *T = llvm::cast<llvm::IntegerType>(L->getType());
      ValueRange TypeRange;
      if (!signedRange(T->getBitWidth(), TypeRange) ||
          ResultRange.Lo < TypeRange.Lo || ResultRange.Hi > TypeRange.Hi)
        Safe = false;
    }
    if (!overflowChecks(Loc) || Safe) {
      llvm::Value *Result = Op == BinaryOp::Add   ? B.CreateAdd(L, R, Name)
                            : Op == BinaryOp::Sub ? B.CreateSub(L, R, Name)
                                                  : B.CreateMul(L, R, Name);
      if (Safe)
        noteRange(Result, ResultRange);
      return Result;
    }
    llvm::Value *Res = B.CreateBinaryIntrinsic(ID, L, R);
    emitOverflowCheck(B.CreateExtractValue(Res, 1), "integer overflow", Loc);
    llvm::Value *Result = B.CreateExtractValue(Res, 0, Name);
    if (Signed) {
      auto *T = llvm::cast<llvm::IntegerType>(L->getType());
      ValueRange TypeRange;
      if (signedRange(T->getBitWidth(), TypeRange))
        noteRange(Result, TypeRange);
    }
    return Result;
  }

  /// if (Cond) integer overflow (What): a runtime error, or (objfpc) an
  /// EIntOverflow exception, as in Free Pascal with SysUtils.
  void emitOverflowCheck(llvm::Value *Cond, const std::string &What,
                         SourceLocation L) {
    if (!LangOpts.ObjKeywords) {
      emitRuntimeCheck(Cond, What, L);
      return;
    }
    auto *ErrBB = newBlock("rt.error");
    auto *ContBB = newBlock("rt.ok");
    llvm::MDBuilder MDB(M.getContext());
    B.CreateCondBr(Cond, ErrBB, ContBB, MDB.createUnlikelyBranchWeights());
    B.SetInsertPoint(ErrBB);
    B.CreateCall(excRuntime("__pas_raise_overflow"), {whereText(L)});
    B.CreateUnreachable();
    B.SetInsertPoint(ContBB);
  }

  //===--------------------------------------------------------------------===//
  // Variables and frames
  //===--------------------------------------------------------------------===//

  /// Parameters passed as a pointer to the caller's variable: var
  /// parameters, const parameters of string and structured types, and
  /// constref parameters (to the caller's variable, or a temporary).
  static bool isByRef(VarDecl *V) {
    return (V->isVarParam() && !V->getType()->isArrayOfConst() &&
            !V->getType()->isOpenArray()) ||
           isConstRefPtr(V) ||
           V->getType()->isUntyped() ||
           (V->isConstParam() && V->getType()->isStructured()) ||
           (V->getVarKind() == VarDecl::Result && returnsByPointer(V->getType()));
  }

  /// A constref parameter passed as a pointer where a const one is not:
  /// of a type other than structured, short string, open array, array of
  /// const or procedural (those are passed as for const).
  static bool isConstRefPtr(VarDecl *P) {
    Type *T = P->getType();
    return P->isConstRefParam() && !T->isStructured() && !T->isShortString() &&
           !T->isOpenArray() && !T->isArrayOfConst() && !T->isUntyped() &&
           !llvm::isa<RoutineType>(T);
  }

  /// Is a function result of type T written through a pointer the caller
  /// passes (to a temporary of its own), instead of being returned? Short
  /// strings are: as a value LLVM would move all their 256 bytes, one by
  /// one. The function's result variable is then that temporary.
  static bool returnsByPointer(Type *T) { return T && T->isShortString(); }

  /// pas.<outer>.<name> for routines, pas.<class>.<name> for methods.
  static std::string mangle(RoutineDecl *R) {
    if (R->isClassInit()) // (beside a method of the same name)
      return "pas." + R->getClass()->getDeclName().lower() + "." +
             R->getName().Name.str() +
             (R->getClassInit() == ClassInitKind::Constructor ? "$cctor"
                                                              : "$cdtor");
    if (R->isMethod())
      return "pas." + R->getClass()->getDeclName().lower() + "." +
             R->getName().Name.str() + overloadSuffix(R);
    if (RoutineDecl *P = R->getParent())
      return mangle(P) + "." + R->getName().Name.str() + overloadSuffix(R);
    return "pas." + R->getName().Name.str() + overloadSuffix(R);
  }

  /// "$N" for the N-th routine of an overloaded name (objfpc).
  static std::string overloadSuffix(RoutineDecl *R) {
    return R->isOverloaded() ? "$" + std::to_string(R->getOverloadIndex())
                             : std::string();
  }

  /// Pointer to the frame of routine Target, reached from the current
  /// routine by following static links.
  llvm::Value *framePtrFor(RoutineDecl *Target) {
    // A routine without a frame record: nothing reads the pointer.
    llvm::Value *Frame =
        CurFrame ? CurFrame : llvm::ConstantPointerNull::get(B.getPtrTy());
    for (RoutineDecl *R = CurRoutine; R != Target; R = R->getParent()) {
      assert(R && R->hasStaticLink() && "target is not an enclosing routine");
      if (!LinkInFrame.count(R)) { // only the current routine: no nested ones
        Frame = CurFn->getArg(0);
        continue;
      }
      llvm::Value *LinkSlot = B.CreateStructGEP(FrameTypes[R], Frame, 0);
      Frame = B.CreateLoad(B.getPtrTy(), LinkSlot, "link");
    }
    return Frame;
  }

  /// Address of the variable holding the PasFile* of input / output.
  llvm::GlobalVariable *standardFile(llvm::StringRef Name) {
    std::string Sym = ("__pas_" + Name).str();
    if (auto *G = M.getNamedGlobal(Sym))
      return G;
    return new llvm::GlobalVariable(M, B.getPtrTy(), false,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Sym);
  }

  llvm::Value *loadFile(llvm::Value *VarAddr) {
    return B.CreateLoad(B.getPtrTy(), VarAddr, "file");
  }
  llvm::Value *outputFile() { return loadFile(standardFile("output")); }
  llvm::Value *inputFile() { return loadFile(standardFile("input")); }

  /// Keeps the alloca Addr in memory: what is stored there before an
  /// exception is raised must be there when the routine continues after
  /// the _setjmp that catches it, where _longjmp has put the registers
  /// back. LLVM does not know that control arrives there from the
  /// protected code, and would read values it has in registers; once the
  /// address has been stored somewhere (a volatile store is never
  /// removed), it assumes that any call, _setjmp too, reads and writes
  /// the alloca. Used for the frame record of a routine that protects code
  /// and for the temporaries that hold managed values (read when an
  /// exception leaves the routine, or by a later statement).
  void pinInMemory(llvm::Value *Addr) {
    if (!CurPin)
      CurPin = createTemp(B.getPtrTy(), "pin");
    B.CreateStore(Addr, CurPin, /*isVolatile=*/true);
  }

  /// Allocates a temporary in the entry block of the current function.
  llvm::Value *createTemp(llvm::Type *Ty, const llvm::Twine &Name = "tmp") {
    llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                            CurFn->getEntryBlock().begin());
    return Entry.CreateAlloca(Ty, nullptr, Name);
  }

  /// Calls Fn(elementAddress) for every element of array A at Addr.
  void forEachElement(llvm::Value *Addr, ArrayType *A,
                      llvm::function_ref<void(llvm::Value *)> Fn,
                      llvm::StringRef What = "files") {
    OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
    uint64_t Count = static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1);
    llvm::BasicBlock *Pre = B.GetInsertBlock();
    auto *Loop = newBlock(What + ".loop");
    auto *Done = newBlock(What + ".done");
    B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
    llvm::PHINode *I = B.CreatePHI(B.getInt64Ty(), 2, "i");
    I->addIncoming(B.getInt64(0), Pre);
    Fn(B.CreateInBoundsGEP(memType(A), Addr, {B.getInt64(0), I}));
    llvm::Value *Next = B.CreateAdd(I, B.getInt64(1));
    I->addIncoming(Next, B.GetInsertBlock());
    B.CreateCondBr(B.CreateICmpEQ(Next, B.getInt64(Count)), Done, Loop);
    B.SetInsertPoint(Done);
  }

  /// Creates (Init) or releases (!Init) the PasFile objects of all files in
  /// an object of type T at Addr: plain files, arrays of files, and file
  /// fields of records (fixed parts).
  void emitFileLifetime(llvm::Value *Addr, Type *T, bool Init,
                        llvm::Value *Name = nullptr) {
    if (!containsFile(T))
      return;
    if (auto *FT = llvm::dyn_cast<FileType>(T)) {
      if (!Init) {
        B.CreateCall(RT.FileFree, {loadFile(Addr)});
        return;
      }
      if (!Name)
        Name = llvm::ConstantPointerNull::get(B.getPtrTy());
      // (an untyped file: kind 2, records of 128 bytes until reset/rewrite)
      llvm::Value *F = B.CreateCall(
          RT.FileNew,
          {B.getInt64(FT->isText() || FT->isUntyped()
                          ? 1
                          : sizeOf(FT->getElementType())),
           B.getInt32(FT->isUntyped() ? 2 : FT->isText()), Name});
      B.CreateStore(F, Addr);
      return;
    }
    if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
      forEachElement(Addr, A, [&](llvm::Value *Elem) {
        emitFileLifetime(Elem, A->getElementType(), Init);
      });
      return;
    }
    if (auto *R = llvm::dyn_cast<RecordType>(T))
      for (FieldDecl *F : R->getFieldList()->Fixed)
        emitFileLifetime(fieldAddress(Addr, R, F), F->getType(), Init);
  }

  /// Creates the PasFile objects for the file variables among Decls.
  /// ParamIndex maps program-parameter names to their argv position.
  void initFileVars(llvm::ArrayRef<Decl *> Decls,
                    const llvm::StringMap<unsigned> *ParamIndex) {
    for (Decl *D : Decls) {
      auto *V = llvm::dyn_cast<VarDecl>(D);
      if (!V || V->getAbsolute() || !containsFile(V->getType()))
        continue;
      llvm::Value *Name = nullptr;
      if (ParamIndex) {
        auto It = ParamIndex->find(V->getName().Name);
        if (It != ParamIndex->end())
          Name = B.CreateCall(RT.ProgramParam,
                              {B.getInt32(It->second),
                               B.CreateGlobalString(V->getName().Spelling)});
      }
      emitFileLifetime(varAddress(V), V->getType(), /*Init=*/true, Name);
    }
  }

  /// The C library's errno is per thread, not a variable: reached through
  /// the library's accessor (__errno_location, __error, _errno).
  static bool isErrno(VarDecl *V) { return V->getExternalName() == "errno"; }

  llvm::Value *errnoAddress() {
    llvm::Triple T(M.getTargetTriple());
    const char *Name = T.isOSDarwin()    ? "__error"
                       : T.isOSWindows() ? "_errno"
                                         : "__errno_location";
    return B.CreateCall(classRuntime(Name, B.getPtrTy(), {}), {}, "errno");
  }

  llvm::Value *varAddress(VarDecl *V) {
    if (Expr *Target = V->getAbsolute()) // an overlay has no storage
      return emitAddress(Target);
    if (llvm::Value *H = HandlerVars.lookup(V))
      return H;
    if (!V->getOwner()) {
      if (V->isPredeclared())
        return standardFile(V->getName().Name);
      if (V->isExternal() && isErrno(V))
        return errnoAddress();
      return Globals.lookup(V);
    }
    llvm::Value *Slot = varSlot(V);
    if (isByRef(V))
      return B.CreateLoad(B.getPtrTy(), Slot, V->getName().Name + ".ref");
    return Slot;
  }

  /// Where a routine's variable V is kept (a by-reference parameter: the
  /// pointer to it): a slot of its routine's frame record, or an alloca of
  /// the current routine.
  llvm::Value *varSlot(VarDecl *V) {
    RoutineDecl *Owner = V->getOwner();
    auto It = FrameSlots.find(V);
    if (It != FrameSlots.end())
      return B.CreateStructGEP(FrameTypes[Owner], framePtrFor(Owner),
                               It->second, V->getName().Name);
    llvm::Value *Slot = Owner == CurRoutine ? LocalAddrs.lookup(V) : nullptr;
    if (!Slot) // Sema marks every variable a nested routine can name
      llvm::report_fatal_error("internal compiler error: variable '" +
                               llvm::Twine(V->getName().Spelling) +
                               "' is not in the frame of its routine");
    return Slot;
  }

  //===--------------------------------------------------------------------===//
  // Dynamic arrays
  //===--------------------------------------------------------------------===//

  llvm::Value *dynLength(llvm::Value *A) {
    return astrCall("__pas_dyn_length", B.getInt64Ty(), {A});
  }

  /// The constant PasDynInfo of arrays of type T (runtime/include/pascalrt.h);
  /// for managed elements, with functions that release and count one.
  llvm::Constant *dynInfoOf(DynArrayType *T) {
    llvm::GlobalVariable *&G = DynInfos[T];
    if (G)
      return G;
    Type *Elem = T->getElementType();
    llvm::Constant *Fini = llvm::ConstantPointerNull::get(B.getPtrTy());
    llvm::Constant *AddRef = Fini;
    if (isManaged(Elem)) {
      auto Op = [&](const char *Name, bool IsFini) {
        auto *F = llvm::Function::Create(
            llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
            llvm::Function::InternalLinkage, Name, M);
        F->getArg(0)->setName("elem");
        DynOpsToEmit.push_back({Elem, F, IsFini});
        return F;
      };
      Fini = Op("dyn.fini", true);
      AddRef = Op("dyn.addref", false);
    }
    auto *Ty = llvm::StructType::get(B.getInt64Ty(), B.getPtrTy(), B.getPtrTy());
    G = new llvm::GlobalVariable(
        M, Ty, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantStruct::get(Ty, {B.getInt64(sizeOf(Elem)), Fini, AddRef}),
        "dyninfo");
    return G;
  }

  /// The constant PasDynInfo of values of type T (size, fini and addref,
  /// as for a dynamic array's elements): a task's result.
  llvm::Constant *typeInfoOf(Type *T) {
    llvm::GlobalVariable *&G = TypeInfos[T];
    if (G)
      return G;
    llvm::Constant *Fini = llvm::ConstantPointerNull::get(B.getPtrTy());
    llvm::Constant *AddRef = Fini;
    if (isManaged(T)) {
      auto Op = [&](const char *Name, bool IsFini) {
        auto *F = llvm::Function::Create(
            llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
            llvm::Function::InternalLinkage, Name, M);
        F->getArg(0)->setName("elem");
        DynOpsToEmit.push_back({T, F, IsFini});
        return F;
      };
      Fini = Op("type.fini", true);
      AddRef = Op("type.addref", false);
    }
    auto *Ty = llvm::StructType::get(B.getInt64Ty(), B.getPtrTy(), B.getPtrTy());
    G = new llvm::GlobalVariable(
        M, Ty, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantStruct::get(Ty, {B.getInt64(sizeOf(T)), Fini, AddRef}),
        "typeinfo");
    return G;
  }
  llvm::DenseMap<Type *, llvm::GlobalVariable *> TypeInfos;

  void emitDynOp(Type *Elem, llvm::Function *F, bool IsFini) {
    CurFn = F;
    AddrAligns.clear();
    CurRoutine = nullptr;
    CurFrame = nullptr;
    CurPin = nullptr;
    PinTemps = true;
    CurSP = nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
    if (IsFini)
      finalizeValue(F->getArg(0), Elem);
    else
      addRefValue(F->getArg(0), Elem);
    B.CreateRetVoid();
  }

  /// The element functions of the array types used: last of all, as any
  /// code, the main program's too, can be the first to use a type.
  void emitDynOps() {
    for (const DynOp &Op : DynOpsToEmit)
      emitDynOp(Op.Elem, Op.F, Op.IsFini);
    DynOpsToEmit.clear();
  }

  /// setlength of the array variable at Addr, of type T: Ns[0] elements,
  /// each of them (arrays) set to the next lengths.
  void emitDynSetLength(llvm::Value *Addr, DynArrayType *T,
                        llvm::ArrayRef<llvm::Value *> Ns) {
    llvm::Constant *Info = dynInfoOf(T);
    astrCall("__pas_dyn_setlength", B.getVoidTy(), {Addr, Info, Ns[0]});
    if (Ns.size() == 1)
      return;
    auto *Elem = llvm::cast<DynArrayType>(T->getElementType());
    llvm::Value *Data = B.CreateLoad(B.getPtrTy(), Addr, "data");
    llvm::BasicBlock *Pre = B.GetInsertBlock();
    auto *Loop = newBlock("dim.loop");
    auto *Done = newBlock("dim.done");
    B.CreateCondBr(B.CreateICmpEQ(Ns[0], B.getInt64(0)), Done, Loop);
    B.SetInsertPoint(Loop);
    llvm::PHINode *I = B.CreatePHI(B.getInt64Ty(), 2, "i");
    I->addIncoming(B.getInt64(0), Pre);
    emitDynSetLength(B.CreateInBoundsGEP(B.getPtrTy(), Data, I), Elem,
                     Ns.drop_front());
    llvm::Value *Next = B.CreateAdd(I, B.getInt64(1));
    I->addIncoming(Next, B.GetInsertBlock());
    B.CreateCondBr(B.CreateICmpEQ(Next, Ns[0]), Done, Loop);
    B.SetInsertPoint(Done);
  }

  /// [a, b, c] as a new dynamic array of type T.
  llvm::Value *emitDynArrayCtor(SetCtorExpr *S, DynArrayType *T) {
    auto Els = S->getElements();
    if (Els.empty())
      return llvm::ConstantPointerNull::get(B.getPtrTy());
    Type *Elem = T->getElementType();
    llvm::Constant *Info = dynInfoOf(T);
    llvm::Value *Data = holdResult(
        astrCall("__pas_dyn_new", B.getPtrTy(), {Info, B.getInt64(Els.size())}), T);
    for (size_t I = 0; I < Els.size(); ++I) {
      Expr *E = Els[I].Lo;
      llvm::Value *Slot = B.CreateInBoundsGEP(memType(Elem), Data, B.getInt64(I));
      if (auto *SS = llvm::dyn_cast<ShortStringType>(Elem))
        emitStringAssign(Slot, SS, E);
      else if (Elem->isAnsiString())
        storeValue(Slot, emitAnsi(E), Elem);
      else if (Elem->isStructured())
        copyValue(Slot, emitAddress(E), Elem);
      else if (llvm::Constant *C = cStringFor(Elem, E))
        B.CreateStore(C, Slot);
      else
        storeValue(Slot,
                   convertForAssign(visitExpr(E), E->getType(), Elem, E->getBeginLoc()),
                   Elem);
    }
    return Data;
  }

  /// Is V kept in its routine's frame record? Variables that nested
  /// routines use are, and those that must be in memory when an exception
  /// arrives through _longjmp: all of them in a routine where execution
  /// continues afterwards (try statements, non-local labels), else the
  /// managed ones, which are released on the way out. The other variables
  /// get an alloca each, which the optimizer turns into registers.
  bool inFrame(VarDecl *V, bool PinAll, const VarSet &PinVars) {
    return PinAll || PinVars.count(V) || V->isCaptured() ||
           (!isByRef(V) && isManaged(V->getType()));
  }

  /// The start value of a local variable or function result at Addr: zero.
  /// ISO leaves them undefined; this makes programs deterministic. Without
  /// zero initialization (-fno-zero-init, the default from -O1) arrays,
  /// records and strings are only cleared when they hold managed values or
  /// files; a string starts empty.
  void emitZeroInit(llvm::Value *Addr, Type *T) {
    llvm::Type *Ty = memType(T);
    if (!T->isStructured()) {
      B.CreateStore(llvm::Constant::getNullValue(Ty), Addr);
      return;
    }
    if (CGOpts.ZeroInit || isManaged(T) || containsFile(T))
      B.CreateMemSet(Addr, B.getInt8(0), DL.getTypeAllocSize(Ty),
                     DL.getABITypeAlign(Ty));
    else if (T->isShortString())
      B.CreateStore(B.getInt8(0), Addr);
  }

  /// Address of field F of record type RT at RecordAddr. Field offsets are
  /// recorded when the record is laid out, which may not have happened yet
  /// (a record reached only through pointers), so lay it out first.
  llvm::Value *fieldAddress(llvm::Value *RecordAddr, Type *RT, FieldDecl *F) {
    if (auto *C = llvm::dyn_cast<ClassType>(RT))
      classLayout(C); // RecordAddr is the object
    else
      memType(RT);
    assert(FieldOffsets.count(F) && "field of another record type");
    uint64_t BaseAlign = addrAlign(RecordAddr, RT);
    llvm::Value *Addr = B.CreateConstInBoundsGEP1_64(
        B.getInt8Ty(), RecordAddr, FieldOffsets[F], F->getName().Name);
    noteComponentAlign(Addr, BaseAlign, FieldOffsets[F], F->getType());
    return Addr;
  }

  //===--------------------------------------------------------------------===//
  // Bitpacked records and arrays (Free Pascal)
  //===--------------------------------------------------------------------===//

  /// Where a bitpacked component's bits are: from bit Bit (an i64, for an
  /// array's element) or ConstBit (a record's field) of the Size bytes at
  /// Base; T is its type.
  struct BitRef {
    llvm::Value *Base;
    llvm::Value *Bit;
    uint64_t ConstBit;
    uint64_t Size;
    Type *T;
  };

  BitRef bitRef(Expr *E) {
    if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
      auto *AT = llvm::cast<ArrayType>(I->getBase()->getType());
      llvm::Value *Base = emitAddress(I->getBase());
      llvm::Value *Idx = visitExpr(I->getIndex());
      Type *IdxTy = I->getIndex()->getType();
      emitRangeCheck(Idx, IdxTy, AT->getIndexType(),
                     I->getIndex()->getBeginLoc(), "array index out of bounds");
      OrdinalBounds Bd = getOrdinalBounds(AT->getIndexType());
      llvm::Value *Offset =
          B.CreateSub(toI64(Idx, IdxTy), B.getInt64(Bd.Lo), "idx");
      Type *Elem = AT->getElementType();
      return {Base, B.CreateMul(Offset, B.getInt64(bitPackedWidth(Elem)), "bit"),
              0, sizeOf(AT), Elem};
    }
    FieldDecl *F;
    Type *RT;
    llvm::Value *Base;
    if (auto *FE = llvm::dyn_cast<FieldExpr>(E)) {
      F = FE->getFieldDecl();
      RT = FE->getBase()->getType();
      Base = emitAddress(FE->getBase());
    } else {
      auto *N = llvm::cast<NameRefExpr>(E);
      F = llvm::cast<FieldDecl>(N->getDecl());
      RT = N->getWith()->Record->getType();
      Base = withBase(N->getWith());
    }
    memType(RT); // computes the bit offsets
    return {Base, nullptr, FieldBitOffsets[F], sizeOf(RT), F->getType()};
  }

  /// The bytes holding R's bits, loaded and stored as one integer (Ty, at
  /// Ptr), and where the bits start in it (Shift, of type Ty). An array's
  /// window never reaches past the array.
  struct BitWindow {
    llvm::Value *Ptr;
    llvm::IntegerType *Ty;
    llvm::Value *Shift;
  };

  BitWindow bitWindow(const BitRef &R) {
    uint64_t W = bitPackedWidth(R.T), K;
    llvm::Value *Byte, *Shift;
    if (!R.Bit) {
      K = (R.ConstBit % 8 + W + 7) / 8;
      Byte = B.getInt64(R.ConstBit / 8);
      Shift = B.getInt64(R.ConstBit % 8);
    } else if (W % 8 == 0) { // whole bytes
      K = W / 8;
      Byte = B.CreateLShr(R.Bit, B.getInt64(3));
      Shift = B.getInt64(0);
    } else if (8 % W == 0) { // never across bytes
      K = 1;
      Byte = B.CreateLShr(R.Bit, B.getInt64(3));
      Shift = B.CreateAnd(R.Bit, B.getInt64(7));
    } else {
      K = std::min<uint64_t>((W + 14) / 8, R.Size);
      if (K == R.Size) {
        Byte = B.getInt64(0);
        Shift = R.Bit;
      } else {
        Byte = B.CreateLShr(R.Bit, B.getInt64(3));
        llvm::Value *Last = B.getInt64(R.Size - K);
        Byte = B.CreateSelect(B.CreateICmpUGT(Byte, Last), Last, Byte);
        Shift = B.CreateSub(R.Bit, B.CreateShl(Byte, B.getInt64(3)));
      }
    }
    llvm::IntegerType *Ty = B.getIntNTy(static_cast<unsigned>(K * 8));
    if (K * 8 < 64)
      Shift = B.CreateTrunc(Shift, Ty);
    else if (K * 8 > 64)
      Shift = B.CreateZExt(Shift, Ty);
    return {B.CreateInBoundsGEP(B.getInt8Ty(), R.Base, Byte), Ty, Shift};
  }

  /// The value (register type) of bitpacked component E.
  llvm::Value *bitLoad(Expr *E) { return bitLoad(bitRef(E)); }

  llvm::Value *bitLoad(const BitRef &R) {
    BitWindow Wd = bitWindow(R);
    unsigned W = bitPackedWidth(R.T);
    llvm::Value *V = B.CreateAlignedLoad(Wd.Ty, Wd.Ptr, llvm::Align(1), "bits");
    V = B.CreateLShr(V, Wd.Shift);
    if (Wd.Ty->getBitWidth() > W)
      V = B.CreateTrunc(V, B.getIntNTy(W));
    auto *Dst = llvm::cast<llvm::IntegerType>(regType(R.T));
    if (Dst->getBitWidth() > W)
      V = bitPackedSigned(R.T) ? B.CreateSExt(V, Dst) : B.CreateZExt(V, Dst);
    return V;
  }

  /// Target := V for bitpacked component Target: its bits replaced.
  void bitStore(Expr *Target, llvm::Value *V) {
    BitRef R = bitRef(Target);
    BitWindow Wd = bitWindow(R);
    unsigned W = bitPackedWidth(R.T), N = Wd.Ty->getBitWidth();
    if (V->getType()->getIntegerBitWidth() > W)
      V = B.CreateTrunc(V, B.getIntNTy(W));
    if (N > W)
      V = B.CreateZExt(V, Wd.Ty);
    V = B.CreateShl(V, Wd.Shift);
    llvm::Value *Mask = B.CreateShl(
        llvm::ConstantInt::get(Wd.Ty, llvm::APInt::getLowBitsSet(N, W)), Wd.Shift);
    llvm::Value *Old = B.CreateAlignedLoad(Wd.Ty, Wd.Ptr, llvm::Align(1), "bits");
    llvm::Value *New = B.CreateOr(B.CreateAnd(Old, B.CreateNot(Mask)), V);
    B.CreateAlignedStore(New, Wd.Ptr, llvm::Align(1));
  }

  /// Address of a designator (variable, a[i], r.f, with-field) or of an
  /// aggregate constant (string literal / string constant).
  llvm::Value *emitAddress(Expr *E) {
    if (E->getConversion()) // converted to a record: a temporary
      return emitConversion(E);
    if (isBitPackedComponent(E)) { // (no address) a copy of its value
      llvm::Value *Tmp = createTemp(memType(E->getType()), "bits.tmp");
      storeScalar(Tmp, bitLoad(E), E->getType());
      return Tmp;
    }
    if (E->getType() && E->getType()->isOpenArray()) // its elements
      return B.CreateLoad(B.getPtrTy(), openDescriptor(E), "data");
    switch (E->getKind()) {
    case Expr::EK_Tuple:
      return emitTuple(llvm::cast<TupleExpr>(E));
    case Expr::EK_Await: // a structured result: in the task
    case Expr::EK_Spawn:
      return visitExpr(E);
    case Expr::EK_Cond: { // the hidden variable, once set
      auto *CE = llvm::cast<CondExpr>(E);
      visitStmt(CE->getLowered());
      return emitAddress(CE->getResult());
    }
    case Expr::EK_Binary:
    case Expr::EK_Unary:
      // A record's operator returning a record: a temporary holding it.
      return visitExpr(E);
    case Expr::EK_NameRef: {
      auto *N = llvm::cast<NameRefExpr>(E);
      if (auto *V = llvm::dyn_cast<VarDecl>(N->getDecl()))
        return varAddress(V);
      if (llvm::isa<RoutineDecl>(N->getDecl())) // f: a record or string result
        return visitExpr(E);
      if (auto *F = llvm::dyn_cast<FieldDecl>(N->getDecl()))
        return fieldAddress(withBase(N->getWith()),
                            N->getWith()->Record->getType(), F);
      if (auto *P = llvm::dyn_cast<PropertyDecl>(N->getDecl())) {
        if (P->isClassProperty())
          return emitPropertyRead(P, nullptr, nullptr);
        WithContext *W = N->getWith();
        return emitPropertyRead(P, withBase(W), W->Record->getType());
      }
      auto *C = llvm::cast<ConstDecl>(N->getDecl());
      return getStringConstant(C->getValue().Str);
    }
    case Expr::EK_StringLiteral:
      return getStringConstant(llvm::cast<StringLiteralExpr>(E)->getValue());
    case Expr::EK_Paren:
      return emitAddress(llvm::cast<ParenExpr>(E)->getInner());
    case Expr::EK_Call:
      if (auto *C = llvm::cast<CallExpr>(E); C->isVarCast())
        return emitAddress(C->getArgs()[0]); // T(x) of an untyped x
      if (auto *C = llvm::cast<CallExpr>(E); C->isMethodCast()) {
        if (C->isMethodCastOfVariable()) // TMethod(m)
          return emitAddress(C->getArgs()[0]);
        llvm::Value *Tmp = createTemp(memType(E->getType()), "method");
        B.CreateStore(visitExpr(C->getArgs()[0]), Tmp);
        return Tmp;
      }
      [[fallthrough]];
    case Expr::EK_IndirectCall:
    case Expr::EK_Inherited:
      // A record (or string) function result: a temporary holding it.
      assert(E->getType()->isStructured() && "call result has no address");
      return visitExpr(E);
    case Expr::EK_Index: {
      auto *I = llvm::cast<IndexExpr>(E);
      if (I->getProperty()) // a string or record value
        return emitArrayPropertyRead(I);
      if (auto *OA = llvm::dyn_cast<OpenArrayType>(I->getBase()->getType())) {
        llvm::Value *Data = emitAddress(I->getBase());
        llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
        if (rangeChecks(I->getIndex()->getBeginLoc()))
          emitRuntimeCheck(B.CreateICmpUGE(X, openCount(openDescriptor(I->getBase()))),
                           "array index out of bounds",
                           I->getIndex()->getBeginLoc());
        return B.CreateInBoundsGEP(memType(OA->getElementType()), Data, X);
      }
      if (auto *AC = llvm::dyn_cast<ArrayOfConstType>(I->getBase()->getType())) {
        llvm::Value *A = visitExpr(I->getBase());
        llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
        if (rangeChecks(I->getIndex()->getBeginLoc()))
          emitRuntimeCheck(B.CreateICmpNE(astrCall("__pas_aoc_bad_index",
                                                   B.getInt32Ty(), {A, X}),
                                          B.getInt32(0)),
                           "array index out of bounds",
                           I->getIndex()->getBeginLoc());
        return B.CreateInBoundsGEP(memType(AC->getElementType()), A,
                                   B.CreateAdd(X, B.getInt64(1)));
      }
      if (auto *PT = llvm::dyn_cast<PointerType>(I->getBase()->getType())) {
        // p[i]: (p + i)^, unchecked as in Free Pascal.
        llvm::Value *P = visitExpr(I->getBase());
        llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
        return B.CreateGEP(pointeeMemType(PT), P, X);
      }
      if (I->getBase()->getType()->isDynArray()) {
        Type *Elem = llvm::cast<DynArrayType>(I->getBase()->getType())->getElementType();
        llvm::Value *Data = visitExpr(I->getBase());
        llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
        if (rangeChecks(I->getIndex()->getBeginLoc()))
          emitRuntimeCheck(B.CreateICmpNE(astrCall("__pas_dyn_bad_index",
                                                   B.getInt32Ty(), {Data, X}),
                                          B.getInt32(0)),
                           "array index out of bounds",
                           I->getIndex()->getBeginLoc());
        return B.CreateInBoundsGEP(memType(Elem), Data, X);
      }
      if (I->getBase()->getType()->isAnsiString()) {
        // An address may be written through: the string must be unshared,
        // but a const parameter's belongs to the caller.
        llvm::Value *Base = emitAddress(I->getBase());
        llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
        llvm::Value *Data =
            isConstParamRef(I->getBase())
                ? B.CreateLoad(B.getPtrTy(), Base, "s")
                : astrCall("__pas_astr_unique", B.getPtrTy(), {Base});
        emitAnsiIndexCheck(Data, X, I->getIndex()->getBeginLoc());
        return B.CreateInBoundsGEP(B.getInt8Ty(), Data,
                                   B.CreateSub(X, B.getInt64(1)));
      }
      if (auto *SS = llvm::dyn_cast<ShortStringType>(I->getBase()->getType())) {
        llvm::Value *Base = emitAddress(I->getBase());
        llvm::Value *Idx = visitExpr(I->getIndex());
        Type *IdxTy = I->getIndex()->getType();
        if (rangeChecks(I->getIndex()->getBeginLoc())) {
          llvm::Value *X = toI64(Idx, IdxTy);
          emitRuntimeCheck(outsideRange(X, 0, SS->getMaxLength()),
                           "string index out of bounds", I->getIndex()->getBeginLoc());
        }
        return B.CreateInBoundsGEP(B.getInt8Ty(), Base, toI64(Idx, IdxTy));
      }
      if (auto *SL = llvm::dyn_cast<StringLiteralType>(I->getBase()->getType())) {
        // A string constant (Turbo/FPC): its characters are 1..N.
        llvm::Value *Base = emitAddress(I->getBase());
        llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
        if (rangeChecks(I->getIndex()->getBeginLoc()))
          emitRuntimeCheck(outsideRange(X, 1, static_cast<int64_t>(SL->getLength())),
                           "string index out of bounds", I->getIndex()->getBeginLoc());
        return B.CreateInBoundsGEP(B.getInt8Ty(), Base,
                                   B.CreateSub(X, B.getInt64(1)));
      }
      auto *AT = llvm::cast<ArrayType>(I->getBase()->getType());
      llvm::Value *Base = emitAddress(I->getBase());
      llvm::Value *Idx = visitExpr(I->getIndex());
      Type *IdxTy = I->getIndex()->getType();
      emitRangeCheck(Idx, IdxTy, AT->getIndexType(),
                     I->getIndex()->getBeginLoc(), "array index out of bounds");
      OrdinalBounds Bd = getOrdinalBounds(AT->getIndexType());
      llvm::Value *Offset =
          B.CreateSub(toI64(Idx, IdxTy), B.getInt64(Bd.Lo), "idx");
      uint64_t BaseAlign = addrAlign(Base, AT);
      llvm::Value *Elem =
          B.CreateInBoundsGEP(memType(AT), Base, {B.getInt64(0), Offset});
      // (elements are a multiple of their alignment apart)
      if (BaseAlign < alignOf(AT->getElementType()))
        AddrAligns[Elem] = BaseAlign;
      return Elem;
    }
    case Expr::EK_Field: {
      auto *F = llvm::cast<FieldExpr>(E);
      if (NameRefExpr *Ref = F->getConstRef()) // TFoo.C: a string constant
        return emitAddress(Ref);
      if (auto *V = llvm::dyn_cast_or_null<VarDecl>(F->getMember()))
        return Globals[V]; // a class variable (the object is not evaluated)
      if (auto *C = llvm::dyn_cast<ClassType>(F->getBase()->getType())) {
        if (!llvm::isa<FieldDecl>(F->getMember()))
          return emitMemberValue(F); // a structured property or result
        return fieldAddress(emitObject(F->getBase()), C, F->getFieldDecl());
      }
      if (!llvm::isa<FieldDecl>(F->getMember())) // a record's method/property
        return visitFieldExpr(F); // a structured result or property
      return fieldAddress(emitAddress(F->getBase()), F->getBase()->getType(),
                          F->getFieldDecl());
    }
    case Expr::EK_Deref: {
      auto *D = llvm::cast<DerefExpr>(E);
      if (D->getBase()->getType()->isFile()) // buffer variable f^
        return B.CreateCall(RT.Buffer, {loadFile(emitAddress(D->getBase()))});
      llvm::Value *P = visitExpr(D->getBase());
      if (rangeChecks(D->getBase()->getBeginLoc()))
        emitRuntimeCheck(B.CreateIsNull(P), "dereference of a nil pointer",
                         D->getBase()->getBeginLoc());
      return P;
    }
    default:
      llvm_unreachable("expression has no address");
    }
  }

  //===--------------------------------------------------------------------===//
  // Debug information
  //===--------------------------------------------------------------------===//

  unsigned lineOf(SourceLocation L) const { return SM.getPresumedLoc(L).Line; }

  void initDebugInfo(llvm::StringRef SourceFileName) {
    DIB = std::make_unique<llvm::DIBuilder>(M);
    llvm::SmallString<256> Abs(SourceFileName);
    llvm::sys::fs::make_absolute(Abs);
    DIUnitFile = DIB->createFile(llvm::sys::path::filename(Abs),
                                 llvm::sys::path::parent_path(Abs));
    CU = DIB->createCompileUnit(llvm::dwarf::DW_LANG_Pascal83, DIUnitFile,
                                "p3", CGOpts.OptLevel > 0, "", 0);
    M.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                    llvm::DEBUG_METADATA_VERSION);
    // DWARF 4: GNU ld mis-merges DWARF 5 string-offset tables when our
    // objects are linked with clang-compiled ones (libpascalrt).
    M.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
  }

  llvm::DIType *debugType(Type *T) {
    if (llvm::DIType *D = DITypes.lookup(T))
      return D;
    llvm::DIType *D = buildDebugType(T);
    // Named arrays, subranges, pointers ...: a typedef carries the name.
    if (!T->getDeclName().empty() && !T->isRecord() && !T->isEnum() &&
        !T->isClass() && !llvm::isa<BuiltinType>(T))
      D = DIB->createTypedef(D, T->getDeclName(), DIUnitFile, 0, CU);
    DITypes[T] = D;
    return D;
  }

  llvm::DIType *buildDebugType(Type *T) {
    using namespace llvm::dwarf;
    switch (T->getKind()) {
    case Type::TK_Integer: {
      auto *I = llvm::cast<IntegerType>(T);
      return DIB->createBasicType(I->getIntegerName(), I->getBits(),
                                  I->isSigned() ? DW_ATE_signed : DW_ATE_unsigned);
    }
    case Type::TK_Real:    return DIB->createBasicType("real", 64, DW_ATE_float);
    case Type::TK_Boolean: return DIB->createBasicType("boolean", 8, DW_ATE_boolean);
    case Type::TK_Char:    return DIB->createBasicType("char", 8, DW_ATE_unsigned_char);
    case Type::TK_WideChar:
      return DIB->createBasicType("widechar", 16, DW_ATE_unsigned_char);
    case Type::TK_Subrange:
      return debugType(llvm::cast<SubrangeType>(T)->getHost());
    case Type::TK_DynArray:
    case Type::TK_OpenArray:
    case Type::TK_ArrayOfConst:
    case Type::TK_Task:
    case Type::TK_Token:
    case Type::TK_Iterator:
      return DIB->createPointerType(nullptr, 64);
    case Type::TK_AnsiString:
      return DIB->createPointerType(
          DIB->createBasicType("char", 8, DW_ATE_unsigned_char), 64);
    case Type::TK_Enum: {
      llvm::SmallVector<llvm::Metadata *, 8> Enumerators;
      for (ConstDecl *C : llvm::cast<EnumType>(T)->getValues())
        Enumerators.push_back(DIB->createEnumerator(C->getName().Spelling,
                                                    C->getValue().Int));
      return DIB->createEnumerationType(
          CU, T->getDeclName(), DIUnitFile, 0, 32, 32,
          DIB->getOrCreateArray(Enumerators),
          DIB->createBasicType("integer", 32, DW_ATE_signed));
    }
    case Type::TK_String:
    case Type::TK_Array: {
      if (auto *A = llvm::dyn_cast<ArrayType>(T); A && A->hasBitElements())
        return DIB->createArrayType( // (its bytes)
            sizeOf(T) * 8, 0, DIB->createBasicType("byte", 8, DW_ATE_unsigned),
            DIB->getOrCreateArray(
                {DIB->getOrCreateSubrange(0, static_cast<int64_t>(sizeOf(T)))}));
      Type *Elem = T->isStringLiteral() ? static_cast<Type *>(nullptr)
                                        : llvm::cast<ArrayType>(T)->getElementType();
      llvm::DIType *ElemDI =
          Elem ? debugType(Elem) : DIB->createBasicType("char", 8, DW_ATE_unsigned_char);
      int64_t Lo = 1, Count;
      if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
        OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
        Lo = Bd.Lo;
        Count = Bd.Hi - Bd.Lo + 1;
      } else {
        Count = static_cast<int64_t>(T->getStringLength());
      }
      llvm::Metadata *Sub = DIB->getOrCreateSubrange(Lo, Count);
      return DIB->createArrayType(sizeOf(T) * 8, 0, ElemDI,
                                  DIB->getOrCreateArray({Sub}));
    }
    case Type::TK_Record: {
      auto *R = llvm::cast<RecordType>(T);
      memType(T); // computes the field offsets
      // Forward-declare first: fields may point back to this record.
      llvm::DICompositeType *Fwd = DIB->createReplaceableCompositeType(
          DW_TAG_structure_type, T->getDeclName(), CU, DIUnitFile, 0);
      DITypes[T] = Fwd;
      llvm::SmallVector<llvm::Metadata *, 8> Members;
      for (FieldDecl *F : R->getAllFields()) {
        if (R->isBitPacked() && F->getType()->isOrdinal()) { // a bit field
          Members.push_back(DIB->createBitFieldMemberType(
              Fwd, F->getName().Spelling, DIUnitFile, lineOf(F->getLocation()),
              bitPackedWidth(F->getType()), FieldBitOffsets[F], 0,
              llvm::DINode::FlagZero, debugType(F->getType())));
          continue;
        }
        Members.push_back(DIB->createMemberType(
            Fwd, F->getName().Spelling, DIUnitFile, lineOf(F->getLocation()),
            sizeOf(F->getType()) * 8, 0, FieldOffsets[F] * 8,
            llvm::DINode::FlagZero, debugType(F->getType())));
      }
      llvm::DICompositeType *Def = DIB->createStructType(
          CU, T->getDeclName(), DIUnitFile, 0, sizeOf(T) * 8, 0,
          llvm::DINode::FlagZero, nullptr, DIB->getOrCreateArray(Members));
      Fwd->replaceAllUsesWith(Def);
      return Def;
    }
    case Type::TK_Pointer: {
      Type *P = llvm::cast<PointerType>(T)->getPointee();
      // `pointer` (untyped) is a void pointer.
      return DIB->createPointerType(P ? debugType(P) : nullptr, 64);
    }
    case Type::TK_Set: { // its bytes
      unsigned Bits = setBits(T);
      llvm::Metadata *Sub = DIB->getOrCreateSubrange(0, Bits / 8);
      return DIB->createArrayType(
          Bits, 0, DIB->createBasicType("byte", 8, DW_ATE_unsigned),
          DIB->getOrCreateArray({Sub}));
    }
    case Type::TK_File:
      return DIB->createPointerType(DIB->createUnspecifiedType("PasFile"), 64);
    case Type::TK_Class: {
      // A pointer to a structure holding the fields (after the VMT pointer).
      auto *C = llvm::cast<ClassType>(T);
      classLayout(C);
      llvm::DICompositeType *Fwd = DIB->createReplaceableCompositeType(
          DW_TAG_structure_type, T->getDeclName(), CU, DIUnitFile, 0);
      llvm::DIType *Ptr = DIB->createPointerType(Fwd, 64);
      DITypes[T] = Ptr; // fields may refer to the class
      llvm::SmallVector<llvm::Metadata *, 8> Members;
      for (FieldDecl *F : C->getAllFields())
        Members.push_back(DIB->createMemberType(
            Fwd, F->getName().Spelling, DIUnitFile, lineOf(F->getLocation()),
            sizeOf(F->getType()) * 8, 0, FieldOffsets[F] * 8,
            llvm::DINode::FlagZero, debugType(F->getType())));
      llvm::DICompositeType *Def = DIB->createStructType(
          CU, T->getDeclName(), DIUnitFile, 0, instanceSize(C) * 8, 0,
          llvm::DINode::FlagZero, nullptr, DIB->getOrCreateArray(Members));
      Fwd->replaceAllUsesWith(Def);
      return Ptr;
    }
    default: // procedural values, nil
      return DIB->createUnspecifiedType(T->getName());
    }
  }

  llvm::DISubprogram *createSubprogram(llvm::Function *F, llvm::StringRef Name,
                                       llvm::DIScope *Scope, unsigned Line,
                                       Type *Result,
                                       llvm::ArrayRef<VarDecl *> Params,
                                       llvm::DIFile *File = nullptr,
                                       VarDecl *Self = nullptr) {
    llvm::SmallVector<llvm::Metadata *, 8> Sig{Result ? debugType(Result) : nullptr};
    if (Self) // a record's method: Self by reference
      Sig.push_back(isByRef(Self)
                        ? DIB->createReferenceType(llvm::dwarf::DW_TAG_reference_type,
                                                   debugType(Self->getType()), 64)
                        : debugType(Self->getType()));
    for (VarDecl *P : Params)
      Sig.push_back(isByRef(P)
                        ? DIB->createReferenceType(llvm::dwarf::DW_TAG_reference_type,
                                                   debugType(P->getType()), 64)
                        : debugType(P->getType()));
    auto *SP = DIB->createFunction(
        Scope, Name, /*LinkageName=*/"", File ? File : DIUnitFile, Line,
        DIB->createSubroutineType(DIB->getOrCreateTypeArray(Sig)), Line,
        llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::SPFlagDefinition |
            (CGOpts.OptLevel ? llvm::DISubprogram::SPFlagOptimized
                             : llvm::DISubprogram::SPFlagZero));
    F->setSubprogram(SP);
    return SP;
  }

  void setDebugLoc(SourceLocation L) {
    if (!DIB || !CurSP)
      return;
    PresumedLoc P = SM.getPresumedLoc(L);
    if (!P.isValid())
      return;
    // Code from another file (a unit's initialization inside main) needs a
    // scope naming that file.
    llvm::DIScope *Scope = CurSP;
    llvm::DIFile *F = diFileFor(L);
    if (F != CurSP->getFile()) {
      llvm::DILexicalBlockFile *&LBF = FileScopes[{CurSP, F}];
      if (!LBF)
        LBF = DIB->createLexicalBlockFile(CurSP, F);
      Scope = LBF;
    }
    B.SetCurrentDebugLocation(
        llvm::DILocation::get(M.getContext(), P.Line, P.Column, Scope));
  }

  /// Describes the variables of R (params, locals, result) to the
  /// debugger: offsets into the frame alloca, or their own allocas.
  void declareFrameVariables(RoutineDecl *R) {
    llvm::StructType *FrameTy = FrameTypes.lookup(R);
    const llvm::StructLayout *SL =
        FrameTy ? DL.getStructLayout(FrameTy) : nullptr;
    auto Declare = [&](VarDecl *V, unsigned ArgNo) {
      llvm::DIType *Ty = debugType(V->getType());
      // Self has no location of its own: the method's.
      unsigned Line = lineOf(V->isPredeclared() ? R->getBodyLocation() : V->getLocation());
      llvm::DILocalVariable *DV =
          ArgNo ? DIB->createParameterVariable(CurSP, V->getName().Spelling, ArgNo,
                                               DIUnitFile, Line, Ty)
                : DIB->createAutoVariable(CurSP, V->getName().Spelling,
                                          DIUnitFile, Line, Ty);
      llvm::SmallVector<uint64_t, 4> Ops;
      auto Slot = FrameSlots.find(V);
      if (Slot != FrameSlots.end()) {
        Ops.push_back(llvm::dwarf::DW_OP_plus_uconst);
        Ops.push_back(SL->getElementOffset(Slot->second));
      }
      if (isByRef(V))
        Ops.push_back(llvm::dwarf::DW_OP_deref);
      DIB->insertDeclare(Slot != FrameSlots.end() ? CurFrame : LocalAddrs[V],
                         DV, DIB->createExpression(Ops),
                         llvm::DILocation::get(M.getContext(), Line, 0, CurSP),
                         B.GetInsertBlock());
    };
    unsigned ArgNo = 1; // DWARF parameter numbers are 1-based
    if (R->hasSelf())
      Declare(R->getSelfVar(), ArgNo++);
    for (VarDecl *P : R->getParams())
      Declare(P, ArgNo++);
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D);
          V && !V->isTypedConst() && !V->getAbsolute())
        Declare(V, 0);
    if (R->isFunction())
      Declare(R->getResultVar(), 0);
  }

  void declareGlobalDebugInfo(VarDecl *V, llvm::GlobalVariable *G) {
    G->addDebugInfo(DIB->createGlobalVariableExpression(
        // No linkage name: debuggers would show "pas.x" instead of "x".
        CU, V->getName().Spelling, /*LinkageName=*/"", DIUnitFile,
        lineOf(V->getLocation()), debugType(V->getType()),
        /*IsLocalToUnit=*/true));
  }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  /// Initial value of a typed constant (or zero).
  llvm::Constant *initializerFor(VarDecl *V, llvm::Type *Ty) {
    if (V->getInit())
      return constantFor(V->getInit(), V->getInitValue(), V->getType(), Ty);
    return llvm::Constant::getNullValue(Ty);
  }

  /// The initial value of static variable V (memory type Ty) when it is
  /// created: one holding procedural values gets its value once the
  /// routines are declared (finishStaticInits).
  llvm::Constant *staticInitializer(VarDecl *V, llvm::Type *Ty) {
    if (V->getInit() && containsRoutineType(V->getType())) {
      DeferredInits.push_back(V);
      return llvm::Constant::getNullValue(Ty);
    }
    return initializerFor(V, Ty);
  }
  std::vector<VarDecl *> DeferredInits;

  void finishStaticInits() {
    for (VarDecl *V : DeferredInits)
      Globals[V]->setInitializer(initializerFor(V, memType(V->getType())));
    DeferredInits.clear();
  }

  /// A procedural constant: nil, a routine, or a lambda (with no frame and
  /// no object), as {function, nil}.
  llvm::Constant *routineConstant(Expr *Init, llvm::Type *Ty) {
    RoutineDecl *R = nullptr;
    if (auto *A = llvm::dyn_cast<AddrOfExpr>(Init))
      R = A->getLambda() ? A->getLambda()
          : A->getCode() ? A->getCode()
                         : llvm::dyn_cast_or_null<RoutineDecl>(
                               llvm::cast<NameRefExpr>(A->getOperand())->getDecl());
    if (!R) // nil
      return llvm::Constant::getNullValue(Ty);
    return llvm::ConstantStruct::get(
        llvm::cast<llvm::StructType>(Ty),
        {llvm::cast<llvm::Constant>(routineFunction(R)),
         llvm::ConstantPointerNull::get(B.getPtrTy())});
  }

  /// The constant for a typed-constant initializer Init of type T (memory
  /// type Ty); Folded is Sema's value for non-aggregates.
  llvm::Constant *constantFor(Expr *Init, const ConstValue &Folded, Type *T,
                              llvm::Type *Ty) {
    if (llvm::isa<RoutineType>(T))
      return routineConstant(Init, Ty);
    auto *Agg = llvm::dyn_cast<AggregateExpr>(Init);
    if (containsAlignedRecord(T)) { // its bytes (Sema checked their types)
      std::string Bytes(sizeOf(T), '\0');
      writeConstantBytes(Init, Folded, T, Bytes, 0);
      return bytesConstant(Bytes, 0, Ty);
    }
    if (!Agg)
      return scalarConstant(Folded, T, Ty);
    auto Elems = Agg->getElements();
    if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
      auto *ATy = llvm::cast<llvm::ArrayType>(Ty);
      llvm::SmallVector<llvm::Constant *, 16> Values;
      for (AggregateElement *El : Elems)
        Values.push_back(constantFor(El->Value, El->Folded, A->getElementType(),
                                     ATy->getElementType()));
      return llvm::ConstantArray::get(ATy, Values);
    }
    if (auto *R = llvm::dyn_cast<RecordType>(T)) {
      // Fixed fields only (Sema rejects variant parts); missing ones are 0.
      auto *ST = llvm::cast<llvm::StructType>(Ty);
      llvm::SmallVector<llvm::Constant *, 16> Values;
      auto Fixed = R->getFieldList()->Fixed;
      for (unsigned I = 0; I < Fixed.size(); ++I) {
        llvm::Type *FTy = ST->getElementType(I);
        llvm::Constant *C = llvm::Constant::getNullValue(FTy);
        for (AggregateElement *El : Elems)
          if (El->Field.Name == Fixed[I]->getName().Name)
            C = constantFor(El->Value, El->Folded, Fixed[I]->getType(), FTy);
        Values.push_back(C);
      }
      return llvm::ConstantStruct::get(ST, Values);
    }
    return constantFor(Elems[0]->Value, Folded, T, Ty); // (scalar)
  }

  /// Does T have a record laid out with a field alignment?
  static bool containsAlignedRecord(Type *T) {
    if (auto *A = llvm::dyn_cast<ArrayType>(T))
      return containsAlignedRecord(A->getElementType());
    if (auto *R = llvm::dyn_cast<RecordType>(T)) {
      if (R->getFieldAlignment())
        return true;
      for (FieldDecl *F : R->getAllFields())
        if (F->getType() && containsAlignedRecord(F->getType()))
          return true;
    }
    return false;
  }

  /// Writes the bytes of a typed constant's value (Init, or Folded for a
  /// scalar) of type T into Bytes from Offset (little-endian): ordinals,
  /// reals, short strings and char arrays, and arrays and records of them.
  void writeConstantBytes(Expr *Init, const ConstValue &Folded, Type *T,
                          std::string &Bytes, uint64_t Offset) {
    auto *Agg = llvm::dyn_cast<AggregateExpr>(Init);
    if (Agg && llvm::isa<ArrayType>(T)) {
      Type *Elem = llvm::cast<ArrayType>(T)->getElementType();
      uint64_t Stride = sizeOf(Elem), I = 0;
      for (AggregateElement *El : Agg->getElements())
        writeConstantBytes(El->Value, El->Folded, Elem, Bytes,
                           Offset + Stride * I++);
      return;
    }
    if (Agg && llvm::isa<RecordType>(T)) {
      auto *R = llvm::cast<RecordType>(T);
      memType(R); // (its field offsets)
      for (AggregateElement *El : Agg->getElements())
        if (FieldDecl *F = R->findField(El->Field.Name))
          writeConstantBytes(El->Value, El->Folded, F->getType(), Bytes,
                             Offset + FieldOffsets[F]);
      return;
    }
    if (Agg) { // (a scalar in parentheses)
      writeConstantBytes(Agg->getElements()[0]->Value, Folded, T, Bytes, Offset);
      return;
    }
    uint64_t Size = sizeOf(T);
    auto PutInt = [&](uint64_t V) {
      for (uint64_t I = 0; I < Size; ++I)
        Bytes[Offset + I] = static_cast<char>((V >> (8 * I)) & 0xff);
    };
    auto PutChars = [&](llvm::StringRef S) { // a short string or char array
      if (auto *SS = llvm::dyn_cast<ShortStringType>(T)) {
        S = S.take_front(SS->getMaxLength());
        Bytes[Offset] = static_cast<char>(S.size());
        for (size_t I = 0; I < S.size(); ++I)
          Bytes[Offset + 1 + I] = S[I];
        return;
      }
      for (size_t I = 0; I < S.size() && I < Size; ++I)
        Bytes[Offset + I] = S[I];
    };
    switch (Folded.K) {
    case ConstValue::Ordinal:
      if (T->isReal()) {
        double D = static_cast<double>(Folded.Int);
        uint64_t U;
        std::memcpy(&U, &D, 8);
        PutInt(U);
      } else if (T->isShortString() || T->isStringType()) {
        char C = static_cast<char>(Folded.Int);
        PutChars(llvm::StringRef(&C, 1));
      } else {
        PutInt(static_cast<uint64_t>(Folded.Int));
      }
      return;
    case ConstValue::Real: {
      uint64_t U;
      std::memcpy(&U, &Folded.RealVal, 8);
      PutInt(U);
      return;
    }
    case ConstValue::String:
      PutChars(Folded.Str);
      return;
    case ConstValue::None:
      return;
    }
  }

  llvm::Constant *scalarConstant(const ConstValue &C, Type *T, llvm::Type *Ty) {
    switch (C.K) {
    case ConstValue::Ordinal:
      if (T->isReal())
        return llvm::ConstantFP::get(Ty, static_cast<double>(C.Int));
      if (T->isAnsiString()) // a char
        return getAnsiConstant(std::string(1, static_cast<char>(C.Int)));
      if (T->isShortString()) // a char
        return initializerForString(std::string(1, static_cast<char>(C.Int)), Ty, T);
      return llvm::ConstantInt::get(Ty, static_cast<uint64_t>(C.Int), true);
    case ConstValue::Real:
      return llvm::ConstantFP::get(Ty, C.RealVal);
    case ConstValue::String:
      if (T->isAnsiString())
        return getAnsiConstant(C.Str);
      if (T->isRecord()) // a TGUID from its string: its bytes
        return bytesConstant(C.Str, 0, Ty);
      return initializerForString(C.Str.str(), Ty, T);
    case ConstValue::None:
      break;
    }
    return llvm::Constant::getNullValue(Ty);
  }

  /// The constant of memory type Ty whose bytes are Bytes from Offset
  /// (integers little-endian; bytes past the end are 0).
  llvm::Constant *bytesConstant(llvm::StringRef Bytes, uint64_t Offset,
                                llvm::Type *Ty) {
    const llvm::DataLayout &DL = M.getDataLayout();
    if (auto *IT = llvm::dyn_cast<llvm::IntegerType>(Ty)) {
      uint64_t V = 0;
      for (uint64_t I = IT->getBitWidth() / 8; I-- > 0;)
        V = V << 8 | (Offset + I < Bytes.size()
                          ? static_cast<uint8_t>(Bytes[Offset + I])
                          : 0);
      return llvm::ConstantInt::get(IT, V);
    }
    if (auto *AT = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
      uint64_t Size = DL.getTypeAllocSize(AT->getElementType());
      llvm::SmallVector<llvm::Constant *, 16> Values;
      for (uint64_t I = 0; I < AT->getNumElements(); ++I)
        Values.push_back(
            bytesConstant(Bytes, Offset + I * Size, AT->getElementType()));
      return llvm::ConstantArray::get(AT, Values);
    }
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty)) {
      const llvm::StructLayout *SL = DL.getStructLayout(ST);
      llvm::SmallVector<llvm::Constant *, 16> Values;
      for (unsigned I = 0; I < ST->getNumElements(); ++I)
        Values.push_back(bytesConstant(Bytes, Offset + SL->getElementOffset(I),
                                       ST->getElementType(I)));
      return llvm::ConstantStruct::get(ST, Values);
    }
    return llvm::Constant::getNullValue(Ty);
  }

  llvm::Constant *initializerForString(std::string S, llvm::Type *Ty, Type *T) {
    uint64_t Size = llvm::cast<llvm::ArrayType>(Ty)->getNumElements();
    if (auto *SS = llvm::dyn_cast<ShortStringType>(T)) {
      S = S.substr(0, SS->getMaxLength());
      S.insert(S.begin(), static_cast<char>(S.size()));
    }
    S.resize(Size, '\0');
    return llvm::ConstantDataArray::getString(M.getContext(), S, false);
  }

  void emitStaticVar(VarDecl *V) {
    llvm::Type *Ty = memType(V->getType());
    bool Imported = ImportedDecls.count(V);
    Globals[V] =
        createGlobal(V, Ty, Imported ? nullptr : staticInitializer(V, Ty));
    if (DIB && !Imported)
      declareGlobalDebugInfo(V, Globals[V]);
  }

  void emitGlobals(Block &Blk) {
    for (Decl *D : withNested(Blk.getDecls())) {
      ClassType *C = definedClass(D);
      if (!C)
        C = advancedRecord(D); // (an advanced record's, a helper's)
      if (C) { // class variables: no debug info
        for (Decl *M : C->getMembers())
          if (auto *V = llvm::dyn_cast<VarDecl>(M)) {
            llvm::Type *Ty = memType(V->getType());
            Globals[V] = createGlobal(V, Ty,
                                      V->isTypedConst()
                                          ? staticInitializer(V, Ty)
                                          : llvm::Constant::getNullValue(Ty));
          }
        continue;
      }
      auto *V = llvm::dyn_cast<VarDecl>(D);
      if (!V || V->getAbsolute())
        continue;
      if (V->isExternal()) { // a C variable: its symbol, defined elsewhere
        if (!isErrno(V)) {
          llvm::GlobalVariable *G = M.getNamedGlobal(V->getExternalName());
          if (!G)
            G = new llvm::GlobalVariable(M, memType(V->getType()),
                                         /*isConstant=*/false,
                                         llvm::GlobalValue::ExternalLinkage,
                                         nullptr, V->getExternalName());
          Globals[V] = G;
        }
        continue;
      }
      if (V->isTypedConst() || V->isInitializedVar()) { // initialized once
        emitStaticVar(V);
        continue;
      }
      llvm::Type *Ty = memType(V->getType());
      // ISO leaves variables undefined; zero-initializing makes programs
      // deterministic at no cost for globals.
      Globals[V] = createGlobal(V, Ty, llvm::Constant::getNullValue(Ty));
      if (DIB && !ImportedDecls.count(V))
        declareGlobalDebugInfo(V, Globals[V]);
    }
  }

  /// Creates LLVM functions and frame types for every routine in Blk
  /// (recursively), and queues their bodies for emission.
  /// Every routine takes its static link first (null at program level), so
  /// procedural parameters can call any routine the same way.
  llvm::FunctionType *routineFnType(llvm::ArrayRef<VarDecl *> Params,
                                    Type *Result, bool HasSelf = false,
                                    bool HasLink = true) {
    llvm::SmallVector<llvm::Type *, 8> ParamTys;
    if (HasLink)
      ParamTys.push_back(B.getPtrTy());
    // The result pointer is first or second so that it can be sret; Self
    // follows it (a method has a static link first).
    if (returnsByPointer(Result)) {
      ParamTys.push_back(B.getPtrTy());
      Result = nullptr;
    }
    if (HasSelf)
      ParamTys.push_back(B.getPtrTy());
    for (VarDecl *P : Params)
      ParamTys.push_back(P->isVarParam() || P->getType()->isStructured() ||
                                 P->getType()->isUntyped() || isConstRefPtr(P)
                             ? B.getPtrTy()
                             : regType(P->getType()));
    llvm::Type *RetTy = Result ? regType(Result) : B.getVoidTy();
    return llvm::FunctionType::get(RetTy, ParamTys, false);
  }

  /// Does R take the hidden static-link first parameter? Only a nested
  /// routine needs it; methods keep it for method pointers, as do async and
  /// iterator routines (their argument record has a link slot) (item 20).
  static bool needsLink(RoutineDecl *R) {
    return R->hasStaticLink() || R->hasSelf() || R->isMethod() ||
           R->isAsync() || R->isIterator();
  }

  llvm::FunctionType *routineFnType(RoutineDecl *R) {
    return routineFnType(R->getParams(),
                         R->isFunction() ? R->getResultType() : nullptr,
                         R->hasSelf(), needsLink(R));
  }

  /// An external (C) routine: no static link, C types (a callback: a
  /// function pointer), maybe variadic.
  void declareExternal(RoutineDecl *R) {
    llvm::SmallVector<llvm::Type *, 8> ParamTys;
    for (VarDecl *P : R->getParams())
      ParamTys.push_back(P->isVarParam() || P->getType()->isUntyped() ||
                                 isConstRefPtr(P) ||
                                 llvm::isa<RoutineType>(P->getType())
                             ? B.getPtrTy()
                             : regType(P->getType()));
    llvm::Type *RetTy = R->isFunction() ? regType(R->getResultType()) : B.getVoidTy();
    llvm::StringRef Sym = R->getExternalName().empty() ? R->getName().Spelling
                                                       : R->getExternalName();
    ExternalFns[R] = M.getOrInsertFunction(
        Sym, llvm::FunctionType::get(RetTy, ParamTys, R->isVarArgs()));
  }

  /// The C-callable entry of a cdecl routine R at program or unit level
  /// (its procedural values point to it): cthunk.<symbol>, which calls R
  /// with a null static link. Created (with its body) right after R.
  llvm::DenseMap<RoutineDecl *, llvm::Function *> CThunks;

  void declareCThunk(RoutineDecl *R) {
    if (!R->isCDecl() || R->isMethod() || R->hasStaticLink())
      return;
    llvm::Function *Target = Functions[R];
    llvm::Type *Result = Target->getReturnType();
    llvm::SmallVector<llvm::Type *, 8> ParamTys(
        Target->getFunctionType()->param_begin() + (needsLink(R) ? 1 : 0),
        Target->getFunctionType()->param_end());
    auto *T = llvm::Function::Create(llvm::FunctionType::get(Result, ParamTys, false),
                                     llvm::Function::InternalLinkage,
                                     "cthunk." + Target->getName(), M);
    unsigned First = R->isFunction() && returnsByPointer(R->getResultType());
    if (First)
      addResultParamAttrs(T->getArg(0), R->getResultType());
    llvm::IRBuilder<> TB(llvm::BasicBlock::Create(M.getContext(), "entry", T));
    llvm::SmallVector<llvm::Value *, 8> Args;
    if (needsLink(R))
      Args.push_back(llvm::ConstantPointerNull::get(B.getPtrTy()));
    for (unsigned I = 0; I < T->arg_size(); ++I) {
      if (I >= First)
        T->getArg(I)->setName(R->getParams()[I - First]->getName().Name);
      Args.push_back(T->getArg(I));
    }
    auto *Call = TB.CreateCall(Target, Args);
    if (First)
      addResultCallAttrs(Call, needsLink(R) ? 1 : 0, R->getResultType());
    llvm::Value *V = Call;
    if (Result->isVoidTy())
      TB.CreateRetVoid();
    else
      TB.CreateRet(V);
    CThunks[R] = T;
  }

  /// Thunks that give a top-level routine the {link, params} signature its
  /// procedural values have (item 20).
  llvm::DenseMap<RoutineDecl *, llvm::Function *> LinkThunks;

  /// The function a procedural value of routine R points to.
  llvm::Value *routineFunction(RoutineDecl *R) {
    if (R->isExternal())
      return ExternalFns[R].getCallee();
    if (llvm::Function *T = CThunks.lookup(R))
      return T;
    if (needsLink(R))
      return Functions[R];
    if (llvm::Function *T = LinkThunks.lookup(R))
      return T;
    llvm::Function *Target = Functions[R];
    auto *T = llvm::Function::Create(
        routineFnType(R->getParams(),
                      R->isFunction() ? R->getResultType() : nullptr, false,
                      /*HasLink=*/true),
        llvm::Function::InternalLinkage,
        "linkthunk." + Target->getName(), M);
    T->arg_begin()->setName("link");
    unsigned First = R->isFunction() && returnsByPointer(R->getResultType());
    if (First)
      addResultParamAttrs(T->getArg(1), R->getResultType());
    llvm::IRBuilder<> TB(llvm::BasicBlock::Create(M.getContext(), "entry", T));
    llvm::SmallVector<llvm::Value *, 8> Args;
    for (unsigned I = 0; I < T->arg_size(); ++I) {
      if (I == 0) // the link this thunk ignores
        continue;
      if (I - 1 >= First)
        T->getArg(I)->setName(R->getParams()[I - 1 - First]->getName().Name);
      Args.push_back(T->getArg(I));
    }
    auto *Call = TB.CreateCall(Target, Args);
    if (First)
      addResultCallAttrs(Call, 0, R->getResultType());
    llvm::Value *V = Call;
    if (V->getType()->isVoidTy())
      TB.CreateRetVoid();
    else
      TB.CreateRet(V);
    LinkThunks[R] = T;
    return T;
  }

  void declareRoutines(Block &Blk) {
    for (Decl *D : withNested(Blk.getDecls())) {
      if (ClassType *C = definedClass(D)) {
        declareClass(D, C);
        continue;
      }
      if (InterfaceType *I = definedInterface(D)) {
        declareInterface(D, I);
        continue;
      }
      if (ClassType *MT = advancedRecord(D)) {
        // A record of a separately compiled unit: its methods, declared.
        for (Decl *M : MT->getMembers())
          if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && ImportedDecls.count(R)) {
            auto *F = llvm::Function::Create(routineFnType(R),
                                             llvm::Function::ExternalLinkage,
                                             ExportNames[R], this->M);
            addResultAttrs(F, R);
            Functions[R] = F;
          }
        continue;
      }
      auto *R = llvm::dyn_cast<RoutineDecl>(D);
      if (R && R->isRedeclaration() && R->getCanonical()->isMethod()) {
        declareRoutine(R->getCanonical()); // a method's implementation
        continue;
      }
      if (!R || R->isRedeclaration() || R->isGeneric())
        continue; // the forward declaration is the canonical node; a
                  // generic is compiled as its specializations
      if (R->isExternal()) {
        declareExternal(R);
        continue;
      }
      if (ImportedDecls.count(R)) {
        // A routine of a separately compiled unit: declared only.
        auto *F = llvm::Function::Create(
            routineFnType(R), llvm::Function::ExternalLinkage, ExportNames[R], M);
        addResultAttrs(F, R);
        Functions[R] = F;
        declareCThunk(R);
        if (R->isAsync() || R->isIterator())
          declareAsync(R, ExportNames[R], /*External=*/true);
        continue;
      }
      declareRoutine(R);
    }
  }

  /// An interface's iid: a constant holding its name (length-prefixed),
  /// pas.<unit>$<name>$iid for an interface of a unit's interface.
  llvm::DenseMap<InterfaceType *, llvm::GlobalVariable *> IIDs;

  void declareInterface(Decl *D, InterfaceType *I) {
    auto Export = ExportNames.find(D);
    std::string Text = std::string(1, static_cast<char>(I->getDeclName().size())) +
                       I->getDeclName().str();
    llvm::Constant *Init =
        llvm::ConstantDataArray::getString(M.getContext(), Text, false);
    auto *G = new llvm::GlobalVariable(
        M, Init->getType(), /*isConstant=*/true,
        Export == ExportNames.end() ? llvm::GlobalValue::InternalLinkage
                                    : llvm::GlobalValue::ExternalLinkage,
        ImportedDecls.count(D) ? nullptr : Init,
        Export == ExportNames.end() ? "iid." + I->getDeclName().lower()
                                    : Export->second);
    IIDs[I] = G;
  }

  /// The interface table of class C (null without interfaces): its
  /// interfaces' iids and method tables.
  llvm::Constant *interfaceTable(ClassType *C) {
    bool Cleanup = hasManagedFields(C);
    if (C->getInterfaces().empty() && !Cleanup)
      return llvm::ConstantPointerNull::get(B.getPtrTy());
    std::string Name = C->getDeclName().lower();
    llvm::Constant *CleanupFn = llvm::ConstantPointerNull::get(B.getPtrTy());
    if (Cleanup) {
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
          llvm::Function::InternalLinkage, "cleanup." + Name, M);
      F->getArg(0)->setName("self");
      CleanupsToEmit.push_back({C, F});
      CleanupFn = F;
    }
    llvm::SmallVector<llvm::Constant *, 8> Entries{
        llvm::ConstantExpr::getIntToPtr(B.getInt64(C->getInterfaces().size()),
                                        B.getPtrTy()),
        CleanupFn};
    for (const InterfaceImpl &I : C->getInterfaces()) {
      llvm::SmallVector<llvm::Constant *, 8> Methods;
      for (RoutineDecl *R : I.Methods)
        Methods.push_back(
            R->isAbstract()
                ? llvm::cast<llvm::Function>(
                      classRuntime("__pas_abstract_error", B.getVoidTy(), {})
                          .getCallee())
                : static_cast<llvm::Constant *>(Functions[R]));
      auto *Ty = llvm::ArrayType::get(B.getPtrTy(), Methods.size());
      auto *T = new llvm::GlobalVariable(
          M, Ty, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
          llvm::ConstantArray::get(Ty, Methods),
          "itable." + Name + "." + I.Intf->getDeclName().lower());
      Entries.push_back(iidOf(I.Intf));
      Entries.push_back(T);
    }
    auto *Ty = llvm::ArrayType::get(B.getPtrTy(), Entries.size());
    return new llvm::GlobalVariable(M, Ty, /*isConstant=*/true,
                                    llvm::GlobalValue::InternalLinkage,
                                    llvm::ConstantArray::get(Ty, Entries),
                                    "intfs." + Name);
  }

  /// A class: its VMT (defined once all methods are declared), and for a
  /// class of a separately compiled unit its methods' declarations.
  void declareClass(Decl *D, ClassType *C) {
    if (ClassSet.insert(C).second)
      AllClasses.push_back(C);
    auto Export = ExportNames.find(D);
    bool Imported = ImportedDecls.count(D);
    auto *Ty = llvm::ArrayType::get(B.getPtrTy(), 6 + C->getVTable().size());
    std::string Name = Export == ExportNames.end()
                           ? "vmt." + C->getDeclName().lower()
                           : Export->second;
    VMTs[C] = new llvm::GlobalVariable(
        M, Ty, /*isConstant=*/true,
        Export == ExportNames.end() ? llvm::GlobalValue::InternalLinkage
                                    : llvm::GlobalValue::ExternalLinkage,
        nullptr, Name);
    if (!Imported) {
      VMTsToDefine.push_back(C);
      return;
    }
    for (Decl *M : C->getMembers())
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
          R && !R->isAbstract() && !R->isGenericMethod()) {
        Functions[R] = llvm::Function::Create(routineFnType(R),
                                              llvm::Function::ExternalLinkage,
                                              ExportNames[R], this->M);
        if (R->isAsync() || R->isIterator())
          declareAsync(R, ExportNames[R], /*External=*/true);
      }
  }

  /// A runtime function used only by programs with classes.
  llvm::FunctionCallee classRuntime(llvm::StringRef Name, llvm::Type *Ret,
                                    llvm::ArrayRef<llvm::Type *> Params) {
    llvm::FunctionCallee F = M.getOrInsertFunction(
        Name, llvm::FunctionType::get(Ret, Params, /*isVarArg=*/false));
    auto *Fn = llvm::cast<llvm::Function>(F.getCallee());
    Fn->addFnAttr(llvm::Attribute::NoUnwind);
    addRuntimeAttrs(Fn);
    RuntimeFns.insert(Fn);
    return F;
  }

  /// Can a call of the runtime function Name raise an exception? Those
  /// that raise one, and those that may run a destructor (which is Pascal
  /// code): releasing an object or an interface reference.
  static bool runtimeMayRaise(llvm::StringRef Name) {
    return Name == "__pas_raise" || Name == "__pas_raise_at" ||
           Name == "__pas_raise_divzero" || Name == "__pas_raise_overflow" ||
           Name == "__pas_reraise" || Name == "__pas_reraise_free" ||
           Name == "__pas_free_object" || Name == "__pas_free_instance" ||
           Name == "__pas_intf_assign" || Name == "__pas_intf_clear" ||
           Name == "__pas_frame_release" || Name == "__pas_closure_assign" ||
           Name == "__pas_task_await" || Name == "__pas_scope_leave" ||
           Name == "__pas_task_assign" || Name == "__pas_task_clear" ||
           Name == "__pas_gen_next" || Name == "__pas_gen_yield" ||
           Name == "__pas_gen_assign" || Name == "__pas_gen_clear" ||
           Name == "__pas_closure_clear" ||
           Name == "__pas_dyn_clear" || Name == "__pas_dyn_assign" ||
           Name == "__pas_dyn_setlength" ||
           Name == "__pas_format_pstr" ||
           Name == "__pas_owned_set" || Name == "__pas_owned_done";
  }

  /// Does the code emitted for the current function so far have a call
  /// that may raise an exception? Any call does, except of an LLVM
  /// intrinsic, of a runtime function that cannot (runtimeMayRaise), of
  /// the function itself and of a routine emitted before that was found
  /// not to (NoRaiseFns). So calls through procedural values and virtual
  /// methods, of external routines and of routines emitted later count:
  /// this errs on the side of protecting.
  bool mayRaise() const {
    for (const llvm::BasicBlock &BB : *CurFn)
      for (const llvm::Instruction &I : BB) {
        auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call)
          continue;
        const llvm::Function *Callee = Call->getCalledFunction();
        if (!Callee)
          return true;
        if (Callee->isIntrinsic() || Callee == CurFn || NoRaiseFns.count(Callee))
          continue;
        if (!RuntimeFns.count(Callee) || runtimeMayRaise(Callee->getName()))
          return true;
      }
    return false;
  }

  /// The VMT of C; the predeclared classes' (TObject, Exception, EDivByZero
  /// and its ancestors) are in the runtime, as __pas_<class>_vmt.
  llvm::Constant *vmtOf(ClassType *C) {
    if (llvm::GlobalVariable *G = VMTs.lookup(C))
      return G;
    std::string Name = "__pas_" + C->getDeclName().lower() + "_vmt";
    llvm::GlobalVariable *G = M.getNamedGlobal(Name);
    if (!G)
      G = new llvm::GlobalVariable(
          M, llvm::ArrayType::get(B.getPtrTy(), 6 + C->getVTable().size()),
          /*isConstant=*/true, llvm::GlobalValue::ExternalLinkage, nullptr,
          Name);
    VMTs[C] = G;
    return G;
  }

  /// The depth of C in its hierarchy (TObject: 0).
  static unsigned classDepth(ClassType *C) {
    unsigned Depth = 0;
    for (ClassType *A = C; A && A->getParent(); A = A->getParent())
      ++Depth;
    return Depth;
  }

  /// The VMTs from C's root to C itself, for a VMT's ancestor display.
  llvm::SmallVector<llvm::Constant *, 8> classAncestors(ClassType *C) {
    llvm::SmallVector<llvm::Constant *, 8> Ancestors;
    for (ClassType *A = C; A; A = A->getParent())
      Ancestors.push_back(vmtOf(A));
    std::reverse(Ancestors.begin(), Ancestors.end());
    return Ancestors;
  }

  /// Fills in the VMTs: parent VMT, class name, instance size, interface
  /// table, hierarchy depth, ancestor display, then the virtual methods.
  void defineVMTs() {
    for (ClassType *C : VMTsToDefine) {
      llvm::SmallVector<llvm::Constant *, 8> Slots;
      Slots.push_back(vmtOf(C->getParent()));
      Slots.push_back(getPStrConstant(C->getDeclName()));
      Slots.push_back(llvm::ConstantExpr::getIntToPtr(
          B.getInt64(instanceSize(C)), B.getPtrTy()));
      Slots.push_back(interfaceTable(C));
      llvm::SmallVector<llvm::Constant *, 8> Ancestors = classAncestors(C);
      auto *AncTy = llvm::ArrayType::get(B.getPtrTy(), Ancestors.size());
      auto *Anc = new llvm::GlobalVariable(
          M, AncTy, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
          llvm::ConstantArray::get(AncTy, Ancestors),
          "ancestors." + C->getDeclName().lower());
      Anc->setAlignment(llvm::Align(8));
      Slots.push_back(llvm::ConstantExpr::getIntToPtr(
          B.getInt64(Ancestors.size() - 1), B.getPtrTy()));
      Slots.push_back(Anc);
      for (RoutineDecl *R : C->getVTable()) {
        if (R->getBuiltin() == TObjectMethod::Destroy)
          Slots.push_back(llvm::cast<llvm::Function>(
              classRuntime("__pas_tobject_destroy", B.getVoidTy(),
                           {B.getPtrTy(), B.getPtrTy()})
                  .getCallee()));
        else if (R->isAbstract())
          Slots.push_back(llvm::cast<llvm::Function>(
              classRuntime("__pas_abstract_error", B.getVoidTy(), {})
                  .getCallee()));
        else
          Slots.push_back(Functions[R]);
      }
      auto *G = llvm::cast<llvm::GlobalVariable>(VMTs[C]);
      G->setInitializer(llvm::ConstantArray::get(
          llvm::cast<llvm::ArrayType>(G->getValueType()), Slots));
    }
    VMTsToDefine.clear();
  }

  /// A by-reference parameter is an address: never nil, and it points to a
  /// value of its type (item 9). LLVM cannot infer this across a call.
  void addByRefParamAttrs(llvm::Argument *A, Type *T) {
    llvm::AttrBuilder B(A->getContext());
    B.addAttribute(llvm::Attribute::NonNull);
    if (!T->isUntyped())
      B.addDereferenceableAttr(DL.getTypeStoreSize(memType(T)).getFixedValue());
    A->addAttrs(B);
  }

  /// The hidden result pointer is a fresh caller temporary (item 4): it
  /// cannot alias an argument, and LLVM's ABI can return through it.
  void addResultParamAttrs(llvm::Argument *A, Type *T) {
    llvm::AttrBuilder B(A->getContext());
    B.addAttribute(llvm::Attribute::NoAlias);
    B.addStructRetAttr(memType(T));
    B.addAttribute(llvm::Attribute::NonNull);
    B.addDereferenceableAttr(DL.getTypeStoreSize(memType(T)).getFixedValue());
    A->addAttrs(B);
  }

  /// The same facts at a call site, where they change the ABI on targets
  /// with an indirect-result convention.
  void addResultCallAttrs(llvm::CallBase *CB, unsigned ArgNo, Type *T) {
    CB->addParamAttr(ArgNo, llvm::Attribute::NoAlias);
    CB->addParamAttr(ArgNo,
                     llvm::Attribute::getWithStructRetType(B.getContext(),
                                                           memType(T)));
  }

  /// Marks the hidden result pointer of a function declaration, if it has
  /// one.
  void addResultAttrs(llvm::Function *F, RoutineDecl *R) {
    if (!R->isFunction() || !returnsByPointer(R->getResultType()))
      return;
    unsigned ArgNo = needsLink(R) ? 1 : 0;
    addResultParamAttrs(F->getArg(ArgNo), R->getResultType());
  }

  /// Creates the LLVM function and frame type of routine R (a method: of
  /// its canonical declaration), queues its body and declares the
  /// routines nested in it.
  void declareRoutine(RoutineDecl *R) {
    // Frame record: [static link], then those of [Self], params, locals
    // and [result] that have to be in it (see inFrame). The link is there
    // for the routines nested in R, which follow it to R's parents.
    llvm::SmallVector<llvm::Type *, 8> Slots;
    bool HasNested = false;
    for (Decl *Local : R->getBody()->getDecls())
      if (auto *N = llvm::dyn_cast<RoutineDecl>(Local))
        HasNested |= !N->isExternal();
    if (R->hasStaticLink() && HasNested) {
      LinkInFrame.insert(R);
      Slots.push_back(B.getPtrTy());
    }
    bool PinAll = hasNonLocalLabels(*R->getBody());
    VarSet PinVars;
    if (!PinAll && R->hasTry())
      PinVars = PinAnalysis(R).run();
    auto AddSlot = [&](VarDecl *V) {
      if (!inFrame(V, PinAll, PinVars))
        return;
      FrameSlots[V] = Slots.size();
      Slots.push_back(isByRef(V) ? B.getPtrTy() : memType(V->getType()));
    };
    if (R->hasSelf())
      AddSlot(R->getSelfVar());
    for (VarDecl *P : R->getParams())
      AddSlot(P);
    for (Decl *Local : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(Local)) {
        if (V->isTypedConst())
          emitStaticVar(V); // static storage, local scope
        else if (!V->getAbsolute())
          AddSlot(V);
      }
    if (R->isFunction())
      AddSlot(R->getResultVar());
    std::string Name = mangle(R);
    if (!Slots.empty())
      FrameTypes[R] = llvm::StructType::create(M.getContext(), Slots,
                                               "frame." + Name.substr(4));

    if (hasNonLocalLabels(*R->getBody())) {
      JmpBufSlots[R] = Slots.size();
      Slots.push_back(jmpBufType());
      FrameTypes[R] = llvm::StructType::create(M.getContext(), Slots,
                                               "frame." + Name.substr(4) + ".jb");
    }

    auto Export = ExportNames.find(R);
    auto *F = llvm::Function::Create(
        routineFnType(R),
        Export == ExportNames.end() ? llvm::Function::InternalLinkage
                                    : llvm::Function::ExternalLinkage,
        Export == ExportNames.end() ? Name : Export->second, M);
    unsigned ArgNo = 0;
    if (needsLink(R))
      F->getArg(ArgNo++)->setName("link");
    if (R->isFunction() && returnsByPointer(R->getResultType())) {
      llvm::Argument *Result = F->getArg(ArgNo++);
      Result->setName("result.ptr");
      addResultParamAttrs(Result, R->getResultType());
    }
    if (R->hasSelf()) {
      llvm::Argument *Self = F->getArg(ArgNo++);
      Self->setName("self");
      if (R->getClass() && R->getClass()->hasSelfByReference())
        addByRefParamAttrs(Self, R->getSelfVar()->getType());
    }
    for (VarDecl *P : R->getParams()) {
      llvm::Argument *A = F->getArg(ArgNo++);
      A->setName(P->getName().Name);
      if (isByRef(P))
        addByRefParamAttrs(A, P->getType());
    }
    if (R->isInline()) // the `inline` directive
      F->addFnAttr(llvm::Attribute::InlineHint);
    Functions[R] = F;
    declareCThunk(R);
    if (R->isAsync() || R->isIterator())
      declareAsync(R, Export == ExportNames.end() ? Name : Export->second,
                   Export != ExportNames.end());
    RoutinesToEmit.push_back(R);
    if (DIB)
      // File scope even for nested routines: lexical nesting would make
      // debuggers look up enclosing variables through DW_AT_static_link,
      // which LLVM cannot emit. Use the debugger's `up` to reach them.
      Subprograms[R] = createSubprogram(
          F,
          R->isMethod() ? R->getClass()->getName() + "." +
                              R->getName().Spelling.str()
                        : R->getName().Spelling.str(),
          diFileFor(R->getBodyLocation()), lineOf(R->getBodyLocation()),
          R->isFunction() ? R->getResultType() : nullptr, R->getParams(),
          diFileFor(R->getBodyLocation()),
          R->hasSelf() ? R->getSelfVar() : nullptr);

    declareRoutines(*R->getBody());
  }

  /// The variables of R's frame that hold managed values R releases when
  /// it returns: its value parameters (one more reference each) and local
  /// variables.
  llvm::SmallVector<VarDecl *, 8> managedFrameVars(RoutineDecl *R) {
    llvm::SmallVector<VarDecl *, 8> Vars;
    for (VarDecl *P : R->getParams())
      if (!isByRef(P) && !P->isConstParam() && !P->isVarParam() &&
          isManaged(P->getType()))
        Vars.push_back(P);
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D);
          V && !V->isTypedConst() && !V->getAbsolute() &&
          isManaged(V->getType()))
        Vars.push_back(V);
    return Vars;
  }

  /// R's frame is on the heap (a closure made in it uses it): the function
  /// that finalizes it when its last reference goes, frame.fini.<R>. It
  /// releases the managed variables (as R does on its way out when its
  /// frame is not on the heap) and the frame of R's parent, if that is on
  /// the heap too (R's frame holds a reference to it, its static link).
  llvm::Function *emitFrameFini(RoutineDecl *R) {
    llvm::StructType *FrameTy = FrameTypes.lookup(R);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
        llvm::Function::InternalLinkage, "frame.fini." + mangle(R).substr(4), M);
    Fn->getArg(0)->setName("frame");
    CurFn = Fn;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", Fn));
    llvm::Value *Frame = Fn->getArg(0);
    for (VarDecl *V : managedFrameVars(R))
      finalizeValue(B.CreateStructGEP(FrameTy, Frame, FrameSlots[V]),
                    V->getType());
    if (parentFrameOnHeap(R))
      B.CreateCall(frameRuntime("__pas_frame_release"),
                   {B.CreateLoad(B.getPtrTy(), B.CreateStructGEP(FrameTy, Frame, 0),
                                 "link")});
    B.CreateRetVoid();
    return Fn;
  }

  /// Does R's frame hold its static link, to a frame on the heap?
  bool parentFrameOnHeap(RoutineDecl *R) {
    return LinkInFrame.count(R) && R->getParent() &&
           R->getParent()->hasHeapFrame() && FrameTypes.lookup(R->getParent());
  }

  /// __pas_frame_new, __pas_frame_addref / release (runtime/io.c).
  llvm::FunctionCallee frameRuntime(llvm::StringRef Name) {
    if (Name == "__pas_frame_new")
      return classRuntime(Name, B.getPtrTy(), {B.getInt64Ty(), B.getPtrTy()});
    return classRuntime(Name, B.getVoidTy(), {B.getPtrTy()});
  }

  /// An async routine's task: its arguments' record [link, Self, params]
  /// (copies), its entry R.task(task) (calls R with them, stores the
  /// result in the task) and R.taskfini(args) (finalizes them).
  struct AsyncParts {
    llvm::StructType *ArgsTy;
    llvm::Function *Entry, *Fini; ///< Fini: null when nothing is counted
  };
  llvm::DenseMap<RoutineDecl *, AsyncParts> Asyncs;
  std::vector<RoutineDecl *> AsyncsToEmit;

  /// (Name: R's function's; External: R is exported or imported, and so
  /// are its task's functions.)
  void declareAsync(RoutineDecl *R, const std::string &Name, bool External) {
    auto Linkage = External ? llvm::Function::ExternalLinkage
                            : llvm::Function::InternalLinkage;
    llvm::SmallVector<llvm::Type *, 8> Slots{B.getPtrTy()}; // the link
    if (R->hasSelf())
      Slots.push_back(B.getPtrTy());
    bool Counted = false;
    for (VarDecl *P : R->getParams()) {
      Slots.push_back(memType(P->getType()));
      Counted |= isManaged(P->getType());
    }
    AsyncParts A;
    A.ArgsTy = llvm::StructType::create(M.getContext(), Slots,
                                        "args." + Name.substr(4));
    A.Entry = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false), Linkage,
        Name + (R->isIterator() ? ".gen" : ".task"), M);
    A.Entry->getArg(0)->setName(R->isIterator() ? "gen" : "task");
    A.Fini = nullptr;
    if (Counted) {
      A.Fini = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false), Linkage,
          Name + (R->isIterator() ? ".genfini" : ".taskfini"), M);
      A.Fini->getArg(0)->setName("args");
    }
    Asyncs[R] = A;
  }

  /// The parameters of R from its task's arguments: their slot index.
  unsigned firstParamSlot(RoutineDecl *R) { return R->hasSelf() ? 2 : 1; }

  void emitAsyncParts(RoutineDecl *R) {
    AsyncParts &A = Asyncs[R];
    llvm::PointerType *Ptr = B.getPtrTy();
    // R.task(task): R(args...), its result into the task's
    CurFn = A.Entry;
    AddrAligns.clear();
    CurRoutine = nullptr;
    CurFrame = nullptr;
    CurPin = nullptr;
    PinTemps = true;
    CurSP = nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", A.Entry));
    llvm::Value *Task = A.Entry->getArg(0);
    llvm::Value *Args = B.CreateCall(
        classRuntime(R->isIterator() ? "__pas_gen_args" : "__pas_task_args", Ptr, {Ptr}),
        {Task}, "args");
    llvm::SmallVector<llvm::Value *, 8> Values;
    Values.push_back(B.CreateLoad(Ptr, B.CreateStructGEP(A.ArgsTy, Args, 0), "link"));
    llvm::Value *ResultSlot = nullptr;
    if (R->isFunction())
      ResultSlot = B.CreateCall(classRuntime("__pas_task_result", Ptr, {Ptr}),
                                {Task}, "result");
    bool ByPointer = R->isFunction() && returnsByPointer(R->getResultType());
    unsigned ResultArgNo = Values.size();
    if (ByPointer)
      Values.push_back(ResultSlot);
    if (R->hasSelf())
      Values.push_back(B.CreateLoad(Ptr, B.CreateStructGEP(A.ArgsTy, Args, 1), "self"));
    unsigned Slot = firstParamSlot(R);
    for (VarDecl *P : R->getParams()) {
      llvm::Value *Addr = B.CreateStructGEP(A.ArgsTy, Args, Slot++);
      Type *T = P->getType();
      if (T->isStructured()) // (the callee copies a value parameter)
        Values.push_back(Addr);
      else
        Values.push_back(loadScalar(Addr, T, P->getName().Name));
    }
    auto *Call = emitCallOrInvoke(Functions[R], Values);
    if (ByPointer)
      addResultCallAttrs(Call, ResultArgNo, R->getResultType());
    llvm::Value *Result = Call;
    if (R->isFunction() && !ByPointer) {
      Type *T = R->getResultType();
      if (T->isBoolean() || T->isOrdinal() || T->isReal())
        storeScalar(ResultSlot, Result, T);
      else // (a counted value moves into the task)
        B.CreateStore(Result, ResultSlot);
    }
    B.CreateRetVoid();
    if (!A.Fini)
      return;
    // R.taskfini(args): the counted arguments released
    CurFn = A.Fini;
    AddrAligns.clear();
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", A.Fini));
    Slot = firstParamSlot(R);
    for (VarDecl *P : R->getParams()) {
      llvm::Value *Addr = B.CreateStructGEP(A.ArgsTy, A.Fini->getArg(0), Slot++);
      if (isManaged(P->getType()))
        finalizeValue(Addr, P->getType());
    }
    B.CreateRetVoid();
  }

  /// How the next async call starts its task (set by spawn and Pool.Run,
  /// taken by the call).
  enum class StartMode { Here, Spawn, Pool };
  StartMode NextStart = StartMode::Here;

  /// A call of async routine R (link, Self: Self's value or null): its
  /// task, made with copies of the arguments and started (here: it runs
  /// until it waits or ends).
  llvm::Value *emitAsyncCall(RoutineDecl *R, llvm::Value *Link, llvm::Value *Self,
                             llvm::ArrayRef<Expr *> Args) {
    StartMode Mode = NextStart;
    NextStart = StartMode::Here;
    AsyncParts &A = Asyncs[R];
    llvm::PointerType *Ptr = B.getPtrTy();
    llvm::Constant *Info = R->isFunction()
                               ? typeInfoOf(R->getResultType())
                               : llvm::ConstantPointerNull::get(Ptr);
    llvm::Constant *Fini = A.Fini ? static_cast<llvm::Constant *>(A.Fini)
                                  : llvm::ConstantPointerNull::get(Ptr);
    llvm::Value *Task = B.CreateCall(
        classRuntime("__pas_task_new", Ptr, {Ptr, B.getInt64Ty(), Ptr, Ptr}),
        {Info, B.getInt64(DL.getTypeAllocSize(A.ArgsTy)), A.Entry, Fini}, "task");
    llvm::Value *ArgsAddr =
        B.CreateCall(classRuntime("__pas_task_args", Ptr, {Ptr}), {Task}, "args");
    B.CreateStore(Link ? Link : llvm::ConstantPointerNull::get(Ptr),
                  B.CreateStructGEP(A.ArgsTy, ArgsAddr, 0));
    if (R->hasSelf())
      B.CreateStore(Self, B.CreateStructGEP(A.ArgsTy, ArgsAddr, 1));
    unsigned Slot = firstParamSlot(R);
    auto Params = R->getParams();
    for (size_t I = 0; I < Params.size(); ++I) {
      Expr *Arg = I < Args.size() ? Args[I] : Params[I]->getDefault();
      storeExpr(B.CreateStructGEP(A.ArgsTy, ArgsAddr, Slot++), Params[I]->getType(),
                Arg);
    }
    const char *Start = Mode == StartMode::Spawn  ? "__pas_task_spawn"
                        : Mode == StartMode::Pool ? "__pas_task_run_pool"
                                                  : "__pas_task_start";
    B.CreateCall(classRuntime(Start, B.getVoidTy(), {Ptr}), {Task});
    return holdResult(Task, R->getAsyncType());
  }

  /// A call of iterator routine R (link, Self: Self's value or null): its
  /// iterator, made with copies of the arguments; its body runs when its
  /// values are asked for.
  llvm::Value *emitIteratorCall(RoutineDecl *R, llvm::Value *Link, llvm::Value *Self,
                                llvm::ArrayRef<Expr *> Args) {
    AsyncParts &A = Asyncs[R];
    llvm::PointerType *Ptr = B.getPtrTy();
    llvm::Constant *Fini = A.Fini ? static_cast<llvm::Constant *>(A.Fini)
                                  : llvm::ConstantPointerNull::get(Ptr);
    llvm::Constant *Info = typeInfoOf(R->getIteratorType()->getElementType());
    llvm::Value *Gen = B.CreateCall(
        classRuntime("__pas_gen_new", Ptr, {Ptr, B.getInt64Ty(), Ptr, Ptr}),
        {Info, B.getInt64(DL.getTypeAllocSize(A.ArgsTy)), A.Entry, Fini}, "iter");
    llvm::Value *ArgsAddr =
        B.CreateCall(classRuntime("__pas_gen_args", Ptr, {Ptr}), {Gen}, "args");
    B.CreateStore(Link ? Link : llvm::ConstantPointerNull::get(Ptr),
                  B.CreateStructGEP(A.ArgsTy, ArgsAddr, 0));
    if (R->hasSelf())
      B.CreateStore(Self, B.CreateStructGEP(A.ArgsTy, ArgsAddr, 1));
    unsigned Slot = firstParamSlot(R);
    auto Params = R->getParams();
    for (size_t I = 0; I < Params.size(); ++I) {
      Expr *Arg = I < Args.size() ? Args[I] : Params[I]->getDefault();
      storeExpr(B.CreateStructGEP(A.ArgsTy, ArgsAddr, Slot++), Params[I]->getType(),
                Arg);
    }
    return holdResult(Gen, R->getIteratorType());
  }

  /// IteratorNext(it): runs it to its next value; an i1.
  llvm::Value *emitIteratorNext(llvm::Value *It) {
    llvm::Value *R = B.CreateCall(
        classRuntime("__pas_gen_next", B.getInt32Ty(), {B.getPtrTy()}), {It}, "more");
    return B.CreateICmpNE(R, B.getInt32(0), "more");
  }

  /// The address of iterator It's current value.
  llvm::Value *iteratorValueAddr(llvm::Value *It) {
    return B.CreateCall(classRuntime("__pas_gen_value", B.getPtrTy(), {B.getPtrTy()}),
                        {It}, "value");
  }

  /// await / Wait: waits for task T (of type TT); its result (an address
  /// for a structured type), or null without one.
  llvm::Value *emitAwait(llvm::Value *T, TaskType *TT) {
    llvm::PointerType *Ptr = B.getPtrTy();
    B.CreateCall(classRuntime("__pas_task_await", B.getVoidTy(), {Ptr}), {T});
    Type *RT = TT->getResult();
    if (!RT)
      return nullptr;
    llvm::Value *Res =
        B.CreateCall(classRuntime("__pas_task_result", Ptr, {Ptr}), {T}, "result");
    if (RT->isStructured())
      return Res;
    return loadScalar(Res, RT, "awaited");
  }

public:
  llvm::Value *visitAwaitExpr(AwaitExpr *E) {
    llvm::Value *T = visitExpr(E->getOperand());
    return emitAwait(T, llvm::cast<TaskType>(E->getOperand()->getType()));
  }

  llvm::Value *visitSpawnExpr(SpawnExpr *E) {
    NextStart = E->isPoolRun() ? StartMode::Pool : StartMode::Spawn;
    llvm::Value *T = visitExpr(E->getCall());
    NextStart = StartMode::Here;
    return T;
  }

  void visitExprStmt(ExprStmt *S) { visitExpr(S->getExpr()); }

  void visitScopeStmt(ScopeStmt *S) {
    llvm::PointerType *Ptr = B.getPtrTy();
    llvm::Value *Token = S->getToken() && !S->getToken()->getType()->isNil()
                             ? visitExpr(S->getToken())
                             : llvm::ConstantPointerNull::get(Ptr);
    llvm::Value *Scope = B.CreateCall(
        classRuntime("__pas_scope_enter", Ptr, {Ptr}), {Token}, "scope");
    llvm::Value *Slot = createTemp(Ptr, "scope.slot");
    B.CreateStore(Scope, Slot);
    pinInMemory(Slot);
    emitTryFinally([&] { emitSequence(S->getBody()); },
                   [&](llvm::Value *State) {
                     llvm::Value *Raised = B.CreateICmpEQ(
                         B.CreateLoad(B.getInt32Ty(), State, "state"),
                         B.getInt32(1), "raised");
                     B.CreateCall(classRuntime("__pas_scope_leave", B.getVoidTy(),
                                               {Ptr, B.getInt32Ty()}),
                                  {B.CreateLoad(Ptr, Slot, "scope"),
                                   B.CreateZExt(Raised, B.getInt32Ty())});
                   });
  }

  /// yield e / yield break / yield from it.
  void visitYieldStmt(YieldStmt *S) {
    llvm::PointerType *Ptr = B.getPtrTy();
    IteratorType *IT = CurRoutine->getIteratorType();
    Type *ET = IT->getElementType();
    // (each value is assigned into the slot: what it held is released)
    auto YieldFrom = [&](llvm::Value *Src, Type *SrcET) {
      llvm::Value *Slot =
          B.CreateCall(classRuntime("__pas_gen_slot", Ptr, {}), {}, "slot");
      if (ET->isStructured()) // (the same type: Sema)
        copyValue(Slot, Src, ET);
      else
        storeValue(Slot,
                   convertForAssign(loadScalar(Src, SrcET), SrcET, ET,
                                    S->getBeginLoc()),
                   ET);
      B.CreateCall(classRuntime("__pas_gen_yield", B.getVoidTy(), {}), {});
    };
    switch (S->getYieldKind()) {
    case YieldStmt::Break:
      emitJump(ExitBlock, 0);
      B.SetInsertPoint(newBlock("after.jump")); // unreachable code follows
      return;
    case YieldStmt::Value: {
      llvm::Value *Slot =
          B.CreateCall(classRuntime("__pas_gen_slot", Ptr, {}), {}, "slot");
      storeExpr(Slot, ET, S->getValue());
      B.CreateCall(classRuntime("__pas_gen_yield", B.getVoidTy(), {}), {});
      return;
    }
    case YieldStmt::From: {
      Expr *E = S->getValue();
      Type *SrcET = llvm::cast<IteratorType>(E->getType())->getElementType();
      llvm::Value *Held = createTemp(Ptr, "from");
      B.CreateStore(llvm::ConstantPointerNull::get(Ptr), Held);
      storeValue(Held, visitExpr(E), E->getType()); // (counted)
      emitTryFinally(
          [&] {
            auto *CondBB = newBlock("from.cond");
            auto *BodyBB = newBlock("from.body");
            auto *EndBB = newBlock("from.end");
            B.CreateBr(CondBB);
            B.SetInsertPoint(CondBB);
            llvm::Value *I = B.CreateLoad(Ptr, Held, "from");
            B.CreateCondBr(emitIteratorNext(I), BodyBB, EndBB);
            B.SetInsertPoint(BodyBB);
            YieldFrom(iteratorValueAddr(B.CreateLoad(Ptr, Held, "from")), SrcET);
            B.CreateBr(CondBB);
            B.SetInsertPoint(EndBB);
          },
          [&](llvm::Value *) { finalizeValue(Held, E->getType()); });
      return;
    }
    }
  }

  void visitDetachedStmt(DetachedStmt *S) {
    llvm::PointerType *Ptr = B.getPtrTy();
    llvm::Value *T = visitExpr(S->getCall());
    llvm::Value *Fn = llvm::ConstantPointerNull::get(Ptr), *Link = Fn;
    if (Expr *H = S->getHandler()) {
      llvm::Value *Pair = emitRoutineValue(H);
      Fn = B.CreateExtractValue(Pair, 0, "handler");
      Link = B.CreateExtractValue(Pair, 1, "link");
    }
    B.CreateCall(classRuntime("__pas_task_detach", B.getVoidTy(), {Ptr, Ptr, Ptr}),
                 {T, Fn, Link});
  }

  /// WhenAll(tasks) / WhenAny(tasks): the tasks' data and count.
  llvm::Value *emitWhen(CallExpr *E, bool All) {
    llvm::PointerType *Ptr = B.getPtrTy();
    Expr *A = E->getArgs()[0];
    Type *AT = A->getType();
    llvm::Value *Data, *N;
    TaskType *TT;
    if (auto *ST = llvm::dyn_cast<ArrayType>(AT)) {
      Data = emitAddress(A);
      OrdinalBounds Bd = getOrdinalBounds(ST->getIndexType());
      N = B.getInt64(Bd.Hi - Bd.Lo + 1);
      TT = llvm::cast<TaskType>(ST->getElementType());
    } else if (auto *DT = llvm::dyn_cast<DynArrayType>(AT)) {
      Data = visitExpr(A);
      N = dynLength(Data);
      TT = llvm::cast<TaskType>(DT->getElementType());
    } else {
      llvm::Value *Desc = openDescriptor(A);
      Data = B.CreateLoad(Ptr, Desc, "data");
      N = openCount(Desc);
      TT = llvm::cast<TaskType>(llvm::cast<OpenArrayType>(AT)->getElementType());
    }
    auto *RT = llvm::cast<TaskType>(E->getType());
    llvm::Value *Task;
    if (All) {
      llvm::Constant *ResInfo = RT->getResult()
                                    ? typeInfoOf(RT->getResult())
                                    : llvm::ConstantPointerNull::get(Ptr);
      llvm::Constant *Elem = TT->getResult() ? typeInfoOf(TT->getResult())
                                             : llvm::ConstantPointerNull::get(Ptr);
      Task = B.CreateCall(
          classRuntime("__pas_when_all", Ptr, {Ptr, B.getInt64Ty(), Ptr, Ptr}),
          {Data, N, ResInfo, Elem}, "whenall");
    } else {
      Task = B.CreateCall(
          classRuntime("__pas_when_any", Ptr, {Ptr, B.getInt64Ty(), Ptr}),
          {Data, N, typeInfoOf(RT->getResult())}, "whenany");
    }
    return holdResult(Task, RT);
  }

  void emitRoutineBody(RoutineDecl *R) {
    if (R->isAsync() || R->isIterator()) // (its task's parts, after it)
      AsyncsToEmit.push_back(R);
    // A frame on the heap: the function that finalizes it, first.
    llvm::Function *FrameFini =
        FrameTypes.lookup(R) && R->hasHeapFrame() ? emitFrameFini(R) : nullptr;
    llvm::Function *F = Functions[R];
    CurFn = F;
    AddrAligns.clear();
    CurRoutine = R;
    CurSP = DIB ? Subprograms[R] : nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
    setDebugLoc(R->getBodyLocation());

    // The frame record, if any, then an alloca for each other variable.
    // On the heap if a closure made here uses it (counted: see
    // emitFrameFini), zeroed.
    llvm::StructType *FrameTy = FrameTypes.lookup(R);
    if (FrameFini)
      CurFrame = B.CreateCall(
          frameRuntime("__pas_frame_new"),
          {B.getInt64(DL.getTypeAllocSize(FrameTy)), FrameFini}, "frame");
    else
      CurFrame = FrameTy ? B.CreateAlloca(FrameTy, nullptr, "frame") : nullptr;
    CurPin = nullptr;
    // Execution continues here after an exception (see inFrame).
    PinTemps = R->hasTry() || hasNonLocalLabels(*R->getBody());
    if (CurFrame && PinTemps)
      pinInMemory(CurFrame);
    LocalAddrs.clear();
    VarDecl *ResultVar = R->isFunction() ? R->getResultVar() : nullptr;
    llvm::SmallVector<VarDecl *, 8> Locals; // start from zero
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D);
          V && !V->isTypedConst() && !V->getAbsolute())
        Locals.push_back(V);
    if (ResultVar)
      Locals.push_back(ResultVar);
    auto AddLocal = [&](VarDecl *V, bool Param) {
      if (!FrameSlots.count(V))
        LocalAddrs[V] = B.CreateAlloca(
            isByRef(V) ? B.getPtrTy() : memType(V->getType()), nullptr,
            Param ? V->getName().Name + ".addr" : V->getName().Name);
    };
    if (R->hasSelf())
      AddLocal(R->getSelfVar(), true);
    for (VarDecl *P : R->getParams())
      AddLocal(P, true);
    for (VarDecl *V : Locals)
      AddLocal(V, false);
    if (DIB)
      declareFrameVariables(R);
    bool ResultByPointer = ResultVar && isByRef(ResultVar);
    for (VarDecl *V : Locals)
      if (V != ResultVar || !ResultByPointer)
        emitZeroInit(varSlot(V), V->getType());

    unsigned ArgNo = needsLink(R) ? 1 : 0;
    if (LinkInFrame.count(R))
      B.CreateStore(F->getArg(0), B.CreateStructGEP(FrameTy, CurFrame, 0));
    if (FrameFini && parentFrameOnHeap(R)) // the frame holds its parent's
      B.CreateCall(frameRuntime("__pas_frame_addref"), {F->getArg(0)});
    if (ResultByPointer) { // the caller's temporary
      B.CreateStore(F->getArg(ArgNo++), varSlot(ResultVar));
      emitZeroInit(varAddress(ResultVar), ResultVar->getType());
    }
    if (R->hasSelf())
      B.CreateStore(F->getArg(ArgNo++), varSlot(R->getSelfVar()));
    // The variables holding COM interfaces: value parameters (one more
    // reference each), then local variables (nil at first).
    llvm::SmallVector<std::pair<llvm::Value *, Type *>, 4> Managed;
    for (VarDecl *P : R->getParams()) {
      llvm::Value *Arg = F->getArg(ArgNo++);
      llvm::Value *Slot = varSlot(P);
      if (isByRef(P))
        B.CreateStore(Arg, Slot);
      else if (auto *SS = llvm::dyn_cast<ShortStringType>(P->getType()))
        // value semantics: the characters in use, cut to fit
        B.CreateCall(RT.StrAssign, {Slot, B.getInt32(SS->getMaxLength()), Arg});
      else if (P->getType()->isStructured())
        copyAggregate(Slot, Arg, P->getType()); // value semantics
      else
        storeScalar(Slot, Arg, P->getType());
      if (auto *OA = llvm::dyn_cast<OpenArrayType>(P->getType());
          OA && !P->isVarParam() && !P->isConstParam())
        copyOpenArray(Slot, OA);
      if (!isByRef(P) && !P->isConstParam() && !P->isVarParam() &&
          isManaged(P->getType())) {
        addRefValue(Slot, P->getType());
        Managed.push_back({Slot, P->getType()});
      }
    }
    // Initialized variables (objfpc) get their initial values on each call.
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->isInitializedVar())
        B.CreateStore(initializerFor(V, memType(V->getType())), varAddress(V));
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D);
          V && !V->isTypedConst() && !V->getAbsolute() &&
          isManaged(V->getType()))
        Managed.push_back({varAddress(V), V->getType()});
    bool ManagedResult = ResultVar && isManaged(ResultVar->getType());

    initFileVars(R->getBody()->getDecls(), nullptr);
    if (JmpBufSlots.count(R))
      emitSetJmpDispatch(*R->getBody(),
                         B.CreateStructGEP(FrameTy, CurFrame, JmpBufSlots[R]));
    // An exception leaving the routine releases them too (and the result):
    // the body runs protected, in a block of its own. Whether it needs to
    // is only known once it is emitted (can anything it calls raise?), so
    // the block before it is finished afterwards.
    llvm::Value *ExcFrame = nullptr;
    llvm::BasicBlock *Raised = nullptr, *BeforeBody = nullptr, *Body = nullptr;
    if (!Managed.empty() || ManagedResult || R->usesManaged() || FrameFini) {
      BeforeBody = B.GetInsertBlock();
      Body = newBlock("managed.body");
      B.SetInsertPoint(Body);
    }
    RoutineTemps.clear();
    ExitBlock = newBlock("exit");
    // The owned variables' objects are freed however the body ends, the
    // last declared first: the body is the protected part of a try-finally.
    llvm::SmallVector<VarDecl *, 4> Owned;
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D);
          V && V->isOwned() && !V->isInline()) // (those: with their sequence)
        Owned.push_back(V);
    if (Owned.empty())
      visitStmt(R->getBody()->getBody());
    else
      emitTryFinally([&] { visitStmt(R->getBody()->getBody()); },
                     [&](llvm::Value *State) { emitFreeOwned(Owned, State); });
    B.CreateBr(ExitBlock);
    if (BeforeBody) {
      llvm::DebugLoc Loc = B.getCurrentDebugLocation();
      B.SetInsertPoint(BeforeBody);
      setDebugLoc(R->getBodyLocation());
      if (mayRaise()) {
        if (!PinTemps) { // what the landing releases
          if (CurFrame)
            pinInMemory(CurFrame);
          for (llvm::Value *Slot : RoutineTemps)
            pinInMemory(Slot);
        }
        Raised = newBlock("managed.raise");
        ExcFrame = emitProtect(Raised, "", Body);
      } else {
        B.CreateBr(Body);
      }
      B.SetCurrentDebugLocation(Loc);
    }
    B.SetInsertPoint(ExitBlock);
    // (A frame on the heap: its variables when its last reference goes.)
    auto Finalize = [&] {
      if (!FrameFini)
        for (auto &[Addr, T] : Managed)
          finalizeValue(Addr, T);
      for (llvm::Value *Slot : RoutineTemps)
        clearTemp(Slot);
    };
    auto ReleaseFrame = [&] {
      if (FrameFini)
        B.CreateCall(frameRuntime("__pas_frame_release"), {CurFrame});
    };
    if (ExcFrame)
      B.CreateCall(excRuntime("__pas_exc_pop"), {ExcFrame});
    Finalize();
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D); V && !V->getAbsolute())
        emitFileLifetime(varAddress(V), V->getType(), /*Init=*/false);

    if (Raised) {
      llvm::BasicBlock *Ret = B.GetInsertBlock();
      B.SetInsertPoint(Raised);
      Finalize();
      if (ManagedResult)
        finalizeValue(varAddress(ResultVar), ResultVar->getType());
      ReleaseFrame();
      B.CreateCall(excRuntime("__pas_reraise"), {ExcFrame});
      B.CreateUnreachable();
      B.SetInsertPoint(Ret);
    }
    RoutineTemps.clear();

    if (R->isFunction() && !ResultByPointer) {
      llvm::Value *Result =
          loadScalar(varSlot(ResultVar), ResultVar->getType(), "result");
      ReleaseFrame();
      B.CreateRet(Result);
    } else {
      ReleaseFrame();
      B.CreateRetVoid();
    }
    if (!mayRaise())
      NoRaiseFns.insert(F);
  }

  //===--------------------------------------------------------------------===//
  // Calls
  //===--------------------------------------------------------------------===//

  llvm::Value *staticLinkFor(RoutineDecl *R) {
    if (R->isClosure() && !R->getCapture().isValid()) // uses no frame
      return llvm::ConstantPointerNull::get(B.getPtrTy());
    return R->hasStaticLink() ? framePtrFor(R->getParent())
                              : llvm::ConstantPointerNull::get(B.getPtrTy());
  }

  /// The link argument of a direct call: null when the routine has no link
  /// parameter (emitCallTo must not pass an extra null).
  llvm::Value *callLink(RoutineDecl *R) {
    return needsLink(R) ? staticLinkFor(R) : nullptr;
  }

  /// {function, static link} for a routine passed as a procedural argument.
  llvm::Value *emitRoutineValue(Expr *Arg) {
    if (auto *A = llvm::dyn_cast<AddrOfExpr>(Arg); A && A->getCode()) {
      // @TFoo.M of a static method
      llvm::Value *Pair = llvm::UndefValue::get(memType(A->getType()));
      Pair = B.CreateInsertValue(Pair, routineFunction(A->getCode()), 0);
      return B.CreateInsertValue(Pair, staticLinkFor(A->getCode()), 1);
    }
    if (auto *A = llvm::dyn_cast<AddrOfExpr>(Arg)) // @f (Turbo/FPC)
      Arg = A->getOperand();
    Decl *D = llvm::cast<NameRefExpr>(Arg)->getDecl();
    if (auto *V = llvm::dyn_cast<VarDecl>(D)) // forwarding a parameter
      return B.CreateLoad(memType(V->getType()), varAddress(V));
    auto *R = llvm::cast<RoutineDecl>(D);
    llvm::Value *Pair = llvm::UndefValue::get(memType(Arg->getType()));
    Pair = B.CreateInsertValue(Pair, routineFunction(R), 0);
    return B.CreateInsertValue(Pair, staticLinkFor(R), 1);
  }

  /// The single place that turns a Pascal call into a call or, under the
  /// table-exception flag (8b2+), an invoke. Stage 8b1 keeps the old call.
  llvm::CallBase *emitCallOrInvoke(llvm::FunctionCallee Callee,
                                   llvm::ArrayRef<llvm::Value *> Args,
                                   const llvm::Twine &Name = "") {
    if (!TableUnwind)
      return B.CreateCall(Callee, Args, Name);
    llvm::BasicBlock *Normal = newBlock("call.cont");
    llvm::CallBase *Call =
        B.CreateInvoke(Callee, Normal, TableUnwind, Args, Name);
    B.SetInsertPoint(Normal);
    return Call;
  }

  llvm::Value *emitCallTo(llvm::FunctionCallee Callee, llvm::Value *Link,
                          llvm::ArrayRef<VarDecl *> Params,
                          llvm::ArrayRef<Expr *> Args,
                          llvm::Value *Self = nullptr,
                          Type *ResultTy = nullptr, bool ToC = false) {
    llvm::SmallVector<llvm::Value *, 8> Values;
    if (Link) // external (C) routines have none
      Values.push_back(Link);
    llvm::Value *ResultTmp = nullptr;
    unsigned ResultArgNo = Values.size();
    bool ByPointer = returnsByPointer(ResultTy) && !ToC;
    if (ByPointer) {
      ResultTmp = createTemp(memType(ResultTy), "result");
      Values.push_back(ResultTmp);
    }
    if (Self) // methods: after the result pointer
      Values.push_back(Self);
    // The arguments' results are held as usual; only a direct call's own
    // ansistring result may move into its assignment target.
    bool NoHold = NoHoldAnsiResult;
    NoHoldAnsiResult = false;
    emitArgValues(Params, Args, Values, ToC);
    NoHoldAnsiResult = NoHold;
    auto *Call = emitCallOrInvoke(Callee, Values);
    if (ByPointer)
      addResultCallAttrs(Call, ResultArgNo, ResultTy);
    llvm::Value *Result = Call;
    if (ResultTmp) // expressions use a string's address
      return ResultTmp;
    // A string or record result comes back by value; expressions use its
    // address. (An interface, {ptr, ptr}, is a value.)
    if (Result->getType()->isArrayTy() ||
        (Result->getType()->isStructTy() &&
         Result->getType() != llvm::StructType::get(B.getPtrTy(), B.getPtrTy()))) {
      if (ResultTy && ResultTy->isRecord() && isManaged(ResultTy))
        return holdRecordResult(Result, ResultTy);
      llvm::Value *Tmp = createTemp(Result->getType(), "result");
      B.CreateStore(Result, Tmp);
      return Tmp;
    }
    if (ResultTy && isManaged(ResultTy)) {
      if (NoHold && ResultTy->isAnsiString()) {
        TookAnsiResult = true;
        return Result;
      }
      return holdResult(Result, ResultTy);
    }
    return Result;
  }

  static Type *resultOf(RoutineDecl *R) {
    return R->isFunction() ? R->getResultType() : nullptr;
  }

  /// Appends the values passed for Args (parameters Params) to Values.
  /// ToC: to an external (C) routine, which takes callbacks as function
  /// pointers, and maybe variadic arguments after Params.
  void emitArgValues(llvm::ArrayRef<VarDecl *> Params,
                     llvm::ArrayRef<Expr *> Args,
                     llvm::SmallVectorImpl<llvm::Value *> &Values,
                     bool ToC = false) {
    // `out` arguments holding managed values are released first, before
    // any other argument is evaluated (another may be the same variable).
    llvm::SmallDenseMap<size_t, llvm::Value *, 2> Released;
    for (size_t I = 0; I < std::min(Args.size(), Params.size()); ++I) {
      VarDecl *P = Params[I];
      if (!P->isOutParam() || !isManaged(P->getType()))
        continue;
      if (auto *OA = llvm::dyn_cast<OpenArrayType>(P->getType())) {
        llvm::Value *Desc = emitOpenArrayArg(Args[I], OA);
        llvm::Value *Slot = createTemp(B.getPtrTy(), "open.slot");
        B.CreateStore(Desc, Slot); // what finalizeValue takes
        finalizeValue(Slot, OA);
        Released[I] = Desc;
        continue;
      }
      llvm::Value *Addr = emitAddress(Args[I]);
      finalizeValue(Addr, P->getType());
      Released[I] = Addr;
    }
    for (size_t I = 0; I < std::max(Args.size(), Params.size()); ++I) {
      // Arguments left out: the parameters' default values.
      Expr *Arg = I < Args.size() ? Args[I] : Params[I]->getDefault();
      if (!Arg)
        break;
      if (I >= Params.size()) {
        Values.push_back(emitVarArg(Arg));
        continue;
      }
      if (auto It = Released.find(I); It != Released.end()) {
        Values.push_back(It->second);
        continue;
      }
      VarDecl *P = Params[I];
      if (isConstRefPtr(P) && Arg->isConstRefAddress()) { // the variable
        Values.push_back(emitAddress(Arg));
        continue;
      }
      if (P->getType()->isUntyped()) // Turbo/FPC: a variable, by reference
        Values.push_back(emitAddress(Arg));
      else if (auto *OA = llvm::dyn_cast<OpenArrayType>(P->getType()))
        Values.push_back(emitOpenArrayArg(Arg, OA));
      else if (P->getType()->isArrayOfConst())
        Values.push_back(emitConstArray(Arg));
      else if (llvm::Constant *CStr = cStringFor(P->getType(), Arg))
        Values.push_back(CStr);
      else if (llvm::isa<RoutineType>(P->getType()) && llvm::isa<NilExpr>(Arg))
        Values.push_back(ToC ? llvm::ConstantPointerNull::get(B.getPtrTy()) // a default
                             : emitRoutinePair(Arg, P->getType()));
      else if (llvm::isa<RoutineType>(P->getType()) && ToC) {
        llvm::Value *Pair = emitRoutineValue(Arg);
        Values.push_back(B.CreateExtractValue(Pair, 0));
      } else if (auto *RT = llvm::dyn_cast<RoutineType>(P->getType());
                 RT && (RT->isOfObject() || RT->isReference()))
        Values.push_back(P->isVarParam() ? emitAddress(Arg) : emitRoutinePair(Arg, RT));
      else if (llvm::isa<RoutineType>(P->getType()))
        Values.push_back(emitRoutineValue(Arg));
      else if (P->getType()->isShortString() && !P->isVarParam())
        // Any string: a const parameter reads it there, a value parameter
        // copies what fits into its own variable.
        Values.push_back(emitPStr(Arg));
      else if (P->isVarParam() || P->getType()->isStructured())
        Values.push_back(emitAddress(Arg));
      else if (P->getType()->isAnsiString())
        Values.push_back(emitAnsi(Arg));
      else
        Values.push_back(convertForAssign(visitExpr(Arg), Arg->getType(),
                                          P->getType(), Arg->getBeginLoc()));
      if (isConstRefPtr(P)) { // a value: passed in a temporary
        llvm::Value *V = Values.pop_back_val();
        llvm::Value *Tmp = createTemp(memType(P->getType()));
        storeScalar(Tmp, V, P->getType());
        Values.push_back(Tmp);
      }
    }
  }

  /// A variadic argument of an external routine, with C's promotions:
  /// ordinals to at least int, reals to double, string constants as C
  /// strings.
  llvm::Value *emitVarArg(Expr *Arg) {
    Type *T = Arg->getType();
    if (T->isStringLiteral()) {
      if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(Arg))
        return B.CreateGlobalString(SL->getValue(), ".cstr");
      auto *C = llvm::cast<ConstDecl>(
          llvm::cast<NameRefExpr>(unwrapConst(Arg))->getDecl());
      return B.CreateGlobalString(C->getValue().Str, ".cstr");
    }
    llvm::Value *V = visitExpr(Arg);
    if (T->isOrdinal() && V->getType()->getIntegerBitWidth() < 32)
      return isSignedOrdinal(T) ? B.CreateSExt(V, B.getInt32Ty())
                                : B.CreateZExt(V, B.getInt32Ty());
    return V; // (reals are doubles)
  }

  /// A string constant passed for a pchar parameter: a NUL-terminated copy.
  llvm::Constant *cStringFor(Type *ParamTy, Expr *Arg) {
    auto *P = llvm::dyn_cast<PointerType>(ParamTy);
    if (!P || P->isUntyped() || !P->getPointee() || !P->getPointee()->isChar())
      return nullptr;
    if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(Arg))
      return B.CreateGlobalString(SL->getValue(), ".cstr");
    auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(Arg));
    auto *C = N ? llvm::dyn_cast_or_null<ConstDecl>(N->getDecl()) : nullptr;
    if (!C)
      return nullptr;
    if (C->getValue().K == ConstValue::String)
      return B.CreateGlobalString(C->getValue().Str, ".cstr");
    if (C->getValue().K == ConstValue::Ordinal && C->getType()->getBase()->isChar())
      return B.CreateGlobalString(
          std::string(1, static_cast<char>(C->getValue().Int)), ".cstr");
    return nullptr;
  }

  llvm::Value *emitCall(RoutineDecl *R, llvm::ArrayRef<Expr *> Args) {
    if (R->isAsync())
      return emitAsyncCall(R, staticLinkFor(R), nullptr, Args);
    if (R->isIterator())
      return emitIteratorCall(R, staticLinkFor(R), nullptr, Args);
    if (R->isExternal())
      return emitCallTo(ExternalFns[R], nullptr, R->getParams(), Args, nullptr,
                        nullptr, /*ToC=*/true);
    return emitCallTo(Functions[R], callLink(R), R->getParams(), Args, nullptr,
                      resultOf(R));
  }

  /// Call through a procedural / functional parameter or variable.
  llvm::Value *emitIndirectCall(VarDecl *V, llvm::ArrayRef<Expr *> Args) {
    auto *RT = llvm::cast<RoutineType>(V->getType());
    llvm::Value *Pair = B.CreateLoad(memType(RT), varAddress(V));
    // Parameters always hold a routine; variables may be nil.
    return emitCallThrough(Pair, RT, Args, V->isParam() ? SourceLocation()
                                                          : V->getLocation());
  }

  /// Calls the {function, static link} pair Pair of type RT. NilCheckLoc:
  /// where to report a nil procedural value (invalid: no check).
  llvm::Value *emitCallThrough(llvm::Value *Pair, RoutineType *RT,
                               llvm::ArrayRef<Expr *> Args,
                               SourceLocation NilCheckLoc) {
    llvm::Value *Fn = B.CreateExtractValue(Pair, 0);
    if (NilCheckLoc.isValid() && rangeChecks(NilCheckLoc))
      emitRuntimeCheck(B.CreateIsNull(Fn), "call through a nil procedural value",
                       NilCheckLoc);
    if (RT->isOfObject()) { // {code, data}: Self comes from the pair
      llvm::Value *Self = B.CreateExtractValue(Pair, 1);
      return emitCallTo({routineFnType(RT->getParams(), RT->getResult(), true), Fn},
                        llvm::ConstantPointerNull::get(B.getPtrTy()), RT->getParams(),
                        Args, Self, RT->getResult());
    }
    if (RT->isCDecl()) { // a C entry (cthunk, external): no static link
      llvm::FunctionType *FTy =
          routineFnType(RT->getParams(), RT->getResult(), false, false);
      return emitCallTo({FTy, Fn}, nullptr, RT->getParams(), Args, nullptr,
                        RT->getResult());
    }
    llvm::FunctionType *FTy = routineFnType(RT->getParams(), RT->getResult());
    return emitCallTo({FTy, Fn}, B.CreateExtractValue(Pair, 1), RT->getParams(),
                      Args, nullptr, RT->getResult());
  }

  /// @obj.M, @TFoo.M, @M: {method's code, object or class}.
  llvm::Value *emitMethodPointer(AddrOfExpr *A) {
    RoutineDecl *R = A->getMethod();
    SourceLocation L = A->getBeginLoc();
    llvm::Value *Obj;
    bool SelfIsClass = false;
    if (auto *F = llvm::dyn_cast<FieldExpr>(A->getOperand())) {
      Expr *Base = F->getBase();
      auto *N = llvm::dyn_cast<NameRefExpr>(Base);
      if (Base->getType()->isRecord()) {
        Obj = emitAddress(Base);
      } else if (namesType(Base)) {
        Obj = vmtOf(llvm::cast<ClassType>(Base->getType()));
        SelfIsClass = true;
      } else {
        Obj = visitExpr(Base);
        SelfIsClass = Base->getType()->isClassRef();
      }
    } else {
      auto *N = llvm::cast<NameRefExpr>(A->getOperand());
      Obj = withBase(N->getWith());
      SelfIsClass = withIsClass(N->getWith());
    }
    llvm::Value *Code = Functions[R];
    if (R->isClassMethod()) {
      Obj = receiverClass(Obj, SelfIsClass, L);
      if (R->isDispatched())
        Code = loadVMTSlot(Obj, 6 + R->getVMTIndex(), "method");
    } else if (R->isDispatched()) {
      emitNilObjectCheck(Obj, L);
      Code = loadVirtual(Obj, static_cast<unsigned>(R->getVMTIndex()));
    }
    llvm::Value *Pair =
        llvm::UndefValue::get(llvm::StructType::get(B.getPtrTy(), B.getPtrTy()));
    Pair = B.CreateInsertValue(Pair, Code, 0);
    return B.CreateInsertValue(Pair, Obj, 1);
  }

  /// A procedural value of type T (Turbo/FPC): @routine, nil, or a
  /// procedural variable / field / element.
  llvm::Value *emitRoutinePair(Expr *E, Type *T) {
    if (llvm::isa<NilExpr>(E))
      return llvm::Constant::getNullValue(memType(T));
    if (llvm::cast<RoutineType>(T)->isOfObject())
      return visitExpr(E);
    if (auto *A = llvm::dyn_cast<AddrOfExpr>(E)) {
      RoutineDecl *R = A->getCode(); // @TFoo.M of a static method
      if (!R)
        R = llvm::cast<RoutineDecl>(
            llvm::cast<NameRefExpr>(A->getOperand())->getDecl());
      llvm::Value *Pair = llvm::UndefValue::get(memType(T));
      Pair = B.CreateInsertValue(Pair, routineFunction(R), 0);
      return B.CreateInsertValue(Pair, staticLinkFor(R), 1);
    }
    if (llvm::cast<RoutineType>(T)->isReference()) // (also a call's result)
      return visitExpr(E);
    return B.CreateLoad(memType(T), emitAddress(E));
  }

  //===--------------------------------------------------------------------===//
  // Objects and methods (objfpc)
  //===--------------------------------------------------------------------===//

  /// The object a with context stands for (Self for a method body), or the
  /// address of a with statement's record.
  //===--------------------------------------------------------------------===//
  // Reference counting (COM interfaces, objfpc)
  //===--------------------------------------------------------------------===//

  /// The runtime's reference counting entry points (runtime/io.c).
  llvm::FunctionCallee intfRuntime(llvm::StringRef Name) {
    llvm::Type *Ptr = B.getPtrTy();
    if (Name == "__pas_intf_assign")
      return classRuntime(Name, B.getVoidTy(), {Ptr, Ptr, Ptr});
    // __pas_intf_addref(obj), __pas_intf_clear(addr), __pas_free_instance,
    // __pas_interfaced_construct(ed)(obj)
    return classRuntime(Name, B.getVoidTy(), {Ptr});
  }

  /// Releases the COM interfaces in the object of managed type T at Addr
  /// and sets them to nil.
  void finalizeValue(llvm::Value *Addr, Type *T) {
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(T)) {
      forEachOpenElement(
          Addr, OA,
          [&](llvm::Value *Elem) { finalizeValue(Elem, OA->getElementType()); },
          "fini");
      return;
    }
    if (T->isAnsiString()) {
      astrCall("__pas_astr_clear", B.getVoidTy(), {Addr});
      return;
    }
    if (T->isDynArray()) {
      astrCall("__pas_dyn_clear", B.getVoidTy(), {Addr});
      return;
    }
    if (T->isInterface()) {
      B.CreateCall(intfRuntime("__pas_intf_clear"), {Addr});
      return;
    }
    if (T->isClosureType()) {
      B.CreateCall(classRuntime("__pas_closure_clear", B.getVoidTy(),
                                {B.getPtrTy()}),
                   {Addr});
      return;
    }
    if (T->isTask() || T->isToken() || T->isIterator()) {
      B.CreateCall(classRuntime(T->isTask()       ? "__pas_task_clear"
                                : T->isIterator() ? "__pas_gen_clear"
                                                  : "__pas_token_clear",
                                B.getVoidTy(), {B.getPtrTy()}),
                   {Addr});
      return;
    }
    if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
      forEachElement(
          Addr, A,
          [&](llvm::Value *Elem) { finalizeValue(Elem, A->getElementType()); },
          "fini");
      return;
    }
    auto *R = llvm::cast<RecordType>(T);
    for (FieldDecl *F : R->getAllFields())
      if (isManaged(F->getType()))
        finalizeValue(fieldAddress(Addr, R, F), F->getType());
  }

  /// One more reference to each COM interface in the object of managed type
  /// T at Addr (a copy was made).
  void addRefValue(llvm::Value *Addr, Type *T) {
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(T)) {
      forEachOpenElement(
          Addr, OA,
          [&](llvm::Value *Elem) { addRefValue(Elem, OA->getElementType()); },
          "addref");
      return;
    }
    if (T->isAnsiString()) {
      astrCall("__pas_astr_addref", B.getVoidTy(),
               {B.CreateLoad(B.getPtrTy(), Addr, "s")});
      return;
    }
    if (T->isDynArray()) {
      astrCall("__pas_dyn_addref", B.getVoidTy(),
               {B.CreateLoad(B.getPtrTy(), Addr, "a")});
      return;
    }
    if (T->isInterface()) {
      llvm::Value *Obj = B.CreateLoad(B.getPtrTy(), Addr, "obj");
      B.CreateCall(intfRuntime("__pas_intf_addref"), {Obj});
      return;
    }
    if (T->isClosureType()) { // its frame
      llvm::Value *Frame = B.CreateLoad(
          B.getPtrTy(), B.CreateStructGEP(memType(T), Addr, 1), "frame");
      B.CreateCall(frameRuntime("__pas_frame_addref"), {Frame});
      return;
    }
    if (T->isTask() || T->isToken() || T->isIterator()) {
      llvm::Value *H = B.CreateLoad(B.getPtrTy(), Addr,
                                    T->isTask()       ? "task"
                                    : T->isIterator() ? "iter"
                                                      : "token");
      B.CreateCall(classRuntime(T->isTask()       ? "__pas_task_addref"
                                : T->isIterator() ? "__pas_gen_addref"
                                                  : "__pas_token_addref",
                                B.getVoidTy(), {B.getPtrTy()}),
                   {H});
      return;
    }
    if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
      forEachElement(
          Addr, A,
          [&](llvm::Value *Elem) { addRefValue(Elem, A->getElementType()); },
          "addref");
      return;
    }
    auto *R = llvm::cast<RecordType>(T);
    for (FieldDecl *F : R->getAllFields())
      if (isManaged(F->getType()))
        addRefValue(fieldAddress(Addr, R, F), F->getType());
  }

  /// *Dst := *Src for a record or array T with COM interfaces.
  void copyManaged(llvm::Value *Dst, llvm::Value *Src, Type *T) {
    addRefValue(Src, T);
    // The old value is released last: Src and Dst may be the same object.
    llvm::Value *Old = createTemp(memType(T), "old");
    copyAggregate(Old, Dst, T);
    copyAggregate(Dst, Src, T);
    finalizeValue(Old, T);
  }

  /// Addr^ := V for a COM interface.
  void storeInterface(llvm::Value *Addr, llvm::Value *V) {
    llvm::Value *Obj = B.CreateExtractValue(V, 0, "obj");
    llvm::Value *Table = B.CreateExtractValue(V, 1, "itable");
    B.CreateCall(intfRuntime("__pas_intf_assign"), {Addr, Obj, Table});
  }

  /// Stores V (scalar, of type T) at Addr: counted for COM interfaces.
  void storeValue(llvm::Value *Addr, llvm::Value *V, Type *T) {
    if (T->isAnsiString())
      // A static string constant has refs < 0: storing it needs no count.
      astrCall(llvm::isa<llvm::Constant>(V) ? "__pas_astr_take"
                                            : "__pas_astr_assign",
               B.getVoidTy(), {Addr, V});
    else if (T->isDynArray())
      astrCall("__pas_dyn_assign", B.getVoidTy(), {Addr, V});
    else if (T->isClosureType())
      B.CreateCall(classRuntime("__pas_closure_assign", B.getVoidTy(),
                                {B.getPtrTy(), B.getPtrTy(), B.getPtrTy()}),
                   {Addr, B.CreateExtractValue(V, 0, "fn"),
                    B.CreateExtractValue(V, 1, "frame")});
    else if (T->isTask() || T->isToken() || T->isIterator())
      B.CreateCall(classRuntime(T->isTask()       ? "__pas_task_assign"
                                : T->isIterator() ? "__pas_gen_assign"
                                                  : "__pas_token_assign",
                                B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()}),
                   {Addr, V});
    else if (isManaged(T))
      storeInterface(Addr, V);
    else
      storeScalar(Addr, V, T);
  }

  /// Copies the record or array T at Src to Dst: counted if managed.
  void copyValue(llvm::Value *Dst, llvm::Value *Src, Type *T) {
    if (isManaged(T))
      copyManaged(Dst, Src, T);
    else
      copyAggregate(Dst, Src, T);
  }

  /// A function's result V, a COM interface (a reference the caller now
  /// holds): kept in a temporary until the statement ends. T: its type, if
  /// that may be a dynamic array.
  llvm::Value *holdResult(llvm::Value *V, Type *T = nullptr) {
    heldTemp(V, T);
    return V;
  }

  /// holdResult's temporary: its address.
  llvm::Value *heldTemp(llvm::Value *V, Type *T = nullptr) {
    // (released by finalizeValue: also a closure's)
    bool Dyn = T && (T->isDynArray() || T->isClosureType() || T->isTask() ||
                     T->isToken() || T->isIterator());
    llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                            CurFn->getEntryBlock().begin());
    llvm::Value *Slot = Entry.CreateAlloca(
        V->getType(), nullptr,
        Dyn ? "dyn.tmp" : V->getType()->isPointerTy() ? "astr.tmp" : "intf.tmp");
    Entry.CreateStore(llvm::Constant::getNullValue(V->getType()), Slot);
    if (Dyn)
      RecordTemps[Slot] = T;
    clearTemp(Slot); // a loop's previous
    if (PinTemps)
      pinInMemory(Slot);
    B.CreateStore(V, Slot);
    LiveTemps.push_back(Slot);
    if (CurRoutine)
      RoutineTemps.push_back(Slot);
    return Slot;
  }

  /// A function's record result V, holding ansistrings the caller now owns:
  /// kept in a temporary until the statement ends. Returns its address.
  llvm::Value *holdRecordResult(llvm::Value *V, Type *T) {
    llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                            CurFn->getEntryBlock().begin());
    llvm::Value *Slot = Entry.CreateAlloca(V->getType(), nullptr, "rec.tmp");
    Entry.CreateStore(llvm::Constant::getNullValue(V->getType()), Slot);
    finalizeValue(Slot, T); // a loop's previous
    if (PinTemps)
      pinInMemory(Slot);
    B.CreateStore(V, Slot);
    RecordTemps[Slot] = T;
    LiveTemps.push_back(Slot);
    if (CurRoutine)
      RoutineTemps.push_back(Slot);
    return Slot;
  }

  /// Releases the temporaries held since LiveTemps had Mark entries.
  void releaseTemps(size_t Mark) {
    for (size_t I = Mark; I < LiveTemps.size(); ++I)
      clearTemp(LiveTemps[I]);
    LiveTemps.resize(Mark);
  }

  /// Releases the ansistring or COM interface held in the temporary Slot.
  void clearTemp(llvm::Value *Slot) {
    if (Type *T = RecordTemps.lookup(Slot))
      finalizeValue(Slot, T);
    else if (llvm::cast<llvm::AllocaInst>(Slot)->getAllocatedType()->isPointerTy())
      astrCall("__pas_astr_clear", B.getVoidTy(), {Slot});
    else
      B.CreateCall(intfRuntime("__pas_intf_clear"), {Slot});
  }

  /// Is E a const parameter (whose value its caller owns)?
  static bool isConstParamRef(Expr *E) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
      if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl()))
        return V->isConstParam();
    return false;
  }

  void emitAnsiIndexCheck(llvm::Value *S, llvm::Value *X, SourceLocation L) {
    if (rangeChecks(L))
      emitRuntimeCheck(
          B.CreateICmpNE(astrCall("__pas_astr_bad_index", B.getInt32Ty(), {S, X}),
                         B.getInt32(0)),
          "string index out of bounds", L);
  }

  /// Is C TInterfacedObject (the predeclared one) or a descendant?
  static bool isInterfacedClass(const ClassType *C) {
    for (; C; C = C->getParent())
      if (!C->getModule() && C->getDeclName() == "TInterfacedObject")
        return true;
    return false;
  }

  /// Does class C have fields of its own holding COM interfaces?
  static bool hasManagedFields(const ClassType *C) {
    for (Decl *D : C->getMembers())
      if (auto *F = llvm::dyn_cast<FieldDecl>(D); F && isManaged(F->getType()))
        return true;
    return false;
  }

  /// The iid of interface I (the predeclared IUnknown's is in the runtime).
  llvm::Constant *iidOf(InterfaceType *I) {
    if (llvm::GlobalVariable *G = IIDs.lookup(I))
      return G;
    std::string Name = "__pas_" + I->getDeclName().lower() + "_iid";
    llvm::GlobalVariable *G = M.getNamedGlobal(Name);
    if (!G)
      G = new llvm::GlobalVariable(M, B.getInt8Ty(), /*isConstant=*/true,
                                   llvm::GlobalValue::ExternalLinkage, nullptr,
                                   Name);
    IIDs[I] = G;
    return G;
  }

  /// The cleanup function of class C: releases its own fields' COM
  /// interfaces (the runtime calls it for each class of a freed object).
  void emitCleanup(ClassType *C, llvm::Function *F) {
    CurFn = F;
    AddrAligns.clear();
    CurRoutine = nullptr;
    CurFrame = nullptr;
    CurPin = nullptr;
    PinTemps = true;
    CurSP = nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
    for (Decl *D : C->getMembers())
      if (auto *Fd = llvm::dyn_cast<FieldDecl>(D); Fd && isManaged(Fd->getType()))
        finalizeValue(fieldAddress(F->getArg(0), C, Fd), Fd->getType());
    B.CreateRetVoid();
  }

  void emitCleanups() {
    for (auto &[C, F] : CleanupsToEmit)
      emitCleanup(C, F);
    CleanupsToEmit.clear();
  }

  llvm::Value *withBase(WithContext *W) {
    if (W->IsSelf) // load Self; a record's (a type helper's) Self is its address
      return W->Record->getType()->isRecord() || isVarParamRef(W->Record)
                 ? emitAddress(W->Record)
                 : visitExpr(W->Record);
    return WithAddrs.lookup(W);
  }

  //===--------------------------------------------------------------------===//
  // Advanced records (objfpc)
  //===--------------------------------------------------------------------===//

  /// The value of E, a user-defined conversion applied (its operator called
  /// with E's unconverted value); an address for a record.
  /// A constant used as a member (TFoo.C, obj.C): its reference, which is
  /// emitted instead; otherwise E.
  static Expr *unwrapConst(Expr *E) {
    if (auto *F = llvm::dyn_cast<FieldExpr>(E))
      if (NameRefExpr *Ref = F->getConstRef())
        return Ref;
    return E;
  }

  llvm::Value *visitExpr(Expr *E) {
    if (E->getConversion())
      return emitConversion(E);
    return ExprVisitor<CodeGenModule, llvm::Value *>::visitExpr(E);
  }

  llvm::Value *emitConversion(Expr *E) {
    RoutineDecl *Op = E->getConversion();
    E->suspendConversion();
    llvm::Value *V = emitCall(Op, {E});
    E->resumeConversion();
    return V;
  }

  static bool isVarParamRef(Expr *E) {
    auto *N = llvm::dyn_cast<NameRefExpr>(E);
    auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
    return V && V->isVarParam();
  }

  /// The method of type helper member D (a property: its getter), or null.
  static RoutineDecl *helperRoutine(Decl *D) {
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
      D = P->getRead();
    auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
    return R && R->getClass() && R->getClass()->isHelper() ? R : nullptr;
  }

  /// The Self of a type helper's member used through F: the address of
  /// its base, or of a temporary holding the base's value.
  llvm::Value *helperSelf(FieldExpr *F) {
    Expr *Base = F->getBase();
    if (!F->isHelperValue())
      return emitAddress(Base);
    Type *T = helperRoutine(F->getMember())->getClass()->getHelperFor();
    if (T->isAnsiString()) {
      llvm::Value *V = emitAnsi(Base);
      astrCall("__pas_astr_addref", B.getVoidTy(), {V});
      return heldTemp(V);
    }
    if (T->isDynArray()) {
      llvm::Value *V = visitExpr(Base);
      astrCall("__pas_dyn_addref", B.getVoidTy(), {V});
      return heldTemp(V, T);
    }
    if (auto *S = llvm::dyn_cast<ShortStringType>(T)) {
      llvm::Value *Tmp = newStringTemp(S->getMaxLength());
      emitStringAssign(Tmp, S, Base);
      return Tmp;
    }
    llvm::Value *Tmp = createTemp(memType(T), "self.tmp");
    if (T->isStructured())
      copyAggregate(Tmp, visitExpr(Base), T);
    else
      storeScalar(Tmp, visitExpr(Base), T);
    return Tmp;
  }

  /// R(Args) on record Obj (its address): a method, or a class method or
  /// operator (static: no Self).
  llvm::Value *emitRecordMethodCall(RoutineDecl *R, llvm::Value *Obj,
                                    llvm::ArrayRef<Expr *> Args) {
    if (!R->hasSelf())
      return emitCall(R, Args);
    llvm::Value *Result =
        emitCallTo(Functions[R], llvm::ConstantPointerNull::get(B.getPtrTy()),
                   R->getParams(), Args, Obj, resultOf(R));
    return R->isFunction() ? Result : nullptr;
  }

  /// TR.Create(args): a new record (zeroed, in a temporary) initialized by
  /// the constructor Ctor.
  llvm::Value *emitRecordConstruction(RecordType *RT, RoutineDecl *Ctor,
                                      llvm::ArrayRef<Expr *> Args) {
    llvm::Type *Ty = memType(RT);
    llvm::Value *Tmp = createTemp(Ty, "rec");
    B.CreateMemSet(Tmp, B.getInt8(0), DL.getTypeAllocSize(Ty),
                   DL.getABITypeAlign(Ty));
    emitCallTo(Functions[Ctor], llvm::ConstantPointerNull::get(B.getPtrTy()),
               Ctor->getParams(), Args, Tmp);
    return Tmp;
  }

  /// The interface I value of object Obj: {Obj, its class's table for I}.
  llvm::Value *toInterface(llvm::Value *Obj, InterfaceType *I) {
    llvm::Value *Table = B.CreateCall(
        classRuntime("__pas_intf_lookup", B.getPtrTy(), {B.getPtrTy(), B.getPtrTy()}),
        {Obj, iidOf(I)}, "itable");
    return makeInterface(Obj, Table);
  }

  llvm::Value *makeInterface(llvm::Value *Obj, llvm::Value *Table) {
    llvm::Type *Ty = llvm::StructType::get(B.getPtrTy(), B.getPtrTy());
    llvm::Value *V = B.CreateInsertValue(llvm::UndefValue::get(Ty), Obj, 0);
    return B.CreateInsertValue(V, Table, 1);
  }

  /// The object of an interface value (nil: itself).
  llvm::Value *interfaceObject(llvm::Value *V) {
    return V->getType()->isStructTy() ? B.CreateExtractValue(V, 0, "obj") : V;
  }

  /// Is W's object a class (Self in a class method)?
  static bool withIsClass(WithContext *W) {
    return W->Record->getType()->isClassRef();
  }

  /// if (Obj == nil) runtime error, under range checks.
  void emitNilObjectCheck(llvm::Value *Obj, SourceLocation L) {
    if (rangeChecks(L))
      emitRuntimeCheck(B.CreateIsNull(Obj), "use of a nil object reference", L);
  }

  /// The object an expression of a class type refers to, checked for nil.
  llvm::Value *emitObject(Expr *E) {
    llvm::Value *Obj = visitExpr(E);
    emitNilObjectCheck(Obj, E->getBeginLoc());
    return Obj;
  }

  /// Address of virtual method Index in the VMT of object Obj.
  llvm::Value *loadVirtual(llvm::Value *Obj, unsigned Index) {
    return loadVMTSlot(B.CreateLoad(B.getPtrTy(), Obj, "vmt"), 6 + Index,
                       "method");
  }

  /// Can no separately compiled unit see C (and so subclass it)?
  bool classIsClosed(ClassType *C) const {
    return !CGOpts.SeparateUnits ||
           (C->getModule() != nullptr && !ExportedClasses.count(C));
  }

  /// Does C have descendants in what this compilation can see? Unknown
  /// (separately compiled) descendants count when C is not closed.
  bool classHasDescendants(ClassType *C) {
    if (C->getModule() == nullptr) // a predeclared class: the runtime's own
      return true;                 // descendants are not in AllClasses
    if (!classIsClosed(C))
      return true;
    for (ClassType *D : AllClasses)
      if (D != C && D->inheritsFrom(C))
        return true;
    return false;
  }

  /// Can a virtual call of R be a direct call? Only if no class of the
  /// whole-program hierarchy that descends from R's overrides it; with
  /// -fseparate-units only for a class that a separately compiled unit
  /// cannot see (item 11).
  bool canDevirtualize(RoutineDecl *R) {
    ClassType *C = R->getClass();
    if (!C || R->isAbstract() || !R->isDispatched() || !Functions.count(R))
      return false;
    if (!classIsClosed(C))
      return false;
    int Idx = R->getVMTIndex();
    llvm::ArrayRef<RoutineDecl *> VTable = C->getVTable();
    if (Idx < 0 || static_cast<size_t>(Idx) >= VTable.size() ||
        VTable[Idx] != R)
      return false;
    for (ClassType *D : AllClasses) {
      if (D == C || !D->inheritsFrom(C))
        continue;
      llvm::ArrayRef<RoutineDecl *> DVT = D->getVTable();
      if (static_cast<size_t>(Idx) < DVT.size() && DVT[Idx] != R)
        return false;
    }
    return true;
  }

  /// Entry Slot of a VMT: 0 the parent's VMT, 1 the class name, 2 the
  /// instance size, 3 the interface table, 4... the virtual methods.
  llvm::Value *loadVMTSlot(llvm::Value *VMT, unsigned Slot,
                           const llvm::Twine &Name) {
    llvm::Value *P = B.CreateConstInBoundsGEP1_64(B.getPtrTy(), VMT, Slot);
    auto *L = B.CreateLoad(B.getPtrTy(), P, Name);
    // A VMT is a constant global: its slots never change (item 9).
    L->setMetadata(llvm::LLVMContext::MD_invariant_load,
                   llvm::MDNode::get(M.getContext(), {}));
    return L;
  }

  /// The class (VMT) of a method call's receiver: Obj itself if SelfIsClass,
  /// else the object's (checked for nil).
  llvm::Value *receiverClass(llvm::Value *Obj, bool SelfIsClass,
                             SourceLocation L) {
    if (SelfIsClass)
      return Obj;
    emitNilObjectCheck(Obj, L);
    return B.CreateLoad(B.getPtrTy(), Obj, "vmt");
  }

  /// if (VMT == nil) runtime error, under range checks.
  void emitNilClassCheck(llvm::Value *VMT, SourceLocation L) {
    if (rangeChecks(L))
      emitRuntimeCheck(B.CreateIsNull(VMT), "use of a nil class reference", L);
  }

  /// obj.Free: if obj <> nil then obj.Destroy (virtual) and release it.
  void emitFree(llvm::Value *Obj) {
    auto *DoBB = newBlock("free.do");
    auto *EndBB = newBlock("free.end");
    B.CreateCondBr(B.CreateIsNull(Obj), EndBB, DoBB);
    B.SetInsertPoint(DoBB);
    llvm::Value *Fn = loadVirtual(Obj, 0);
    llvm::FunctionType *FTy =
        llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()}, false);
    B.CreateCall(FTy, Fn, {llvm::ConstantPointerNull::get(B.getPtrTy()), Obj});
    B.CreateCall(intfRuntime("__pas_free_instance"), {Obj});
    B.CreateBr(EndBB);
    B.SetInsertPoint(EndBB);
  }

  /// Calls method R on object Obj. Inherited: a static call of the
  /// inherited method (no dispatch, a destructor does not release the
  /// object); otherwise a virtual method is called through the VMT and a
  /// destructor releases the object afterwards. Returns the result (an
  /// address for strings and records), or null.
  /// SelfIsClass: Obj is a class (a VMT), as in TFoo.M, c.M (c: class of
  /// T) and in class methods; then R is a class method or a constructor,
  /// which creates an object of that class.
  llvm::Value *emitMethodCall(RoutineDecl *R, llvm::Value *Obj,
                              llvm::ArrayRef<Expr *> Args, bool Inherited,
                              SourceLocation L, bool SelfIsClass = false) {
    if (R->isAsync())
      return emitAsyncCall(R, nullptr,
                           R->isClassMethod() ? receiverClass(Obj, SelfIsClass, L)
                                              : Obj,
                           Args);
    if (R->isIterator())
      return emitIteratorCall(R, nullptr,
                              R->isClassMethod() ? receiverClass(Obj, SelfIsClass, L)
                                                 : Obj,
                              Args);
    if (R->getClass() && R->getClass()->hasSelfByReference())
      return emitRecordMethodCall(R, Obj, Args);
    if (R->isConstructor() && SelfIsClass && !Inherited)
      return emitConstructionThrough(Obj, R, Args, L);
    switch (R->getBuiltin()) {
    case TObjectMethod::Create: // TObject's constructor does nothing
      return nullptr;
    case TObjectMethod::ClassType:
      return B.CreateLoad(B.getPtrTy(), Obj, "vmt");
    case TObjectMethod::ClassParent:
      return loadVMTSlot(receiverClass(Obj, SelfIsClass, L), 0, "parent");
    case TObjectMethod::InstanceSize:
      return B.CreatePtrToInt(
          loadVMTSlot(receiverClass(Obj, SelfIsClass, L), 2, "size"),
          B.getInt64Ty(), "instancesize");
    case TObjectMethod::InheritsFrom: {
      llvm::Value *VMT = receiverClass(Obj, SelfIsClass, L);
      llvm::Value *Arg = visitExpr(Args[0]);
      llvm::FunctionCallee Fn = classRuntime(
          "__pas_class_inherits", B.getInt32Ty(), {B.getPtrTy(), B.getPtrTy()});
      llvm::Value *R = B.CreateCall(Fn, {VMT, Arg});
      return B.CreateICmpNE(R, B.getInt32(0), "inherits");
    }
    case TObjectMethod::ExceptionCreate: { // FMessage := msg
      ClassType *C = R->getClass();
      auto *Msg = llvm::cast<FieldDecl>(C->findOwnMember("fmessage"));
      emitStringAssign(fieldAddress(Obj, C, Msg),
                       llvm::cast<ShortStringType>(Msg->getType()), Args[0]);
      return nullptr;
    }
    case TObjectMethod::ExceptionCreateFmt: { // FMessage := Format(msg, args)
      ClassType *C = R->getClass();
      auto *Msg = llvm::cast<FieldDecl>(C->findOwnMember("fmessage"));
      llvm::Value *Fmt = emitAnsi(Args[0]);
      llvm::Value *A = emitConstArray(Args[1]);
      astrCall("__pas_format_pstr", B.getVoidTy(),
               {fieldAddress(Obj, C, Msg), Fmt, A});
      return nullptr;
    }
    case TObjectMethod::Free:
      emitFree(Obj);
      return nullptr;
    case TObjectMethod::ClassName:
      return loadVMTSlot(
          SelfIsClass ? Obj : B.CreateLoad(B.getPtrTy(), Obj, "vmt"), 1,
          "classname");
    case TObjectMethod::Destroy:
      if (Inherited) // TObject's destructor does nothing
        return nullptr;
      break;
    case TObjectMethod::None:
      break;
    }
    llvm::Value *Result;
    if (R->isClassMethod()) { // Self: the class
      llvm::Value *Self = receiverClass(Obj, SelfIsClass, L);
      llvm::FunctionCallee Fn = Functions[R];
      if (!Inherited && R->isDispatched() && !canDevirtualize(R)) {
        emitNilClassCheck(Self, L);
        Fn = {routineFnType(R), loadVMTSlot(Self, 6 + R->getVMTIndex(), "method")};
      }
      Result = emitCallTo(Fn, llvm::ConstantPointerNull::get(B.getPtrTy()),
                          R->getParams(), Args, Self, resultOf(R));
      return R->isFunction() ? Result : nullptr;
    }
    ClassType *Exact = KnownClass.lookup(Obj);
    RoutineDecl *Impl = nullptr;
    if (!Inherited && R->isDispatched() && Exact && R->getVMTIndex() >= 0) {
      auto VTable = Exact->getVTable();
      unsigned Idx = static_cast<unsigned>(R->getVMTIndex());
      if (Idx < VTable.size()) {
        Impl = VTable[Idx];
        if (!Impl || Impl->isAbstract() || !Functions.count(Impl))
          Impl = nullptr;
      }
    }
    if (!Inherited && R->isDispatched() && !Impl && !canDevirtualize(R)) {
      emitNilObjectCheck(Obj, L);
      llvm::Value *Fn = loadVirtual(Obj, static_cast<unsigned>(R->getVMTIndex()));
      Result = emitCallTo({routineFnType(R), Fn},
                          llvm::ConstantPointerNull::get(B.getPtrTy()),
                          R->getParams(), Args, Obj, resultOf(R));
    } else {
      RoutineDecl *Direct = Impl ? Impl : R;
      Result = emitCallTo(Functions[Direct],
                          llvm::ConstantPointerNull::get(B.getPtrTy()),
                          R->getParams(), Args, Obj, resultOf(Direct));
    }
    if (R->isDestructor() && !Inherited)
      B.CreateCall(intfRuntime("__pas_free_instance"), {Obj});
    return R->isFunction() ? Result : nullptr;
  }

  /// TFoo.Create(args): a new object of class C, initialized by Ctor.
  /// The arguments are evaluated first. An exception leaving a constructor
  /// written in Pascal destroys and releases the new object (as in Free
  /// Pascal), so the call runs protected.
  llvm::Value *emitConstruction(ClassType *C, RoutineDecl *Ctor,
                                llvm::ArrayRef<Expr *> Args, SourceLocation L) {
    if (Ctor->getBuiltin() != TObjectMethod::None) {
      llvm::Value *Obj =
          B.CreateCall(RT.New, {B.getInt64(instanceSize(C))}, "obj");
      B.CreateStore(vmtOf(C), Obj);
      KnownClass[Obj] = C;
      emitMethodCall(Ctor, Obj, Args, /*Inherited=*/true, L);
      return Obj;
    }
    llvm::SmallVector<llvm::Value *, 8> Values{
        llvm::ConstantPointerNull::get(B.getPtrTy())};
    Values.push_back(nullptr); // Self
    emitArgValues(Ctor->getParams(), Args, Values);
    llvm::Value *Obj =
        B.CreateCall(RT.New, {B.getInt64(instanceSize(C))}, "obj");
    B.CreateStore(vmtOf(C), Obj);
    Values[1] = Obj;
    bool Counted = isInterfacedClass(Ctor->getClass());
    if (Counted)
      B.CreateCall(intfRuntime("__pas_interfaced_construct"), {Obj});
    if (NoRaiseFns.count(Functions[Ctor])) { // nothing to protect against
      emitCallOrInvoke(Functions[Ctor], Values);
      if (Counted)
        B.CreateCall(intfRuntime("__pas_interfaced_constructed"), {Obj});
      KnownClass[Obj] = C;
      return Obj;
    }
    auto *Failed = newBlock("ctor.raise");
    llvm::Value *F = emitProtect(Failed, "ctor.call");
    emitCallOrInvoke(Functions[Ctor], Values);
    B.CreateCall(excRuntime("__pas_exc_pop"), {F});
    if (Counted)
      B.CreateCall(intfRuntime("__pas_interfaced_constructed"), {Obj});
    auto *Done = newBlock("ctor.done");
    B.CreateBr(Done);
    B.SetInsertPoint(Failed);
    B.CreateCall(excRuntime("__pas_reraise_free"), {F, Obj});
    B.CreateUnreachable();
    B.SetInsertPoint(Done);
    KnownClass[Obj] = C;
    return Obj;
  }

  /// c.Create(args) with c a class (a VMT; also Self in a class method): a
  /// new object of the class c refers to, of its size, initialized by Ctor
  /// (virtual: c's). Protected like emitConstruction.
  llvm::Value *emitConstructionThrough(llvm::Value *VMT, RoutineDecl *Ctor,
                                       llvm::ArrayRef<Expr *> Args,
                                       SourceLocation L) {
    emitNilClassCheck(VMT, L);
    auto Allocate = [&] {
      llvm::Value *Size =
          B.CreatePtrToInt(loadVMTSlot(VMT, 2, "size"), B.getInt64Ty());
      llvm::Value *Obj = B.CreateCall(RT.New, {Size}, "obj");
      B.CreateStore(VMT, Obj);
      return Obj;
    };
    if (Ctor->getBuiltin() != TObjectMethod::None) {
      llvm::Value *Obj = Allocate();
      emitMethodCall(Ctor, Obj, Args, /*Inherited=*/true, L);
      return Obj;
    }
    llvm::SmallVector<llvm::Value *, 8> Values{
        llvm::ConstantPointerNull::get(B.getPtrTy())};
    Values.push_back(nullptr); // Self
    emitArgValues(Ctor->getParams(), Args, Values);
    llvm::Value *Obj = Allocate();
    Values[1] = Obj;
    bool Counted = isInterfacedClass(Ctor->getClass());
    if (Counted)
      B.CreateCall(intfRuntime("__pas_interfaced_construct"), {Obj});
    auto *Failed = newBlock("ctor.raise");
    llvm::Value *F = emitProtect(Failed, "ctor.call");
    if (Ctor->isDispatched())
      B.CreateCall(routineFnType(Ctor),
                   loadVMTSlot(VMT, 6 + Ctor->getVMTIndex(), "ctor"), Values);
    else
      emitCallOrInvoke(Functions[Ctor], Values);
    B.CreateCall(excRuntime("__pas_exc_pop"), {F});
    if (Counted)
      B.CreateCall(intfRuntime("__pas_interfaced_constructed"), {Obj});
    auto *Done = newBlock("ctor.done");
    B.CreateBr(Done);
    B.SetInsertPoint(Failed);
    B.CreateCall(excRuntime("__pas_reraise_free"), {F, Obj});
    B.CreateUnreachable();
    B.SetInsertPoint(Done);
    return Obj;
  }

  /// obj.M(args) or TFoo.Create(args) through FieldExpr F.
  /// Base.R(Args), Base an interface value: R through its method table.
  llvm::Value *emitInterfaceCall(RoutineDecl *R, Expr *Base, SourceLocation L,
                                 llvm::ArrayRef<Expr *> Args) {
    llvm::Value *V = visitExpr(Base);
    llvm::Value *Obj = B.CreateExtractValue(V, 0, "obj");
    llvm::Value *Table = B.CreateExtractValue(V, 1, "itable");
    if (rangeChecks(L))
      emitRuntimeCheck(B.CreateIsNull(Obj), "use of a nil interface reference", L);
    llvm::Value *Slot =
        B.CreateConstInBoundsGEP1_64(B.getPtrTy(), Table, R->getVMTIndex());
    llvm::Value *Fn = B.CreateLoad(B.getPtrTy(), Slot, "method");
    llvm::Value *Result =
        emitCallTo({routineFnType(R), Fn},
                   llvm::ConstantPointerNull::get(B.getPtrTy()),
                   R->getParams(), Args, Obj, resultOf(R));
    return R->isFunction() ? Result : nullptr;
  }

  llvm::Value *emitMemberCall(FieldExpr *F, llvm::ArrayRef<Expr *> Args) {
    auto *R = llvm::cast<RoutineDecl>(F->getMember());
    if (R->getClass() && R->getClass()->isHelper()) // a type helper's
      return R->hasSelf() ? emitRecordMethodCall(R, helperSelf(F), Args)
                          : emitCall(R, Args);
    if (R->isStatic() && !F->getBase()->getType()->isRecord())
      return emitCall(R, Args); // a class's static method: no Self
    if (auto *RT = llvm::dyn_cast<RecordType>(F->getBase()->getType())) {
      if (namesType(F->getBase())) { // TR.Create, TR.M
        if (R->isConstructor())
          return emitRecordConstruction(RT, R, Args);
        return emitCall(R, Args);
      }
      if (!R->hasSelf())
        return emitCall(R, Args);
      return emitRecordMethodCall(R, emitAddress(F->getBase()), Args);
    }
    if (F->getBase()->getType()->isInterface()) // through the method table
      return emitInterfaceCall(R, F->getBase(), F->getField().Loc, Args);
    if (namesType(F->getBase())) { // TFoo.Create, TOuter.TInner.Create
      auto *C = llvm::cast<ClassType>(F->getBase()->getType());
      if (R->isConstructor())
        return emitConstruction(C, R, Args, F->getBeginLoc());
      return emitMethodCall(R, vmtOf(C), Args, /*Inherited=*/false,
                            F->getField().Loc, /*SelfIsClass=*/true);
    }
    if (F->getBase()->getType()->isClassRef()) // c.M, c: class of T
      return emitMethodCall(R, visitExpr(F->getBase()), Args, /*Inherited=*/false,
                            F->getField().Loc, /*SelfIsClass=*/true);
    // Not checked for nil: Free accepts nil, and a static method may be
    // called on nil (as in Free Pascal); virtual calls are checked.
    llvm::Value *Obj = visitExpr(F->getBase());
    return emitMethodCall(R, Obj, Args, /*Inherited=*/false, F->getField().Loc);
  }

  /// inherited [M] [(args)]: on the enclosing method's Self.
  llvm::Value *emitInheritedCall(InheritedExpr *I, llvm::ArrayRef<Expr *> Args) {
    if (!I->getMethod()) // a bare `inherited` with no inherited method
      return nullptr;
    llvm::Value *Self = B.CreateLoad(B.getPtrTy(),
                                     varAddress(I->getEnclosing()->getSelfVar()),
                                     "self");
    if (I->isBare() && Args.empty())
      Args = I->getImplicitArgs();
    return emitMethodCall(I->getMethod(), Self, Args, /*Inherited=*/true,
                          I->getBeginLoc(), I->getEnclosing()->isClassMethod());
  }

  /// The value of property P of object Obj (static class C; for an advanced
  /// record, Obj is its address and C the record type): an address for
  /// strings and records. Indexes: an array property's.
  llvm::Value *emitPropertyRead(PropertyDecl *P, llvm::Value *Obj, Type *C,
                                llvm::ArrayRef<Expr *> Indexes = {}) {
    Type *T = P->getType();
    if (P->isClassProperty()) { // a class variable or a static method
      if (auto *V = llvm::dyn_cast<VarDecl>(P->getRead()))
        return T->isStructured() ? Globals[V]
                                 : loadScalar(Globals[V], T, P->getName().Name);
      return emitCall(llvm::cast<RoutineDecl>(P->getRead()),
                      accessorArgs(P, Indexes, nullptr));
    }
    if (auto *F = llvm::dyn_cast<FieldDecl>(P->getRead())) {
      llvm::Value *Addr = fieldAddress(Obj, C, F);
      if (T->isStructured())
        return Addr;
      return loadScalar(Addr, T, P->getName().Name);
    }
    return emitMethodCall(llvm::cast<RoutineDecl>(P->getRead()), Obj,
                          accessorArgs(P, Indexes, nullptr),
                          /*Inherited=*/false, P->getLocation());
  }

  /// The arguments of a property's accessor method: the indexes of an
  /// array property, the `index` constant, the value written (if any).
  llvm::SmallVector<Expr *, 4> accessorArgs(PropertyDecl *P,
                                            llvm::ArrayRef<Expr *> Indexes,
                                            Expr *Value) {
    llvm::SmallVector<Expr *, 4> Args(Indexes.begin(), Indexes.end());
    if (Expr *I = P->getIndexSpec())
      Args.push_back(I);
    if (Value)
      Args.push_back(Value);
    return Args;
  }

  /// The object of an array property access E (obj.P[i], P[i], obj[i]),
  /// and its static class C (an advanced record: its address and type).
  llvm::Value *propertyObject(IndexExpr *E, Type *&C) {
    Expr *Root = E->getPropertyRoot();
    if (auto *F = llvm::dyn_cast<FieldExpr>(Root);
        F && F->getMember() == E->getProperty()) {
      C = F->getBase()->getType();
      return memberObject(F->getBase());
    }
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Root);
        N && N->getDecl() == E->getProperty()) {
      C = N->getWith()->Record->getType();
      return withBase(N->getWith());
    }
    C = Root->getType(); // the default property
    return emitObject(Root);
  }

  /// The object whose member Base.M is used: Base's object (checked for
  /// nil), or a record's address.
  llvm::Value *memberObject(Expr *Base) {
    if (Base->getType()->isRecord())
      return emitAddress(Base);
    return emitObject(Base);
  }

  /// The value of array property access E (an address for strings and
  /// records).
  llvm::Value *emitArrayPropertyRead(IndexExpr *E) {
    if (E->getProperty()->isClassProperty())
      return emitPropertyRead(E->getProperty(), nullptr, nullptr,
                              E->getPropertyArgs());
    Type *C;
    llvm::Value *Obj = propertyObject(E, C);
    return emitPropertyRead(E->getProperty(), Obj, C, E->getPropertyArgs());
  }

  /// A member of a class used as a value through FieldExpr F: a field, a
  /// property, or a call of a method without arguments.
  llvm::Value *emitMemberValue(FieldExpr *F) {
    Decl *D = F->getMember();
    if (llvm::isa<RoutineDecl>(D))
      return emitMemberCall(F, {});
    if (auto *V = llvm::dyn_cast<VarDecl>(D)) { // a class variable
      if (V->getType()->isShortString())
        return Globals[V];
      return loadScalar(Globals[V], V->getType(), V->getName().Name);
    }
    Type *C = F->getBase()->getType();
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D); P && P->isClassProperty())
      return emitPropertyRead(P, nullptr, nullptr); // (no object)
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D);
        P && helperRoutine(P)) // a type helper's
      return emitPropertyRead(P, helperSelf(F), C);
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D); P && C->isInterface())
      return emitInterfaceCall(llvm::cast<RoutineDecl>(P->getRead()), F->getBase(),
                               F->getField().Loc, {}); // (its getter)
    llvm::Value *Obj = memberObject(F->getBase());
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
      return emitPropertyRead(P, Obj, C);
    llvm::Value *Addr = fieldAddress(Obj, C, F->getFieldDecl());
    if (F->getType()->isShortString())
      return Addr;
    return loadScalar(Addr, F->getType(), F->getField().Name);
  }

  //===--------------------------------------------------------------------===//
  // goto
  //===--------------------------------------------------------------------===//

  llvm::BasicBlock *labelBlock(LabelDecl *L) {
    llvm::BasicBlock *&BB = LabelBlocks[L];
    if (!BB)
      BB = newBlock("label." + L->getName().Name);
    return BB;
  }

  /// At entry of a block that is the target of non-local gotos:
  /// switch (_setjmp(buf)) { case k: goto label_k; }
  void emitSetJmpDispatch(Block &Blk, llvm::Value *Buf) {
    llvm::Value *R = B.CreateCall(RT.SetJmp, {Buf}, "jmp");
    auto *Start = newBlock("body");
    llvm::SwitchInst *SW = B.CreateSwitch(R, Start);
    for (Decl *D : Blk.getDecls())
      if (auto *L = llvm::dyn_cast<LabelDecl>(D); L && L->isNonLocal())
        SW->addCase(B.getInt32(L->getIndex()), labelBlock(L));
    B.SetInsertPoint(Start);
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

  llvm::Constant *intConst(llvm::Value *Like, int64_t V) {
    return llvm::ConstantInt::get(Like->getType(), static_cast<uint64_t>(V), true);
  }

  llvm::Value *emitDiv(llvm::Value *L, llvm::Value *R, bool Signed,
                       SourceLocation Loc) {
    if (!isConstNonZero(R))
      emitDivZeroCheck(B.CreateICmpEQ(R, intConst(R, 0)), Loc);
    if (!Signed)
      return B.CreateUDiv(L, R, "div");
    // INT_MIN div -1 overflows (and traps on x86).
    if (!isConstNotMinusOne(R)) {
      llvm::Value *Min = llvm::ConstantInt::get(
          L->getType(),
          llvm::APInt::getSignedMinValue(L->getType()->getIntegerBitWidth()));
      llvm::Value *IsMin = B.CreateICmpEQ(L, Min);
      llvm::Value *IsMinusOne = B.CreateICmpEQ(R, intConst(R, -1));
      llvm::Value *Overflow = B.CreateAnd(IsMin, IsMinusOne);
      emitOverflowCheck(Overflow, "integer overflow in 'div'", Loc);
    }
    return B.CreateSDiv(L, R, "div");
  }

  llvm::Value *emitMod(llvm::Value *L, llvm::Value *R, bool Signed,
                       SourceLocation Loc) {
    if (LangOpts.Mode == LangMode::ISO) {
      // ISO 7185 6.7.2.2: error if j <= 0; result is i - k*j with
      // 0 <= result < j.
      auto *CR = llvm::dyn_cast<llvm::ConstantInt>(R);
      if (!CR || !CR->getValue().isStrictlyPositive())
        emitRuntimeCheck(B.CreateICmpSLE(R, intConst(R, 0)),
                         "'mod' by zero or a negative number", Loc);
      llvm::Value *Rem = B.CreateSRem(L, R);
      llvm::Value *Fixed = B.CreateAdd(Rem, R);
      return B.CreateSelect(B.CreateICmpSLT(Rem, intConst(Rem, 0)), Fixed, Rem,
                            "mod");
    }
    // Turbo/FPC: sign follows the dividend (like C %).
    if (!isConstNonZero(R))
      emitDivZeroCheck(B.CreateICmpEQ(R, intConst(R, 0)), Loc);
    if (!Signed)
      return B.CreateURem(L, R, "mod");
    // x mod -1 == 0, but INT_MIN srem -1 traps on x86: divide by 1 instead.
    llvm::Value *SafeR = B.CreateSelect(B.CreateICmpEQ(R, intConst(R, -1)),
                                        intConst(R, 1), R);
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
    if (Op == BinaryOp::In)
      return emitIn(E);
    if (LT->isSet()) {
      // Compared in the larger of the two representations.
      Type *Wide = setBits(RTy) > setBits(LT) ? RTy : LT;
      llvm::Value *L = visitExpr(E->getLHS());
      L = convertSet(L, LT, Wide);
      llvm::Value *R = visitExpr(E->getRHS());
      R = convertSet(R, RTy, Wide);
      llvm::Value *Zero = llvm::ConstantInt::get(setType(Wide), 0);
      switch (Op) {
      case BinaryOp::Eq: return B.CreateICmpEQ(L, R, "cmp");
      case BinaryOp::Ne: return B.CreateICmpNE(L, R, "cmp");
      case BinaryOp::Le: // L is a subset of R
        return B.CreateICmpEQ(B.CreateAnd(L, B.CreateNot(R)), Zero, "subset");
      default:           // L is a superset of R
        return B.CreateICmpEQ(B.CreateAnd(R, B.CreateNot(L)), Zero, "superset");
      }
    }
    if (needsStringCompare(LT, RTy) &&
        (LT->isAnsiString() || RTy->isAnsiString())) {
      llvm::Value *L = emitAnsi(E->getLHS());
      llvm::Value *R = emitAnsi(E->getRHS());
      return compareInts(Op, astrCall("__pas_astr_compare", B.getInt32Ty(), {L, R}),
                         B.getInt32(0), /*Signed=*/true);
    }
    if (needsStringCompare(LT, RTy)) { // Turbo/FPC string comparison
      llvm::Value *Cmp = B.CreateCall(
          RT.StrCompare, {emitPStr(E->getLHS()), emitPStr(E->getRHS())});
      return compareInts(Op, Cmp, B.getInt32(0), /*Signed=*/true);
    }
    if (isMethodPointerType(LT) || isMethodPointerType(RTy)) {
      auto Part = [&](Expr *X) -> llvm::Value * {
        return llvm::isa<NilExpr>(X)
                   ? llvm::Constant::getNullValue(
                         llvm::StructType::get(B.getPtrTy(), B.getPtrTy()))
                   : visitExpr(X);
      };
      llvm::Value *L = Part(E->getLHS());
      llvm::Value *R = Part(E->getRHS());
      // The code only, as in Free Pascal.
      llvm::Value *LC = B.CreateExtractValue(L, 0);
      llvm::Value *RC = B.CreateExtractValue(R, 0);
      return Op == BinaryOp::Eq ? B.CreateICmpEQ(LC, RC, "cmp")
                                : B.CreateICmpNE(LC, RC, "cmp");
    }
    if (LT->isPointer() || LT->isNil() || LT->isClass() || LT->isClassRef() ||
        LT->isInterface() || LT->isDynArray()) { // interfaces: their objects
      llvm::Value *L = interfaceObject(visitExpr(E->getLHS()));
      llvm::Value *R = interfaceObject(visitExpr(E->getRHS()));
      switch (Op) {
      case BinaryOp::Eq: return B.CreateICmpEQ(L, R, "cmp");
      case BinaryOp::Ne: return B.CreateICmpNE(L, R, "cmp");
      // Pointers, by address (Turbo/FPC).
      case BinaryOp::Lt: return B.CreateICmpULT(L, R, "cmp");
      case BinaryOp::Le: return B.CreateICmpULE(L, R, "cmp");
      case BinaryOp::Gt: return B.CreateICmpUGT(L, R, "cmp");
      default:           return B.CreateICmpUGE(L, R, "cmp");
      }
    }

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
    if (Type *OpTy = E->getOperandType()) { // integers of possibly different types
      L = convertOrdinal(L, LT, OpTy);
      R = convertOrdinal(R, RTy, OpTy);
      return compareInts(Op, L, R, isSignedOrdinal(OpTy));
    }
    return compareInts(Op, L, R, isSignedOrdinal(LT));
  }

  //===--------------------------------------------------------------------===//
  // Built-in procedures and functions
  //===--------------------------------------------------------------------===//

  /// Splits off a leading file argument of read/write; returns the PasFile*.
  llvm::Value *fileTarget(llvm::ArrayRef<Expr *> &Args, bool Input,
                          FileType *&FT) {
    FT = nullptr;
    if (!Args.empty() && Args[0]->getType() && Args[0]->getType()->isFile()) {
      FT = llvm::cast<FileType>(Args[0]->getType());
      llvm::Value *F = loadFile(emitAddress(Args[0]));
      Args = Args.drop_front();
      return F;
    }
    return Input ? inputFile() : outputFile();
  }

  /// Address of a value convertible to type T: the designator itself if it
  /// already has type T, else a temporary holding the converted value.
  llvm::Value *addressForValue(Expr *E, Type *T) {
    if (E->getType() == T && (T->isStructured() || T->isStringType()))
      return emitAddress(E);
    if (T->isStructured()) {
      llvm::Value *Tmp = createTemp(memType(T));
      copyAggregate(Tmp, emitAddress(E), T);
      return Tmp;
    }
    llvm::Value *Tmp = createTemp(memType(T));
    storeScalar(Tmp, convertForAssign(visitExpr(E), E->getType(), T,
                                      E->getBeginLoc()),
                T);
    return Tmp;
  }

  /// An expression that cannot call Pascal code: its evaluation cannot raise
  /// an exception, so it is safe to format it into the write buffer. Runtime
  /// calls (bounds checks, string concatenation) are fine: the end of the
  /// statement flushes the buffer, or the next statement discards it.
  static bool simpleWriteExpr(Expr *E) {
    if (auto *P = llvm::dyn_cast<ParenExpr>(E))
      return simpleWriteExpr(P->getInner());
    if (llvm::isa<IntegerLiteralExpr>(E) || llvm::isa<RealLiteralExpr>(E) ||
        llvm::isa<StringLiteralExpr>(E))
      return true;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
      Decl *D = N->getDecl();
      return !N->getWith() && D &&
             (llvm::isa<VarDecl>(D) || llvm::isa<ConstDecl>(D));
    }
    if (auto *U = llvm::dyn_cast<UnaryExpr>(E))
      return !U->getOperator() && simpleWriteExpr(U->getOperand());
    if (auto *B = llvm::dyn_cast<BinaryExpr>(E))
      return !B->getOperator() && simpleWriteExpr(B->getLHS()) &&
             simpleWriteExpr(B->getRHS());
    if (auto *I = llvm::dyn_cast<IndexExpr>(E))
      return !I->getProperty() && simpleWriteExpr(I->getBase()) &&
             simpleWriteExpr(I->getIndex());
    if (auto *F = llvm::dyn_cast<FieldExpr>(E))
      return F->getMember() && llvm::isa<FieldDecl>(F->getMember()) &&
             simpleWriteExpr(F->getBase());
    return false;
  }

  static bool simpleWriteArg(Expr *E) {
    if (auto *F = llvm::dyn_cast<FormattedExpr>(E))
      return simpleWriteExpr(F->getValue()) &&
             (!F->getWidth() || simpleWriteExpr(F->getWidth())) &&
             (!F->getPrecision() || simpleWriteExpr(F->getPrecision()));
    return simpleWriteExpr(E);
  }

  /// Appends a compile-time constant write argument to a merged literal.
  /// Only ordinals: a real would need the runtime's exact formatting.
  static bool appendConstText(Expr *E, std::string &Text) {
    if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(E)) {
      if (SL->isWideCharLiteral())
        return false;
      Text += SL->getValue().str();
      return true;
    }
    if (auto *IL = llvm::dyn_cast<IntegerLiteralExpr>(E)) {
      Type *T = IL->getType();
      if (!T || !T->isInteger())
        return false;
      if (isSignedOrdinal(T))
        Text += std::to_string((int64_t)IL->getValue());
      else
        Text += std::to_string(IL->getValue());
      return true;
    }
    return false;
  }

  void emitWrite(ProcCallStmt *S, bool IsWriteln) {
    llvm::ArrayRef<Expr *> Args = S->getArgs();
    FileType *FT;
    llvm::Value *File = fileTarget(Args, /*Input=*/false, FT);
    if (FT && !FT->isText()) {
      for (Expr *Arg : Args)
        B.CreateCall(RT.WriteElem,
                     {File, addressForValue(Arg, FT->getElementType())});
      return;
    }
    bool Batch = true;
    for (Expr *Arg : Args)
      if (!simpleWriteArg(Arg)) {
        Batch = false;
        break;
      }
    B.CreateCall(Batch ? RT.WriteBegin : RT.WriteDiscard, {File});
    // Adjacent string, character and ordinal literals become one write; a
    // trailing literal of writeln gets the newline (items 19 and 2).
    bool NewlineWritten = false;
    for (size_t I = 0; I < Args.size();) {
      std::string Text;
      size_t J = I;
      while (J < Args.size()) {
        if (!appendConstText(Args[J], Text))
          break;
        ++J;
      }
      if (J > I) {
        bool Last = J == Args.size();
        if (Last && IsWriteln)
          Text.push_back('\n');
        llvm::Constant *S = B.CreateGlobalString(Text, ".str");
        B.CreateCall(RT.WriteStr,
                     {File, S, B.getInt64(Text.size()), B.getInt32(0)});
        if (Last && IsWriteln) {
          NewlineWritten = true;
          break;
        }
        I = J;
        continue;
      }
      emitWriteArg(File, Args[I]);
      ++I;
    }
    if (IsWriteln && !NewlineWritten)
      B.CreateCall(RT.Writeln, {File});
    if (Batch)
      B.CreateCall(RT.WriteEnd, {File});
  }

  void emitRead(ProcCallStmt *S, bool IsReadln) {
    llvm::ArrayRef<Expr *> Args = S->getArgs();
    FileType *FT;
    llvm::Value *File = fileTarget(Args, /*Input=*/true, FT);
    if (FT && !FT->isText()) {
      Type *Elem = FT->getElementType();
      for (Expr *Arg : Args) {
        if (Arg->getType() == Elem) {
          B.CreateCall(RT.ReadElem, {File, emitAddress(Arg)});
          continue;
        }
        llvm::Value *Tmp = createTemp(memType(Elem));
        B.CreateCall(RT.ReadElem, {File, Tmp});
        llvm::Value *V = convertForAssign(loadScalar(Tmp, Elem), Elem,
                                          Arg->getType(), Arg->getBeginLoc());
        storeScalar(emitAddress(Arg), V, Arg->getType());
      }
      return;
    }
    for (Expr *Arg : Args)
      emitReadArg(File, Arg);
    if (IsReadln)
      B.CreateCall(RT.Readln, {File});
  }

  void emitWriteArg(llvm::Value *File, Expr *Arg) {
    Expr *Value = Arg;
    llvm::Value *Width = B.getInt32(0); // no field width
    llvm::Value *Precision = B.getInt32(-1);
    if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
      Value = F->getValue();
      Width = emitI32(F->getWidth());
      if (F->getPrecision())
        Precision = emitI32(F->getPrecision());
    }

    Type *T = Value->getType();
    if (T->isAnsiString()) {
      astrCall("__pas_write_astr", B.getVoidTy(), {File, visitExpr(Value), Width});
      return;
    }
    if (T->isPointer()) { // a pchar: the C string it points to
      astrCall("__pas_write_cstr", B.getVoidTy(), {File, visitExpr(Value), Width});
      return;
    }
    if (T->isShortString()) {
      B.CreateCall(RT.WritePStr, {File, emitPStr(Value), Width});
      return;
    }
    if (T->isStringType()) {
      B.CreateCall(RT.WriteStr, {File, emitAddress(Value),
                                 B.getInt64(T->getStringLength()), Width});
      return;
    }
    llvm::Value *V = visitExpr(Value);
    if (T->isReal())
      B.CreateCall(RT.WriteReal, {File, V, Width, Precision});
    else if (T->getBase()->isChar())
      B.CreateCall(RT.WriteChar, {File, V, Width});
    else if (T->getBase()->isWideChar())
      B.CreateCall(RT.WriteChar, {File, wideToChar(V), Width});
    else if (T->getBase()->isBoolean())
      B.CreateCall(RT.WriteBool, {File, B.CreateZExt(V, B.getInt32Ty()), Width});
    else if (auto *ET = llvm::dyn_cast<EnumType>(T->getBase()))
      emitWriteEnum(File, V, T, ET, Width, Value->getBeginLoc());
    else if (isQWord(T->getBase()))
      B.CreateCall(classRuntime("__pas_write_uint", B.getVoidTy(),
                                {B.getPtrTy(), B.getInt64Ty(), B.getInt32Ty()}),
                   {File, V, Width});
    else
      B.CreateCall(RT.WriteInt, {File, toI64(V, T), Width});
  }

  /// The names of E's values as length-prefixed strings: a private
  /// constant [N x ptr], one per enumeration type.
  llvm::GlobalVariable *enumNameTable(EnumType *E) {
    llvm::GlobalVariable *&G = EnumNameTables[E];
    if (G)
      return G;
    llvm::SmallVector<llvm::Constant *, 16> Names;
    for (ConstDecl *V : E->getValues())
      Names.push_back(getPStrConstant(V->getName().Spelling));
    auto *Ty = llvm::ArrayType::get(B.getPtrTy(), Names.size());
    G = new llvm::GlobalVariable(M, Ty, true, llvm::GlobalValue::PrivateLinkage,
                                 llvm::ConstantArray::get(Ty, Names),
                                 ".enumnames");
    G->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return G;
  }

  /// write(e) of an enumeration's value (Turbo/FPC): its name, as declared,
  /// padded on the right to the field width. A value outside the type is a
  /// runtime error (always, as in Free Pascal).
  void emitWriteEnum(llvm::Value *File, llvm::Value *V, Type *T, EnumType *E,
                     llvm::Value *Width, SourceLocation L) {
    llvm::Value *X = toI64(V, T);
    uint64_t N = E->getValues().size();
    emitRuntimeCheck(B.CreateICmpUGE(X, B.getInt64(N)),
                     "invalid enumeration value", L);
    llvm::GlobalVariable *Table = enumNameTable(E);
    B.CreateCall(classRuntime("__pas_write_enum", B.getVoidTy(),
                              {B.getPtrTy(), B.getPtrTy(), B.getInt64Ty(),
                               B.getInt32Ty()}),
                 {File, Table, X, Width});
  }

  void emitReadArg(llvm::Value *File, Expr *Arg) {
    Type *T = Arg->getType();
    llvm::Value *Addr = emitAddress(Arg);
    if (T->isAnsiString()) {
      astrCall("__pas_read_astr", B.getVoidTy(), {File, Addr});
      return;
    }
    if (auto *SS = llvm::dyn_cast<ShortStringType>(T)) {
      B.CreateCall(RT.ReadStr, {File, Addr, B.getInt32(SS->getMaxLength())});
      return;
    }
    if (T->isReal()) {
      B.CreateStore(B.CreateCall(RT.ReadReal, {File}), Addr);
      return;
    }
    if (T->getBase()->isChar()) {
      llvm::Value *V = B.CreateCall(RT.ReadChar, {File});
      emitRangeCheck(V, T->getBase(), T, Arg->getBeginLoc());
      B.CreateStore(V, Addr);
      return;
    }
    if (isQWord(T)) { // all of 0 .. 2^64-1
      llvm::Value *V = B.CreateCall(
          classRuntime("__pas_read_uint64", B.getInt64Ty(), {B.getPtrTy()}),
          {File});
      B.CreateStore(V, Addr);
      return;
    }
    // Integers: read as i32 (or i64 for types that need it), then check the
    // value against the target type.
    bool Wide = intBits(T) == 64 || !isSignedOrdinal(T);
    llvm::Value *V = B.CreateCall(Wide ? RT.ReadInt64 : RT.ReadInt, {File});
    llvm::Value *X = B.CreateSExt(V, B.getInt64Ty());
    OrdinalBounds TB = getOrdinalBounds(T);
    if (rangeChecks(Arg->getBeginLoc()) &&
        (TB.Lo > (Wide ? INT64_MIN : INT32_MIN) ||
         TB.Hi < (Wide ? INT64_MAX : INT32_MAX)))
      emitRuntimeCheck(outsideRange(X, TB.Lo, TB.Hi), "value out of range",
                       Arg->getBeginLoc());
    B.CreateStore(B.CreateTrunc(X, regType(T)), Addr);
  }

  /// pack(a, i, z) / unpack(z, a, i): packed and unpacked arrays share a
  /// representation, so both are a bounds-checked memcpy.
  void emitPackUnpack(ProcCallStmt *S, bool IsPack) {
    auto Args = S->getArgs();
    Expr *Unpacked = IsPack ? Args[0] : Args[1];
    Expr *Index = IsPack ? Args[1] : Args[2];
    Expr *Packed = IsPack ? Args[2] : Args[0];
    auto *UA = llvm::cast<ArrayType>(Unpacked->getType());
    auto *PA = llvm::cast<ArrayType>(Packed->getType());
    OrdinalBounds UB = getOrdinalBounds(UA->getIndexType());
    OrdinalBounds PB = getOrdinalBounds(PA->getIndexType());
    int64_t Count = PB.Hi - PB.Lo + 1;
    llvm::Value *I = toI64(visitExpr(Index), Index->getType());
    if (rangeChecks(Index->getBeginLoc()))
      emitRuntimeCheck(
          outsideRange(I, UB.Lo, UB.Hi - Count + 1),
          IsPack ? "'pack' index out of range" : "'unpack' index out of range",
          Index->getBeginLoc());
    llvm::Value *Base = emitAddress(Unpacked);
    llvm::Value *Offset = B.CreateSub(I, B.getInt64(UB.Lo));
    llvm::Value *Elem =
        B.CreateInBoundsGEP(memType(UA), Base, {B.getInt64(0), Offset});
    llvm::Value *PackedAddr = emitAddress(Packed);
    uint64_t Bytes = Count * sizeOf(UA->getElementType());
    if (IsPack)
      B.CreateMemCpy(PackedAddr, llvm::MaybeAlign(1), Elem, llvm::MaybeAlign(1), Bytes);
    else
      B.CreateMemCpy(Elem, llvm::MaybeAlign(1), PackedAddr, llvm::MaybeAlign(1), Bytes);
  }

  llvm::Value *emitFileQuery(BuiltinKind K, Expr *FileArg) {
    llvm::Value *File = FileArg ? loadFile(emitAddress(FileArg)) : inputFile();
    llvm::Value *R = B.CreateCall(K == BuiltinKind::Eof ? RT.Eof : RT.Eoln, {File});
    return B.CreateICmpNE(R, B.getInt32(0), K == BuiltinKind::Eof ? "eof" : "eoln");
  }

  /// T(x): ordinal conversions keep the low bits (sign/zero-extending by
  /// the source); pointer casts change only the type.
  llvm::Value *emitTypecast(CallExpr *E) {
    Expr *Arg = E->getArgs()[0];
    if (isMethodPointerType(E->getType()) && Arg->getType()->isRecord()) // TEv(m)
      return B.CreateLoad(memType(E->getType()), emitAddress(Arg));
    if (E->getType()->isAnsiString() && Arg->getType()->isPointer() &&
        llvm::cast<PointerType>(Arg->getType())->isUntyped())
      return visitExpr(Arg); // the pointer is an ansistring: borrowed
    if (E->getType()->isAnsiString())
      return emitAnsi(Arg);
    if (E->getType()->isShortString())
      return emitPStr(Arg);
    llvm::Value *V = visitExpr(Arg);
    if (Arg->getType()->isAnsiString()) { // pchar(s) is never nil
      auto *P = llvm::cast<PointerType>(E->getType());
      if (!P->isUntyped() && P->getPointee()->isChar())
        return astrCall("__pas_astr_pchar", B.getPtrTy(), {V});
      return V;
    }
    if (E->getType()->isOrdinal())
      return convertOrdinal(V, Arg->getType(), E->getType());
    if (E->getType()->isInterface())
      return convertForAssign(V, Arg->getType(), E->getType(), E->getBeginLoc());
    return V;
  }

  /// A runtime file routine (declared when first used).
  llvm::FunctionCallee fileRuntime(llvm::StringRef Name, llvm::Type *Ret,
                                   llvm::ArrayRef<llvm::Type *> Params) {
    return classRuntime(Name, Ret, Params);
  }

  llvm::Value *emitBuiltinCall(CallExpr *E, BuiltinDecl *BD) {
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Wait:
      return emitAwait(visitExpr(E->getArgs()[0]),
                       llvm::cast<TaskType>(E->getArgs()[0]->getType()));
    case BuiltinKind::WhenAll:
    case BuiltinKind::WhenAny:
      return emitWhen(E, BD->getBuiltinKind() == BuiltinKind::WhenAll);
    case BuiltinKind::IteratorNext:
      return emitIteratorNext(visitExpr(E->getArgs()[0]));
    case BuiltinKind::IteratorValue: { // (read as a variable's value)
      Type *ET = E->getType();
      llvm::Value *Addr = iteratorValueAddr(visitExpr(E->getArgs()[0]));
      return ET->isStructured() ? Addr : loadScalar(Addr, ET, "value");
    }
    case BuiltinKind::AddChecked:
    case BuiltinKind::SubChecked:
    case BuiltinKind::MulChecked: { // the tuple (a op b, no overflow)
      auto *TT = llvm::cast<RecordType>(E->getType());
      Type *T = TT->getAllFields()[0]->getType();
      Expr *A = E->getArgs()[0], *C = E->getArgs()[1];
      llvm::Value *L = convertOrdinal(visitExpr(A), A->getType(), T);
      llvm::Value *R = convertOrdinal(visitExpr(C), C->getType(), T);
      bool Signed = isSignedOrdinal(T);
      BuiltinKind K = BD->getBuiltinKind();
      llvm::Intrinsic::ID ID =
          K == BuiltinKind::AddChecked
              ? (Signed ? llvm::Intrinsic::sadd_with_overflow
                        : llvm::Intrinsic::uadd_with_overflow)
          : K == BuiltinKind::SubChecked
              ? (Signed ? llvm::Intrinsic::ssub_with_overflow
                        : llvm::Intrinsic::usub_with_overflow)
              : (Signed ? llvm::Intrinsic::smul_with_overflow
                        : llvm::Intrinsic::umul_with_overflow);
      llvm::Value *Res = B.CreateBinaryIntrinsic(ID, L, R);
      llvm::Value *Tmp = createTemp(memType(TT), "checked");
      storeScalar(fieldAddress(Tmp, TT, TT->getAllFields()[0]),
                  B.CreateExtractValue(Res, 0, "value"), T);
      storeScalar(fieldAddress(Tmp, TT, TT->getAllFields()[1]),
                  B.CreateNot(B.CreateExtractValue(Res, 1), "ok"),
                  TT->getAllFields()[1]->getType());
      return Tmp;
    }
    case BuiltinKind::FilePos:
    case BuiltinKind::FileSize:
      return B.CreateCall(
          fileRuntime(BD->getBuiltinKind() == BuiltinKind::FilePos
                          ? "__pas_filepos"
                          : "__pas_filesize",
                      B.getInt64Ty(), {B.getPtrTy()}),
          {loadFile(emitAddress(E->getArgs()[0]))},
          BD->getBuiltinKind() == BuiltinKind::FilePos ? "filepos" : "filesize");
    case BuiltinKind::GetFileHandle:
      return B.CreateCall(fileRuntime("__pas_file_handle", B.getInt32Ty(),
                                      {B.getPtrTy()}),
                          {loadFile(emitAddress(E->getArgs()[0]))}, "handle");
    case BuiltinKind::SizeOf: // the argument is not evaluated
      if (auto *OA = llvm::dyn_cast<OpenArrayType>(E->getArgs()[0]->getType()))
        return B.CreateMul(openCount(openDescriptor(E->getArgs()[0])),
                           B.getInt64(sizeOf(OA->getElementType())));
      return B.getInt64(sizeOf(E->getArgs()[0]->getType()));
    case BuiltinKind::Low:
    case BuiltinKind::High: {
      bool IsHigh = BD->getBuiltinKind() == BuiltinKind::High;
      Type *AT = E->getArgs()[0]->getType();
      if (AT->isOpenArray()) {
        if (!IsHigh)
          return B.getInt32(0);
        llvm::Value *N = openCount(openDescriptor(E->getArgs()[0]));
        return B.CreateSub(B.CreateTrunc(N, B.getInt32Ty()), B.getInt32(1));
      }
      if (AT->isDynArray()) {
        if (!IsHigh)
          return B.getInt32(0);
        llvm::Value *N = dynLength(visitExpr(E->getArgs()[0]));
        return B.CreateSub(B.CreateTrunc(N, B.getInt32Ty()), B.getInt32(1));
      }
      if (AT->isArrayOfConst()) {
        if (!IsHigh)
          return B.getInt32(0);
        llvm::Value *N = B.CreateLoad(B.getInt64Ty(), visitExpr(E->getArgs()[0]));
        return B.CreateSub(B.CreateTrunc(N, B.getInt32Ty()), B.getInt32(1));
      }
      int64_t V;
      if (auto *SS = llvm::dyn_cast<ShortStringType>(AT)) {
        V = IsHigh ? SS->getMaxLength() : 0;
      } else {
        if (auto *A = llvm::dyn_cast<ArrayType>(AT))
          AT = A->getIndexType();
        OrdinalBounds Bd = getOrdinalBounds(AT);
        V = IsHigh ? (isQWord(AT) ? -1 : Bd.Hi) : Bd.Lo; // (qword: 2^64-1)
      }
      return llvm::ConstantInt::get(regType(E->getType()),
                                    static_cast<uint64_t>(V), true);
    }
    case BuiltinKind::Assigned: {
      Expr *Arg = E->getArgs()[0];
      llvm::Value *P;
      if (isMethodPointerType(Arg->getType()) || Arg->getType()->isClosureType())
        P = B.CreateExtractValue(visitExpr(Arg), 0);
      else if (llvm::isa<RoutineType>(Arg->getType()))
        P = B.CreateExtractValue(
            B.CreateLoad(memType(Arg->getType()), emitAddress(Arg)), 0);
      else
        P = interfaceObject(visitExpr(Arg));
      return B.CreateIsNotNull(P, "assigned");
    }
    case BuiltinKind::ParamCount:
      return B.CreateCall(RT.ParamCount, {}, "paramcount");
    case BuiltinKind::ExceptAddr:
      return exceptAddr();
    case BuiltinKind::ParamStr: {
      llvm::Value *Tmp = newStringTemp();
      B.CreateCall(RT.ParamStr, {Tmp, emitI32(E->getArgs()[0])});
      return Tmp;
    }
    default:
      break;
    }
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Length: case BuiltinKind::Copy: case BuiltinKind::Pos:
    case BuiltinKind::Concat: case BuiltinKind::UpCase:
      return emitStringFunction(E, BD->getBuiltinKind());
    default:
      break;
    }
    if (BD->getBuiltinKind() == BuiltinKind::Eof ||
        BD->getBuiltinKind() == BuiltinKind::Eoln)
      return emitFileQuery(BD->getBuiltinKind(), E->getArgs()[0]);
    Expr *ArgE = E->getArgs()[0];
    Type *AT = ArgE->getType();
    llvm::Value *Arg = visitExpr(ArgE);
    SourceLocation Loc = E->getBeginLoc();

    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Abs: {
      if (AT->isReal())
        return B.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, Arg, nullptr, "abs");
      Arg = convertOrdinal(Arg, AT, E->getType());
      if (!isSignedOrdinal(E->getType()))
        return Arg;
      llvm::Value *Min = llvm::ConstantInt::get(
          Arg->getType(),
          llvm::APInt::getSignedMinValue(Arg->getType()->getIntegerBitWidth()));
      if (overflowChecks(Loc))
        emitOverflowCheck(B.CreateICmpEQ(Arg, Min), "integer overflow in 'abs'", Loc);
      return B.CreateBinaryIntrinsic(llvm::Intrinsic::abs, Arg, B.getFalse(),
                                     nullptr, "abs");
    }
    case BuiltinKind::Sqr:
      if (AT->isReal())
        return B.CreateFMul(Arg, Arg, "sqr");
      Arg = convertOrdinal(Arg, AT, E->getType());
      return emitCheckedArith(BinaryOp::Mul, isSignedOrdinal(E->getType()), Arg,
                              Arg, Loc, "sqr");
    case BuiltinKind::Odd:
      return B.CreateTrunc(Arg, B.getInt1Ty(), "odd");
    case BuiltinKind::Ord:
      return convertOrdinal(Arg, AT, E->getType());
    case BuiltinKind::Chr:
      emitRangeCheck(Arg, AT, E->getType(), Loc, "'chr' argument out of range");
      return B.CreateTrunc(Arg, B.getInt8Ty(), "chr");
    case BuiltinKind::Succ:
    case BuiltinKind::Pred: {
      bool IsSucc = BD->getBuiltinKind() == BuiltinKind::Succ;
      Type *Base = AT->getBase();
      OrdinalBounds Bd = getOrdinalBounds(Base);
      if (isQWord(Base))
        Bd.Hi = -1; // 2^64-1
      if (rangeChecks(Loc))
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
      if (rangeChecks(Loc))
        emitRuntimeCheck(
            B.CreateFCmpULE(X, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
            "'ln' of zero or a negative number", Loc);
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::log, X);
    }
    case BuiltinKind::Sqrt: {
      llvm::Value *X = toDouble(Arg, AT);
      if (rangeChecks(Loc))
        emitRuntimeCheck(
            B.CreateFCmpULT(X, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
            "'sqrt' of a negative number", Loc);
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, X);
    }
    case BuiltinKind::Trunc:
    case BuiltinKind::Round: {
      if (!AT->isReal())
        return convertOrdinal(Arg, AT, E->getType());
      bool IsRound = BD->getBuiltinKind() == BuiltinKind::Round;
      // ISO: round() rounds halves away from zero, like llvm.round.
      // Turbo/FPC round halves to even (banker's rounding).
      llvm::Value *X =
          !IsRound ? Arg
          : B.CreateUnaryIntrinsic(LangOpts.Mode == LangMode::ISO
                                       ? llvm::Intrinsic::round
                                       : llvm::Intrinsic::roundeven,
                                   Arg);
      llvm::Type *ResTy = regType(E->getType());
      if (rangeChecks(Loc)) {
        // The result must lie in the integer (or int64) range; the bounds
        // are exact doubles. Also catches NaN (unordered comparisons are true).
        bool Is64 = ResTy->isIntegerTy(64);
        auto *Lo = llvm::ConstantFP::get(B.getDoubleTy(),
                                         Is64 ? -9223372036854775808.0 : -2147483649.0);
        auto *Hi = llvm::ConstantFP::get(B.getDoubleTy(),
                                         Is64 ? 9223372036854775808.0 : 2147483648.0);
        llvm::Value *Low = Is64 ? B.CreateFCmpULT(X, Lo) : B.CreateFCmpULE(X, Lo);
        emitRuntimeCheck(B.CreateOr(Low, B.CreateFCmpUGE(X, Hi)),
                         (llvm::Twine(IsRound ? "'round'" : "'trunc'") +
                          " result does not fit in " + E->getType()->getName())
                             .str(),
                         Loc);
      }
      return B.CreateFPToSI(X, ResTy, IsRound ? "round" : "trunc");
    }
    default:
      llvm_unreachable("not a builtin function");
    }
  }

public:
  CodeGenModule(llvm::Module &M, const SourceManager &SM,
                const LangOptions &LangOpts, const CodeGenOptions &CGOpts)
      : M(M), DL(M.getDataLayout()), B(M.getContext()), RT(M), SM(SM),
        LangOpts(LangOpts), CGOpts(CGOpts) {
    if (CGOpts.FastMath) { // on the FP instructions IRBuilder creates
      llvm::FastMathFlags FMF;
      FMF.setFast();
      B.setFastMathFlags(FMF);
    }
    for (const llvm::Function &F : M) // RT's declarations
      RuntimeFns.insert(&F);
  }

  void emitProgram(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units) {
    llvm::LLVMContext &C = M.getContext();
    auto *MainTy = llvm::FunctionType::get(
        B.getInt32Ty(), {B.getInt32Ty(), llvm::PointerType::getUnqual(C)},
        false);
    llvm::Function *Main = llvm::Function::Create(
        MainTy, llvm::Function::ExternalLinkage, "main", M);
    Main->getArg(0)->setName("argc");
    Main->getArg(1)->setName("argv");

    Block &Blk = *P.getBlock();
    bool Separate = CGOpts.SeparateUnits;
    if (CGOpts.DebugInfo)
      initDebugInfo(M.getSourceFileName());
    if (Separate)
      for (UnitDecl *U : Units)
        collectExports(*U, /*Imported=*/true);
    for (UnitDecl *U : Units) {
      emitGlobals(*U->getInterface());
      if (!Separate)
        emitGlobals(*U->getImplementation());
    }
    emitGlobals(Blk);
    if (hasNonLocalLabels(Blk)) { // needed by gotos in routine bodies
      MainJmpBuf = new llvm::GlobalVariable(
          M, jmpBufType(), false, llvm::GlobalValue::InternalLinkage,
          llvm::Constant::getNullValue(jmpBufType()), "pas.jmpbuf");
      MainJmpBuf->setAlignment(llvm::Align(16));
    }
    for (UnitDecl *U : Units) {
      declareRoutines(*U->getInterface());
      if (!Separate)
        declareRoutines(*U->getImplementation());
    }
    declareRoutines(Blk);
    finishStaticInits();
    defineVMTs();
    for (RoutineDecl *R : RoutinesToEmit)
      emitRoutineBody(R);
    for (RoutineDecl *R : AsyncsToEmit)
      emitAsyncParts(R);
    AsyncsToEmit.clear();
    emitCleanups();

    CurFn = Main;

    AddrAligns.clear();
    CurRoutine = nullptr;
    CurFrame = nullptr;
    CurPin = nullptr;
    PinTemps = true;
    CurSP = DIB ? createSubprogram(Main, "main", DIUnitFile,
                                   lineOf(P.getLocation()), nullptr, {})
                : nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Main));
    setDebugLoc(P.getLocation());
    B.CreateCall(RT.Init, {Main->getArg(0), Main->getArg(1)});
    if (LangOpts.Mode != LangMode::ISO)
      B.CreateStore(B.getInt32(1),
                    new llvm::GlobalVariable(M, B.getInt32Ty(), false,
                                             llvm::GlobalValue::ExternalLinkage,
                                             nullptr, "__pas_tp_mode"));
    // Program parameters other than input/output bind to argv[1], ...
    llvm::StringMap<unsigned> ParamIndex;
    for (const Identifier &Param : P.getParams())
      if (Param.Name != "input" && Param.Name != "output")
        ParamIndex.try_emplace(Param.Name, ParamIndex.size() + 1);
    if (Separate) {
      // Separately compiled units initialize themselves, in uses order.
      for (UnitDecl *U : Units)
        B.CreateCall(M.getOrInsertFunction(
            initName(*U), llvm::FunctionType::get(B.getVoidTy(), false)));
    } else {
      // Units: their files, then their initialization parts, in uses order.
      for (UnitDecl *U : Units) {
        initFileVars(U->getInterface()->getDecls(), nullptr);
        initFileVars(U->getImplementation()->getDecls(), nullptr);
      }
      for (UnitDecl *U : Units) {
        emitClassInits(U->getInterface()->getDecls(),
                       U->getImplementation()->getDecls());
        visitStmt(U->getImplementation()->getBody());
      }
    }
    initFileVars(Blk.getDecls(), &ParamIndex);
    if (MainJmpBuf)
      emitSetJmpDispatch(Blk, MainJmpBuf);
    emitClassInits(Blk.getDecls(), {});
    ExitBlock = newBlock("exit");
    visitStmt(Blk.getBody());
    B.CreateBr(ExitBlock);
    B.SetInsertPoint(ExitBlock);
    B.CreateCall(RT.Fini);
    B.CreateRet(B.getInt32(0));
    emitDynOps();
    markDefinedFunctionsNounwind();
    if (DIB)
      DIB->finalize();
    if (!CGOpts.KeepUnused)
      removeUnreachable();
  }

  /// Nothing unwinds through p3's frames: exceptions are _setjmp/_longjmp
  /// based (item 10). LLVM cannot infer it for calls through procedural
  /// values, virtual methods or separately compiled units.
  void markDefinedFunctionsNounwind() {
    bool TargetAttrs = CGOpts.TargetCPU != "generic" ||
                        !CGOpts.TargetFeatures.empty();
    for (llvm::Function &F : M)
      if (!F.isDeclaration()) {
        F.addFnAttr(llvm::Attribute::NoUnwind);
        if (CGOpts.SizeLevel == 1)
          F.addFnAttr(llvm::Attribute::OptimizeForSize);
        else if (CGOpts.SizeLevel == 2)
          F.addFnAttr(llvm::Attribute::MinSize);
        if (TargetAttrs) {
          F.addFnAttr("target-cpu", CGOpts.TargetCPU);
          if (!CGOpts.TargetFeatures.empty())
            F.addFnAttr("target-features", CGOpts.TargetFeatures);
        }
      }
  }

  /// Removes the definitions with internal linkage (routines, constants,
  /// variables) that nothing reachable refers to. Reachable: the
  /// definitions with external linkage (main, a unit's initialization and
  /// interface), and what the code of a reachable function or the
  /// initializer of a reachable variable refers to. So the routines of a
  /// used unit that the program does not call are not compiled to machine
  /// code, nor are the methods of a class whose VMT nothing refers to. All
  /// routines are still generated, in declaration order: what is known
  /// about a routine emitted earlier (can it raise?) stays the same, and so
  /// does the code of the routines that remain.
  void removeUnreachable() {
    llvm::SmallPtrSet<const llvm::GlobalValue *, 32> Live;
    llvm::SmallPtrSet<const llvm::Constant *, 32> Seen;
    llvm::SmallVector<const llvm::GlobalValue *, 64> Work;
    llvm::SmallVector<const llvm::Value *, 16> Pending;
    auto Refer = [&](const llvm::Value *Root) {
      Pending.push_back(Root);
      while (!Pending.empty()) {
        const llvm::Value *V = Pending.pop_back_val();
        if (auto *G = llvm::dyn_cast<llvm::GlobalValue>(V)) {
          if (Live.insert(G).second)
            Work.push_back(G);
        } else if (auto *K = llvm::dyn_cast<llvm::Constant>(V);
                   K && !llvm::isa<llvm::ConstantData>(K) &&
                   Seen.insert(K).second) {
          for (const llvm::Use &U : K->operands())
            Pending.push_back(U.get());
        }
      }
    };
    auto Removable = [](const llvm::GlobalValue &G) {
      return G.hasLocalLinkage() && !G.isDeclaration();
    };
    for (const llvm::Function &F : M)
      if (!F.isDeclaration() && !Removable(F))
        Refer(&F);
    for (const llvm::GlobalVariable &G : M.globals())
      if (!G.isDeclaration() && !Removable(G))
        Refer(&G);
    while (!Work.empty()) {
      const llvm::GlobalValue *G = Work.pop_back_val();
      if (auto *F = llvm::dyn_cast<llvm::Function>(G)) {
        for (const llvm::BasicBlock &BB : *F)
          for (const llvm::Instruction &I : BB)
            for (const llvm::Use &U : I.operands())
              Refer(U.get());
      } else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G)) {
        if (GV->hasInitializer())
          Refer(GV->getInitializer());
      }
    }
    llvm::SmallVector<llvm::Function *, 64> DeadFns;
    llvm::SmallVector<llvm::GlobalVariable *, 64> DeadVars;
    for (llvm::Function &F : M)
      if (Removable(F) && !Live.count(&F))
        DeadFns.push_back(&F);
    for (llvm::GlobalVariable &G : M.globals())
      if (Removable(G) && !Live.count(&G))
        DeadVars.push_back(&G);
    // They may refer to each other: drop the references first.
    for (llvm::Function *F : DeadFns)
      F->deleteBody();
    for (llvm::GlobalVariable *G : DeadVars)
      G->setInitializer(nullptr);
    for (llvm::Function *F : DeadFns) {
      F->removeDeadConstantUsers();
      F->eraseFromParent();
    }
    for (llvm::GlobalVariable *G : DeadVars) {
      G->removeDeadConstantUsers();
      G->eraseFromParent();
    }
  }

  /// The start of a program or unit (Decls, then More: its declarations):
  /// the class constructors of the classes and advanced records it
  /// declares (nested ones too), in declaration order; then its class
  /// destructors are registered with the runtime, which runs them at exit
  /// (last registered first: the program's, then the units' in reverse
  /// order, each one's in declaration order, as in Free Pascal).
  void emitClassInits(llvm::ArrayRef<Decl *> Decls, llvm::ArrayRef<Decl *> More) {
    llvm::SmallVector<RoutineDecl *, 4> Dtors;
    auto Visit = [&](llvm::ArrayRef<Decl *> Ds) {
      for (Decl *D : withNested(Ds)) {
        ClassType *C = definedClass(D);
        if (!C)
          C = advancedRecord(D);
        if (!C || ImportedDecls.count(D))
          continue;
        if (RoutineDecl *R = C->getClassConstructor(); R && Functions.count(R))
          emitCallOrInvoke(Functions[R],
                           {llvm::ConstantPointerNull::get(B.getPtrTy())});
        if (RoutineDecl *R = C->getClassDestructor(); R && Functions.count(R))
          Dtors.push_back(R);
      }
    };
    Visit(Decls);
    Visit(More);
    for (RoutineDecl *R : llvm::reverse(Dtors))
      B.CreateCall(classRuntime("__pas_at_exit", B.getVoidTy(), {B.getPtrTy()}),
                   {Functions[R]});
  }

  /// A unit compiled on its own: its variables and routines, and its
  /// initialization part as pas.<unit>$$init (files, then the statements).
  void emitUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps) {
    llvm::LLVMContext &C = M.getContext();
    llvm::Function *Init = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), false),
        llvm::Function::ExternalLinkage, initName(U), M);
    if (CGOpts.DebugInfo)
      initDebugInfo(M.getSourceFileName());
    for (UnitDecl *D : Deps)
      collectExports(*D, /*Imported=*/true);
    collectExports(U, /*Imported=*/false);
    for (UnitDecl *D : Deps)
      emitGlobals(*D->getInterface());
    emitGlobals(*U.getInterface());
    emitGlobals(*U.getImplementation());
    for (UnitDecl *D : Deps)
      declareRoutines(*D->getInterface());
    declareRoutines(*U.getInterface());
    declareRoutines(*U.getImplementation());
    finishStaticInits();
    defineVMTs();
    for (RoutineDecl *R : RoutinesToEmit)
      emitRoutineBody(R);
    for (RoutineDecl *R : AsyncsToEmit)
      emitAsyncParts(R);
    AsyncsToEmit.clear();
    emitCleanups();

    CurFn = Init;

    AddrAligns.clear();
    CurRoutine = nullptr;
    CurFrame = nullptr;
    CurPin = nullptr;
    PinTemps = true;
    CurSP = DIB ? createSubprogram(Init, U.getName().Spelling, DIUnitFile,
                                   lineOf(U.getLocation()), nullptr, {})
                : nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Init));
    setDebugLoc(U.getLocation());
    initFileVars(U.getInterface()->getDecls(), nullptr);
    initFileVars(U.getImplementation()->getDecls(), nullptr);
    emitClassInits(U.getInterface()->getDecls(),
                   U.getImplementation()->getDecls());
    ExitBlock = newBlock("exit");
    visitStmt(U.getImplementation()->getBody());
    B.CreateBr(ExitBlock);
    B.SetInsertPoint(ExitBlock);
    B.CreateRetVoid();
    emitDynOps();
    markDefinedFunctionsNounwind();
    if (DIB)
      DIB->finalize();
    if (!CGOpts.KeepUnused)
      removeUnreachable();
  }

  /// Statements carry their source position into the debug info.
  void visitStmt(Stmt *S) {
    setDebugLoc(S->getBeginLoc());
    size_t Mark = LiveTemps.size(), StringMark = StringTemps.size();
    StmtVisitor<CodeGenModule>::visitStmt(S);
    releaseTemps(Mark); // the results held by its expressions
    releaseStringTemps(StringMark);
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *) {}

  void visitCompoundStmt(CompoundStmt *S) { emitSequence(S->getBody()); }

  /// The statements of a sequence. After one that declares owned
  /// variables, the rest is the protected part of a try-finally that frees
  /// them: their scope ends with the sequence, however it is left.
  void emitSequence(llvm::ArrayRef<Stmt *> Body) {
    for (size_t I = 0; I < Body.size(); ++I) {
      visitStmt(Body[I]);
      auto *D = llvm::dyn_cast<VarDeclStmt>(Body[I]);
      if (!D)
        continue;
      llvm::SmallVector<VarDecl *, 2> Owned;
      for (VarDecl *V : D->getVars())
        if (V->isOwned())
          Owned.push_back(V);
      if (Owned.empty())
        continue;
      emitTryFinally([&] { emitSequence(Body.drop_front(I + 1)); },
                     [&](llvm::Value *State) { emitFreeOwned(Owned, State); });
      return;
    }
  }

  /// var x: T [:= e]: x is set each time the statement runs, to e or to
  /// zero (what it held, in an earlier iteration, is released).
  void visitVarDeclStmt(VarDeclStmt *S) {
    if (AssignStmt *A = S->getInitAssign()) {
      visitStmt(A);
      return;
    }
    for (VarDecl *V : S->getVars()) {
      Type *T = V->getType();
      llvm::Value *Addr = varAddress(V);
      if (V->isOwned()) {
        B.CreateCall(classRuntime("__pas_owned_set", B.getVoidTy(),
                                  {B.getPtrTy(), B.getPtrTy()}),
                     {Addr, llvm::ConstantPointerNull::get(B.getPtrTy())});
        continue;
      }
      if (containsFile(T))
        continue; // (opened when its routine starts)
      if (isManaged(T))
        finalizeValue(Addr, T);
      emitZeroInit(Addr, T);
    }
  }

  /// If Value is Target + x, for a short string variable Target named in
  /// both places: x.
  static Expr *appendedTo(Expr *Target, Expr *Value) {
    auto *T = llvm::dyn_cast<NameRefExpr>(Target);
    auto *Sum = llvm::dyn_cast<BinaryExpr>(Value);
    if (!T || !Sum || Sum->getOp() != BinaryOp::Add || Sum->getOperator() ||
        !Sum->getType()->isShortString())
      return nullptr;
    auto *L = llvm::dyn_cast<NameRefExpr>(Sum->getLHS());
    if (!L || !llvm::isa_and_nonnull<VarDecl>(T->getDecl()) ||
        L->getDecl() != T->getDecl() || T->getWith() || L->getWith())
      return nullptr;
    return Sum->getRHS();
  }

  static Expr *skipParens(Expr *E) {
    while (auto *P = llvm::dyn_cast<ParenExpr>(E))
      E = P->getInner();
    return E;
  }

  static bool isTargetName(Expr *E, VarDecl *V) {
    auto *N = llvm::dyn_cast<NameRefExpr>(skipParens(E));
    return N && N->getDecl() == V && !N->getWith();
  }

  /// Is E, after parentheses and ansistring casts, Target itself?
  static bool isDirectTargetRef(Expr *E, VarDecl *V) {
    E = skipParens(E);
    if (auto *C = llvm::dyn_cast<CallExpr>(E);
        C && E->getType()->isAnsiString() && C->getArgs().size() == 1 &&
        (C->isVarCast() || llvm::isa_and_nonnull<TypeDecl>(C->getDecl())))
      return isDirectTargetRef(C->getArgs()[0], V);
    return isTargetName(E, V);
  }

  /// The right operands of a left-nested ansistring addition whose leftmost
  /// operand is V: s + a + b gives {a, b}.
  static bool ansiAppends(Expr *Value, VarDecl *V,
                          llvm::SmallVectorImpl<Expr *> &Parts) {
    auto *Sum = llvm::dyn_cast<BinaryExpr>(Value);
    if (!Sum || Sum->getOp() != BinaryOp::Add || Sum->getOperator() ||
        !Value->getType()->isAnsiString())
      return false;
    if (!isTargetName(Sum->getLHS(), V) &&
        !ansiAppends(Sum->getLHS(), V, Parts))
      return false;
    Parts.push_back(Sum->getRHS());
    return true;
  }

  /// A direct call of a function (or through a procedural value) whose
  /// result is an ansistring: the reference it returns is new, so an
  /// assignment can move it instead of counting it and releasing it later.
  static bool isFreshAnsiCall(Expr *Value) {
    if (!Value->getType()->isAnsiString())
      return false;
    if (llvm::isa<InheritedExpr>(Value) ||
        llvm::isa<IndirectCallExpr>(Value))
      return true;
    auto *C = llvm::dyn_cast<CallExpr>(Value);
    if (!C || C->isVarCast() || C->isMethodCast())
      return false;
    if (C->getCalleeValue())
      return true;
    return llvm::isa_and_nonnull<RoutineDecl>(C->getDecl()) ||
           llvm::isa_and_nonnull<VarDecl>(C->getDecl());
  }

  void visitAssignStmt(AssignStmt *S) {
    Expr *Target = S->getTarget(), *Value = S->getValue();
    Type *TT = Target->getType();
    if (PropertyDecl *P = assignedProperty(Target)) {
      emitPropertyWrite(P, Target, Value);
      return;
    }
    if (auto *SS = llvm::dyn_cast<ShortStringType>(TT)) {
      llvm::Value *Dst = emitAddress(Target);
      if (Expr *More = appendedTo(Target, Value)) {
        // s := s + x: x goes to the end of s, without a temporary.
        llvm::Value *Src = emitPStr(More);
        B.CreateCall(classRuntime("__pas_str_append", B.getVoidTy(),
                                  {B.getPtrTy(), B.getInt32Ty(), B.getPtrTy()}),
                     {Dst, B.getInt32(SS->getMaxLength()), Src});
        return;
      }
      emitStringAssign(Dst, SS, Value);
      return;
    }
    if (TT->isAnsiString()) {
      VarDecl *TV = nullptr;
      if (auto *N = llvm::dyn_cast<NameRefExpr>(Target); N && !N->getWith())
        TV = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
      llvm::SmallVector<Expr *, 4> Parts;
      if (TV && ansiAppends(Value, TV, Parts)) {
        if (Parts.size() == 1) {
          // s := s + x: x goes to the end of s, in place when s is unique.
          llvm::Value *Dst = emitAddress(Target);
          llvm::Value *Src = emitAnsi(Parts[0]);
          astrCall("__pas_astr_append", B.getVoidTy(), {Dst, Src});
          return;
        }
        // s + a + b ...: evaluate every part before changing s, then append
        // them in order. (A bare s among the parts would read it after the
        // first append; keep the general concatenation for that.)
        bool Safe = true;
        for (Expr *P : Parts)
          if (isDirectTargetRef(P, TV)) {
            Safe = false;
            break;
          }
        if (Safe) {
          llvm::Value *Dst = emitAddress(Target);
          llvm::SmallVector<llvm::Value *, 4> Values;
          for (Expr *P : Parts)
            Values.push_back(emitAnsi(P));
          for (llvm::Value *V : Values)
            astrCall("__pas_astr_append", B.getVoidTy(), {Dst, V});
          return;
        }
      }
      bool Take = isFreshAnsiCall(Value) && llvm::isa<NameRefExpr>(Target);
      if (Take) {
        NoHoldAnsiResult = true;
        TookAnsiResult = false;
      }
      llvm::Value *V = emitAnsi(Value);
      if (Take) {
        NoHoldAnsiResult = false;
        Take = TookAnsiResult;
      }
      llvm::Value *Dst = emitAddress(Target);
      if (Take) {
        astrCall("__pas_astr_take", B.getVoidTy(), {Dst, V});
        return;
      }
      storeValue(Dst, V, TT);
      return;
    }
    if (TT->isStructured()) {
      llvm::Value *Dst = emitAddress(Target);
      copyValue(Dst, emitAddress(Value), TT);
      return;
    }
    if (llvm::isa<RoutineType>(TT)) { // procedural variable (Turbo/FPC)
      llvm::Value *Pair = emitRoutinePair(Value, TT);
      storeValue(emitAddress(Target), Pair, TT); // (a closure: counted)
      return;
    }
    llvm::Value *V = convertForAssign(visitExpr(Value), Value->getType(), TT,
                                      Value->getBeginLoc());
    if (isOwnedVar(Target)) { // frees the object it had
      llvm::Value *Slot = emitAddress(Target);
      B.CreateCall(classRuntime("__pas_owned_set", B.getVoidTy(),
                                {B.getPtrTy(), B.getPtrTy()}),
                   {Slot, V});
      return;
    }
    if (isBitPackedComponent(Target)) {
      bitStore(Target, V);
      return;
    }
    storeValue(emitAddress(Target), V, TT);
  }

  /// Is E an owned variable (`x: TFoo owned`)?
  static bool isOwnedVar(Expr *E) {
    auto *N = llvm::dyn_cast<NameRefExpr>(E);
    auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
    return V && V->isOwned();
  }

  static PropertyDecl *assignedProperty(Expr *Target) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target))
      return llvm::dyn_cast_or_null<PropertyDecl>(N->getDecl());
    if (auto *F = llvm::dyn_cast<FieldExpr>(Target))
      return llvm::dyn_cast_or_null<PropertyDecl>(F->getMember());
    if (auto *I = llvm::dyn_cast<IndexExpr>(Target))
      return I->getProperty();
    return nullptr;
  }

  /// Target := Value for a property Target: stores into its field or
  /// calls its write procedure.
  void emitPropertyWrite(PropertyDecl *P, Expr *Target, Expr *Value) {
    llvm::Value *Obj = nullptr;
    Type *C = nullptr;
    if (P->isClassProperty()) { // a class variable or a static method
      if (auto *R = llvm::dyn_cast<RoutineDecl>(P->getWrite())) {
        auto *I = llvm::dyn_cast<IndexExpr>(Target);
        emitCall(R, accessorArgs(P, I ? I->getPropertyArgs()
                                      : llvm::ArrayRef<Expr *>(),
                                 Value));
        return;
      }
      storeProperty(P, Globals[llvm::cast<VarDecl>(P->getWrite())], Value);
      return;
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(Target)) {
      Obj = propertyObject(I, C);
    } else if (auto *N = llvm::dyn_cast<NameRefExpr>(Target)) {
      Obj = withBase(N->getWith());
      C = N->getWith()->Record->getType();
    } else {
      auto *F = llvm::cast<FieldExpr>(Target);
      if (F->getBase()->getType()->isInterface()) { // its setter
        auto *R = llvm::cast<RoutineDecl>(P->getWrite());
        emitInterfaceCall(R, F->getBase(), F->getField().Loc,
                          accessorArgs(P, {}, Value));
        return;
      }
      Obj = memberObject(F->getBase());
      C = F->getBase()->getType();
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(P->getWrite())) {
      auto *I = llvm::dyn_cast<IndexExpr>(Target);
      emitMethodCall(R, Obj,
                     accessorArgs(P, I ? I->getPropertyArgs()
                                       : llvm::ArrayRef<Expr *>(),
                                  Value),
                     /*Inherited=*/false, Target->getBeginLoc());
      return;
    }
    storeProperty(P, fieldAddress(Obj, C, llvm::cast<FieldDecl>(P->getWrite())),
                  Value);
  }

  /// Value stored at Addr, property P's field or class variable.
  void storeProperty(PropertyDecl *P, llvm::Value *Addr, Expr *Value) {
    storeExpr(Addr, P->getType(), Value);
  }

  /// (a, b): its hidden variable, each element assigned its value.
  llvm::Value *emitTuple(TupleExpr *E) {
    auto *R = llvm::cast<RecordType>(E->getType());
    llvm::Value *Addr = varAddress(E->getTemp());
    for (size_t I = 0; I < E->getElements().size(); ++I) {
      FieldDecl *F = R->getAllFields()[I];
      storeExpr(fieldAddress(Addr, R, F), F->getType(), E->getElements()[I]);
    }
    return Addr;
  }

  void visitDestructureStmt(DestructureStmt *S) {
    if (S->getInit())
      visitStmt(S->getInit());
    else if (!S->getAssigns().empty())
      emitAddress(S->getValue());
    for (AssignStmt *A : S->getAssigns())
      visitStmt(A);
  }

  /// Value, assignable to type T, stored at Addr (a variable of type T).
  void storeExpr(llvm::Value *Addr, Type *T, Expr *Value) {
    if (auto *SS = llvm::dyn_cast<ShortStringType>(T)) {
      emitStringAssign(Addr, SS, Value);
      return;
    }
    if (T->isAnsiString()) {
      storeValue(Addr, emitAnsi(Value), T);
      return;
    }
    if (T->isStructured()) {
      copyValue(Addr, emitAddress(Value), T);
      return;
    }
    if (llvm::isa<RoutineType>(T)) {
      storeValue(Addr, emitRoutinePair(Value, T), T); // (a closure: counted)
      return;
    }
    storeValue(Addr,
               convertForAssign(visitExpr(Value), Value->getType(), T,
                                Value->getBeginLoc()),
               T);
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    if (Expr *V = S->getCalleeValue()) { // a procedural field or property
      emitCallThrough(visitExpr(V), llvm::cast<RoutineType>(V->getType()),
                      S->getArgs(), S->getBeginLoc());
      return;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(S->getDecl())) {
      if (R->isStatic()) // a record's static method
        emitCall(R, S->getArgs());
      else if (R->isMethod()) // through Self or a with statement's object
        emitMethodCall(R, withBase(S->getWith()), S->getArgs(),
                       /*Inherited=*/false, S->getBeginLoc(),
                       withIsClass(S->getWith()));
      else
        emitCall(R, S->getArgs());
      return;
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(S->getDecl())) {
      emitIndirectCall(V, S->getArgs());
      return;
    }
    auto *BD = llvm::cast<BuiltinDecl>(S->getDecl());
    if (BD->getBuiltinKind() == BuiltinKind::Wait) { // Wait(t);
      emitAwait(visitExpr(S->getArgs()[0]),
                llvm::cast<TaskType>(S->getArgs()[0]->getType()));
      return;
    }
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Write:
    case BuiltinKind::Writeln:
      emitWrite(S, BD->getBuiltinKind() == BuiltinKind::Writeln);
      return;
    case BuiltinKind::Read:
    case BuiltinKind::Readln:
      emitRead(S, BD->getBuiltinKind() == BuiltinKind::Readln);
      return;
    case BuiltinKind::Reset: case BuiltinKind::Rewrite: {
      // An untyped file's: with its record size (128 if not given).
      auto *FT = llvm::cast<FileType>(S->getArgs()[0]->getType());
      if (!FT->isUntyped())
        goto plain_file_proc;
      llvm::Value *File = loadFile(emitAddress(S->getArgs()[0]));
      llvm::Value *Size = S->getArgs().size() == 2
                              ? toI64(visitExpr(S->getArgs()[1]),
                                      S->getArgs()[1]->getType())
                              : B.getInt64(128);
      B.CreateCall(fileRuntime(BD->getBuiltinKind() == BuiltinKind::Reset
                                   ? "__pas_reset_size"
                                   : "__pas_rewrite_size",
                               B.getVoidTy(), {B.getPtrTy(), B.getInt64Ty()}),
                   {File, Size});
      return;
    }
    case BuiltinKind::Close: case BuiltinKind::CloseFile:
      B.CreateCall(fileRuntime("__pas_close", B.getVoidTy(), {B.getPtrTy()}),
                   {loadFile(emitAddress(S->getArgs()[0]))});
      return;
    case BuiltinKind::Flush:
      B.CreateCall(fileRuntime("__pas_flush", B.getVoidTy(), {B.getPtrTy()}),
                   {loadFile(emitAddress(S->getArgs()[0]))});
      return;
    case BuiltinKind::Assign: case BuiltinKind::AssignFile: {
      llvm::Value *File = loadFile(emitAddress(S->getArgs()[0]));
      llvm::Value *Name = emitAnsi(S->getArgs()[1]);
      B.CreateCall(fileRuntime("__pas_assign", B.getVoidTy(),
                               {B.getPtrTy(), B.getPtrTy()}),
                   {File, Name});
      return;
    }
    case BuiltinKind::BlockRead: case BuiltinKind::BlockWrite: {
      auto Args = S->getArgs();
      bool Read = BD->getBuiltinKind() == BuiltinKind::BlockRead;
      llvm::Value *File = loadFile(emitAddress(Args[0]));
      llvm::Value *Buf = emitAddress(Args[1]);
      llvm::Value *Count = toI64(visitExpr(Args[2]), Args[2]->getType());
      llvm::Value *Done = B.CreateCall(
          fileRuntime(Read ? "__pas_blockread" : "__pas_blockwrite",
                      B.getInt64Ty(),
                      {B.getPtrTy(), B.getPtrTy(), B.getInt64Ty(),
                       B.getInt32Ty()}),
          {File, Buf, Count, B.getInt32(Args.size() == 3)});
      if (Args.size() == 4) // the records done
        storeScalar(emitAddress(Args[3]),
                    B.CreateSExtOrTrunc(Done, regType(Args[3]->getType())),
                    Args[3]->getType());
      return;
    }
    case BuiltinKind::Seek: {
      llvm::Value *File = loadFile(emitAddress(S->getArgs()[0]));
      llvm::Value *N = toI64(visitExpr(S->getArgs()[1]), S->getArgs()[1]->getType());
      B.CreateCall(fileRuntime("__pas_seek", B.getVoidTy(),
                               {B.getPtrTy(), B.getInt64Ty()}),
                   {File, N});
      return;
    }
    case BuiltinKind::Get: case BuiltinKind::Put: case BuiltinKind::Page:
    plain_file_proc: {
      llvm::FunctionCallee Fn;
      switch (BD->getBuiltinKind()) {
      case BuiltinKind::Reset:   Fn = RT.Reset; break;
      case BuiltinKind::Rewrite: Fn = RT.Rewrite; break;
      case BuiltinKind::Get:     Fn = RT.Get; break;
      case BuiltinKind::Put:     Fn = RT.Put; break;
      default:                   Fn = RT.Page; break;
      }
      llvm::Value *File = S->getArgs().empty()
                              ? outputFile()
                              : loadFile(emitAddress(S->getArgs()[0]));
      B.CreateCall(Fn, {File});
      return;
    }
    case BuiltinKind::Pack:
    case BuiltinKind::Unpack:
      emitPackUnpack(S, BD->getBuiltinKind() == BuiltinKind::Pack);
      return;
    case BuiltinKind::Break:
    case BuiltinKind::Continue:
    case BuiltinKind::Exit: {
      if (!S->getArgs().empty()) { // exit(value): set the result first
        VarDecl *Result = CurRoutine->getResultVar();
        Expr *Value = S->getArgs()[0];
        Type *RT = Result->getType();
        llvm::Value *Addr = varAddress(Result);
        if (auto *SS = llvm::dyn_cast<ShortStringType>(RT))
          emitStringAssign(Addr, SS, Value);
        else if (RT->isAnsiString())
          storeValue(Addr, emitAnsi(Value), RT);
        else if (isMethodPointerType(RT) || RT->isClosureType())
          storeValue(Addr, emitRoutinePair(Value, RT), RT);
        else if (RT->isStructured())
          copyValue(Addr, emitAddress(Value), RT);
        else
          storeValue(Addr,
                     convertForAssign(visitExpr(Value), Value->getType(), RT,
                                      Value->getBeginLoc()),
                     RT);
      }
      if (BD->getBuiltinKind() == BuiltinKind::Exit)
        emitJump(ExitBlock, 0);
      else if (BD->getBuiltinKind() == BuiltinKind::Continue)
        emitJump(Loops.back().Continue, Loops.back().Cleanups);
      else
        emitJump(Loops.back().Break, Loops.back().Cleanups);
      B.SetInsertPoint(newBlock("after.jump")); // unreachable code follows
      return;
    }
    case BuiltinKind::SetLength: {
      auto A = S->getArgs();
      llvm::Value *Addr = emitAddress(A[0]);
      if (auto *DA = llvm::dyn_cast<DynArrayType>(A[0]->getType())) {
        llvm::SmallVector<llvm::Value *, 2> Ns;
        for (Expr *N : A.drop_front())
          Ns.push_back(toI64(visitExpr(N), N->getType()));
        emitDynSetLength(Addr, DA, Ns);
        return;
      }
      llvm::Value *N = emitI32(A[1]);
      if (A[0]->getType()->isAnsiString())
        astrCall("__pas_astr_setlength", B.getVoidTy(), {Addr, N});
      else
        astrCall("__pas_str_setlength", B.getVoidTy(),
                 {Addr,
                  B.getInt32(llvm::cast<ShortStringType>(A[0]->getType())->getMaxLength()),
                  N});
      return;
    }
    case BuiltinKind::Delete: {
      auto A = S->getArgs();
      if (A[0]->getType()->isAnsiString()) {
        llvm::Value *Addr = emitAddress(A[0]);
        llvm::Value *Index = emitI32(A[1]);
        llvm::Value *Count = emitI32(A[2]);
        astrCall("__pas_astr_delete", B.getVoidTy(), {Addr, Index, Count});
        return;
      }
      B.CreateCall(RT.StrDelete, {emitAddress(A[0]), emitI32(A[1]), emitI32(A[2])});
      return;
    }
    case BuiltinKind::Insert: {
      auto A = S->getArgs();
      if (A[1]->getType()->isAnsiString()) {
        llvm::Value *Src = emitAnsi(A[0]);
        llvm::Value *Addr = emitAddress(A[1]);
        llvm::Value *Index = emitI32(A[2]);
        astrCall("__pas_astr_insert", B.getVoidTy(), {Src, Addr, Index});
        return;
      }
      auto *SS = llvm::cast<ShortStringType>(A[1]->getType());
      B.CreateCall(RT.StrInsert, {emitPStr(A[0]), emitAddress(A[1]),
                                  B.getInt32(SS->getMaxLength()), emitI32(A[2])});
      return;
    }
    case BuiltinKind::Inc:
    case BuiltinKind::Dec: {
      Expr *Var = S->getArgs()[0];
      Type *T = Var->getType();
      llvm::Value *Addr = emitAddress(Var);
      llvm::Value *Old = loadScalar(Addr, T);
      llvm::Value *Step =
          S->getArgs().size() == 2
              ? toI64(visitExpr(S->getArgs()[1]), S->getArgs()[1]->getType())
              : B.getInt64(1);
      bool IsInc = BD->getBuiltinKind() == BuiltinKind::Inc;
      SourceLocation L = S->getBeginLoc();
      if (auto *PT = llvm::dyn_cast<PointerType>(T)) { // by n of its pointee
        llvm::Value *Delta = IsInc ? Step : B.CreateNeg(Step);
        B.CreateStore(B.CreateGEP(pointeeMemType(PT), Old, Delta), Addr);
        return;
      }
      if (T->isIntegerLike() && intBits(T) == 64) { // no wider type to check in
        // (qword: unsigned)
        llvm::Value *New = emitCheckedArith(IsInc ? BinaryOp::Add : BinaryOp::Sub,
                                            isSignedOrdinal(T), Old, Step, L,
                                            S->getCallee().Name);
        emitRangeCheck(New, T->getBase(), T, L);
        storeScalar(Addr, New, T);
        return;
      }
      // Compute in i64 so the range check sees the true result.
      llvm::Value *Old64 = toI64(Old, T);
      llvm::Value *Delta = IsInc ? Step : B.CreateNeg(Step);
      llvm::Value *Wide = B.CreateAdd(Old64, Delta);
      if (T->isIntegerLike()
              ? overflowChecks(L) ||
                    (rangeChecks(L) && !(wraps(L) && llvm::isa<IntegerType>(T)))
              : rangeChecks(L)) {
        OrdinalBounds Bd = getOrdinalBounds(T);
        if (T->isIntegerLike())
          emitOverflowCheck(outsideRange(Wide, Bd.Lo, Bd.Hi),
                            "integer overflow in '" + S->getCallee().Name.str() + "'",
                            L);
        else
          emitRuntimeCheck(outsideRange(Wide, Bd.Lo, Bd.Hi), "value out of range", L);
      }
      storeScalar(Addr, B.CreateTrunc(Wide, regType(T)), T);
      return;
    }
    case BuiltinKind::GetMem: {
      auto A = S->getArgs();
      llvm::Value *Mem =
          B.CreateCall(RT.New, {toI64(visitExpr(A[1]), A[1]->getType())});
      B.CreateStore(Mem, emitAddress(A[0]));
      return;
    }
    case BuiltinKind::FreeMem:
      B.CreateCall(RT.Free, {visitExpr(S->getArgs()[0])});
      return;
    case BuiltinKind::ReallocMem: {
      auto A = S->getArgs();
      llvm::Value *Addr = emitAddress(A[0]);
      llvm::Value *Size = toI64(visitExpr(A[1]), A[1]->getType());
      llvm::Value *Old = B.CreateLoad(B.getPtrTy(), Addr);
      B.CreateStore(B.CreateCall(RT.Realloc, {Old, Size}), Addr);
      return;
    }
    case BuiltinKind::FillChar:
    case BuiltinKind::Move: {
      // A negative count does nothing.
      auto A = S->getArgs();
      bool IsFill = BD->getBuiltinKind() == BuiltinKind::FillChar;
      llvm::Value *Dst = emitAddress(A[IsFill ? 0 : 1]);
      Expr *CountE = A[IsFill ? 1 : 2];
      llvm::Value *Count = toI64(visitExpr(CountE), CountE->getType());
      Count = B.CreateSelect(B.CreateICmpSLT(Count, B.getInt64(0)),
                             B.getInt64(0), Count);
      if (IsFill) {
        llvm::Value *V = B.CreateTrunc(toI64(visitExpr(A[2]), A[2]->getType()),
                                       B.getInt8Ty());
        B.CreateMemSet(Dst, V, Count, llvm::MaybeAlign(1));
      } else {
        B.CreateMemMove(Dst, llvm::MaybeAlign(1), emitAddress(A[0]),
                        llvm::MaybeAlign(1), Count);
      }
      return;
    }
    case BuiltinKind::Halt: {
      llvm::Value *Code =
          S->getArgs().empty() ? B.getInt32(0) : emitI32(S->getArgs()[0]);
      B.CreateCall(RT.Halt, {Code});
      B.CreateUnreachable();
      B.SetInsertPoint(newBlock("after.halt")); // unreachable code follows
      return;
    }
    case BuiltinKind::New: {
      Expr *Arg = S->getArgs()[0];
      Type *Pointee = llvm::cast<PointerType>(Arg->getType())->getPointee();
      llvm::Value *Mem = B.CreateCall(RT.New, {B.getInt64(sizeOf(Pointee))});
      emitFileLifetime(Mem, Pointee, /*Init=*/true);
      B.CreateStore(Mem, emitAddress(Arg));
      return;
    }
    case BuiltinKind::Dispose: {
      Expr *Arg = S->getArgs()[0];
      llvm::Value *P = visitExpr(Arg);
      Type *Pointee = llvm::cast<PointerType>(Arg->getType())->getPointee();
      if (containsFile(Pointee)) {
        if (rangeChecks(Arg->getBeginLoc()))
          emitRuntimeCheck(B.CreateIsNull(P), "'dispose' of a nil pointer",
                           Arg->getBeginLoc());
        emitFileLifetime(P, Pointee, /*Init=*/false);
      }
      if (isManaged(Pointee)) {
        if (rangeChecks(Arg->getBeginLoc()))
          emitRuntimeCheck(B.CreateIsNull(P), "'dispose' of a nil pointer",
                           Arg->getBeginLoc());
        finalizeValue(P, Pointee);
      }
      B.CreateCall(RT.Dispose, {P});
      return;
    }
    default:
      llvm_unreachable("Sema only allows procedures here");
    }
  }

  void visitIndirectCallStmt(IndirectCallStmt *S) {
    visitIndirectCallExpr(S->getCall());
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
    Loops.push_back({CondBB, EndBB, Cleanups.size()});
    visitStmt(S->getBody());
    Loops.pop_back();
    B.CreateBr(CondBB);

    B.SetInsertPoint(EndBB);
  }

  void visitRepeatStmt(RepeatStmt *S) {
    auto *BodyBB = newBlock("repeat.body");
    auto *CondBB = newBlock("repeat.cond");
    auto *EndBB = newBlock("repeat.end");
    B.CreateBr(BodyBB);

    B.SetInsertPoint(BodyBB);
    Loops.push_back({CondBB, EndBB, Cleanups.size()});
    emitSequence(S->getBody());
    Loops.pop_back();
    B.CreateBr(CondBB);
    B.SetInsertPoint(CondBB);
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
    // The bounds may have other integer types than the variable: compare
    // them as i64, check them, then convert them to the variable's type.
    llvm::Value *Start64 = toI64(Start, S->getInit()->getType());
    llvm::Value *Final64 = toI64(Final, S->getFinal()->getType());
    // (a qword variable: its bounds compare unsigned)
    llvm::Value *Enter =
        isQWord(VarTy)
            ? (Down ? B.CreateICmpUGE(Start64, Final64)
                    : B.CreateICmpULE(Start64, Final64))
            : (Down ? B.CreateICmpSGE(Start64, Final64)
                    : B.CreateICmpSLE(Start64, Final64));
    (void)Signed;
    auto *CheckBB = newBlock("for.check");
    auto *LoopBB = newBlock("for.body");
    auto *LatchBB = newBlock("for.latch");
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
    Start = convertOrdinal(Start, S->getInit()->getType(), VarTy);
    Final = convertOrdinal(Final, S->getFinal()->getType(), VarTy);
    llvm::BasicBlock *PreBB = B.GetInsertBlock();
    B.CreateBr(LoopBB);

    B.SetInsertPoint(LoopBB);
    llvm::PHINode *I = B.CreatePHI(Start->getType(), 2, V->getName().Name);
    I->addIncoming(Start, PreBB);
    storeScalar(varAddress(V), I, VarTy);
    auto OldRange = VarRanges.find(V);
    bool HadRange = OldRange != VarRanges.end();
    ValueRange Saved = HadRange ? OldRange->second : ValueRange{};
    ValueRange StartRange = rangeOfValue(Start), FinalRange = rangeOfValue(Final);
    if (isSignedOrdinal(VarTy) && StartRange.Known && FinalRange.Known)
      VarRanges[V] = {true, std::min(StartRange.Lo, FinalRange.Lo),
                      std::max(StartRange.Hi, FinalRange.Hi)};
    else
      VarRanges.erase(V);
    Loops.push_back({LatchBB, EndBB, Cleanups.size()});
    visitStmt(S->getBody());
    Loops.pop_back();
    if (HadRange)
      VarRanges[V] = Saved;
    else
      VarRanges.erase(V);
    B.CreateBr(LatchBB);
    B.SetInsertPoint(LatchBB);
    B.CreateCondBr(B.CreateICmpEQ(I, Final, "for.done"), EndBB, IncBB);

    B.SetInsertPoint(IncBB);
    llvm::Value *One = llvm::ConstantInt::get(I->getType(), 1);
    llvm::Value *Next = Down ? B.CreateSub(I, One, "for.next")
                             : B.CreateAdd(I, One, "for.next");
    I->addIncoming(Next, IncBB);
    B.CreateBr(LoopBB);

    B.SetInsertPoint(EndBB);
  }

  //   [hold the collection] lo, hi = its first and last index (i64)
  //   if lo > hi goto end
  // loop:
  //   i = phi [lo, pre], [next, inc]
  //   [bind the element variable to element i]; Var := element; Body
  // latch:                                    (continue)
  //   if i == hi goto end                     (test before stepping)
  // inc:
  //   next = i + 1; goto loop
  // An enumerator: e := Coll.GetEnumerator; while e.MoveNext do begin
  // Var := e.Current; Body end, a class's e freed in a finally part.
  void visitForInStmt(ForInStmt *S) {
    Expr *C = S->getCollection();
    Type *Elem = S->getElementType();
    auto Iteration = [&](llvm::Value *ElemAddr) {
      if (VarDecl *EV = S->getElementVar())
        HandlerVars[EV] = ElemAddr;
      if (S->getStep())
        visitStmt(S->getStep());
      visitStmt(S->getBody());
    };
    auto Indexed = [&](llvm::Value *Lo, llvm::Value *Hi, llvm::Type *ElemTy,
                       llvm::Value *Base) {
      emitForInLoop(Lo, Hi, [&](llvm::Value *I, llvm::BasicBlock *) {
        Iteration(B.CreateInBoundsGEP(ElemTy, Base, I, "in.elem"));
      });
    };
    switch (S->getInKind()) {
    case ForInStmt::Invalid:
      return; // (errors were reported)
    case ForInStmt::Enumerator:
      emitForInEnumerator(S);
      return;
    case ForInStmt::Iterator: {
      // Held during the loop (and released after it, which stops it if it
      // has more values: its finally parts run).
      llvm::PointerType *Ptr = B.getPtrTy();
      llvm::Value *It = visitExpr(C);
      B.CreateCall(classRuntime("__pas_gen_addref", B.getVoidTy(), {Ptr}), {It});
      holdResult(It, C->getType());
      auto *CondBB = newBlock("forin.cond");
      auto *BodyBB = newBlock("forin.body");
      auto *EndBB = newBlock("forin.end");
      B.CreateBr(CondBB);
      B.SetInsertPoint(CondBB);
      B.CreateCondBr(emitIteratorNext(It), BodyBB, EndBB);
      B.SetInsertPoint(BodyBB);
      Loops.push_back({CondBB, EndBB, Cleanups.size()});
      Iteration(iteratorValueAddr(It));
      Loops.pop_back();
      B.CreateBr(CondBB);
      B.SetInsertPoint(EndBB);
      return;
    }
    case ForInStmt::Array: {
      auto *AT = llvm::cast<ArrayType>(C->getType());
      llvm::Value *Base = emitAddress(C);
      OrdinalBounds Bd = getOrdinalBounds(AT->getIndexType());
      if (AT->hasBitElements()) { // each element's bits, copied
        Type *ET = AT->getElementType();
        llvm::Value *Tmp = createTemp(memType(ET), "in.bits");
        emitForInLoop(B.getInt64(0), B.getInt64(Bd.Hi - Bd.Lo),
                      [&](llvm::Value *I, llvm::BasicBlock *) {
                        llvm::Value *Bit = B.CreateMul(
                            I, B.getInt64(bitPackedWidth(ET)), "bit");
                        storeScalar(Tmp, bitLoad({Base, Bit, 0, sizeOf(AT), ET}), ET);
                        Iteration(Tmp);
                      });
        return;
      }
      emitForInLoop(B.getInt64(0), B.getInt64(Bd.Hi - Bd.Lo),
                    [&](llvm::Value *I, llvm::BasicBlock *) {
                      Iteration(B.CreateInBoundsGEP(memType(AT), Base,
                                                    {B.getInt64(0), I}, "in.elem"));
                    });
      return;
    }
    case ForInStmt::OpenArray: {
      llvm::Value *Desc = openDescriptor(C);
      llvm::Value *Data = B.CreateLoad(B.getPtrTy(), Desc, "data");
      llvm::Value *Hi = B.CreateSub(openCount(Desc), B.getInt64(1), "in.hi");
      Indexed(B.getInt64(0), Hi, memType(Elem), Data);
      return;
    }
    case ForInStmt::DynArray:
    case ForInStmt::AnsiString: {
      // A reference is held during the loop: changing the variable does
      // not change the elements enumerated, as in Free Pascal.
      bool Dyn = S->getInKind() == ForInStmt::DynArray;
      llvm::Value *V = visitExpr(C);
      astrCall(Dyn ? "__pas_dyn_addref" : "__pas_astr_addref", B.getVoidTy(), {V});
      holdResult(V, C->getType());
      llvm::Value *N =
          Dyn ? dynLength(V)
              : B.CreateZExt(astrCall("__pas_astr_length", B.getInt32Ty(), {V}),
                             B.getInt64Ty());
      Indexed(B.getInt64(0), B.CreateSub(N, B.getInt64(1), "in.hi"),
              memType(Elem), V);
      return;
    }
    case ForInStmt::ShortString: {
      llvm::Value *Base = emitPStr(C); // its characters are 1..length
      llvm::Value *N = B.CreateZExt(B.CreateLoad(B.getInt8Ty(), Base, "length"),
                                    B.getInt64Ty());
      Indexed(B.getInt64(1), N, B.getInt8Ty(), Base);
      return;
    }
    case ForInStmt::StringConst:
      Indexed(B.getInt64(0),
              B.getInt64(static_cast<int64_t>(C->getType()->getStringLength()) - 1),
              B.getInt8Ty(), emitAddress(C));
      return;
    case ForInStmt::Set: {
      // The members in order: the set is evaluated once.
      llvm::Value *V = visitExpr(C);
      int64_t Lo = 0, Hi = -1; // [] has none
      if (Elem) {
        OrdinalBounds Bd = getOrdinalBounds(Elem);
        Lo = std::max<int64_t>(Bd.Lo, 0);
        Hi = std::min<int64_t>(Bd.Hi, setBits(C->getType()) - 1);
      }
      llvm::Value *Slot = Elem ? createTemp(memType(Elem), "in.elem") : nullptr;
      emitForInLoop(B.getInt64(Lo), B.getInt64(Hi),
                    [&](llvm::Value *I, llvm::BasicBlock *Latch) {
                      llvm::Value *Bit = B.CreateTrunc(
                          B.CreateLShr(V, B.CreateZExtOrTrunc(I, V->getType())),
                          B.getInt1Ty(), "in.member");
                      auto *Member = newBlock("forin.member");
                      B.CreateCondBr(Bit, Member, Latch);
                      B.SetInsertPoint(Member);
                      if (Slot)
                        storeScalar(Slot, B.CreateTrunc(I, regType(Elem)), Elem);
                      Iteration(Slot);
                    });
      return;
    }
    case ForInStmt::Ordinals: {
      OrdinalBounds Bd = getOrdinalBounds(Elem);
      llvm::Value *Slot = createTemp(memType(Elem), "in.elem");
      emitForInLoop(B.getInt64(Bd.Lo), B.getInt64(Bd.Hi),
                    [&](llvm::Value *I, llvm::BasicBlock *) {
                      storeScalar(Slot, B.CreateTrunc(I, regType(Elem)), Elem);
                      Iteration(Slot);
                    });
      return;
    }
    }
  }

  /// for I := Lo to Hi (i64, signed; nothing if Lo > Hi), with break and
  /// continue: Fn(I, latch) emits an iteration, which may branch to the
  /// latch to skip the rest of it.
  void emitForInLoop(
      llvm::Value *Lo, llvm::Value *Hi,
      llvm::function_ref<void(llvm::Value *, llvm::BasicBlock *)> Fn) {
    auto *LoopBB = newBlock("forin.body");
    auto *LatchBB = newBlock("forin.latch");
    auto *IncBB = newBlock("forin.inc");
    auto *EndBB = newBlock("forin.end");
    llvm::BasicBlock *PreBB = B.GetInsertBlock();
    B.CreateCondBr(B.CreateICmpSLE(Lo, Hi), LoopBB, EndBB);
    B.SetInsertPoint(LoopBB);
    llvm::PHINode *I = B.CreatePHI(B.getInt64Ty(), 2, "in.index");
    I->addIncoming(Lo, PreBB);
    Loops.push_back({LatchBB, EndBB, Cleanups.size()});
    Fn(I, LatchBB);
    Loops.pop_back();
    B.CreateBr(LatchBB);
    B.SetInsertPoint(LatchBB);
    B.CreateCondBr(B.CreateICmpEQ(I, Hi, "in.done"), EndBB, IncBB);
    B.SetInsertPoint(IncBB);
    I->addIncoming(B.CreateAdd(I, B.getInt64(1), "in.next"), IncBB);
    B.CreateBr(LoopBB);
    B.SetInsertPoint(EndBB);
  }

  void emitForInEnumerator(ForInStmt *S) {
    Expr *C = S->getCollection();
    Type *CT = C->getType();
    if (CT->isClass() || CT->isInterface()) {
      llvm::Value *Slot = createTemp(memType(CT), "in.coll");
      B.CreateStore(visitExpr(C), Slot);
      HandlerVars[S->getCollectionVar()] = Slot;
    } else { // a record: its methods get its address
      HandlerVars[S->getCollectionVar()] = emitAddress(C);
    }
    VarDecl *EnumV = S->getEnumeratorVar();
    Type *ET = EnumV->getType();
    if (!ET->isClass() && !ET->isInterface()) {
      HandlerVars[EnumV] = emitAddress(S->getGetEnumerator());
      emitEnumeratorLoop(S);
      return;
    }
    llvm::Value *Slot = createTemp(memType(ET), "in.enum");
    B.CreateStore(visitExpr(S->getGetEnumerator()), Slot);
    HandlerVars[EnumV] = Slot;
    if (!ET->isClass()) {
      emitEnumeratorLoop(S);
      return;
    }
    emitTryFinally([&] { emitEnumeratorLoop(S); }, [&](llvm::Value *) {
      emitFree(B.CreateLoad(B.getPtrTy(), Slot, "enum"));
    });
  }

  //   while e.MoveNext do begin Var := e.Current; Body end
  void emitEnumeratorLoop(ForInStmt *S) {
    auto *CondBB = newBlock("forin.cond");
    auto *BodyBB = newBlock("forin.body");
    auto *EndBB = newBlock("forin.end");
    B.CreateBr(CondBB);
    B.SetInsertPoint(CondBB);
    B.CreateCondBr(visitExpr(S->getMoveNext()), BodyBB, EndBB);
    B.SetInsertPoint(BodyBB);
    Loops.push_back({CondBB, EndBB, Cleanups.size()});
    if (S->getStep())
      visitStmt(S->getStep());
    visitStmt(S->getBody());
    Loops.pop_back();
    B.CreateBr(CondBB);
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
        // (the number of values less one; unsigned for qword's labels)
        uint64_t Span = static_cast<uint64_t>(L->HiValue) -
                        static_cast<uint64_t>(L->LoValue);
        if (Span >= static_cast<uint64_t>(MaxExpandedRange)) {
          LargeRanges.push_back({L->LoValue, L->HiValue, ArmBB});
          continue;
        }
        for (uint64_t K = 0; K <= Span; ++K)
          Switch->addCase(
              Const(static_cast<int64_t>(static_cast<uint64_t>(L->LoValue) + K)),
              ArmBB);
      }
      B.SetInsertPoint(ArmBB);
      visitStmt(Arm->getBody());
      B.CreateBr(EndBB);
    }

    B.SetInsertPoint(DefaultBB);
    for (const LargeRange &R : LargeRanges) {
      llvm::Value *AboveLo =
          Signed ? B.CreateICmpSGE(Sel, Const(R.Lo)) : B.CreateICmpUGE(Sel, Const(R.Lo));
      llvm::Value *BelowHi =
          Signed ? B.CreateICmpSLE(Sel, Const(R.Hi)) : B.CreateICmpULE(Sel, Const(R.Hi));
      llvm::Value *InRange = B.CreateAnd(AboveLo, BelowHi);
      auto *NextBB = newBlock("case.range");
      B.CreateCondBr(InRange, R.Target, NextBB);
      B.SetInsertPoint(NextBB);
    }
    if (S->hasElse()) {
      emitSequence(S->getElseBody());
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

  void visitLabeledStmt(LabeledStmt *S) {
    llvm::BasicBlock *BB = labelBlock(S->getDecl());
    B.CreateBr(BB);
    B.SetInsertPoint(BB);
    visitStmt(S->getBody());
  }

  void visitGotoStmt(GotoStmt *S) {
    LabelDecl *L = S->getDecl();
    if (L->getOwner() == CurRoutine) {
      B.CreateBr(labelBlock(L));
    } else {
      // Non-local: unwind to the owner's activation with _longjmp.
      RoutineDecl *Owner = L->getOwner();
      llvm::Value *Buf =
          Owner ? B.CreateStructGEP(FrameTypes[Owner], framePtrFor(Owner),
                                    JmpBufSlots[Owner])
                : static_cast<llvm::Value *>(MainJmpBuf);
      B.CreateCall(RT.LongJmp, {Buf, B.getInt32(L->getIndex())});
      B.CreateUnreachable();
    }
    B.SetInsertPoint(newBlock("after.goto")); // unreachable code follows
  }

  //===--------------------------------------------------------------------===//
  // Exceptions (objfpc)
  //===--------------------------------------------------------------------===//

  /// PasExcFrame (runtime/include/pascalrt.h): prev, object, where,
  /// address, jmp_buf.
  llvm::StructType *excFrameType() {
    return llvm::StructType::get(B.getPtrTy(), B.getPtrTy(), B.getPtrTy(),
                                 B.getPtrTy(), jmpBufType());
  }

  /// The runtime's exception entry points, declared when first used.
  llvm::FunctionCallee excRuntime(llvm::StringRef Name) {
    llvm::Type *Ptr = B.getPtrTy();
    llvm::Type *Void = B.getVoidTy();
    bool NoReturn = false;
    llvm::FunctionCallee F;
    if (Name == "__pas_raise_divzero" || Name == "__pas_raise_overflow") {
      F = classRuntime(Name, Void, {Ptr});
      llvm::cast<llvm::Function>(F.getCallee())->addFnAttr(llvm::Attribute::Cold);
      NoReturn = true;
    } else if (Name == "__pas_raise" || Name == "__pas_reraise_free") {
      F = classRuntime(Name, Void, {Ptr, Ptr});
      NoReturn = true;
    } else if (Name == "__pas_raise_at") {
      F = classRuntime(Name, Void, {Ptr, Ptr, Ptr});
      NoReturn = true;
    } else {
      // __pas_exc_pop, __pas_free_object, __pas_reraise
      F = classRuntime(Name, Void, {Ptr});
      NoReturn = Name == "__pas_reraise";
    }
    if (NoReturn)
      llvm::cast<llvm::Function>(F.getCallee())
          ->addFnAttr(llvm::Attribute::NoReturn);
    return F;
  }

  /// Pushes a new PasExcFrame and calls _setjmp on it: an exception raised
  /// to the frame continues at Landing, the protected code in a new block
  /// named BodyName, or in Body if given (the insertion point). Returns
  /// the frame.
  llvm::Value *emitProtect(llvm::BasicBlock *Landing, const llvm::Twine &BodyName,
                           llvm::BasicBlock *Body = nullptr) {
    llvm::Value *F = createTemp(excFrameType(), "excframe");
    B.CreateCall(excRuntime("__pas_exc_push"), {F});
    llvm::Value *R = B.CreateCall(
        RT.SetJmp, {B.CreateStructGEP(excFrameType(), F, 4)}, "jmp");
    if (!Body)
      Body = newBlock(BodyName);
    B.CreateCondBr(B.CreateICmpNE(R, B.getInt32(0)), Landing, Body);
    B.SetInsertPoint(Body);
    return F;
  }

  /// Branches to Target (exit, break, continue), leaving the Cleanups
  /// above Depth on the way.
  void emitJump(llvm::BasicBlock *Target, size_t Depth) {
    for (size_t I = Cleanups.size(); I > Depth; --I) {
      Cleanup &C = Cleanups[I - 1];
      B.CreateCall(excRuntime("__pas_exc_pop"), {C.Frame});
      if (C.K == Cleanup::Handler) {
        B.CreateCall(excRuntime("__pas_free_object"), {C.Object});
      } else if (C.K == Cleanup::Finally) {
        B.CreateStore(B.getInt32(2 + C.Exits.size()), C.State);
        auto *Cont = newBlock("finally.cont");
        C.Exits.push_back(Cont);
        B.CreateBr(C.FinallyBB);
        B.SetInsertPoint(Cont);
      }
    }
    B.CreateBr(Target);
  }

  /// Stage 8b3 can lower a catch-all try body made of ordinary statements.
  /// Nested try/scope/for-in bodies and raise still use the setjmp machinery
  /// (8d/8e/8b5), so mixing them under this flag is refused.
  bool tableSafeStmt(Stmt *S) {
    if (!S)
      return true;
    if (llvm::isa<TryStmt>(S) || llvm::isa<ScopeStmt>(S) ||
        llvm::isa<ForInStmt>(S) || llvm::isa<RaiseStmt>(S))
      return false;
    if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
      for (Stmt *B : C->getBody())
        if (!tableSafeStmt(B))
          return false;
      return true;
    }
    if (auto *I = llvm::dyn_cast<IfStmt>(S))
      return tableSafeStmt(I->getThen()) && tableSafeStmt(I->getElse());
    if (auto *W = llvm::dyn_cast<WhileStmt>(S))
      return tableSafeStmt(W->getBody());
    if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
      for (Stmt *B : R->getBody())
        if (!tableSafeStmt(B))
          return false;
      return true;
    }
    if (auto *F = llvm::dyn_cast<ForStmt>(S))
      return tableSafeStmt(F->getBody());
    if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
      for (CaseArm *A : C->getArms())
        if (!tableSafeStmt(A->getBody()))
          return false;
      for (Stmt *B : C->getElseBody())
        if (!tableSafeStmt(B))
          return false;
      return true;
    }
    if (auto *W = llvm::dyn_cast<WithStmt>(S))
      return tableSafeStmt(W->getBody());
    if (auto *L = llvm::dyn_cast<LabeledStmt>(S))
      return tableSafeStmt(L->getBody());
    return true;
  }

  bool tableTryCandidate(TryStmt *S) {
    if (S->isFinally() || !S->catchesAll())
      return false;
    for (Stmt *B : S->getBody())
      if (!tableSafeStmt(B))
        return false;
    return true;
  }

  void emitTableTryExcept(TryStmt *S) {
    auto *Landing = newBlock("try.landing");
    auto *End = newBlock("try.end");
    llvm::FunctionCallee Personality =
        classRuntime("__pas_personality", B.getInt32Ty(),
                     {B.getInt32Ty(), B.getInt32Ty(), B.getInt64Ty(),
                      B.getPtrTy(), B.getPtrTy()});
    CurFn->setPersonalityFn(
        llvm::cast<llvm::Constant>(Personality.getCallee()));
    llvm::BasicBlock *Saved = TableUnwind;
    TableUnwind = Landing;
    emitSequence(S->getBody());
    TableUnwind = Saved;
    B.CreateBr(End);

    B.SetInsertPoint(Landing);
    llvm::StructType *LPType =
        llvm::StructType::get(B.getPtrTy(), B.getInt32Ty());
    llvm::LandingPadInst *LP = B.CreateLandingPad(LPType, 0, "lp");
    LP->setCleanup(true);
    llvm::Value *Exc = B.CreateExtractValue(LP, 0, "unwind.exc");
    llvm::FunctionCallee Resume = M.getOrInsertFunction(
        "_Unwind_Resume", llvm::FunctionType::get(B.getVoidTy(),
                                                  {B.getPtrTy()}, false));
    llvm::cast<llvm::Function>(Resume.getCallee())
        ->addFnAttr(llvm::Attribute::NoReturn);
    B.CreateCall(Resume, {Exc});
    B.CreateUnreachable();
    B.SetInsertPoint(End);
  }

  void visitTryStmt(TryStmt *S) {
    if (CGOpts.TableExceptions && tableTryCandidate(S)) {
      emitTableTryExcept(S);
      return;
    }
    if (S->isFinally()) {
      emitTryFinally(S);
      return;
    }
    //   push F; if (_setjmp(F)) goto except;  body; pop F; goto end
    // except:
    //   exc = F.object; push H; if (_setjmp(H)) goto escape
    //   if (exc is C1) { E1 := exc; handler1; pop H; free exc; goto end } ...
    //   [else part; pop H; free exc; goto end]  or  pop H; reraise F
    // escape: (an exception left a handler) free exc unless it is the one
    //   raised; reraise it
    auto *Except = newBlock("try.except");
    auto *End = newBlock("try.end");
    llvm::Value *F = emitProtect(Except, "try.body");
    Cleanups.emplace_back(Cleanup::TryBody, F);
    emitSequence(S->getBody());
    Cleanups.pop_back();
    B.CreateCall(excRuntime("__pas_exc_pop"), {F});
    B.CreateBr(End);

    B.SetInsertPoint(Except);
    llvm::Value *Obj = B.CreateLoad(
        B.getPtrTy(), B.CreateStructGEP(excFrameType(), F, 1), "exc");
    auto *Escape = newBlock("handler.raise");
    llvm::Value *H = emitProtect(Escape, "handler.match");
    Cleanups.emplace_back(Cleanup::Handler, H, Obj);
    HandledFrames.push_back(F);
    auto Done = [&] { // a handler ran to its end
      B.CreateCall(excRuntime("__pas_exc_pop"), {H});
      B.CreateCall(excRuntime("__pas_free_object"), {Obj});
      B.CreateBr(End);
    };
    for (ExceptHandler *Handler : S->getHandlers()) {
      llvm::Value *HandlerVMT = B.CreateLoad(B.getPtrTy(), Obj, "vmt");
      llvm::Value *Is = classIsVMT(HandlerVMT, Handler->getClass());
      auto *Do = newBlock("handler.do");
      auto *Next = newBlock("handler.next");
      B.CreateCondBr(Is, Do, Next);
      B.SetInsertPoint(Do);
      if (VarDecl *V = Handler->getVar()) {
        llvm::Value *Slot = createTemp(B.getPtrTy(), V->getName().Name);
        B.CreateStore(Obj, Slot);
        HandlerVars[V] = Slot;
      }
      visitStmt(Handler->getBody());
      Done();
      B.SetInsertPoint(Next);
    }
    if (S->catchesAll()) {
      emitSequence(S->getExceptBody());
      Done();
    } else { // no handler matches: on to the next try statement
      B.CreateCall(excRuntime("__pas_exc_pop"), {H});
      B.CreateCall(excRuntime("__pas_reraise"), {F});
      B.CreateUnreachable();
    }
    HandledFrames.pop_back();
    Cleanups.pop_back();

    B.SetInsertPoint(Escape);
    B.CreateCall(excRuntime("__pas_reraise_free"), {H, Obj});
    B.CreateUnreachable();
    B.SetInsertPoint(End);
  }

  //   push F; if (_setjmp(F)) goto raised;  body; pop F; state = 0
  // finally:
  //   finally part
  //   switch (state) { 0: end; 1: reraise F; 2 + k: exit k (exit, break or
  //     continue out of the body) }
  // raised: state = 1; goto finally
  void emitTryFinally(TryStmt *S) {
    emitTryFinally(
        [&] { emitSequence(S->getBody()); },
        [&](llvm::Value *) { emitSequence(S->getFinallyBody()); });
  }

  /// The finally part of a routine with owned variables: frees their
  /// objects, the last declared first. A destructor's exception does not
  /// stop the others; the first one is raised afterwards, unless an
  /// exception is already leaving the routine (State 1), which goes on.
  void emitFreeOwned(llvm::ArrayRef<VarDecl *> Owned, llvm::Value *State) {
    llvm::Type *Ptr = B.getPtrTy();
    llvm::StructType *PendingTy = llvm::StructType::get(Ptr, Ptr, Ptr);
    llvm::Value *Pending = createTemp(PendingTy, "owned.exc");
    B.CreateStore(llvm::Constant::getNullValue(PendingTy), Pending);
    for (VarDecl *V : llvm::reverse(Owned))
      B.CreateCall(classRuntime("__pas_owned_free", B.getVoidTy(), {Ptr, Ptr}),
                   {varAddress(V), Pending});
    llvm::Value *Raised = B.CreateICmpEQ(
        B.CreateLoad(B.getInt32Ty(), State, "state"), B.getInt32(1), "raised");
    B.CreateCall(classRuntime("__pas_owned_done", B.getVoidTy(),
                              {Ptr, B.getInt32Ty()}),
                 {Pending, B.CreateZExt(Raised, B.getInt32Ty())});
  }

  /// FinallyPart is given the state's variable: 0 after the body's normal
  /// end, 1 after an exception, 2 + k after an exit, break or continue.
  void emitTryFinally(llvm::function_ref<void()> Body,
                      llvm::function_ref<void(llvm::Value *)> FinallyPart) {
    auto *Raised = newBlock("finally.raise");
    auto *Finally = newBlock("finally");
    auto *End = newBlock("try.end");
    llvm::Value *State = createTemp(B.getInt32Ty(), "finally.state");
    llvm::Value *F = emitProtect(Raised, "try.body");
    Cleanups.emplace_back(Cleanup::Finally, F, nullptr, State, Finally);
    Body();
    llvm::SmallVector<llvm::BasicBlock *, 2> Exits =
        std::move(Cleanups.back().Exits);
    Cleanups.pop_back();
    B.CreateCall(excRuntime("__pas_exc_pop"), {F});
    B.CreateStore(B.getInt32(0), State);
    B.CreateBr(Finally);

    B.SetInsertPoint(Raised);
    B.CreateStore(B.getInt32(1), State);
    B.CreateBr(Finally);

    B.SetInsertPoint(Finally);
    FinallyPart(State);
    auto *Reraise = newBlock("finally.reraise");
    llvm::SwitchInst *SW =
        B.CreateSwitch(B.CreateLoad(B.getInt32Ty(), State, "state"), End);
    SW->addCase(B.getInt32(1), Reraise);
    for (size_t K = 0; K < Exits.size(); ++K)
      SW->addCase(B.getInt32(2 + K), Exits[K]);
    B.SetInsertPoint(Reraise);
    B.CreateCall(excRuntime("__pas_reraise"), {F});
    B.CreateUnreachable();
    B.SetInsertPoint(End);
  }

  /// "file:line:col" for the runtime's exception functions, or null.
  llvm::Value *whereText(SourceLocation L) {
    std::string Where = where(L);
    if (Where.empty())
      return llvm::ConstantPointerNull::get(B.getPtrTy());
    return B.CreateGlobalString(Where.substr(4), ".where");
  }

  /// if (Cond) integer division by zero: a runtime error, or (objfpc) an
  /// EDivByZero exception, as in Free Pascal with SysUtils.
  void emitDivZeroCheck(llvm::Value *Cond, SourceLocation L) {
    if (!LangOpts.ObjKeywords) {
      emitRuntimeCheck(Cond, "division by zero", L);
      return;
    }
    if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(Cond); C && C->isZero())
      return; // provably fine
    auto *ErrBB = newBlock("rt.error");
    auto *ContBB = newBlock("rt.ok");
    llvm::MDBuilder MDB(M.getContext());
    B.CreateCondBr(Cond, ErrBB, ContBB, MDB.createUnlikelyBranchWeights());
    B.SetInsertPoint(ErrBB);
    B.CreateCall(excRuntime("__pas_raise_divzero"), {whereText(L)});
    B.CreateUnreachable();
    B.SetInsertPoint(ContBB);
  }

  /// ExceptAddr: the address of the exception the innermost enclosing
  /// handler handles (the frame it was raised to), nil outside handlers.
  llvm::Value *exceptAddr() {
    if (HandledFrames.empty())
      return llvm::ConstantPointerNull::get(B.getPtrTy());
    return B.CreateLoad(
        B.getPtrTy(),
        B.CreateStructGEP(excFrameType(), HandledFrames.back(), 3),
        "exceptaddr");
  }

  void visitRaiseStmt(RaiseStmt *S) {
    if (S->isReraise()) {
      B.CreateCall(excRuntime("__pas_reraise"), {HandledFrames.back()});
    } else if (S->getAddress()) {
      // Free Pascal's order: the frame (only evaluated), the address, the
      // object.
      if (S->getFrame())
        visitExpr(S->getFrame());
      llvm::Value *Addr = visitExpr(S->getAddress());
      llvm::Value *Obj = visitExpr(S->getObject());
      B.CreateCall(excRuntime("__pas_raise_at"),
                   {Obj, whereText(S->getBeginLoc()), Addr});
    } else {
      llvm::Value *Obj = visitExpr(S->getObject());
      B.CreateCall(excRuntime("__pas_raise"), {Obj, whereText(S->getBeginLoc())});
    }
    B.CreateUnreachable();
    B.SetInsertPoint(newBlock("after.raise")); // unreachable code follows
  }

  void visitWithStmt(WithStmt *S) {
    // The record designators are evaluated once, on entry (ISO 6.8.3.10).
    // An object (objfpc) is evaluated, a record's address taken.
    for (WithContext *W : S->getRecords())
      WithAddrs[W] = W->Record->getType()->isClass() ? emitObject(W->Record)
                                                     : emitAddress(W->Record);
    visitStmt(S->getBody());
  }

  //===--------------------------------------------------------------------===//
  // Expressions (scalar values in register representation)
  //===--------------------------------------------------------------------===//

  llvm::Value *visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    llvm::Value *V =
        llvm::ConstantInt::get(regType(E->getType()), E->getValue());
    if (isSignedOrdinal(E->getType()))
      noteRange(V, {true, static_cast<int64_t>(E->getValue()),
                    static_cast<int64_t>(E->getValue())});
    return V;
  }

  llvm::Value *visitRealLiteralExpr(RealLiteralExpr *E) {
    return llvm::ConstantFP::get(B.getDoubleTy(), E->getValue());
  }

  llvm::Value *visitStringLiteralExpr(StringLiteralExpr *E) {
    // Longer strings are aggregates and go through emitAddress.
    if (E->isWideCharLiteral())
      return B.getInt16(static_cast<uint16_t>(E->getWideCode()));
    assert(E->isCharLiteral() && "string values are handled by address");
    return B.getInt8(static_cast<uint8_t>(E->getValue()[0]));
  }

  llvm::Value *visitNameRefExpr(NameRefExpr *E) {
    Decl *D = E->getDecl();
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D)) {
      if (P->isClassProperty())
        return emitPropertyRead(P, nullptr, nullptr);
      WithContext *W = E->getWith();
      return emitPropertyRead(P, withBase(W), W->Record->getType());
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isStatic())
      return emitCall(R, {}); // a record's static method
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isMethod())
      return emitMethodCall(R, withBase(E->getWith()), {}, /*Inherited=*/false,
                            E->getBeginLoc(), withIsClass(E->getWith()));
    if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) // a class as a value
      return vmtOf(llvm::cast<ClassType>(TD->getType()));
    if (E->getType()->isShortString() &&
        (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D)))
      return emitAddress(E);
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && llvm::isa_and_nonnull<RoutineType>(V->getType())) {
      if (llvm::isa<RoutineType>(E->getType())) // the value itself (TP)
        return B.CreateLoad(memType(V->getType()), varAddress(V));
      return emitIndirectCall(V, {}); // parameterless functional parameter
    }
    if (isBitPackedComponent(E))
      return bitLoad(E);
    if (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D)) {
      llvm::Value *V = loadScalar(emitAddress(E), E->getType(), E->getName().Name);
      if (auto *VD = llvm::dyn_cast<VarDecl>(D)) {
        auto It = VarRanges.find(VD);
        if (It != VarRanges.end())
          noteRange(V, It->second);
      }
      return V;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D))
      return emitCall(R, {}); // parameterless function
    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D)) {
      if (BD->getBuiltinKind() == BuiltinKind::ParamCount)
        return B.CreateCall(RT.ParamCount, {}, "paramcount");
      if (BD->getBuiltinKind() == BuiltinKind::ExceptAddr)
        return exceptAddr();
      return emitFileQuery(BD->getBuiltinKind(), nullptr); // eof / eoln
    }
    auto *C = llvm::cast<ConstDecl>(D);
    const ConstValue &V = C->getValue();
    if (V.K == ConstValue::Real)
      return llvm::ConstantFP::get(B.getDoubleTy(), V.RealVal);
    llvm::Value *R = llvm::ConstantInt::get(regType(C->getType()),
                                            static_cast<uint64_t>(V.Int), true);
    if (isSignedOrdinal(C->getType()))
      noteRange(R, {true, V.Int, V.Int});
    return R;
  }

  llvm::Value *visitParenExpr(ParenExpr *E) { return visitExpr(E->getInner()); }

  /// move p: p's object; p is nil afterwards.
  llvm::Value *visitMoveExpr(MoveExpr *E) {
    llvm::Value *Slot = emitAddress(E->getOperand());
    llvm::Value *Obj = B.CreateLoad(B.getPtrTy(), Slot, "moved");
    B.CreateStore(llvm::ConstantPointerNull::get(B.getPtrTy()), Slot);
    if (E->getType()->isDynArray()) // (its reference, held to the statement's end)
      return holdResult(Obj, E->getType());
    return Obj;
  }

  llvm::Value *visitUnaryExpr(UnaryExpr *E) {
    if (RoutineDecl *Op = E->getOperator()) // a record's class operator
      return emitCall(Op, {E->getOperand()});
    llvm::Value *V = visitExpr(E->getOperand());
    Type *T = E->getType();
    if (T->isIntegerLike()) // promoted operand (byte -> integer, ...)
      V = convertOrdinal(V, E->getOperand()->getType(), T);
    switch (E->getOp()) {
    case UnaryOp::Plus:
      return V;
    case UnaryOp::Minus:
      if (T->isReal())
        return B.CreateFNeg(V, "neg");
      return emitCheckedArith(BinaryOp::Sub, isSignedOrdinal(T), intConst(V, 0),
                              V, E->getOpLoc(), "neg");
    case UnaryOp::Not:
      return B.CreateNot(V, "not");
    }
    llvm_unreachable("bad unary op");
  }

  /// obj is TFoo / obj as TFoo: the runtime walks obj's VMT chain.
  /// Is a VMT the VMT of C or of a descendant? A class with no descendants
  /// only needs the equality test; otherwise the depth and ancestor display
  /// make it two loads and two compares (item 12).
  llvm::Value *classIsVMT(llvm::Value *VMT, ClassType *C) {
    llvm::Constant *Target = vmtOf(C);
    if (!classHasDescendants(C))
      return B.CreateICmpEQ(VMT, Target, "is");
    unsigned Depth = classDepth(C);
    llvm::Value *Exact = B.CreateICmpEQ(VMT, Target, "is.exact");
    llvm::Value *ODepth =
        B.CreatePtrToInt(loadVMTSlot(VMT, 4, "depth"), B.getInt64Ty());
    llvm::Value *Ancestors = loadVMTSlot(VMT, 5, "ancestors");
    llvm::Value *Slot =
        B.CreateConstInBoundsGEP1_64(B.getPtrTy(), Ancestors, Depth);
    llvm::Value *Ancestor = B.CreateLoad(B.getPtrTy(), Slot, "ancestor");
    llvm::Value *In = B.CreateAnd(
        B.CreateICmpUGE(ODepth, B.getInt64(Depth)),
        B.CreateICmpEQ(Ancestor, Target), "is.display");
    return B.CreateOr(Exact, In, "is");
  }

  /// obj is C: false for nil, else classIsVMT.
  llvm::Value *emitClassIs(llvm::Value *Obj, ClassType *C) {
    auto *NullBB = newBlock("is.null");
    auto *CheckBB = newBlock("is.check");
    auto *EndBB = newBlock("is.end");
    B.CreateCondBr(B.CreateIsNull(Obj), NullBB, CheckBB);
    B.SetInsertPoint(NullBB);
    B.CreateBr(EndBB);
    B.SetInsertPoint(CheckBB);
    llvm::Value *VMT = B.CreateLoad(B.getPtrTy(), Obj, "vmt");
    llvm::Value *R = classIsVMT(VMT, C);
    llvm::BasicBlock *CheckEnd = B.GetInsertBlock();
    B.CreateBr(EndBB);
    B.SetInsertPoint(EndBB);
    auto *Phi = B.CreatePHI(B.getInt1Ty(), 2, "is");
    Phi->addIncoming(B.getFalse(), NullBB);
    Phi->addIncoming(R, CheckEnd);
    return Phi;
  }

  /// obj as C: obj, or a runtime error if it is not a C (item 12).
  llvm::Value *emitClassAs(llvm::Value *Obj, ClassType *C, SourceLocation L) {
    auto *NullBB = newBlock("as.null");
    auto *CheckBB = newBlock("as.check");
    auto *ErrBB = newBlock("as.error");
    auto *OkBB = newBlock("as.ok");
    B.CreateCondBr(B.CreateIsNull(Obj), NullBB, CheckBB);
    B.SetInsertPoint(NullBB);
    B.CreateBr(OkBB);
    B.SetInsertPoint(CheckBB);
    llvm::Value *VMT = B.CreateLoad(B.getPtrTy(), Obj, "vmt");
    B.CreateCondBr(classIsVMT(VMT, C), OkBB, ErrBB);
    B.SetInsertPoint(ErrBB);
    emitRuntimeError("invalid class typecast", L);
    B.SetInsertPoint(OkBB);
    return Obj;
  }

  llvm::Value *emitIsAs(BinaryExpr *E) {
    llvm::Value *Obj = visitExpr(E->getLHS());
    if (E->getLHS()->getType()->isInterface()) // a COM interface's object
      Obj = B.CreateExtractValue(Obj, 0, "obj");
    if (auto *I = llvm::dyn_cast<InterfaceType>(E->getRHS()->getType())) {
      if (E->getOp() == BinaryOp::Is) {
        llvm::Value *T = B.CreateCall(
            classRuntime("__pas_intf_lookup", B.getPtrTy(),
                         {B.getPtrTy(), B.getPtrTy()}),
            {Obj, iidOf(I)});
        return B.CreateIsNotNull(T, "is");
      }
      llvm::Constant *Msg = B.CreateGlobalString(
          ("invalid interface typecast" + where(E->getOpLoc())), ".rterr");
      llvm::Value *T = B.CreateCall(
          classRuntime("__pas_intf_as", B.getPtrTy(),
                       {B.getPtrTy(), B.getPtrTy(), B.getPtrTy()}),
          {Obj, iidOf(I), Msg}, "itable");
      return makeInterface(Obj, T);
    }
    auto *C = llvm::cast<ClassType>(E->getRHS()->getType());
    if (E->getOp() == BinaryOp::Is)
      return emitClassIs(Obj, C);
    return emitClassAs(Obj, C, E->getOpLoc());
  }

  llvm::Value *visitBinaryExpr(BinaryExpr *E) {
    BinaryOp Op = E->getOp();
    if (RoutineDecl *ClassOp = E->getOperator()) { // a record's operator
      llvm::Value *V = emitCall(ClassOp, {E->getLHS(), E->getRHS()});
      return E->isNegated() ? B.CreateNot(V, "ne") : V;
    }
    if (Op == BinaryOp::Is || Op == BinaryOp::As)
      return emitIsAs(E);
    if ((Op == BinaryOp::And || Op == BinaryOp::Or) && E->getType()->isBoolean())
      return emitShortCircuit(E);
    if (E->getType()->isAnsiString()) {
      llvm::Value *L = emitAnsi(E->getLHS());
      llvm::Value *R = emitAnsi(E->getRHS());
      return holdResult(astrCall("__pas_astr_concat", B.getPtrTy(), {L, R}));
    }
    if (E->getType()->isShortString()) { // Turbo/FPC concatenation
      llvm::Value *Tmp = newStringTemp();
      B.CreateCall(RT.StrConcat,
                   {Tmp, emitPStr(E->getLHS()), emitPStr(E->getRHS())});
      return Tmp;
    }
    if (isRelational(Op))
      return emitCompare(E);

    Type *LT = E->getLHS()->getType(), *RTy = E->getRHS()->getType();
    llvm::Value *L = visitExpr(E->getLHS());
    llvm::Value *R = visitExpr(E->getRHS());

    // Turbo/FPC pointer arithmetic, in units of what is pointed to.
    if (auto *PT = llvm::dyn_cast<PointerType>(E->getType())) {
      if (!LT->isPointer()) // n + p
        return B.CreateGEP(pointeeMemType(PT), R, toI64(L, LT), "ptr.add");
      llvm::Value *N = toI64(R, RTy);
      if (Op == BinaryOp::Sub)
        N = B.CreateNeg(N);
      return B.CreateGEP(pointeeMemType(PT), L, N, "ptr.add");
    }
    if (auto *PT = llvm::dyn_cast<PointerType>(LT)) { // p - q
      // (instructions even for constant addresses: the self-hosted compiler
      // writes the same)
      llvm::Value *LI = B.Insert(llvm::CastInst::Create(
          llvm::Instruction::PtrToInt, L, B.getInt64Ty()));
      llvm::Value *RI = B.Insert(llvm::CastInst::Create(
          llvm::Instruction::PtrToInt, R, B.getInt64Ty()));
      llvm::Value *Diff = B.CreateSub(LI, RI, "ptr.diff");
      uint64_t Size = DL.getTypeAllocSize(pointeeMemType(PT));
      return Size > 1 ? B.CreateSDiv(Diff, B.getInt64(Size), "ptr.count") : Diff;
    }

    if (E->getType()->isSet()) {
      // (the result has the larger of the two representations)
      L = convertSet(L, LT, E->getType());
      R = convertSet(R, RTy, E->getType());
      switch (Op) {
      case BinaryOp::Add: return B.CreateOr(L, R, "union");
      case BinaryOp::Mul: return B.CreateAnd(L, R, "intersection");
      default:            return B.CreateAnd(L, B.CreateNot(R), "difference");
      }
    }

    if (E->getType()->isReal()) {
      L = toDouble(L, LT);
      R = toDouble(R, RTy);
      switch (Op) {
      case BinaryOp::Add: return B.CreateFAdd(L, R, "add");
      case BinaryOp::Sub: return B.CreateFSub(L, R, "sub");
      case BinaryOp::Mul: return B.CreateFMul(L, R, "mul");
      case BinaryOp::RealDiv:
        if (rangeChecks(E->getOpLoc()))
          emitRuntimeCheck(
              B.CreateFCmpOEQ(R, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
              "division by zero", E->getOpLoc());
        return B.CreateFDiv(L, R, "div");
      default: llvm_unreachable("not a real operator");
      }
    }

    SourceLocation Loc = E->getOpLoc();
    // Integer operators: both operands in the operand type chosen by Sema.
    bool Signed = true;
    if (Type *OpTy = E->getOperandType()) {
      L = convertOrdinal(L, LT, OpTy);
      R = convertOrdinal(R, RTy, OpTy);
      Signed = isSignedOrdinal(OpTy);
    }
    switch (Op) {
    // Turbo/FPC bitwise operators (booleans: xor only).
    case BinaryOp::And: return B.CreateAnd(L, R, "and");
    case BinaryOp::Or:  return B.CreateOr(L, R, "or");
    case BinaryOp::Xor: return B.CreateXor(L, R, "xor");
    case BinaryOp::Shl: {
      unsigned Bits = L->getType()->getIntegerBitWidth();
      return B.CreateShl(L, B.CreateAnd(R, intConst(R, Bits - 1)), "shl");
    }
    case BinaryOp::Shr: { // logical, like Turbo/FPC
      unsigned Bits = L->getType()->getIntegerBitWidth();
      return B.CreateLShr(L, B.CreateAnd(R, intConst(R, Bits - 1)), "shr");
    }
    case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
      return emitCheckedArith(Op, Signed, L, R, Loc,
                              Op == BinaryOp::Add   ? "add"
                              : Op == BinaryOp::Sub ? "sub"
                                                    : "mul");
    case BinaryOp::IntDiv: return emitDiv(L, R, Signed, Loc);
    case BinaryOp::Mod:    return emitMod(L, R, Signed, Loc);
    default:
      llvm_unreachable("operator rejected by Sema");
    }
  }

  llvm::Value *visitCallExpr(CallExpr *E) {
    if (E->isMethodCast())
      return emitAddress(E);
    if (Expr *V = E->getCalleeValue()) // a procedural field or property
      return emitCallThrough(visitExpr(V), llvm::cast<RoutineType>(V->getType()),
                             E->getArgs(), E->getBeginLoc());
    if (E->isVarCast()) { // T(x) of an untyped parameter x
      if (E->getType()->isShortString())
        return emitAddress(E);
      return loadScalar(emitAddress(E), E->getType());
    }
    if (llvm::isa<TypeDecl>(E->getDecl()))
      return emitTypecast(E);
    if (auto *R = llvm::dyn_cast<RoutineDecl>(E->getDecl())) {
      if (R->isStatic()) // a record's static method
        return emitCall(R, E->getArgs());
      if (R->isMethod())
        return emitMethodCall(R, withBase(E->getWith()), E->getArgs(),
                              /*Inherited=*/false, E->getBeginLoc(),
                              withIsClass(E->getWith()));
      return emitCall(R, E->getArgs());
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(E->getDecl()))
      return emitIndirectCall(V, E->getArgs());
    return emitBuiltinCall(E, llvm::cast<BuiltinDecl>(E->getDecl()));
  }

  /// `a ${x} b`: each value is written, as `write` writes it, to the
  /// runtime's text file that collects characters (__pas_tmpl_sink);
  /// __pas_tmpl_add then makes the string so far, what was collected and
  /// the text that follows into one string. (Nothing but the write itself
  /// runs between the two, so a template inside a value is done by then.)
  llvm::Value *visitTemplateExpr(TemplateExpr *E) {
    llvm::ArrayRef<Expr *> Parts = E->getParts();
    llvm::Type *Ptr = B.getPtrTy();
    auto Text = [&](Expr *P) -> llvm::Value * {
      auto *SL = llvm::cast<StringLiteralExpr>(P);
      if (SL->getValue().empty())
        return llvm::ConstantPointerNull::get(B.getPtrTy());
      return getAnsiConstant(SL->getValue());
    };
    llvm::Value *R = Text(Parts[0]);
    for (size_t I = 1; I < Parts.size(); I += 2) {
      llvm::Value *File = astrCall("__pas_tmpl_sink", Ptr, {});
      emitWriteArg(File, Parts[I]);
      R = holdResult(astrCall("__pas_tmpl_add", Ptr, {R, Text(Parts[I + 1])}));
    }
    return R;
  }

  llvm::Value *visitFormattedExpr(FormattedExpr *) {
    llvm_unreachable("formatted expressions are handled by emitWriteArg");
  }

  llvm::Value *visitNamedArgExpr(NamedArgExpr *) {
    llvm_unreachable("Sema puts named arguments in the parameters' order");
  }

  llvm::Value *visitIndirectCallExpr(IndirectCallExpr *E) {
    Expr *Callee = E->getCallee();
    if (auto *I = llvm::dyn_cast<InheritedExpr>(Callee))
      return emitInheritedCall(I, E->getArgs());
    if (auto *F = llvm::dyn_cast<FieldExpr>(Callee);
        F && llvm::isa_and_nonnull<RoutineDecl>(F->getMember()))
      return emitMemberCall(F, E->getArgs());
    auto *RT = llvm::cast<RoutineType>(Callee->getType());
    llvm::Value *Pair = RT->isOfObject() || RT->isReference()
                            ? visitExpr(Callee) // (a closure: also a call's result)
                            : B.CreateLoad(memType(RT), emitAddress(Callee));
    return emitCallThrough(Pair, RT, E->getArgs(), Callee->getBeginLoc());
  }

  llvm::Value *visitInheritedExpr(InheritedExpr *E) {
    return emitInheritedCall(E, {});
  }

  llvm::Value *visitTupleExpr(TupleExpr *E) { return emitTuple(E); }

  /// Is the reference of type T at Addr nil? (an i1)
  llvm::Value *isNilAt(llvm::Value *Addr, Type *T) {
    llvm::Value *P;
    if (T->isInterface()) // {object, table}
      P = B.CreateLoad(B.getPtrTy(), Addr, "obj");
    else
      P = B.CreateLoad(B.getPtrTy(), Addr, "ref");
    return B.CreateICmpEQ(P, llvm::ConstantPointerNull::get(B.getPtrTy()), "isnil");
  }

  /// The default value of type T (0, false, '', nil), as visitExpr gives it.
  llvm::Value *zeroValue(Type *T) {
    if (T->getBase()->isBoolean())
      return B.getInt1(false);
    return llvm::Constant::getNullValue(memType(T));
  }

  /// x?.m: x's value in a hidden variable; where it is not nil, m (in
  /// another, which is the value; else the default value) or the call.
  llvm::Value *visitSafeNavExpr(SafeNavExpr *E) {
    Expr *Base = E->getBase();
    Type *BT = Base->getType();
    llvm::Value *Tmp = varAddress(E->getTmp());
    storeExpr(Tmp, BT, Base);
    llvm::Value *Res = E->getRes() ? varAddress(E->getRes()) : nullptr;
    Type *RT = E->getRes() ? E->getRes()->getType() : nullptr;
    if (Res)
      storeValue(Res, zeroValue(RT), RT);
    auto *ThenBB = newBlock("nav.then");
    auto *EndBB = newBlock("nav.end");
    B.CreateCondBr(isNilAt(Tmp, BT), EndBB, ThenBB);
    B.SetInsertPoint(ThenBB);
    if (E->isStatement()) {
      visitStmt(E->getCall());
    } else {
      storeExpr(Res, RT, E->getAccess());
      if (VarDecl *Ok = E->getOk())
        storeScalar(varAddress(Ok), B.getInt1(true), Ok->getType());
    }
    B.CreateBr(EndBB);
    B.SetInsertPoint(EndBB);
    return E->isStatement() ? nullptr : visitExpr(E->getResRef());
  }

  /// a ?? b: a in a hidden variable, replaced by b where a is nil (or where
  /// a's chain of ?. stopped).
  llvm::Value *visitCoalesceExpr(CoalesceExpr *E) {
    Expr *L = E->getLeft();
    while (auto *P = llvm::dyn_cast<ParenExpr>(L))
      L = P->getInner();
    VarDecl *Ok = nullptr;
    if (auto *Nav = llvm::dyn_cast<SafeNavExpr>(L)) {
      while (auto *Inner = llvm::dyn_cast<SafeNavExpr>(Nav->getAccess()))
        Nav = Inner;
      Ok = Nav->getOk();
    }
    if (Ok)
      storeScalar(varAddress(Ok), B.getInt1(false), Ok->getType());
    Type *T = E->getType();
    llvm::Value *Res = varAddress(E->getRes());
    storeExpr(Res, T, E->getLeft());
    llvm::Value *Missing =
        Ok ? B.CreateNot(loadScalar(varAddress(Ok), Ok->getType()), "missing")
           : isNilAt(Res, T);
    auto *ThenBB = newBlock("coalesce.else");
    auto *EndBB = newBlock("coalesce.end");
    B.CreateCondBr(Missing, ThenBB, EndBB);
    B.SetInsertPoint(ThenBB);
    storeExpr(Res, T, E->getRight());
    B.CreateBr(EndBB);
    B.SetInsertPoint(EndBB);
    return visitExpr(E->getResRef());
  }

  /// if c then a else b: the if statement Sema made of it sets a hidden
  /// variable, which is the value.
  llvm::Value *visitCondExpr(CondExpr *E) {
    visitStmt(E->getLowered());
    return E->getType()->isStructured() ? emitAddress(E->getResult())
                                        : visitExpr(E->getResult());
  }

  llvm::Value *visitAggregateExpr(AggregateExpr *) {
    llvm_unreachable("aggregates only initialize typed constants");
  }

  llvm::Value *visitAddrOfExpr(AddrOfExpr *E) {
    if (E->getMethod())
      return emitMethodPointer(E);
    if (RoutineDecl *L = E->getLambda(); L && L->isMethodLambda()) {
      // {code, object}: the object is the Self where it is made
      VarDecl *Obj = L->getLambdaSelf();
      llvm::Value *Pair = llvm::UndefValue::get(
          llvm::StructType::get(B.getPtrTy(), B.getPtrTy()));
      Pair = B.CreateInsertValue(Pair, Functions[L], 0);
      return B.CreateInsertValue(
          Pair,
          Obj ? loadScalar(varAddress(Obj), Obj->getType(), "obj")
              : llvm::ConstantPointerNull::get(B.getPtrTy()),
          1);
    }
    if (E->getCode())
      return Functions[E->getCode()];
    return emitAddress(E->getOperand());
  }

  llvm::Value *visitNilExpr(NilExpr *) {
    return llvm::ConstantPointerNull::get(B.getPtrTy());
  }

  llvm::Value *visitDerefExpr(DerefExpr *E) {
    if (E->getType()->isShortString())
      return emitAddress(E);
    return loadScalar(emitAddress(E), E->getType());
  }

  llvm::Value *visitSetCtorExpr(SetCtorExpr *E) {
    if (auto *DA = llvm::dyn_cast_or_null<DynArrayType>(E->getType()))
      return emitDynArrayCtor(E, DA);
    return emitSetCtor(E);
  }

  llvm::Value *visitIndexExpr(IndexExpr *E) {
    if (E->getProperty())
      return emitArrayPropertyRead(E);
    if (isBitPackedComponent(E))
      return bitLoad(E);
    if (E->getType()->isShortString())
      return emitAddress(E);
    if (E->getBase()->getType()->isAnsiString()) { // reading: no copy
      llvm::Value *S = visitExpr(E->getBase());
      llvm::Value *X = toI64(visitExpr(E->getIndex()), E->getIndex()->getType());
      emitAnsiIndexCheck(S, X, E->getIndex()->getBeginLoc());
      return loadScalar(B.CreateInBoundsGEP(B.getInt8Ty(), S,
                                            B.CreateSub(X, B.getInt64(1))),
                        E->getType());
    }
    return loadScalar(emitAddress(E), E->getType());
  }

  llvm::Value *visitFieldExpr(FieldExpr *E) {
    if (NameRefExpr *Ref = E->getConstRef()) // TFoo.C
      return visitExpr(Ref);
    if (isBitPackedComponent(E))
      return bitLoad(E);
    if (E->getBase()->getType()->isRecord() &&
        !llvm::isa_and_nonnull<FieldDecl>(E->getMember())) {
      if (llvm::isa_and_nonnull<RoutineDecl>(E->getMember()))
        return emitMemberCall(E, {}); // a record's method
      return emitMemberValue(E);      // a record's property
    }
    if (!llvm::isa_and_nonnull<FieldDecl>(E->getMember()) ||
        E->getBase()->getType()->isClass() ||
        E->getBase()->getType()->isClassRef())
      return emitMemberValue(E); // objfpc: a member of an object
    if (E->getType()->isShortString())
      return emitAddress(E);
    return loadScalar(emitAddress(E), E->getType(), E->getField().Name);
  }
};

} // namespace

std::unique_ptr<llvm::Module>
p3::generateModule(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                    const SourceManager &SM,
                    const LangOptions &LangOpts, const CodeGenOptions &CGOpts,
                    llvm::LLVMContext &Ctx, llvm::TargetMachine &TM,
                    llvm::StringRef SourceFileName) {
  auto M = std::make_unique<llvm::Module>(SourceFileName, Ctx);
  M->setSourceFileName(SourceFileName);
  M->setTargetTriple(TM.getTargetTriple().str());
  M->setDataLayout(TM.createDataLayout());
  CodeGenModule(*M, SM, LangOpts, CGOpts).emitProgram(P, Units);
  return M;
}

std::unique_ptr<llvm::Module>
p3::generateUnitModule(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps,
                        const SourceManager &SM, const LangOptions &LangOpts,
                        const CodeGenOptions &CGOpts, llvm::LLVMContext &Ctx,
                        llvm::TargetMachine &TM, llvm::StringRef SourceFileName) {
  auto M = std::make_unique<llvm::Module>(SourceFileName, Ctx);
  M->setSourceFileName(SourceFileName);
  M->setTargetTriple(TM.getTargetTriple().str());
  M->setDataLayout(TM.createDataLayout());
  CodeGenModule(*M, SM, LangOpts, CGOpts).emitUnit(U, Deps);
  return M;
}
