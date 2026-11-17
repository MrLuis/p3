//===- Sema.cpp - Semantic analysis -------------------------------------===//
//
// One pass over the AST, in declaration order (Pascal requires
// declare-before-use; `forward` covers mutual recursion):
//
//   * scopes: standard scope > program block > routine blocks > with scopes
//   * declarations: constants are folded, types are built (name
//     equivalence), routines get their parent/level/result slot
//   * statements and expressions are type-checked against ISO 7185
//     compatibility (6.4.5) and assignment-compatibility (6.4.6) rules
//
//===----------------------------------------------------------------------===//

#include "p3/Sema/Sema.h"
#include "p3/AST/ASTContext.h"
#include "p3/AST/ASTVisitor.h"
#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/LangOptions.h"
#include "p3/Sema/Scope.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <bitset>
#include <map>
#include <cmath>
#include <memory>
#include <optional>

using namespace p3;

namespace {

/// Predeclared names from PLAN.md 4.5 that later milestones implement. Using
/// one gives "not supported yet" instead of "undeclared identifier".
bool isFutureBuiltin(llvm::StringRef Name) {
  (void)Name; // every name of PLAN.md 4.5 is implemented
  return false;
}

/// A folded constant.
struct Constant {
  Type *Ty;
  ConstValue Value;
};

class Checker : public StmtVisitor<Checker>,
                public ExprVisitor<Checker, Type *> {
  ASTContext &Ctx;
  DiagnosticsEngine &Diags;
  const LangOptions &LangOpts;

  Scope Standard;              ///< predeclared entities
  Scope *Cur = &Standard;
  /// Units: interface and implementation scopes, by canonical unit name.
  std::vector<std::unique_ptr<Scope>> UnitScopes;
  llvm::StringMap<Scope *> UnitInterfaces;
  Scope *CurUnitInterface = nullptr; ///< while checking an implementation
  RoutineDecl *CurRoutine = nullptr; ///< null in the program block
  /// The program or unit being checked (classes remember theirs: private
  /// members are visible in the same module).
  const void *CurModule = nullptr;
  /// The target of the assignment being checked: a property there is
  /// written, not read.
  Expr *CurAssignTarget = nullptr;
  /// f += e in function f: the f read again, which is f's result too (as in
  /// Free Pascal), not a call.
  NameRefExpr *CompoundResultRef = nullptr;
  VarDecl *CompoundResultVar = nullptr;
  /// The procedural type an `@routine` is assigned to (picks an overload).
  RoutineType *AddrOfTarget = nullptr;
  /// The base of the IndexExpr being checked: an array property may appear
  /// there without its indexes. Pending: such a base (or an IndexExpr
  /// still short of indexes), with the property, the chain's root and the
  /// indexes so far.
  Expr *IndexBase = nullptr;
  struct PendingIndex {
    PropertyDecl *Prop;
    Expr *Root;
    llvm::SmallVector<Expr *, 2> Args;
  };
  llvm::DenseMap<Expr *, PendingIndex> PendingIndexes;
  /// The predeclared classes TObject and Exception (objfpc).
  ClassType *TObjectTy = nullptr;
  ClassType *ExceptionTy = nullptr;
  /// IUnknown (= IInterface), the root of the COM interfaces, and
  /// TInterfacedObject, the root of the classes implementing them.
  InterfaceType *IUnknownTy = nullptr;
  ClassType *TInterfacedObjectTy = nullptr;
  /// TSharedObject (objfpc): the root of the thread-safe classes.
  ClassType *TSharedObjectTy = nullptr;
  /// The advanced record being declared by a type declaration (records
  /// with methods must be named, so others are rejected).
  RecordTypeSpec *NamedAdvancedRecord = nullptr;
  /// What a typed constant's or initialized variable's value is called in
  /// diagnostics ("... must be a constant").
  const char *ConstInitWhat = "typed constant value";
  /// While the members of a class or advanced record are declared: its
  /// name, which prefixes those of the types it declares (TOuter.TInner).
  llvm::StringRef NestPrefix;

  /// Generic routines (objfpc): parses a generic's declaration again; the
  /// specializations made (by generic and type arguments); those whose
  /// bodies are to be checked (in their scope, for a use); those of the
  /// module being checked, added to its block for code generation.
  Sema::ReparseFn Reparse;
  Sema::ReparseTypeFn ReparseType;
  /// The specializations of generic types, by generic and type arguments.
  std::map<std::pair<TypeDecl *, std::vector<Type *>>, TypeDecl *> TypeInstances;
  /// While a generic type's specialization is declared: its methods'
  /// implementations, whose bodies are checked later (checkInstances).
  std::vector<RoutineDecl *> *DeferredBodies = nullptr;
  /// The scopes of a generic type's specializations (see declareRoutine).
  std::map<TypeDecl *, std::vector<Scope *>> InstanceScopesOf;
  std::map<std::pair<RoutineDecl *, std::vector<Type *>>, RoutineDecl *> Instances;
  struct PendingInstance {
    RoutineDecl *R;
    Scope *S;
    SourceLocation Use;
  };
  std::vector<PendingInstance> PendingInstances;
  std::vector<std::unique_ptr<Scope>> InstanceScopes;
  llvm::SmallVector<Decl *, 4> ModuleInstances;
  /// `class of C` for each class C: the type of C used as a value, and of
  /// Self in class methods (`TFooClass = class of TFoo` is a type of its own).
  llvm::DenseMap<ClassType *, ClassRefType *> ClassRefs;

  ClassRefType *classRefOf(ClassType *C) {
    ClassRefType *&R = ClassRefs[C];
    if (!R)
      R = Ctx.create<ClassRefType>(C);
    return R;
  }
  /// `TFoo = class;` not defined yet in the current type section.
  llvm::SmallVector<std::pair<TypeDecl *, ClassType *>, 2> PendingClasses;
  /// `IFoo = interface;` not defined yet in the current type section.
  llvm::SmallVector<std::pair<TypeDecl *, InterfaceType *>, 2> PendingInterfaces;

  /// Enclosing for statements (also for-in), innermost last: the control
  /// variable and the statement. Their control variables must not be
  /// assigned in the loop body (ISO 7185 6.8.3.9 "threatened").
  struct ActiveFor {
    Decl *Var;
    SourceLocation Loc;
  };
  llvm::SmallVector<ActiveFor, 4> ActiveFors;

  unsigned LoopDepth = 0; ///< for break / continue

  // Exceptions (objfpc). HandlerDepth: exception handlers around the
  // statement being checked (a bare `raise` needs one). FinallyLoopDepth:
  // LoopDepth on entry to the innermost enclosing finally part, or -1; exit,
  // and break/continue out of it, are not allowed there. (objfpc has no
  // gotos, so none can leave a try statement.)
  unsigned HandlerDepth = 0;
  int FinallyLoopDepth = -1;

  //===--------------------------------------------------------------------===//
  // Owned variables (objfpc): which may have been moved
  //===--------------------------------------------------------------------===//
  //
  // The statements of a routine with owned variables are followed in the
  // order they run, with the state of each owned variable: it may hold its
  // object (Valid), it may have been moved (Moved), or both, after paths
  // that differ. A use of a variable that may have been moved is an error.
  // Unreachable code has neither bit of any variable.
  //
  // A loop is followed once. Entry[i] is the outermost enclosing loop
  // (counted from 1) whose every earlier iteration may have left variable i
  // as it is, or NoLoop: a use of the variable then is recorded in those
  // loops, and reported when the loop turns out to end with it moved.
  enum { MaxOwned = 32, NoLoop = 255 };
  /// Set in Valid of every state that is reached (a routine may have no
  /// owned variable yet: they can be declared among the statements).
  static constexpr uint64_t LiveBit = uint64_t(1) << MaxOwned;
  struct OwnedState {
    uint64_t Valid = 0, Moved = 0;
    uint8_t Entry[MaxOwned];
    OwnedState() { std::fill(Entry, Entry + MaxOwned, uint8_t(NoLoop)); }
    bool isLive() const { return (Valid | Moved) != 0; }
    void join(const OwnedState &O) {
      Valid |= O.Valid;
      Moved |= O.Moved;
      for (unsigned I = 0; I < MaxOwned; ++I)
        Entry[I] = std::min(Entry[I], O.Entry[I]);
    }
  };
  struct OwnedLoop {
    OwnedState Breaks, Continues;
    uint64_t Used = 0; ///< variables used as the loop found them
    SourceLocation EntryUse[MaxOwned];
    OwnedLoop *Prev = nullptr;
  };
  /// A try statement's protected part: every state in it, joined (an
  /// exception may leave it anywhere).
  struct OwnedTry {
    OwnedState Seen;
    OwnedTry *Prev = nullptr;
  };
  struct OwnedFlow {
    VarDecl *Vars[MaxOwned];
    unsigned N = 0;
    OwnedState S;
    SourceLocation MoveLoc[MaxOwned];
    unsigned Changed[MaxOwned] = {}; ///< Tick of the last move or assignment
    unsigned Tick = 0;
    OwnedLoop *Loop = nullptr;
    unsigned Depth = 0;
    OwnedTry *Try = nullptr;
  };
  OwnedFlow *Flow = nullptr; ///< of the block whose statements are checked
  /// The variables the statements of that block declare (VarDeclStmt).
  std::vector<Decl *> *InlineVars = nullptr;
  bool ArgsFailed = false; ///< see orderedArgs
  bool KeepCharLiterals = false; ///< see visitDestructureStmt
  Expr *WantedCall = nullptr; ///< the call checked for a value of
  Type *WantedType = nullptr; ///< WantedType (checkValueFor)
  std::vector<RecordType *> TupleTypes; ///< see getTupleType
  unsigned HiddenVars = 0;              ///< see makeHiddenVar
  /// The scope of the variables declared in the statement sequence being
  /// checked, made when the first one is (see DirectSequence).
  std::unique_ptr<Scope> *SeqScope = nullptr;
  Expr *OwnedTarget = nullptr; ///< the assignment target being checked

  /// E, if it is an owned variable (in parentheses or not).
  static VarDecl *ownedVarOf(Expr *E) {
    while (auto *P = llvm::dyn_cast<ParenExpr>(E))
      E = P->getInner();
    auto *N = llvm::dyn_cast<NameRefExpr>(E);
    auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
    return V && V->isOwned() ? V : nullptr;
  }

  int ownedIndex(VarDecl *V) {
    if (!Flow || !V->isOwned())
      return -1;
    for (unsigned I = 0; I < Flow->N; ++I)
      if (Flow->Vars[I] == V)
        return static_cast<int>(I);
    return -1; // an enclosing routine's: not followed
  }

  OwnedState ownedGet() { return Flow ? Flow->S : OwnedState(); }
  void ownedSet(const OwnedState &S) {
    if (Flow)
      Flow->S = S;
  }
  void ownedJoin(const OwnedState &S) {
    if (Flow)
      Flow->S.join(S);
  }
  /// What follows is not reached (after exit, break, continue, raise).
  void ownedDead() {
    if (Flow)
      Flow->S = OwnedState();
  }

  /// A use of the owned variable V: an error if it may have been moved.
  void ownedUse(VarDecl *V, SourceRange R) {
    int I = ownedIndex(V);
    if (I < 0 || !Flow->S.isLive())
      return;
    uint64_t Bit = uint64_t(1) << I;
    OwnedState &S = Flow->S;
    if (S.Moved & Bit) {
      Diags.error(R.getBegin(),
                  "owned variable '" + V->getName().Spelling +
                      ((S.Valid & Bit) ? "' may have been moved"
                                       : "' was moved"),
                  {R});
      Diags.note(Flow->MoveLoc[I], "moved here");
      S.Moved &= ~Bit; // reported once
      S.Valid |= Bit;
      return;
    }
    unsigned D = Flow->Depth;
    for (OwnedLoop *L = Flow->Loop; L && D >= S.Entry[I]; L = L->Prev, --D)
      if (!(L->Used & Bit)) {
        L->Used |= Bit;
        L->EntryUse[I] = R.getBegin();
      }
  }

  void ownedChanged(int I) {
    Flow->S.Entry[I] = NoLoop;
    Flow->Changed[I] = ++Flow->Tick;
    for (OwnedTry *T = Flow->Try; T; T = T->Prev)
      T->Seen.join(Flow->S);
  }

  /// V := object.
  void ownedAssign(VarDecl *V) {
    int I = ownedIndex(V);
    if (I < 0 || !Flow->S.isLive())
      return;
    Flow->S.Valid |= uint64_t(1) << I;
    Flow->S.Moved &= ~(uint64_t(1) << I);
    ownedChanged(I);
  }

  /// move V.
  void ownedMove(VarDecl *V, SourceLocation Loc) {
    int I = ownedIndex(V);
    if (I < 0 || !Flow->S.isLive())
      return;
    Flow->S.Valid &= ~(uint64_t(1) << I);
    Flow->S.Moved |= uint64_t(1) << I;
    Flow->MoveLoc[I] = Loc;
    ownedChanged(I);
  }

  /// V, an owned variable of the block being checked, is declared.
  void ownedDeclare(VarDecl *V) {
    if (!Flow)
      return;
    if (Flow->N == MaxOwned) {
      Diags.error(V->getLocation(),
                  "too many owned variables in one routine (at most " +
                      std::to_string(unsigned(MaxOwned)) + ")",
                  {V->getName().getRange()});
      return;
    }
    Flow->Vars[Flow->N++] = V;
    ownedAssign(V); // nil: nothing to free, not moved
  }

  void ownedLoopBegin(OwnedLoop &L) {
    if (!Flow)
      return;
    L.Prev = Flow->Loop;
    Flow->Loop = &L;
    ++Flow->Depth;
    if (Flow->S.isLive())
      for (unsigned I = 0; I < Flow->N; ++I)
        if (Flow->S.Entry[I] > Flow->Depth)
          Flow->S.Entry[I] = static_cast<uint8_t>(Flow->Depth);
  }

  /// The loop L ends: Before is the state in which it runs no (more)
  /// iterations. A variable that an iteration may leave moved, and that an
  /// iteration uses as it finds it, is used after a move.
  void ownedLoopEnd(OwnedLoop &L, const OwnedState &Before) {
    if (!Flow)
      return;
    OwnedState Back = Flow->S;
    Back.join(L.Continues);
    for (unsigned I = 0; I < Flow->N; ++I)
      if ((Back.Moved & L.Used) & (uint64_t(1) << I)) {
        Diags.error(L.EntryUse[I],
                    "owned variable '" + Flow->Vars[I]->getName().Spelling +
                        "' may have been moved by an earlier iteration of "
                        "the loop");
        Diags.note(Flow->MoveLoc[I], "moved here");
      }
    Back.join(Before);
    Back.join(L.Breaks);
    for (unsigned I = 0; I < Flow->N; ++I)
      if (Back.Entry[I] != NoLoop && Back.Entry[I] >= Flow->Depth)
        Back.Entry[I] = NoLoop;
    Flow->Loop = L.Prev;
    --Flow->Depth;
    Flow->S = Back;
  }

  void ownedBreak(bool IsContinue) {
    if (!Flow || !Flow->Loop)
      return;
    (IsContinue ? Flow->Loop->Continues : Flow->Loop->Breaks).join(Flow->S);
    ownedDead();
  }

  void ownedTryBegin(OwnedTry &T) {
    if (!Flow)
      return;
    T.Seen = Flow->S;
    T.Prev = Flow->Try;
    Flow->Try = &T;
  }
  void ownedTryEnd(OwnedTry &T) {
    if (Flow)
      Flow->Try = T.Prev;
  }

  /// Functions whose result has been assigned somewhere in their body.
  llvm::DenseSet<RoutineDecl *> ResultAssigned;

  /// Variables referenced anywhere (read or written).
  llvm::DenseSet<Decl *> Used;

  /// `^T` where T was not declared yet: resolved at the end of the type
  /// section (ISO 7185 6.2.2.9 allows pointers to later types).
  llvm::SmallVector<std::pair<PointerTypeSpec *, PointerType *>, 4>
      PendingPointers;

  // goto / labels (ISO 7185 6.8.1). Containers holds the enclosing statement
  // sequences and labeled statements of the statement being checked; each
  // goto remembers them and is validated once all labels are defined.
  llvm::SmallVector<const void *, 16> Containers;
  const void *DirectSequence = nullptr; ///< sequence of the current statement
  CompoundStmt *RoutineBody = nullptr;  ///< outermost sequence of the block
  struct PendingGoto {
    GotoStmt *S;
    llvm::SmallVector<const void *, 16> Containers;
    RoutineDecl *From;
  };
  std::vector<PendingGoto> PendingGotos;
  llvm::DenseMap<RoutineDecl *, unsigned> NonLocalLabelCount;

  static std::string labelKey(llvm::StringRef Name) { return "#" + Name.str(); }

  Type *errorType() { return Ctx.getErrorType(); }
  bool isISO() const { return LangOpts.Mode == LangMode::ISO; }

  //===--------------------------------------------------------------------===//
  // Standard scope
  //===--------------------------------------------------------------------===//

  Identifier predeclaredName(llvm::StringRef N) {
    llvm::StringRef Interned = Ctx.getStrings().intern(N);
    return {Interned, Interned, SourceLocation()};
  }

  void declareStandardScope() {
    auto Ty = [&](llvm::StringRef N, Type *T) {
      Standard.insert(N, Ctx.create<TypeDecl>(SourceLocation(),
                                              predeclaredName(N), T));
    };
    Ty("integer", Ctx.getIntegerType());
    Ty("real", Ctx.getRealType());
    Ty("boolean", Ctx.getBooleanType());
    Ty("char", Ctx.getCharType());
    Ty("text", Ctx.getTextType());
    if (!isISO()) {
      Ty("pointer", Ctx.getUntypedPointerType());
      // Turbo/FPC integer types; longint is integer (32-bit), and double is
      // real.
      Ty("longint", Ctx.getIntegerType());
      Ty("int64", Ctx.getInt64Type());
      // FPC's pointer-sized signed integers (every target is 64-bit).
      Ty("sizeint", Ctx.getInt64Type());
      Ty("ptrint", Ctx.getInt64Type());
      Ty("nativeint", Ctx.getInt64Type());
      Ty("cardinal", Ctx.getCardinalType());
      Ty("longword", Ctx.getCardinalType());
      Ty("dword", Ctx.getCardinalType());
      // Unsigned 64-bit, and FPC's pointer-sized unsigned integers.
      Ty("qword", Ctx.getQWordType());
      Ty("uint64", Ctx.getQWordType());
      Ty("nativeuint", Ctx.getQWordType());
      Ty("ptruint", Ctx.getQWordType());
      Ty("sizeuint", Ctx.getQWordType());
      // FPC's THandle (a file descriptor on Unix): longint.
      Ty("thandle", Ctx.getIntegerType());
      Ty("byte", Ctx.getByteType());
      Ty("word", Ctx.getWordType());
      Ty("shortint", Ctx.getShortIntType());
      Ty("smallint", Ctx.getSmallIntType());
      Ty("double", Ctx.getRealType());
      // pchar: a pointer to char, for C strings (external routines).
      auto *PChar = Ctx.create<PointerType>(Ctx.getCharType());
      PChar->setDeclName("pchar");
      Ty("pchar", PChar);
      PCharTy = PChar;
      // Free Pascal's names for the one-byte char (char is AnsiChar there).
      Ty("ansichar", Ctx.getCharType());
      // A 16-bit character code (FPC's widechar; unicodechar is the same).
      Ty("widechar", Ctx.getWideCharType());
      Ty("unicodechar", Ctx.getWideCharType());
      Ty("pansichar", PChar);
      Ty("ansistring", Ctx.getAnsiStringType());
      Ty("shortstring", Ctx.getShortStringType());
      declareTGUID();
    }

    if (LangOpts.Mode == LangMode::TP)
      declareVarRec();
    if (LangOpts.ObjKeywords) {
      declareTObject();
      declareTMethod();
      declareVarRec();
      declareException();
      declareInterfacedObject();
      declareSharedObject();
    }

    // The standard files (ISO 7185 6.10).
    for (llvm::StringRef N : {"input", "output"}) {
      auto *V = Ctx.create<VarDecl>(predeclaredName(N), nullptr);
      V->setType(Ctx.getTextType());
      Standard.insert(N, V);
    }

    auto Const = [&](llvm::StringRef N, Type *T, int64_t V) {
      Standard.insert(N, Ctx.create<ConstDecl>(SourceLocation(),
                                               predeclaredName(N), T,
                                               ConstValue::ordinal(V)));
    };
    Const("false", Ctx.getBooleanType(), 0);
    Const("true", Ctx.getBooleanType(), 1);
    Const("maxint", Ctx.getIntegerType(), PascalMaxInt);

#define BUILTIN_PROC(ID, NAME)                                                 \
  if ((!isTPOnly(BuiltinKind::ID) || !isISO()) &&                              \
      (!isObjFPCOnly(BuiltinKind::ID) || LangOpts.ObjKeywords))                \
    Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),       \
                                                  BuiltinKind::ID, true));
#define BUILTIN_FUNC(ID, NAME)                                                 \
  if ((!isTPOnly(BuiltinKind::ID) || !isISO()) &&                              \
      (!isObjFPCOnly(BuiltinKind::ID) || LangOpts.ObjKeywords))                \
    Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),       \
                                                  BuiltinKind::ID, false));
#include "p3/AST/Builtins.def"
  }

  /// Identifier for a predeclared member: lookup name lowercase, spelled
  /// as in Free Pascal.
  Identifier memberName(llvm::StringRef Spelling) {
    return {Ctx.getStrings().intern(Spelling.lower()),
            Ctx.getStrings().intern(Spelling), SourceLocation()};
  }

  /// class TObject: constructor Create; destructor Destroy; virtual;
  /// procedure Free; function ClassName: string; (bodies in codegen and the
  /// runtime). Destroy is virtual method 0.
  void declareTObject() {
    TObjectTy = Ctx.create<ClassType>();
    TObjectTy->setDeclName("TObject");
    auto Method = [&](llvm::StringRef Name, MethodKind MK, Type *Result,
                      TObjectMethod B) {
      auto *R = Ctx.create<RoutineDecl>(SourceLocation(), memberName(Name),
                                        Result != nullptr,
                                        llvm::ArrayRef<VarDecl *>(), false,
                                        nullptr, nullptr, false);
      R->setMethodKind(MK);
      R->setClass(TObjectTy);
      R->setBuiltin(B);
      if (Result)
        R->setResultType(Result);
      return R;
    };
    RoutineDecl *Destroy = Method("Destroy", MethodKind::Destructor, nullptr,
                                  TObjectMethod::Destroy);
    Destroy->setDirectives(/*Virtual=*/true, false, false);
    Destroy->setVMTIndex(0);
    // TClass = class of TObject; class functions take Self = a class.
    ClassRefType *TClass = classRefOf(TObjectTy);
    auto ClassFn = [&](llvm::StringRef Name, Type *Result, TObjectMethod B) {
      RoutineDecl *R = Method(Name, MethodKind::Method, Result, B);
      R->setClassMethod();
      return R;
    };
    auto *AClass = Ctx.create<VarDecl>(memberName("AClass"), nullptr,
                                       VarDecl::ValueParam);
    AClass->setType(TClass);
    VarDecl *InheritsParams[] = {AClass};
    auto *InheritsFrom = Ctx.create<RoutineDecl>(
        SourceLocation(), memberName("InheritsFrom"), true,
        Ctx.copyArray<VarDecl *>(InheritsParams), true, nullptr, nullptr, false);
    InheritsFrom->setMethodKind(MethodKind::Method);
    InheritsFrom->setClass(TObjectTy);
    InheritsFrom->setBuiltin(TObjectMethod::InheritsFrom);
    InheritsFrom->setResultType(Ctx.getBooleanType());
    InheritsFrom->setClassMethod();
    Decl *Members[] = {
        Method("Create", MethodKind::Constructor, nullptr, TObjectMethod::Create),
        Destroy,
        Method("Free", MethodKind::Method, nullptr, TObjectMethod::Free),
        ClassFn("ClassName", Ctx.getShortStringType(), TObjectMethod::ClassName),
        Method("ClassType", MethodKind::Method, TClass, TObjectMethod::ClassType),
        ClassFn("ClassParent", TClass, TObjectMethod::ClassParent),
        InheritsFrom,
        ClassFn("InstanceSize", Ctx.getInt64Type(), TObjectMethod::InstanceSize)};
    RoutineDecl *VTable[] = {Destroy};
    TObjectTy->define(nullptr, Ctx.copyArray<Decl *>(Members), {},
                      Ctx.copyArray<RoutineDecl *>(VTable), nullptr);
    Standard.insert("tobject", Ctx.create<TypeDecl>(SourceLocation(),
                                                    memberName("TObject"),
                                                    TObjectTy));
    auto *TClassNamed = Ctx.create<ClassRefType>(TObjectTy);
    TClassNamed->setDeclName("TClass");
    Standard.insert("tclass", Ctx.create<TypeDecl>(SourceLocation(),
                                                   memberName("TClass"),
                                                   TClassNamed));
  }

  /// TVarRec, the elements of an array of const, and the vt* constants:
  ///   record VType: int64; case of ... VInteger, VBoolean, VChar, ... end
  void declareVarRec() {
    Type *Int64Ty = Ctx.getInt64Type();
    auto Field = [&](llvm::StringRef Name, Type *T) {
      auto *F = Ctx.create<FieldDecl>(memberName(Name), nullptr);
      F->setType(T);
      return F;
    };
    Type *Untyped = Ctx.getUntypedPointerType();
    auto Ptr = [&](Type *T) { return Ctx.create<PointerType>(T); };
    std::pair<llvm::StringRef, Type *> Variants[] = {
        {"VInteger", Ctx.getIntegerType()},
        {"VBoolean", Ctx.getBooleanType()},
        {"VChar", Ctx.getCharType()},
        {"VExtended", Ptr(Ctx.getRealType())},
        {"VString", Ptr(Ctx.getShortStringType())},
        {"VPointer", Untyped},
        {"VPChar", PCharTy},
        {"VAnsiString", Untyped},
        {"VInt64", Ptr(Int64Ty)},
    };
    llvm::SmallVector<std::pair<llvm::StringRef, Type *>, 12> AllVariants(
        std::begin(Variants), std::end(Variants));
    if (LangOpts.ObjKeywords) { // classes exist
      AllVariants.push_back({"VObject", TObjectTy});
      AllVariants.push_back({"VClass", classRefOf(TObjectTy)});
    }
    llvm::SmallVector<FieldDecl *, 12> All;
    FieldDecl *VType = Field("VType", Int64Ty);
    All.push_back(VType);
    llvm::SmallVector<Variant *, 12> Vs;
    for (auto &[Name, T] : AllVariants) {
      FieldDecl *F = Field(Name, T);
      All.push_back(F);
      FieldDecl *One[] = {F};
      auto *FL = Ctx.create<FieldList>();
      FL->Fixed = Ctx.copyArray<FieldDecl *>(One);
      auto *V = Ctx.create<Variant>();
      V->Fields = FL;
      Vs.push_back(V);
    }
    auto *VP = Ctx.create<VariantPart>();
    VP->Variants = Ctx.copyArray<Variant *>(Vs);
    FieldDecl *Fixed[] = {VType};
    auto *FL = Ctx.create<FieldList>();
    FL->Fixed = Ctx.copyArray<FieldDecl *>(Fixed);
    FL->Variant = VP;
    auto *Rec = Ctx.create<RecordType>(FL, Ctx.copyArray<FieldDecl *>(All), false);
    Rec->setDeclName("TVarRec");
    Standard.insert("tvarrec", Ctx.create<TypeDecl>(SourceLocation(),
                                                    memberName("TVarRec"), Rec));
    ArrayOfConstTy = Ctx.create<ArrayOfConstType>(Rec);
    std::pair<llvm::StringRef, int64_t> Kinds[] = {
        {"vtinteger", 0}, {"vtboolean", 1}, {"vtchar", 2}, {"vtextended", 3},
        {"vtstring", 4}, {"vtpointer", 5}, {"vtpchar", 6}, {"vtobject", 7},
        {"vtclass", 8}, {"vtansistring", 11}, {"vtint64", 16},
    };
    for (auto &[Name, V] : Kinds)
      Standard.insert(Name, Ctx.create<ConstDecl>(SourceLocation(),
                                                  predeclaredName(Name),
                                                  Ctx.getIntegerType(),
                                                  ConstValue::ordinal(V)));
  }

  /// TGUID = packed record case integer of
  ///   1: (Data1: longword; Data2, Data3: word; Data4: array[0..7] of byte);
  ///   2: (D1: longword; D2, D3: word; D4: array[0..7] of byte);
  ///   3: (time_low: longword; time_mid, time_hi_and_version: word;
  ///       clock_seq_hi_and_reserved, clock_seq_low: byte;
  ///       node: array[0..5] of byte);
  /// end; and PGUID = ^TGUID, as in Free Pascal's system unit (tp, objfpc).
  void declareTGUID() {
    auto Bytes = [&](int64_t Hi) {
      return Ctx.create<ArrayType>(
          Ctx.create<SubrangeType>(Ctx.getIntegerType(), 0, Hi),
          Ctx.getByteType(), false);
    };
    Type *LongWord = Ctx.getCardinalType(), *Word = Ctx.getWordType(),
         *Byte = Ctx.getByteType();
    std::pair<llvm::StringRef, Type *> V1[] = {
        {"Data1", LongWord}, {"Data2", Word}, {"Data3", Word},
        {"Data4", Bytes(7)}};
    std::pair<llvm::StringRef, Type *> V2[] = {
        {"D1", LongWord}, {"D2", Word}, {"D3", Word}, {"D4", Bytes(7)}};
    std::pair<llvm::StringRef, Type *> V3[] = {
        {"time_low", LongWord},
        {"time_mid", Word},
        {"time_hi_and_version", Word},
        {"clock_seq_hi_and_reserved", Byte},
        {"clock_seq_low", Byte},
        {"node", Bytes(5)}};
    llvm::SmallVector<FieldDecl *, 14> All;
    llvm::SmallVector<Variant *, 3> Vs;
    for (llvm::ArrayRef<std::pair<llvm::StringRef, Type *>> Fs :
         {llvm::ArrayRef(V1), llvm::ArrayRef(V2), llvm::ArrayRef(V3)}) {
      llvm::SmallVector<FieldDecl *, 6> Fixed;
      for (auto &[Name, T] : Fs) {
        auto *F = Ctx.create<FieldDecl>(memberName(Name), nullptr);
        F->setType(T);
        Fixed.push_back(F);
        All.push_back(F);
      }
      auto *FL = Ctx.create<FieldList>();
      FL->Fixed = Ctx.copyArray<FieldDecl *>(Fixed);
      auto *V = Ctx.create<Variant>();
      V->Fields = FL;
      Vs.push_back(V);
    }
    auto *VP = Ctx.create<VariantPart>();
    VP->Variants = Ctx.copyArray<Variant *>(Vs);
    auto *FL = Ctx.create<FieldList>();
    FL->Variant = VP;
    auto *Rec = Ctx.create<RecordType>(FL, Ctx.copyArray<FieldDecl *>(All),
                                       /*Packed=*/false);
    Rec->setDeclName("TGUID");
    TGuidTy = Rec;
    Standard.insert("tguid", Ctx.create<TypeDecl>(SourceLocation(),
                                                  memberName("TGUID"), Rec));
    auto *PGuid = Ctx.create<PointerType>(Rec);
    PGuid->setDeclName("PGUID");
    Standard.insert("pguid", Ctx.create<TypeDecl>(SourceLocation(),
                                                  memberName("PGUID"), PGuid));
  }

  Type *ArrayOfConstTy = nullptr;
  Type *TMethodTy = nullptr;
  RecordType *TGuidTy = nullptr; ///< the predeclared TGUID
  bool AddrOfWantsCode = false; ///< @TFoo.M for a pointer: its code address

  /// TMethod = record Code, Data: pointer; end (objfpc).
  void declareTMethod() {
    llvm::SmallVector<FieldDecl *, 2> All;
    for (llvm::StringRef Name : {"Code", "Data"}) {
      auto *F = Ctx.create<FieldDecl>(memberName(Name), nullptr);
      F->setType(Ctx.getUntypedPointerType());
      All.push_back(F);
    }
    auto *FL = Ctx.create<FieldList>();
    FL->Fixed = Ctx.copyArray<FieldDecl *>(All);
    auto *Rec = Ctx.create<RecordType>(FL, FL->Fixed, false);
    Rec->setDeclName("TMethod");
    Standard.insert("tmethod", Ctx.create<TypeDecl>(SourceLocation(),
                                                    memberName("TMethod"), Rec));
    TMethodTy = Rec;
  }
  Type *PCharTy = nullptr;

  /// class Exception (Free Pascal's SysUtils): private FMessage: string;
  /// constructor Create(const msg: string); property Message: string read
  /// FMessage write FMessage. Its VMT is in the runtime, which prints the
  /// message of an unhandled exception.
  void declareException() {
    ExceptionTy = Ctx.create<ClassType>();
    ExceptionTy->setDeclName("Exception");
    auto *Msg = Ctx.create<FieldDecl>(memberName("FMessage"), nullptr);
    Msg->setType(Ctx.getShortStringType());
    Msg->setVisibility(Visibility::Private);
    auto *Param = Ctx.create<VarDecl>(memberName("msg"), nullptr,
                                      VarDecl::ConstParam);
    Param->setType(Ctx.getShortStringType());
    VarDecl *Params[] = {Param};
    auto *Create = Ctx.create<RoutineDecl>(
        SourceLocation(), memberName("Create"), false,
        Ctx.copyArray<VarDecl *>(Params), true, nullptr, nullptr, false);
    Create->setMethodKind(MethodKind::Constructor);
    Create->setClass(ExceptionTy);
    Create->setBuiltin(TObjectMethod::ExceptionCreate);
    auto *Message = Ctx.create<PropertyDecl>(memberName("Message"), nullptr,
                                             memberName("FMessage"),
                                             memberName("FMessage"));
    Message->setType(Ctx.getShortStringType());
    Message->setAccessors(Msg, Msg);
    auto *FmtMsg = Ctx.create<VarDecl>(memberName("msg"), nullptr,
                                       VarDecl::ConstParam);
    FmtMsg->setType(Ctx.getShortStringType());
    auto *FmtArgs = Ctx.create<VarDecl>(memberName("args"), nullptr,
                                        VarDecl::ConstParam);
    FmtArgs->setType(ArrayOfConstTy);
    VarDecl *FmtParams[] = {FmtMsg, FmtArgs};
    auto *CreateFmt = Ctx.create<RoutineDecl>(
        SourceLocation(), memberName("CreateFmt"), false,
        Ctx.copyArray<VarDecl *>(FmtParams), true, nullptr, nullptr, false);
    CreateFmt->setMethodKind(MethodKind::Constructor);
    CreateFmt->setClass(ExceptionTy);
    CreateFmt->setBuiltin(TObjectMethod::ExceptionCreateFmt);
    Decl *Members[] = {Msg, Create, CreateFmt, Message};
    llvm::SmallVector<FieldDecl *, 2> Fields(TObjectTy->getAllFields());
    Fields.push_back(Msg);
    ExceptionTy->define(TObjectTy, Ctx.copyArray<Decl *>(Members),
                        Ctx.copyArray<FieldDecl *>(Fields),
                        TObjectTy->getVTable(), nullptr);
    Standard.insert("exception", Ctx.create<TypeDecl>(SourceLocation(),
                                                      memberName("Exception"),
                                                      ExceptionTy));
    // The exception for integer div and mod by zero, as in SysUtils.
    ClassType *External = declareExceptionClass("EExternal", ExceptionTy);
    ClassType *IntError = declareExceptionClass("EIntError", External);
    declareExceptionClass("EDivByZero", IntError);
    declareExceptionClass("EIntOverflow", IntError); // (objfpc overflow)
    // Tasks: a cancelled task's wait raises EOperationCancelled; tokens.
    declareExceptionClass("EOperationCancelled", ExceptionTy);
    Standard.insert("tcanceltoken",
                    Ctx.create<TypeDecl>(SourceLocation(), memberName("TCancelToken"),
                                         Ctx.getTokenType()));
    declareExceptionClass("EConvertError", ExceptionTy);
  }

  /// IUnknown = interface end (COM; also named IInterface: Free Pascal's
  /// QueryInterface, _AddRef and _Release are not declared, the compiled
  /// code counts references itself) and
  ///   TInterfacedObject = class(TObject, IUnknown)
  ///   protected FRefCount: longint;
  ///   public property RefCount: longint read FRefCount; end;
  /// Their iid and VMT are in the runtime.
  void declareInterfacedObject() {
    IUnknownTy = Ctx.create<InterfaceType>();
    IUnknownTy->setDeclName("IUnknown");
    IUnknownTy->setCOM(true);
    IUnknownTy->define(nullptr, {}, {}, nullptr);
    Standard.insert("iunknown", Ctx.create<TypeDecl>(SourceLocation(),
                                                     memberName("IUnknown"),
                                                     IUnknownTy));
    Standard.insert("iinterface", Ctx.create<TypeDecl>(SourceLocation(),
                                                       memberName("IInterface"),
                                                       IUnknownTy));

    TInterfacedObjectTy = Ctx.create<ClassType>();
    TInterfacedObjectTy->setDeclName("TInterfacedObject");
    auto *Count = Ctx.create<FieldDecl>(memberName("FRefCount"), nullptr);
    Count->setType(Ctx.getIntegerType());
    Count->setVisibility(Visibility::Protected);
    auto *RefCount = Ctx.create<PropertyDecl>(memberName("RefCount"), nullptr,
                                              memberName("FRefCount"),
                                              Identifier());
    RefCount->setType(Ctx.getIntegerType());
    RefCount->setAccessors(Count, nullptr);
    Decl *Members[] = {Count, RefCount};
    FieldDecl *Fields[] = {Count};
    TInterfacedObjectTy->define(TObjectTy, Ctx.copyArray<Decl *>(Members),
                                Ctx.copyArray<FieldDecl *>(Fields),
                                TObjectTy->getVTable(), nullptr);
    InterfaceImpl Impls[] = {{IUnknownTy, {}}};
    TInterfacedObjectTy->setInterfaces(Ctx.copyArray<InterfaceImpl>(Impls));
    Standard.insert("tinterfacedobject",
                    Ctx.create<TypeDecl>(SourceLocation(),
                                         memberName("TInterfacedObject"),
                                         TInterfacedObjectTy));
  }

  /// TSharedObject = class(TObject) end: a class derived from it says it is
  /// thread-safe (its methods synchronize what they share), so `shared`
  /// variables can hold its objects. Its VMT is in the runtime.
  void declareSharedObject() {
    TSharedObjectTy = Ctx.create<ClassType>();
    TSharedObjectTy->setDeclName("TSharedObject");
    TSharedObjectTy->define(TObjectTy, {}, TObjectTy->getAllFields(),
                            TObjectTy->getVTable(), nullptr);
    Identifier Id = memberName("TSharedObject");
    Standard.insert(Id.Name, Ctx.create<TypeDecl>(SourceLocation(), Id,
                                                  TSharedObjectTy));
  }

  /// A predeclared `Name = class(Parent);` (its VMT is in the runtime).
  ClassType *declareExceptionClass(llvm::StringRef Name, ClassType *Parent) {
    auto *C = Ctx.create<ClassType>();
    C->setDeclName(Name);
    C->define(Parent, {}, Parent->getAllFields(), Parent->getVTable(), nullptr);
    Identifier Id = memberName(Name);
    Standard.insert(Id.Name, Ctx.create<TypeDecl>(SourceLocation(), Id, C));
    return C;
  }

  /// Turbo/FPC builtins, predeclared only with --mode=tp.
  static bool isTPOnly(BuiltinKind K) {
    switch (K) {
    case BuiltinKind::Break: case BuiltinKind::Continue: case BuiltinKind::Exit:
    case BuiltinKind::Inc: case BuiltinKind::Dec: case BuiltinKind::Delete:
    case BuiltinKind::Insert: case BuiltinKind::SetLength: case BuiltinKind::Slice:
    case BuiltinKind::Length: case BuiltinKind::Copy:
    case BuiltinKind::Pos: case BuiltinKind::Concat: case BuiltinKind::UpCase:
    case BuiltinKind::GetMem: case BuiltinKind::FreeMem:
    case BuiltinKind::ReallocMem: case BuiltinKind::FillChar:
    case BuiltinKind::Move: case BuiltinKind::Halt: case BuiltinKind::SizeOf:
    case BuiltinKind::Low: case BuiltinKind::High: case BuiltinKind::Assigned:
    case BuiltinKind::ParamCount: case BuiltinKind::ParamStr:
    case BuiltinKind::Assign: case BuiltinKind::Close: case BuiltinKind::Flush:
    case BuiltinKind::BlockRead: case BuiltinKind::BlockWrite:
    case BuiltinKind::Seek: case BuiltinKind::FilePos:
    case BuiltinKind::FileSize: case BuiltinKind::GetFileHandle:
      return true;
    case BuiltinKind::AssignFile: case BuiltinKind::CloseFile:
    case BuiltinKind::ExceptAddr:
      return true; // (and objfpc only: see isObjFPCOnly)
    default:
      return false;
    }
  }

  /// Free Pascal's objpas built-ins, predeclared only with --mode=objfpc.
  static bool isObjFPCOnly(BuiltinKind K) {
    return K == BuiltinKind::AssignFile || K == BuiltinKind::CloseFile ||
           K == BuiltinKind::ExceptAddr || K == BuiltinKind::AddChecked ||
           K == BuiltinKind::SubChecked || K == BuiltinKind::MulChecked ||
           K == BuiltinKind::Wait || K == BuiltinKind::WhenAll ||
           K == BuiltinKind::WhenAny;
  }

  //===--------------------------------------------------------------------===//
  // Diagnostics helpers
  //===--------------------------------------------------------------------===//

  /// The visible name closest to Name (edit distance), for "did you mean".
  std::string suggest(llvm::StringRef Name) const {
    // Allow one edit for short names, else a third of the length (at least
    // two, so a swapped pair of letters is found).
    unsigned Best =
        (Name.size() <= 3 ? 1 : std::max<unsigned>(2, Name.size() / 3)) + 1;
    std::string Result;
    // Inner scopes win; within a scope, the closest name, and among equally
    // close ones the alphabetically first (independent of hash order, so
    // the self-hosted compiler can match it).
    for (const Scope *S = Cur; S; S = S->getParent()) {
      unsigned ScopeBest = Best;
      std::string ScopeResult;
      S->forEachName([&](llvm::StringRef Candidate) {
        // (labels; generic routines' keys, F<1>)
        if (Candidate.starts_with("#") || Candidate.ends_with(">") ||
            Candidate == Name)
          return;
        unsigned D = Name.edit_distance(Candidate, true, Best - 1);
        if (D < ScopeBest || (D == ScopeBest && D < Best && Candidate < ScopeResult)) {
          ScopeBest = D;
          ScopeResult = Candidate.str();
        }
      });
      if (ScopeBest < Best) {
        Best = ScopeBest;
        Result = ScopeResult;
      }
    }
    return Result;
  }

  std::string didYouMean(llvm::StringRef Name) const {
    std::string S = suggest(Name);
    if (S.empty())
      return "";
    // Show the name as it was declared (Point, not point).
    if (Decl *D = Cur->lookup(S); D && !D->getName().Spelling.empty())
      S = D->getName().Spelling.str();
    return "; did you mean '" + S + "'?";
  }

  void diagUndeclared(const Identifier &Id) {
    // In a record's static method, the record's instance members.
    for (RoutineDecl *R = CurRoutine; R; R = R->getParent())
      if (R->isStatic()) {
        if (Decl *D = R->getClass()->findMember(Id.Name)) {
          Diags.error(Id.Loc,
                      (isInstanceMember(D) ? "instance member '"
                                           : "class method '") +
                          Id.Spelling +
                          "' cannot be used in a static class method",
                      {Id.getRange()});
          noteDeclaredHere(D);
          return;
        }
        break;
      }
    if (isGenericName(Id.Name)) {
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a generic; give its type "
                  "arguments: specialize " + Id.Spelling + "<...>",
                  {Id.getRange()});
      return;
    }
    Diags.error(Id.Loc,
                "use of undeclared identifier '" + Id.Spelling + "'" +
                    didYouMean(Id.Name),
                {Id.getRange()});
  }

  /// A use (named Id) of D: warnings for its hint directives (deprecated,
  /// experimental, platform, library, unimplemented), as in Free Pascal.
  void warnHints(Decl *D, const Identifier &Id) {
    unsigned char H = D ? D->getHints() : 0;
    if (!H || !Diags.isWarningEnabled("deprecated"))
      return;
    std::string Name = "'" + D->getName().Spelling.str() + "'";
    auto Warn = [&](const std::string &What) {
      Diags.warning("deprecated", Id.Loc, Name + What, {Id.getRange()});
    };
    if (H & Decl::HintDeprecated)
      Warn(" is deprecated" + (D->getHintMessage().empty()
                                   ? std::string()
                                   : ": " + D->getHintMessage().str()));
    if (H & Decl::HintExperimental)
      Warn(" is experimental");
    if (H & Decl::HintPlatform)
      Warn(" is not portable (platform-specific)");
    if (H & Decl::HintLibrary)
      Warn(" belongs to a library");
    if (H & Decl::HintUnimplemented)
      Warn(" is not implemented");
    noteDeclaredHere(D);
  }

  void noteDeclaredHere(Decl *D) {
    if (!D->isPredeclared())
      Diags.note(D->getLocation(),
                 "'" + D->getName().Spelling + "' declared here");
  }

  void declare(Decl *D) {
    const Identifier &Id = D->getName();
    if (Decl *Prev = Cur->insert(Id.Name, D)) {
      Diags.error(Id.Loc, "redefinition of '" + Id.Spelling + "'",
                  {Id.getRange()});
      if (!Prev->isPredeclared())
        Diags.note(Prev->getLocation(), "previous definition is here");
    }
  }

  static std::string quoted(Type *T) { return "'" + T->getName() + "'"; }

  /// A routine's calling convention in a message (see CallConv).
  static const char *callConvDesc(bool CDecl) {
    return CDecl ? "'cdecl'" : "'stdcall' (the default)";
  }

  //===--------------------------------------------------------------------===//
  // Integer types (Turbo/FPC rules; ISO has only `integer`)
  //===--------------------------------------------------------------------===//

  /// Integer operands narrower than 32 bits widen to integer.
  Type *promoteInteger(Type *T) {
    auto *I = llvm::cast<IntegerType>(T->getBase());
    return I->getBits() < 32 ? Ctx.getIntegerType() : I;
  }

  /// Is E a constant that a cardinal can hold?
  bool isCardinalConstant(Expr *E) {
    auto C = fold(E);
    return C && C->Value.K == ConstValue::Ordinal && C->Value.Int >= 0 &&
           C->Value.Int <= UINT32_MAX;
  }

  /// Is E a constant of zero or more (that a qword can hold)?
  bool isNonNegativeConstant(Expr *E) {
    auto C = fold(E);
    return C && C->Value.K == ConstValue::Ordinal && C->Value.Int >= 0;
  }

  /// The type integer operands LE and RE are converted to for an operator:
  /// qword if one is qword and the other an int64 constant of zero or more;
  /// int64 if either is int64; qword if either is qword (the other, of 32
  /// bits or less, is converted to qword, as in FPC); cardinal if both are
  /// unsigned (or the other is a constant that fits); int64 when cardinal
  /// meets a signed value (so no value is lost, as in FPC); integer
  /// otherwise.
  Type *integerOperandType(Expr *LE, Type *L, Expr *RE, Type *R) {
    Type *PL = promoteInteger(L), *PR = promoteInteger(R);
    Type *I64 = Ctx.getInt64Type(), *Card = Ctx.getCardinalType();
    if ((isQWord(PL) && PR == I64 && isNonNegativeConstant(RE)) ||
        (isQWord(PR) && PL == I64 && isNonNegativeConstant(LE)))
      return Ctx.getQWordType();
    if (PL == I64 || PR == I64)
      return I64;
    if (isQWord(PL) || isQWord(PR))
      return Ctx.getQWordType();
    if (PL == Card && PR == Card)
      return Card;
    if (PL == Card || PR == Card) {
      Expr *Other = PL == Card ? RE : LE;
      Type *OtherTy = PL == Card ? R : L;
      if (!OtherTy->isSignedIntegerLike() || isCardinalConstant(Other))
        return Card;
      return I64;
    }
    return Ctx.getIntegerType();
  }

  /// Type-checks E once (later calls return the cached type).
  Type *check(Expr *E) {
    bool Allow = AllowUntyped;
    AllowUntyped = false;
    if (Type *Cached = E->getType())
      return Cached;
    Type *T = visitExpr(E);
    if (T->isUntyped() && !Allow) {
      auto *N = llvm::dyn_cast<NameRefExpr>(E);
      Diags.error(E->getBeginLoc(),
                  "'" + (N ? N->getName().Spelling : llvm::StringRef("?")) +
                      "' is an untyped parameter: use a typecast such as "
                      "longint(" +
                      (N ? N->getName().Spelling : llvm::StringRef("x")) +
                      "), its address, or pass it to an untyped parameter",
                  {E->getRange()});
      T = errorType();
    }
    E->setType(T);
    if (isManaged(T))
      markUsesManaged();
    return T;
  }

  /// Untyped parameters (Turbo/FPC  var x;  const x) can only be typecast,
  /// have their address taken, or be passed to untyped parameters (and to
  /// move and fillchar): those operands are checked with this.
  bool AllowUntyped = false;
  Type *checkAllowingUntyped(Expr *E) {
    AllowUntyped = true;
    Type *T = check(E);
    AllowUntyped = false;
    return T;
  }

  /// The current routine has a try statement (see hasTry).
  void markHasTry() {
    if (!CurRoutine)
      return;
    CurRoutine->setHasTry();
    if (RoutineDecl *C = CurRoutine->getCanonical())
      C->setHasTry();
  }

  /// The current routine computes a managed value (see usesManaged).
  void markUsesManaged() {
    if (!CurRoutine)
      return;
    CurRoutine->setUsesManaged();
    if (RoutineDecl *C = CurRoutine->getCanonical())
      C->setUsesManaged();
  }

  //===--------------------------------------------------------------------===//
  // Type relations (ISO 7185 6.4.5 / 6.4.6)
  //===--------------------------------------------------------------------===//

  /// Compatible types: identical, ordinal types with the same host, or
  /// string types of the same length.
  static bool areCompatible(Type *A, Type *B) {
    if (A == B)
      return true;
    if ((A->isPointer() && B->isNil()) || (A->isNil() && B->isPointer()))
      return true;
    if ((A->isClass() && B->isNil()) || (A->isNil() && B->isClass()))
      return true;
    if ((A->isDynArray() && B->isNil()) || (A->isNil() && B->isDynArray()))
      return true;
    if ((A->isClassRef() && B->isNil()) || (A->isNil() && B->isClassRef()))
      return true;
    if (A->isClassRef() && B->isClassRef()) { // related classes (objfpc)
      ClassType *CA = llvm::cast<ClassRefType>(A)->getClass();
      ClassType *CB = llvm::cast<ClassRefType>(B)->getClass();
      return CA->inheritsFrom(CB) || CB->inheritsFrom(CA);
    }
    if ((A->isInterface() && B->isNil()) || (A->isNil() && B->isInterface()))
      return true;
    if (A->isInterface() && B->isInterface()) { // related interfaces
      auto *IA = llvm::cast<InterfaceType>(A), *IB = llvm::cast<InterfaceType>(B);
      return IA->inheritsFrom(IB) || IB->inheritsFrom(IA);
    }
    if (A->isClass() && B->isClass()) // related classes (objfpc)
      return llvm::cast<ClassType>(A)->inheritsFrom(llvm::cast<ClassType>(B)) ||
             llvm::cast<ClassType>(B)->inheritsFrom(llvm::cast<ClassType>(A));
    if (A->isPointer() && B->isPointer() &&
        (llvm::cast<PointerType>(A)->isUntyped() ||
         llvm::cast<PointerType>(B)->isUntyped()))
      return true; // Turbo/FPC untyped `pointer`
    if (A->isSet() && B->isSet()) {
      auto *SA = llvm::cast<SetType>(A), *SB = llvm::cast<SetType>(B);
      return SA->isEmptySet() || SB->isEmptySet() ||
             haveSameOrdinalHost(SA->getElementType(), SB->getElementType());
    }
    if (A->isOrdinal() && B->isOrdinal())
      return haveSameOrdinalHost(A, B);
    if (A->isStringType() && B->isStringType())
      return A->getStringLength() == B->getStringLength();
    return false;
  }

  static bool isPChar(Type *T) {
    auto *P = llvm::dyn_cast<PointerType>(T);
    return P && !P->isUntyped() && P->getPointee() && P->getPointee()->isChar();
  }

  /// Are A and B tuple types with the same element types, in turn (the
  /// names may differ)? Such tuples have one layout.
  static bool sameTupleShape(Type *A, Type *B) {
    auto *RA = llvm::dyn_cast<RecordType>(A);
    auto *RB = llvm::dyn_cast<RecordType>(B);
    if (!RA || !RB || !RA->isTuple() || !RB->isTuple() ||
        RA->getAllFields().size() != RB->getAllFields().size())
      return false;
    for (size_t I = 0; I < RA->getAllFields().size(); ++I) {
      Type *TA = RA->getAllFields()[I]->getType();
      Type *TB = RB->getAllFields()[I]->getType();
      if (TA != TB && !sameTupleShape(TA, TB))
        return false;
    }
    return true;
  }

  /// Can a value of type Value be assigned to a variable of type Target?
  static bool isAssignable(Type *Target, Type *Value) {
    if (Target->isOpenArray() || Value->isOpenArray())
      return false; // only passed along and indexed
    if (containsFile(Target) || containsFile(Value))
      return false; // files cannot be copied
    if (Target == Value)
      return true;
    if (sameTupleShape(Target, Value))
      return true;
    if (Target->isReal() && Value->isIntegerLike())
      return true;
    if (Target->isPointer() && (Value->isNil() || Value->isPointer()))
      return areCompatible(Target, Value);
    // Classes (objfpc): an object of the class or a descendant, or nil; any
    // object fits an untyped pointer.
    if (auto *TC = llvm::dyn_cast<ClassType>(Target))
      return Value->isNil() || (Value->isClass() &&
                                llvm::cast<ClassType>(Value)->inheritsFrom(TC));
    // Interfaces: nil, a descendant interface, an object of a class that
    // implements it (listed by the class or an ancestor).
    if (auto *TI = llvm::dyn_cast<InterfaceType>(Target)) {
      if (Value->isNil())
        return true;
      if (auto *VI = llvm::dyn_cast<InterfaceType>(Value))
        return VI->inheritsFrom(TI);
      if (auto *VC = llvm::dyn_cast<ClassType>(Value))
        return VC->findInterface(TI) != nullptr;
      return false;
    }
    // Class references: the class or a descendant, or nil.
    if (auto *TR = llvm::dyn_cast<ClassRefType>(Target))
      return Value->isNil() ||
             (Value->isClassRef() &&
              llvm::cast<ClassRefType>(Value)->getClass()->inheritsFrom(
                  TR->getClass()));
    if (Target->isPointer() && (Value->isClass() || Value->isClassRef()))
      return llvm::cast<PointerType>(Target)->isUntyped();
    if (Target->isDynArray() || Target->isTask() || Target->isToken())
      return Value->isNil();
    if (Target->isShortString() && Value->isStringish())
      return true; // truncated to the target's length at runtime
    if (Target->isAnsiString())
      return Value->isStringish() || isPChar(Value);
    if (Target->isSet() && Value->isSet())
      return areCompatible(Target, Value); // range-checked at runtime
    // Procedural values (Turbo/FPC): congruent headings, or nil.
    if (auto *TR = llvm::dyn_cast<RoutineType>(Target)) {
      if (Value->isNil())
        return true;
      auto *VR = llvm::dyn_cast<RoutineType>(Value);
      return VR && VR->isCDecl() == TR->isCDecl() &&
             VR->isOfObject() == TR->isOfObject() &&
             VR->isReference() == TR->isReference() &&
             congruent(TR->getParams(), TR->getResult(), VR->getParams(),
                       VR->getResult());
    }
    if (Target->isOrdinal() && Value->isOrdinal())
      return haveSameOrdinalHost(Target, Value); // range-checked at runtime
    if (Target->isStringType() && Value->isStringType() &&
        !Target->isStringLiteral())
      return Target->getStringLength() == Value->getStringLength();
    return false;
  }

  /// Can E be used where a variable is required (assignment target, `var`
  /// argument, with record)?
  static bool isVariableReference(Expr *E) {
    if (isBitField(E)) // assignable, but without an address
      return false;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
      Decl *D = N->getDecl();
      return D && (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D));
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
      if (I->getBase()->getType() && (I->getBase()->getType()->isDynArray() ||
                                      I->getBase()->getType()->isPointer()))
        return !I->getProperty(); // on the heap, like p^; p[i]
      return !I->getProperty() && isVariableReference(I->getBase());
    }
    if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
      if (llvm::isa_and_nonnull<VarDecl>(F->getMember()))
        return true; // a class variable
      // obj.f is a variable like p^.f, whatever obj is (objfpc).
      if (isObject(F->getBase()))
        return llvm::isa_and_nonnull<FieldDecl>(F->getMember());
      return isVariableReference(F->getBase());
    }
    // T(x) of an untyped parameter x (Turbo/FPC)
    if (auto *C = llvm::dyn_cast<CallExpr>(E))
      return C->isVarCast() || C->isMethodCastOfVariable();
    // p^ is a variable even when p is a function result.
    return llvm::isa<DerefExpr>(E);
  }

  static bool isBitField(Expr *E) { return isBitPackedComponent(E); }

  /// E (a bit field) where an address is needed.
  void diagBitFieldAddress(Expr *E) {
    Diags.error(E->getBeginLoc(),
                "the elements and fields of bitpacked arrays and records have "
                "no address (they cannot be passed as 'var' parameters or used "
                "with '@')",
                {E->getRange()});
  }

  /// Does T have a record laid out with a field alignment (packed, or
  /// {$packrecords N}): itself, an element, a field?
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

  /// A type in T whose values a constant's byte image cannot hold (one that
  /// is not an ordinal, real or short string, or an array or record of
  /// those), or null. (The constants of records laid out with a field
  /// alignment are their bytes.)
  static Type *notByteImage(Type *T) {
    if (auto *A = llvm::dyn_cast<ArrayType>(T))
      return notByteImage(A->getElementType());
    if (auto *R = llvm::dyn_cast<RecordType>(T)) {
      for (FieldDecl *F : R->getAllFields())
        if (Type *Bad = F->getType() ? notByteImage(F->getType()) : nullptr)
          return Bad;
      return nullptr;
    }
    if (T->isOrdinal() || T->isReal() || T->isShortString() || T->isError())
      return nullptr;
    return T;
  }

  /// Does T have a bitpacked part (itself, an element, a field)?
  static bool containsBitPacked(Type *T) {
    if (auto *A = llvm::dyn_cast<ArrayType>(T))
      return A->isBitPacked() || containsBitPacked(A->getElementType());
    if (auto *R = llvm::dyn_cast<RecordType>(T)) {
      if (R->isBitPacked())
        return true;
      for (FieldDecl *F : R->getAllFields())
        if (F->getType() && containsBitPacked(F->getType()))
          return true;
    }
    return false;
  }

  /// Does a value of the non-ordinal type T need no alignment (it can sit
  /// at any byte of a bitpacked record)?
  static bool isByteAligned(Type *T) {
    if (T->isOrdinal()) // as an array's element: stored in a byte?
      return T->getBase()->isBoolean() || T->getBase()->isChar() ||
             (T->isInteger() && llvm::cast<IntegerType>(T)->getBits() == 8);
    if (T->isShortString())
      return true;
    if (auto *R = llvm::dyn_cast<RecordType>(T))
      return R->isBitPacked();
    if (auto *A = llvm::dyn_cast<ArrayType>(T))
      return A->hasBitElements() || isByteAligned(A->getElementType());
    return false;
  }

  /// Is E a variable a type helper's method can change (not a method's
  /// result: r.M, r.M.f)?
  static bool isHelperVariable(Expr *E) {
    if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
      if (llvm::isa_and_nonnull<VarDecl>(F->getMember()))
        return true;
      if (!llvm::isa_and_nonnull<FieldDecl>(F->getMember()))
        return false;
      return isObject(F->getBase()) || isHelperVariable(F->getBase());
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(E);
        I && !I->getProperty() && I->getBase()->getType() &&
        !I->getBase()->getType()->isDynArray() &&
        !I->getBase()->getType()->isPointer())
      return isHelperVariable(I->getBase());
    return isVariableReference(E);
  }

  /// Is E (already checked) a value of a class type?
  static bool isObject(Expr *E) {
    return E->getType() && E->getType()->isClass();
  }

  /// The `const` parameter a designator is part of (x, x[i], x.f; not x^ or
  /// obj.f, whose target is a different variable), if any.
  static VarDecl *constParamRoot(Expr *E) {
    while (true) {
      if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
        if (I->getProperty() || I->getBase()->getType()->isDynArray() ||
            I->getBase()->getType()->isPointer())
          return nullptr; // (a dynamic array's elements are not the parameter)
        E = I->getBase();
      } else if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
        if (isObject(F->getBase()) ||
            llvm::isa_and_nonnull<VarDecl>(F->getMember()))
          return nullptr;
        E = F->getBase();
      } else if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
        E = P->getInner();
      } else if (auto *C = llvm::dyn_cast<CallExpr>(E);
                 C && (C->isVarCast() || C->isMethodCast())) {
        E = C->getArgs()[0]; // T(x): x
      } else {
        auto *N = llvm::dyn_cast<NameRefExpr>(E);
        auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
        return V && (V->isConstParam() || V->isReadOnly()) ? V : nullptr;
      }
    }
  }

  /// Reports a change to a `const` parameter. Returns false if reported.
  bool checkModifiable(Expr *E) {
    if (VarDecl *OV = ownedVarOf(E); OV && E != OwnedTarget) {
      // (the callee could replace the object without freeing it)
      Diags.error(E->getBeginLoc(),
                  "owned variable '" + OV->getName().Spelling +
                      "' cannot be passed as a variable parameter",
                  {E->getRange()});
      return false;
    }
    VarDecl *V = constParamRoot(E);
    if (!V)
      return true;
    if (V->getOwner() && V->getOwner()->isCopyFactory())
      Diags.error(E->getBeginLoc(),
                  "cannot modify '" + V->getName().Spelling +
                      "', a copy made by this lambda (copies are read-only)",
                  {E->getRange()});
    else
      Diags.error(E->getBeginLoc(),
                  llvm::Twine(V->isReadOnly() ? "cannot modify constant '"
                                              : "cannot modify constant parameter '") +
                      V->getName().Spelling + "'",
                  {E->getRange()});
    noteDeclaredHere(V);
    return false;
  }

  //===--------------------------------------------------------------------===//
  // Constant folding
  //===--------------------------------------------------------------------===//

  /// ISO 7185 6.3 constant: [sign] (number | constant-identifier) | string.
  static bool isISOConstantForm(Expr *E) {
    if (auto *U = llvm::dyn_cast<UnaryExpr>(E)) {
      if (U->getOp() == UnaryOp::Not)
        return false;
      E = U->getOperand();
      return llvm::isa<IntegerLiteralExpr>(E) || llvm::isa<RealLiteralExpr>(E) ||
             llvm::isa<NameRefExpr>(E);
    }
    return llvm::isa<IntegerLiteralExpr>(E) || llvm::isa<RealLiteralExpr>(E) ||
           llvm::isa<StringLiteralExpr>(E) || llvm::isa<NameRefExpr>(E);
  }

  static double asReal(const Constant &C) {
    if (C.Value.K == ConstValue::Real)
      return C.Value.RealVal;
    if (C.Ty && isQWord(C.Ty->getBase()))
      return static_cast<double>(static_cast<uint64_t>(C.Value.Int));
    return static_cast<double>(C.Value.Int);
  }

  /// Is a value of type T kept as the 64 bits of a qword (read unsigned)?
  static bool isQWordValue(Type *T) { return T && isQWord(T->getBase()); }

  /// Does constant V of type VTy lie within T's values? (qword values are
  /// unsigned.)
  static bool constantInRange(int64_t V, Type *VTy, Type *T) {
    if (VTy->getBase()->isWideChar() && T->getBase()->isChar())
      V = wideToChar(V);
    bool Unsigned = isQWordValue(VTy);
    if (isQWord(T))
      return Unsigned || V >= 0;
    if (Unsigned && V < 0) // 2^63 or more
      return false;
    OrdinalBounds Bd = getOrdinalBounds(T);
    return V >= Bd.Lo && V <= Bd.Hi;
  }

  /// Folds an already type-checked expression. Returns nullopt if it is not
  /// constant (without diagnosing).
  std::optional<Constant> fold(Expr *E) {
    Type *T = E->getType();
    if (!T || T->isError())
      return std::nullopt;
    switch (E->getKind()) {
    case Expr::EK_IntegerLiteral:
      return Constant{T, ConstValue::ordinal(static_cast<int64_t>(
                             llvm::cast<IntegerLiteralExpr>(E)->getValue()))};
    case Expr::EK_RealLiteral:
      return Constant{T, ConstValue::real(llvm::cast<RealLiteralExpr>(E)->getValue())};
    case Expr::EK_StringLiteral: {
      auto *SL = llvm::cast<StringLiteralExpr>(E);
      if (SL->isWideCharLiteral())
        return Constant{T, ConstValue::ordinal(SL->getWideCode())};
      llvm::StringRef S = SL->getValue();
      if (S.size() == 1)
        return Constant{T, ConstValue::ordinal(static_cast<uint8_t>(S[0]))};
      return Constant{T, ConstValue::string(S)};
    }
    case Expr::EK_NameRef: {
      auto *C = llvm::dyn_cast_or_null<ConstDecl>(
          llvm::cast<NameRefExpr>(E)->getDecl());
      if (!C || !C->getValue().isValid())
        return std::nullopt;
      return Constant{C->getType(), C->getValue()};
    }
    case Expr::EK_Paren:
      return fold(llvm::cast<ParenExpr>(E)->getInner());
    case Expr::EK_Field: // TFoo.C
      if (NameRefExpr *Ref = llvm::cast<FieldExpr>(E)->getConstRef())
        return fold(Ref);
      return std::nullopt;
    case Expr::EK_Unary: {
      auto *U = llvm::cast<UnaryExpr>(E);
      auto V = fold(U->getOperand());
      if (!V)
        return std::nullopt;
      switch (U->getOp()) {
      case UnaryOp::Plus:
        return Constant{T, V->Value};
      case UnaryOp::Minus:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(-V->Value.RealVal)};
        return Constant{T, ConstValue::ordinal(static_cast<int64_t>(
                               0 - static_cast<uint64_t>(V->Value.Int)))};
      case UnaryOp::Not: // boolean not, or bitwise in the operand's type
        if (T->getBase()->isBoolean())
          return Constant{T, ConstValue::ordinal(!V->Value.Int)};
        return Constant{T, ConstValue::ordinal(castOrdinalValue(~V->Value.Int, T, T))};
      }
      return std::nullopt;
    }
    case Expr::EK_Binary: {
      auto *B = llvm::cast<BinaryExpr>(E);
      auto L = fold(B->getLHS()), R = fold(B->getRHS());
      if (!L || !R || L->Value.K == ConstValue::String ||
          R->Value.K == ConstValue::String)
        return std::nullopt;
      if (!T->isOrdinal() && !T->isReal())
        return std::nullopt; // 'a' + 'b': a string (not folded)
      bool IsReal = L->Value.K == ConstValue::Real || R->Value.K == ConstValue::Real;
      int64_t A = L->Value.Int, C = R->Value.Int;
      double X = asReal(*L), Y = asReal(*R);
      auto Ord = [&](int64_t V) { return Constant{T, ConstValue::ordinal(V)}; };
      auto Real = [&](double V) { return Constant{T, ConstValue::real(V)}; };
      // Wrap around in 64 bits (no undefined behavior); results that do not
      // fit their type are reported by evaluateConstant.
      auto UA = static_cast<uint64_t>(A), UC = static_cast<uint64_t>(C);
      bool Wide = T->isIntegerLike() &&
                  llvm::cast<IntegerType>(T->getBase())->getBits() == 64;
      // qword operands: div, mod and comparisons are unsigned.
      bool U64 = !IsReal && B->getOperandType() && isQWord(B->getOperandType());
      switch (B->getOp()) {
      case BinaryOp::Add: return IsReal ? Real(X + Y) : Ord(static_cast<int64_t>(UA + UC));
      case BinaryOp::Sub: return IsReal ? Real(X - Y) : Ord(static_cast<int64_t>(UA - UC));
      case BinaryOp::Mul: return IsReal ? Real(X * Y) : Ord(static_cast<int64_t>(UA * UC));
      case BinaryOp::RealDiv:
        if (Y == 0)
          return std::nullopt;
        return Real(X / Y);
      case BinaryOp::IntDiv:
        if (U64)
          return UC == 0 ? std::nullopt
                         : std::optional(Ord(static_cast<int64_t>(UA / UC)));
        if (C == 0 || (A == INT64_MIN && C == -1))
          return std::nullopt;
        return Ord(A / C);
      case BinaryOp::Mod:
        if (U64)
          return UC == 0 ? std::nullopt
                         : std::optional(Ord(static_cast<int64_t>(UA % UC)));
        if (C <= 0)
          return std::nullopt;
        return Ord(((A % C) + C) % C);
      case BinaryOp::And: return T->isBoolean() ? Ord(A && C) : Ord(A & C);
      case BinaryOp::Or:  return T->isBoolean() ? Ord(A || C) : Ord(A | C);
      case BinaryOp::Xor: return Ord(A ^ C);
      case BinaryOp::Shl:
        if (Wide)
          return Ord(static_cast<int64_t>(UA << (C & 63)));
        return Ord(static_cast<int32_t>(static_cast<uint32_t>(A) << (C & 31)));
      case BinaryOp::Shr:
        if (Wide)
          return Ord(static_cast<int64_t>(UA >> (C & 63)));
        return Ord(static_cast<int32_t>(static_cast<uint32_t>(A) >> (C & 31)));
      case BinaryOp::Eq:  return Ord(IsReal ? X == Y : A == C);
      case BinaryOp::Ne:  return Ord(IsReal ? X != Y : A != C);
      case BinaryOp::Lt:  return Ord(IsReal ? X < Y : U64 ? UA < UC : A < C);
      case BinaryOp::Le:  return Ord(IsReal ? X <= Y : U64 ? UA <= UC : A <= C);
      case BinaryOp::Gt:  return Ord(IsReal ? X > Y : U64 ? UA > UC : A > C);
      case BinaryOp::Ge:  return Ord(IsReal ? X >= Y : U64 ? UA >= UC : A >= C);
      case BinaryOp::In: case BinaryOp::Is: case BinaryOp::As:
        return std::nullopt;
      }
      return std::nullopt;
    }
    case Expr::EK_Call: {
      auto *Call = llvm::cast<CallExpr>(E);
      if (llvm::isa_and_nonnull<TypeDecl>(Call->getDecl())) { // T(x)
        auto V = fold(Call->getArgs()[0]);
        if (!V || V->Value.K != ConstValue::Ordinal || !T->isOrdinal())
          return std::nullopt;
        return Constant{T, ConstValue::ordinal(
                               castOrdinalValue(V->Value.Int, V->Ty, T))};
      }
      auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(Call->getDecl());
      if (!BD || Call->getArgs().size() != 1)
        return std::nullopt;
      if (BD->getBuiltinKind() == BuiltinKind::Low ||
          BD->getBuiltinKind() == BuiltinKind::High) {
        bool IsHigh = BD->getBuiltinKind() == BuiltinKind::High;
        Type *AT = Call->getArgs()[0]->getType();
        if (AT->isArrayOfConst() || AT->isOpenArray() || AT->isDynArray())
          return std::nullopt; // known only when the program runs
        if (auto *SS = llvm::dyn_cast<ShortStringType>(AT))
          return Constant{T, ConstValue::ordinal(IsHigh ? SS->getMaxLength() : 0)};
        if (auto *A = llvm::dyn_cast<ArrayType>(AT))
          AT = A->getIndexType();
        OrdinalBounds Bd = getOrdinalBounds(AT);
        if (IsHigh && isQWord(AT)) // 2^64-1: its 64 bits
          return Constant{T, ConstValue::ordinal(-1)};
        return Constant{T, ConstValue::ordinal(IsHigh ? Bd.Hi : Bd.Lo)};
      }
      auto V = fold(Call->getArgs()[0]);
      if (!V || V->Value.K == ConstValue::String)
        return std::nullopt;
      auto Ord = [&](int64_t X) { return Constant{T, ConstValue::ordinal(X)}; };
      // Integer results wrap around in 64 bits (no undefined behavior).
      auto U = static_cast<uint64_t>(V->Value.Int);
      switch (BD->getBuiltinKind()) {
      case BuiltinKind::Ord: case BuiltinKind::Chr: return Ord(V->Value.Int);
      case BuiltinKind::Succ: return Ord(static_cast<int64_t>(U + 1));
      case BuiltinKind::Pred: return Ord(static_cast<int64_t>(U - 1));
      case BuiltinKind::Odd:  return Ord(V->Value.Int & 1);
      case BuiltinKind::Abs:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(std::fabs(V->Value.RealVal))};
        if (isQWordValue(V->Ty)) // already non-negative
          return Ord(V->Value.Int);
        return Ord(V->Value.Int < 0 ? static_cast<int64_t>(0 - U) : V->Value.Int);
      case BuiltinKind::Sqr:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(V->Value.RealVal * V->Value.RealVal)};
        return Ord(static_cast<int64_t>(U * U));
      case BuiltinKind::Trunc:
      case BuiltinKind::Round: {
        // ISO rounds halves away from zero; Turbo/FPC to even (as codegen).
        double X = asReal(*V);
        if (BD->getBuiltinKind() == BuiltinKind::Round)
          X = isISO() ? std::round(X) : std::nearbyint(X);
        else
          X = std::trunc(X);
        if (!(X >= -9223372036854775808.0 && X < 9223372036854775808.0))
          return std::nullopt; // out of range: left to the runtime check
        return Ord(static_cast<int64_t>(X));
      }
      default: return std::nullopt;
      }
    }
    default:
      return std::nullopt;
    }
  }

  /// Checks and folds a constant expression in a declaration (const value,
  /// subrange bound, case/variant label). Reports non-constants and, in ISO
  /// mode, constant expressions beyond the ISO `constant` syntax.
  std::optional<Constant> evaluateConstant(Expr *E, llvm::StringRef What) {
    Type *T = check(E);
    if (T->isError())
      return std::nullopt;
    auto C = fold(E);
    if (!C) {
      Diags.error(E->getBeginLoc(), What + " must be a constant",
                  {E->getRange()});
      return std::nullopt;
    }
    if (isISO() && !isISOConstantForm(E))
      Diags.error(E->getBeginLoc(),
                  "constant expressions are a Turbo/Free Pascal extension; "
                  "use --mode=tp",
                  {E->getRange()});
    if (C->Value.K == ConstValue::Ordinal && C->Ty->isIntegerLike() &&
        !isQWordValue(C->Ty)) { // (any 64 bits are a qword)
      OrdinalBounds Bd = getOrdinalBounds(C->Ty->getBase());
      if (C->Value.Int < Bd.Lo || C->Value.Int > Bd.Hi) {
        // Turbo/FPC: integer constant expressions widen to int64.
        if (!isISO() && C->Ty->getBase()->isSignedIntegerLike()) {
          C->Ty = Ctx.getInt64Type();
          return C;
        }
        Diags.error(E->getBeginLoc(),
                    "constant value is out of range for " +
                        C->Ty->getBase()->getName(),
                    {E->getRange()});
        return std::nullopt;
      }
    }
    return C;
  }

  /// The value of a typecast T(V) of a value of type From: integer types
  /// wrap to their width (byte(300) = 44), and widechar to 16 bits; a
  /// widechar above 255 is '?' as a char (as Free Pascal converts it);
  /// other ordinal types keep the value.
  static int64_t castOrdinalValue(int64_t V, Type *From, Type *T) {
    if (T->getBase()->isWideChar())
      return V & 0xFFFF;
    if (T->getBase()->isChar() && From->getBase()->isWideChar())
      return wideToChar(V);
    auto *I = llvm::dyn_cast<IntegerType>(T->getBase());
    if (!I || I->getBits() == 64)
      return V;
    uint64_t Mask = (uint64_t(1) << I->getBits()) - 1;
    uint64_t U = static_cast<uint64_t>(V) & Mask;
    if (I->isSigned() && (U >> (I->getBits() - 1)))
      U |= ~Mask; // sign-extend
    return static_cast<int64_t>(U);
  }

  /// A widechar's code as a char: '?' above 255 (Free Pascal's
  /// conversion).
  static int64_t wideToChar(int64_t V) { return V > 255 ? '?' : V; }

  /// A widechar constant above 255 that becomes a char or a string: a
  /// warning, as Free Pascal gives ('?' in its place).
  void checkWideToChar(Type *Target, Expr *E) {
    if (!E->getType() || !E->getType()->getBase()->isWideChar() ||
        !(Target->getBase()->isChar() ||
          (Target->isStringish() && !Target->isCharLike())))
      return;
    auto C = fold(E);
    if (C && C->Value.K == ConstValue::Ordinal && C->Value.Int > 255)
      Diags.warning(E->getBeginLoc(),
                    "the widechar #" + std::to_string(C->Value.Int) +
                        " becomes '?' in " + quoted(Target),
                    {E->getRange()});
  }

  static std::string formatValue(Type *T, int64_t V) {
    Type *B = T->getBase();
    if (B->isWideChar() && (V > 127 || !llvm::isPrint(static_cast<char>(V))))
      return "#" + std::to_string(V);
    if (B->isChar() || B->isWideChar())
      return llvm::isPrint(static_cast<char>(V))
                 ? "'" + std::string(1, static_cast<char>(V)) + "'"
                 : "chr(" + std::to_string(V) + ")";
    if (B->isBoolean())
      return V ? "true" : "false";
    if (auto *E = llvm::dyn_cast<EnumType>(B))
      if (V >= 0 && static_cast<size_t>(V) < E->getValues().size())
        return E->getValues()[V]->getName().Spelling.str();
    if (isQWord(B))
      return std::to_string(static_cast<uint64_t>(V));
    return std::to_string(V);
  }

  //===--------------------------------------------------------------------===//
  // Types
  //===--------------------------------------------------------------------===//

  Type *resolveType(TypeSpec *TS, llvm::StringRef DeclName = {}) {
    if (Type *T = TS->getResolved())
      return T;
    Type *T = buildType(TS);
    auto *Rec = llvm::dyn_cast<RecordType>(T);
    if (!DeclName.empty() && !T->isError() && !llvm::isa<BuiltinType>(T) &&
        !llvm::isa<SpecializeTypeSpec>(TS) && // (so is a specialization)
        !(Rec && Rec->isTuple()) && // (a tuple type is shared by its uses)
        !T->isTask())               // (and so is a task type)
      T->setDeclName(DeclName);
    TS->setResolved(T);
    return T;
  }

  Type *buildType(TypeSpec *TS) {
    switch (TS->getKind()) {
    case TypeSpec::TS_ArrayOfConst:
      return ArrayOfConstTy;
    case TypeSpec::TS_DynArray: {
      auto *DS = llvm::cast<DynArrayTypeSpec>(TS);
      Type *Elem = resolveType(DS->getElement());
      if (Elem->isError())
        return errorType();
      if (containsFile(Elem)) {
        Diags.error(DS->getElement()->getBeginLoc(),
                    "the elements of a dynamic array cannot be files",
                    {DS->getElement()->getRange()});
        return errorType();
      }
      return Ctx.getDynArrayType(Elem);
    }
    case TypeSpec::TS_OpenArray: {
      auto *OS = llvm::cast<OpenArrayTypeSpec>(TS);
      Type *Elem = resolveType(OS->getElement());
      if (Elem->isError())
        return errorType();
      if (containsFile(Elem)) {
        Diags.error(OS->getElement()->getBeginLoc(),
                    "the elements of an open array cannot be files",
                    {OS->getElement()->getRange()});
        return errorType();
      }
      return Ctx.getOpenArrayType(Elem);
    }
    case TypeSpec::TS_Untyped:
      return Ctx.getUntypedType();
    case TypeSpec::TS_Named: {
      const Identifier &Id = llvm::cast<NamedTypeSpec>(TS)->getName();
      if (auto Quals = llvm::cast<NamedTypeSpec>(TS)->getQualifiers();
          !Quals.empty()) { // TOuter.TInner
        Decl *D = lookupMemberPath(Quals, Id);
        if (!D)
          return errorType();
        if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) {
          warnHints(TD, Id);
          return TD->getType() ? TD->getType() : errorType();
        }
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' is a " + D->getKindName() +
                        ", not a type",
                    {Id.getRange()});
        noteDeclaredHere(D);
        return errorType();
      }
      Decl *D = Cur->lookup(Id.Name);
      if (llvm::isa_and_nonnull<TypeDecl>(D))
        warnHints(D, Id);
      if (!D) {
        if (isFutureBuiltin(Id.Name))
          Diags.error(Id.Loc, "type '" + Id.Spelling + "' is not supported yet",
                      {Id.getRange()});
        else if (isGenericName(Id.Name))
          Diags.error(Id.Loc,
                      "'" + Id.Spelling + "' is a generic; give its type "
                      "arguments: specialize " + Id.Spelling + "<...>",
                      {Id.getRange()});
        else
          Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'" +
                        didYouMean(Id.Name),
                      {Id.getRange()});
        return errorType();
      }
      if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) {
        if (auto *C = llvm::dyn_cast_or_null<ClassType>(TD->getType());
            C && C->isHelper()) {
          Diags.error(Id.Loc,
                      C->helperKindName() + " '" + Id.Spelling +
                          "' cannot be used as a type",
                      {Id.getRange()});
          return errorType();
        }
        return TD->getType() ? TD->getType() : errorType();
      }
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a type",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }

    case TypeSpec::TS_Specialize: {
      auto *SS = llvm::cast<SpecializeTypeSpec>(TS);
      TypeDecl *I = specializeType(SS->getName(), SS->getArgs());
      if (!I)
        return errorType();
      if (I->getType())
        return I->getType();
      // The specialization being declared, inside itself: as its name.
      Identifier Id = I->getName();
      Id.Loc = SS->getName().Loc;
      return resolveType(Ctx.create<NamedTypeSpec>(Id));
    }

    case TypeSpec::TS_Tuple: {
      auto *TS2 = llvm::cast<TupleTypeSpec>(TS);
      llvm::SmallVector<Type *, 4> Types;
      llvm::SmallVector<Identifier, 4> Names;
      bool OK = true;
      for (const TupleElementSpec &El : TS2->getElements()) {
        Type *T = resolveType(El.Type);
        if (T->isError()) {
          OK = false;
        } else if (containsFile(T)) {
          Diags.error(El.Type->getBeginLoc(),
                      "the elements of a tuple cannot be files",
                      {El.Type->getRange()});
          OK = false;
        }
        Identifier Name = El.Name;
        if (!Name.isValid())
          Name = tupleItemName(Names.size() + 1, El.Type->getBeginLoc());
        for (const Identifier &Prev : Names)
          if (Prev.Name == Name.Name) {
            Diags.error(El.Name.isValid() ? El.Name.Loc
                                          : El.Type->getBeginLoc(),
                        "duplicate element name '" + Name.Spelling + "'",
                        {El.Type->getRange()});
            OK = false;
            break;
          }
        Types.push_back(T);
        Names.push_back(Name);
      }
      if (!OK)
        return errorType();
      return getTupleType(Types, Names);
    }

    case TypeSpec::TS_Enum: {
      auto *ES = llvm::cast<EnumTypeSpec>(TS);
      // (integer, string): every name is a type's, so it is a tuple type.
      if (TupleTypeSpec *TT = ES->getTuple()) {
        bool AllTypes = true;
        for (const Identifier &Id : ES->getNames())
          AllTypes &= llvm::isa_and_nonnull<TypeDecl>(Cur->lookup(Id.Name));
        if (AllTypes)
          return resolveType(TT);
      }
      auto *E = Ctx.create<EnumType>();
      llvm::SmallVector<ConstDecl *, 8> Values;
      for (const Identifier &Id : ES->getNames()) {
        auto *C = Ctx.create<ConstDecl>(
            Id.Loc, Id, E, ConstValue::ordinal(static_cast<int64_t>(Values.size())));
        if (!ES->isScoped()) {
          declare(C);
        } else { // reached only as TColor.Red: not in the scope
          for (ConstDecl *Prev : Values)
            if (Prev->getName().Name == Id.Name) {
              Diags.error(Id.Loc, "redefinition of '" + Id.Spelling + "'",
                          {Id.getRange()});
              Diags.note(Prev->getLocation(), "previous definition is here");
              break;
            }
        }
        Values.push_back(C);
      }
      E->setValues(Ctx.copyArray<ConstDecl *>(Values));
      return E;
    }

    case TypeSpec::TS_Subrange: {
      auto *SS = llvm::cast<SubrangeTypeSpec>(TS);
      auto Lo = evaluateConstant(SS->getLo(), "subrange bound");
      auto Hi = evaluateConstant(SS->getHi(), "subrange bound");
      if (!Lo || !Hi)
        return errorType();
      if (!Lo->Ty->isOrdinal() || !Hi->Ty->isOrdinal()) {
        Diags.error(SS->getBeginLoc(),
                    "subrange bounds must be of an ordinal type",
                    {SS->getRange()});
        return errorType();
      }
      if (!haveSameOrdinalHost(Lo->Ty, Hi->Ty)) {
        Diags.error(SS->getBeginLoc(),
                    "subrange bounds have different types (" +
                        quoted(Lo->Ty->getBase()) + " and " +
                        quoted(Hi->Ty->getBase()) + ")",
                    {SS->getRange()});
        return errorType();
      }
      if (Lo->Value.Int > Hi->Value.Int) {
        Diags.error(SS->getBeginLoc(),
                    "subrange lower bound " +
                        formatValue(Lo->Ty, Lo->Value.Int) +
                        " is greater than upper bound " +
                        formatValue(Hi->Ty, Hi->Value.Int),
                    {SS->getRange()});
        return errorType();
      }
      // Integer bounds of different types (1..5000000000): the wider host.
      Type *Host = Lo->Ty->getBase();
      if (Host->isInteger() &&
          llvm::cast<IntegerType>(Hi->Ty->getBase())->getBits() >
              llvm::cast<IntegerType>(Host)->getBits())
        Host = Hi->Ty->getBase();
      if (Hi->Ty->getBase()->isWideChar()) // 'a'..#$100: of widechar
        Host = Hi->Ty->getBase();
      return Ctx.create<SubrangeType>(Host, Lo->Value.Int, Hi->Value.Int);
    }

    case TypeSpec::TS_Array: {
      auto *AS = llvm::cast<ArrayTypeSpec>(TS);
      Type *Elem = resolveType(AS->getElement());
      llvm::SmallVector<Type *, 2> Indexes;
      bool OK = !Elem->isError();
      for (TypeSpec *IdxTS : AS->getIndexes()) {
        Type *Idx = resolveType(IdxTS);
        if (Idx->isError()) {
          OK = false;
        } else if (!Idx->isOrdinal()) {
          Diags.error(IdxTS->getBeginLoc(),
                      "array index type must be an ordinal type, not " +
                          quoted(Idx),
                      {IdxTS->getRange()});
          OK = false;
        } else if (Idx->isInteger() &&
                   llvm::cast<IntegerType>(Idx)->getBits() >= 32) {
          Diags.error(IdxTS->getBeginLoc(),
                      "array index type " + quoted(Idx) +
                          " is too large; use a subrange such as 1..100",
                      {IdxTS->getRange()});
          OK = false;
        }
        Indexes.push_back(Idx);
      }
      if (!OK)
        return errorType();
      // array [A, B] of E == array [A] of array [B] of E
      Type *T = Elem;
      for (auto It = Indexes.rbegin(); It != Indexes.rend(); ++It) {
        auto *A = Ctx.create<ArrayType>(*It, T, AS->isPacked());
        if (AS->isBitPacked())
          A->setBitPacked();
        T = A;
      }
      return T;
    }

    case TypeSpec::TS_Pointer: {
      // Always resolved at the end of the type section: `^T` binds to a T
      // declared later in the same block even if an outer T is visible.
      auto *P = Ctx.create<PointerType>(nullptr);
      PendingPointers.push_back({llvm::cast<PointerTypeSpec>(TS), P});
      return P;
    }

    case TypeSpec::TS_Async: {
      auto *AS = llvm::cast<AsyncTypeSpec>(TS);
      Type *Inner = nullptr;
      if (AS->getInner()) {
        Inner = resolveType(AS->getInner());
        if (Inner->isError())
          return errorType();
      }
      return Ctx.getTaskType(Inner);
    }

    case TypeSpec::TS_Routine: {
      auto *RS = llvm::cast<RoutineTypeSpec>(TS);
      for (VarDecl *P : RS->getParams()) {
        P->setType(resolveType(P->getTypeSpec()));
        checkParamDefault(P);
      }
      Type *Result = nullptr;
      if (RS->isFunction()) {
        Result = resolveType(RS->getResult());
        bool MethodResult =
            (RS->isOfObject() || RS->isReference()) && !Result->isError() &&
            (Result->isShortString() || isMethodPointer(Result) ||
             (Result->isRecord() && !containsFile(Result) &&
              !containsInterface(Result)));
        if (!Result->isError() && !Result->isValidResultType() && !MethodResult) {
          Diags.error(RS->getResult()->getBeginLoc(),
                      "function result type must be a simple or pointer "
                      "type, not " + quoted(Result),
                      {RS->getResult()->getRange()});
          Result = errorType();
        }
      }
      return Ctx.create<RoutineType>(RS->getParams(), Result,
                                     RS->getCallConv() == CallConv::CDecl,
                                     RS->isOfObject(), RS->isReference());
    }

    case TypeSpec::TS_String: {
      auto *SS = llvm::cast<StringTypeSpec>(TS);
      if (!SS->getMaxLength())
        return SS->isLong() ? Ctx.getAnsiStringType()
                            : Ctx.getShortStringType(255);
      auto C = evaluateConstant(SS->getMaxLength(), "string length");
      if (!C)
        return errorType();
      if (!C->Ty->isIntegerLike() || C->Value.Int < 1 || C->Value.Int > 255) {
        Diags.error(SS->getMaxLength()->getBeginLoc(),
                    "string length must be an integer in 1..255",
                    {SS->getMaxLength()->getRange()});
        return errorType();
      }
      return Ctx.getShortStringType(static_cast<unsigned>(C->Value.Int));
    }

    case TypeSpec::TS_File: {
      auto *FS = llvm::cast<FileTypeSpec>(TS);
      if (!FS->getElement()) // file (Turbo/FPC): untyped
        return Ctx.create<FileType>(nullptr, false);
      Type *Elem = resolveType(FS->getElement());
      if (Elem->isError())
        return errorType();
      if (containsFile(Elem)) {
        Diags.error(FS->getElement()->getBeginLoc(),
                    "the components of a file cannot be files",
                    {FS->getElement()->getRange()});
        return errorType();
      }
      if (isManaged(Elem)) {
        Diags.error(FS->getElement()->getBeginLoc(),
                    Elem->isAnsiString() ? "the components of a file cannot be ansistrings"
                    : Elem->isDynArray()
                        ? "the components of a file cannot be dynamic arrays"
                        : "the components of a file cannot be COM interfaces",
                    {FS->getElement()->getRange()});
        return errorType();
      }
      return Ctx.create<FileType>(Elem, false);
    }

    case TypeSpec::TS_Set: {
      auto *SS = llvm::cast<SetTypeSpec>(TS);
      Type *Elem = resolveType(SS->getElement());
      if (Elem->isError())
        return errorType();
      if (!Elem->isOrdinal()) {
        Diags.error(SS->getElement()->getBeginLoc(),
                    "set element type must be an ordinal type, not " +
                        quoted(Elem),
                    {SS->getElement()->getRange()});
        return errorType();
      }
      OrdinalBounds Bd = getOrdinalBounds(Elem);
      if (Bd.Lo < 0 || Bd.Hi > 255) {
        Diags.error(SS->getElement()->getBeginLoc(),
                    "set element type " + quoted(Elem) +
                        " has values outside 0..255; use a subrange",
                    {SS->getElement()->getRange()});
        return errorType();
      }
      return Ctx.create<SetType>(Elem);
    }

    case TypeSpec::TS_Class:
      Diags.error(TS->getBeginLoc(),
                  "a class type must be declared in a 'type' section",
                  {TS->getRange()});
      return errorType();

    case TypeSpec::TS_Interface:
      Diags.error(TS->getBeginLoc(),
                  "an interface type must be declared in a 'type' section",
                  {TS->getRange()});
      return errorType();

    case TypeSpec::TS_Helper:
      Diags.error(TS->getBeginLoc(),
                  llvm::Twine("a ") +
                      (llvm::cast<HelperTypeSpec>(TS)->isRecordHelper()
                           ? "record helper"
                           : "type helper") +
                      " must be declared in a 'type' section",
                  {TS->getRange()});
      return errorType();

    case TypeSpec::TS_ClassRef: { // class of TFoo (TFoo may be forward)
      const Identifier &Id = llvm::cast<ClassRefTypeSpec>(TS)->getClassName();
      Decl *D = Cur->lookup(Id.Name);
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(D);
      if (TD && TD->getType() && TD->getType()->isError())
        return errorType();
      auto *C = TD ? llvm::dyn_cast_or_null<ClassType>(TD->getType()) : nullptr;
      if (!C) {
        if (!D)
          Diags.error(Id.Loc,
                      "unknown class name '" + Id.Spelling + "'" +
                          didYouMean(Id.Name),
                      {Id.getRange()});
        else {
          Diags.error(Id.Loc, "'" + Id.Spelling + "' is not a class type",
                      {Id.getRange()});
          noteDeclaredHere(D);
        }
        return errorType();
      }
      return Ctx.create<ClassRefType>(C);
    }

    case TypeSpec::TS_Record: {
      auto *RS = llvm::cast<RecordTypeSpec>(TS);
      if (!RS->getMembers().empty() && RS != NamedAdvancedRecord)
        Diags.error(RS->getBeginLoc(),
                    "methods are not allowed in anonymous or local records; "
                    "declare the record type in a 'type' section at program "
                    "or unit level",
                    {RS->getRange()});
      llvm::SmallVector<FieldDecl *, 16> All;
      llvm::StringMap<FieldDecl *> Seen;
      if (!resolveFieldList(RS->getFields(), All, Seen))
        return errorType();
      auto *R = Ctx.create<RecordType>(RS->getFields(),
                                       Ctx.copyArray<FieldDecl *>(All),
                                       RS->isPacked());
      R->setPackRecords(RS->getPackRecords());
      if (RS->isBitPacked()) {
        R->setBitPacked();
        for (FieldDecl *F : All) // ordinals, and what needs no alignment
          if (Type *FT = F->getType();
              FT && !FT->isError() && !FT->isOrdinal() && !isByteAligned(FT))
            Diags.error(F->getLocation(),
                        "a field of type " + quoted(FT) +
                            " in a bitpacked record is not supported yet",
                        {F->getName().getRange()});
      }
      return R;
    }
    }
    return errorType();
  }

  /// The name of a tuple's N-th element that has none: ItemN.
  Identifier tupleItemName(size_t N, SourceLocation Loc) {
    std::string S = "Item" + std::to_string(N);
    return {Ctx.getStrings().intern(llvm::StringRef(S).lower()),
            Ctx.getStrings().intern(S), Loc};
  }

  /// The tuple type with these element types and names (Names empty:
  /// Item1, Item2, ...). There is one for each such list, so that tuple
  /// types written alike are the same type.
  RecordType *getTupleType(llvm::ArrayRef<Type *> Types,
                           llvm::ArrayRef<Identifier> Names) {
    llvm::SmallVector<Identifier, 4> Own;
    if (Names.empty()) {
      for (size_t I = 0; I < Types.size(); ++I)
        Own.push_back(tupleItemName(I + 1, SourceLocation()));
      Names = Own;
    }
    for (RecordType *R : TupleTypes) {
      auto Fields = R->getAllFields();
      bool Same = Fields.size() == Types.size();
      for (size_t I = 0; Same && I < Fields.size(); ++I)
        Same = Fields[I]->getType() == Types[I] &&
               Fields[I]->getName().Name == Names[I].Name;
      if (Same)
        return R;
    }
    llvm::SmallVector<FieldDecl *, 4> Fields;
    for (size_t I = 0; I < Types.size(); ++I) {
      auto *F = Ctx.create<FieldDecl>(Names[I], nullptr);
      F->setType(Types[I]);
      Fields.push_back(F);
    }
    auto *FL = Ctx.create<FieldList>();
    FL->Fixed = Ctx.copyArray<FieldDecl *>(Fields);
    auto *R = Ctx.create<RecordType>(FL, FL->Fixed, /*Packed=*/false);
    R->setTuple();
    TupleTypes.push_back(R);
    return R;
  }

  /// A hidden variable of type T of the block being checked (a tuple is
  /// built in one); null outside the statements of a block.
  VarDecl *makeHiddenVar(Type *T, SourceLocation Loc) {
    if (!InlineVars)
      return nullptr;
    std::string S = "$tuple" + std::to_string(++HiddenVars);
    llvm::StringRef N = Ctx.getStrings().intern(S);
    auto *V = Ctx.create<VarDecl>(Identifier{N, N, Loc}, nullptr);
    V->setInline();
    V->setType(T);
    V->setOwner(CurRoutine);
    Used.insert(V);
    InlineVars->push_back(V);
    return V;
  }

  bool addField(FieldDecl *F, llvm::SmallVectorImpl<FieldDecl *> &All,
                llvm::StringMap<FieldDecl *> &Seen) {
    auto [It, Inserted] = Seen.try_emplace(F->getName().Name, F);
    if (!Inserted) {
      Diags.error(F->getLocation(),
                  "duplicate field name '" + F->getName().Spelling + "'",
                  {F->getName().getRange()});
      Diags.note(It->second->getLocation(), "previous field is here");
      return false;
    }
    All.push_back(F);
    return true;
  }

  bool resolveFieldList(FieldList *FL, llvm::SmallVectorImpl<FieldDecl *> &All,
                        llvm::StringMap<FieldDecl *> &Seen) {
    bool OK = true;
    for (FieldDecl *F : FL->Fixed) {
      F->setType(resolveType(F->getTypeSpec()));
      OK &= !F->getType()->isError();

      OK &= addField(F, All, Seen);
    }
    VariantPart *VP = FL->Variant;
    if (!VP)
      return OK;

    Type *TagTy = resolveType(VP->TagTypeSpec);
    if (!TagTy->isError() && !TagTy->isOrdinal()) {
      Diags.error(VP->TagTypeSpec->getBeginLoc(),
                  "variant tag type must be an ordinal type, not " +
                      quoted(TagTy),
                  {VP->TagTypeSpec->getRange()});
      TagTy = errorType();
    }
    VP->TagType = TagTy;
    OK &= !TagTy->isError();
    if (VP->Tag) {
      VP->Tag->setType(TagTy);
      OK &= addField(VP->Tag, All, Seen);
    }
    for (Variant *V : VP->Variants) {
      for (CaseLabel *L : V->Labels)
        OK &= checkLabel(L, TagTy, "variant label");
      size_t First = All.size();
      OK &= resolveFieldList(V->Fields, All, Seen);
      // The variants overlap, so their fields cannot be reference counted.
      for (size_t I = First; I < All.size(); ++I)
        if (All[I]->getType() && isManaged(All[I]->getType())) {
          Diags.error(All[I]->getLocation(),
                      All[I]->getType()->isAnsiString()
                          ? "a variant part cannot contain ansistrings"
                      : All[I]->getType()->isDynArray()
                          ? "a variant part cannot contain dynamic arrays"
                          : "a variant part cannot contain COM interfaces",
                      {All[I]->getName().getRange()});
          OK = false;
        }
    }
    return OK;
  }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  void declareConst(ConstDecl *C) {
    if (auto V = evaluateConstant(C->getInit(), "constant value"))
      C->setValue(V->Ty, V->Value);
    else
      C->setValue(errorType(), ConstValue());
    declare(C);
  }

  void declareType(TypeDecl *TD) {
    if (TD->isGeneric())
      return declareGenericType(TD);
    if (auto *CS = llvm::dyn_cast<ClassTypeSpec>(TD->getTypeSpec()))
      return declareClass(TD, CS);
    if (auto *HS = llvm::dyn_cast<HelperTypeSpec>(TD->getTypeSpec()))
      return declareHelper(TD, HS);
    if (auto *IS = llvm::dyn_cast<InterfaceTypeSpec>(TD->getTypeSpec()))
      return declareInterface(TD, IS);
    auto *RS = llvm::dyn_cast<RecordTypeSpec>(TD->getTypeSpec());
    if (RS && !RS->getMembers().empty() && !CurRoutine)
      return declareAdvancedRecord(TD, RS);
    TD->setType(resolveType(TD->getTypeSpec(), typeDeclName(TD->getName())));
    declare(TD);
  }

  /// The name of the type declared as Id: TOuter.TInner for a nested one.
  llvm::StringRef typeDeclName(const Identifier &Id) {
    if (NestPrefix.empty())
      return Id.Spelling;
    return Ctx.getStrings().intern((NestPrefix + "." + Id.Spelling).str());
  }

  //===--------------------------------------------------------------------===//
  // Type helpers (objfpc, {$modeswitch typehelpers})
  //===--------------------------------------------------------------------===//

  /// The scope key of the helper active for T.
  llvm::StringRef helperKey(Type *T) {
    return Ctx.getStrings().intern(
        "#helper:" + llvm::utohexstr(reinterpret_cast<uintptr_t>(T)));
  }

  /// The type helper active for values of type T (the last declared in
  /// scope; a string constant: the ansistring or shortstring one), or null.
  ClassType *helperFor(Type *T) {
    if (!T || T->isError() || !LangOpts.ObjKeywords)
      return nullptr;
    if (T->isStringLiteral()) {
      if (ClassType *H = helperFor(Ctx.getAnsiStringType()))
        return H;
      return helperFor(Ctx.getShortStringType());
    }
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(helperKey(T)));
    if (TD)
      return llvm::dyn_cast_or_null<ClassType>(TD->getType());
    return genericHelperFor(T);
  }

  /// The scope key of the last generic type helper declared (see
  /// TypeDecl::getPrevGenericHelper).
  llvm::StringRef genericHelpersKey() {
    return Ctx.getStrings().intern("#generichelpers");
  }

  /// The generic type helpers in scope, the last declared first, whose
  /// extended type T matches (generic TH<T> = type helper for array of T:
  /// any dynamic array): the first one's specialization for the type
  /// arguments the match gives, or null.
  ClassType *genericHelperFor(Type *T) {
    if (T->isClass() || T->isClassRef() || T->isInterface())
      return nullptr;
    for (auto *G = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(genericHelpersKey()));
         G; G = G->getPrevGenericHelper()) {
      auto *HS = llvm::cast<HelperTypeSpec>(G->getTypeSpec());
      llvm::SmallVector<Type *, 2> Bound(G->getTypeParams().size(), nullptr);
      if (!matchTypePattern(G, HS->getExtended(), T, Bound))
        continue;
      if (llvm::is_contained(Bound, nullptr))
        continue;
      TypeDecl *I = specializeTypeFor(G->getName(), G, Bound);
      return I ? llvm::dyn_cast_or_null<ClassType>(I->getType()) : nullptr;
    }
    return nullptr;
  }

  /// Does type T match TS, a type written with the type parameters of
  /// generic G (a parameter, `array of P`, `specialize TGen<P...>`, or a
  /// type without them)? The types the parameters stand for go to Bound.
  bool matchTypePattern(TypeDecl *G, TypeSpec *TS, Type *T,
                        llvm::SmallVectorImpl<Type *> &Bound) {
    auto Params = G->getTypeParams();
    if (auto *N = llvm::dyn_cast<NamedTypeSpec>(TS); N && N->getQualifiers().empty())
      for (size_t K = 0; K < Params.size(); ++K)
        if (Params[K].Name == N->getName().Name) {
          if (Bound[K])
            return Bound[K] == T;
          Bound[K] = T;
          return true;
        }
    if (auto *DS = llvm::dyn_cast<DynArrayTypeSpec>(TS)) {
      auto *DT = llvm::dyn_cast<DynArrayType>(T);
      return DT && matchTypePattern(G, DS->getElement(), DT->getElementType(), Bound);
    }
    if (auto *SS = llvm::dyn_cast<SpecializeTypeSpec>(TS)) {
      auto *GS = static_cast<Scope *>(G->getGenericScope());
      auto *Of = llvm::dyn_cast_or_null<TypeDecl>(
          GS->lookup(genericKey(SS->getName().Name, SS->getArgs().size())));
      if (!Of)
        return false;
      for (auto &[Key, I] : TypeInstances)
        if (Key.first == Of && I->getType() == T) {
          for (size_t K = 0; K < SS->getArgs().size(); ++K)
            if (!matchTypePattern(G, SS->getArgs()[K], Key.second[K], Bound))
              return false;
          return true;
        }
      return false;
    }
    // a type without the parameters: that type
    if (!llvm::isa<NamedTypeSpec>(TS))
      return false;
    Scope *Saved = Cur;
    Cur = static_cast<Scope *>(G->getGenericScope());
    Type *Named = resolveType(TS);
    Cur = Saved;
    return !Named->isError() && Named == T;
  }

  /// The helper of the type Base names (AnsiString.Join), or null.
  ClassType *helperNamedBy(Expr *Base) {
    TypeDecl *TD = LangOpts.ObjKeywords ? typeNamedBy(Base) : nullptr;
    Type *T = TD ? TD->getType() : nullptr;
    if (!T || T->isClass() || memberTableOf(T))
      return nullptr;
    ClassType *H = helperFor(T);
    if (!H)
      return nullptr;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Base))
      N->setDecl(TD);
    Base->setType(T);
    return H;
  }

  /// The type helper D is a member of (a property: by its getter), or null.
  static ClassType *helperOfMember(Decl *D) {
    if (auto *P = llvm::dyn_cast_or_null<PropertyDecl>(D))
      D = P->getRead();
    auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
    return R && R->getClass() && R->getClass()->isHelper() ? R->getClass()
                                                           : nullptr;
  }
  static bool isHelperMember(Decl *D) { return helperOfMember(D); }

  /// TH = type helper [(Parent)] for T ... end: a member table (a ClassType
  /// that is not a type of values) whose methods' Self is a T, by
  /// reference; from here on the helper active for T. A record helper is
  /// the same, for a record type T.
  void declareHelper(TypeDecl *TD, HelperTypeSpec *HS) {
    const Identifier &Id = TD->getName();
    auto *C = Ctx.create<ClassType>();
    C->setDeclName(typeDeclName(Id));
    C->setModule(CurModule);
    if (HS->isRecordHelper())
      C->setRecordHelper();
    llvm::StringRef Kind = C->helperKindName();
    TD->setType(C);
    declare(TD);
    auto Empty = [&] { // (its methods' bodies are not checked)
      C->define(nullptr, {}, {}, {}, CurModule);
      TD->setType(errorType());
    };
    if (CurRoutine) {
      Diags.error(Id.Loc,
                  Kind + " '" + Id.Spelling +
                      "' must be declared at program or unit level",
                  {Id.getRange()});
      return Empty();
    }
    Type *T = resolveType(HS->getExtended());
    if (T->isError())
      return Empty();
    if (T->isClass() || T->isClassRef() || T->isInterface()) {
      Diags.error(HS->getExtended()->getBeginLoc(),
                  "type helpers for classes and interfaces are not supported "
                  "yet",
                  {HS->getExtended()->getRange()});
      return Empty();
    }
    if (HS->isRecordHelper() && !T->isRecord()) {
      Diags.error(HS->getExtended()->getBeginLoc(),
                  "a record helper can only extend a record type, not " +
                      quoted(T) + " (use a type helper)",
                  {HS->getExtended()->getRange()});
      return Empty();
    }
    C->setHelperFor(T);
    ClassType *Parent = nullptr;
    if (const Identifier &P = HS->getParent(); P.isValid()) {
      auto *PD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(P.Name));
      auto *PC = PD ? llvm::dyn_cast_or_null<ClassType>(PD->getType()) : nullptr;
      if (!PC || !PC->isHelper()) // (a record helper can extend a type
                                  // helper, and the reverse)
        Diags.error(P.Loc,
                    "'" + P.Spelling + "' is not a " +
                        (C->isRecordHelper() ? "helper" : "type helper"),
                    {P.getRange()});
      else if (PC->getHelperFor() != T)
        Diags.error(P.Loc,
                    "'" + P.Spelling + "' is a helper for " +
                        quoted(PC->getHelperFor()) + ", not for " + quoted(T),
                    {P.getRange()});
      else
        Parent = PC;
    }
    Scope MemberScope(Cur);
    Scope *SavedScope = Cur;
    Cur = &MemberScope;
    llvm::SmallVector<Decl *, 16> Members;
    llvm::StringMap<Decl *> Own;
    for (Decl *M : HS->getMembers()) {
      const Identifier &MN = M->getName();
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isGenericMethod()) {
        if (Decl *Prev = Own.lookup(R->getMemberKey())) {
          Diags.error(MN.Loc,
                      "redefinition of generic method '" + MN.Spelling + "'",
                      {MN.getRange()});
          Diags.note(Prev->getLocation(), "previous definition is here");
          continue;
        }
        defineGenericMethod(C, R);
        if (R->isClassMethod())
          R->setStatic(); // no Self
        Own[R->getMemberKey()] = R;
        Members.push_back(R);
        continue;
      }
      RoutineDecl *OverloadOf = nullptr;
      if (Decl *Prev = Own.lookup(MN.Name)) {
        OverloadOf = llvm::dyn_cast<RoutineDecl>(Prev);
        if (!OverloadOf || !llvm::isa<RoutineDecl>(M)) {
          Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                      {MN.getRange()});
          Diags.note(Prev->getLocation(), "previous definition is here");
          continue;
        }
      } else {
        Own[MN.Name] = M;
      }
      Members.push_back(M);
      if (auto *CD = llvm::dyn_cast<ConstDecl>(M)) {
        declareConst(CD);
        continue;
      }
      if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // class var
        declareClassVar(C, V);
        continue;
      }
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
        R->setClass(C);
        R->setParent(nullptr);
        resolveSignature(R);
        if (R->isVirtual() || R->isOverride() || R->isAbstract())
          Diags.error(MN.Loc,
                      "the methods of a " + Kind +
                          " cannot be 'virtual', 'override' or 'abstract'",
                      {MN.getRange()});
        if (R->isClassMethod())
          R->setStatic(); // no Self
        else
          makeSelf(R, C);
        if (OverloadOf && !addOverload(OverloadOf, R)) {
          R->setClass(nullptr); // rejected: not a member
          Members.pop_back();
        }
        continue;
      }
      auto *P = llvm::cast<PropertyDecl>(M);
      if (P->isDefault())
        Diags.error(P->getLocation(),
                    "a " + Kind + " cannot have a default property",
                    {P->getName().getRange()});
      definePropertyAccessors(C, P, Own, Parent);
    }
    Cur = SavedScope;
    C->define(Parent, Ctx.copyArray<Decl *>(Members), {}, {}, CurModule);
    Cur->set(helperKey(T), TD);
  }

  //===--------------------------------------------------------------------===//
  // Advanced records (objfpc, {$modeswitch advancedrecords})
  //===--------------------------------------------------------------------===//

  /// TR = record fields; methods; operators; properties end; -- the record
  /// type (its fields), then its member table (see RecordType).
  void declareAdvancedRecord(TypeDecl *TD, RecordTypeSpec *RS) {
    // Its nested constants and types first: its fields may use them.
    llvm::StringRef Name = typeDeclName(TD->getName());
    Scope MemberScope(Cur);
    Scope *SavedScope = Cur;
    llvm::StringRef SavedPrefix = NestPrefix;
    Cur = &MemberScope;
    NestPrefix = Name;
    for (Decl *M : RS->getMembers()) {
      if (auto *CD = llvm::dyn_cast<ConstDecl>(M))
        declareConst(CD);
      else if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
        declareNestedType(NT);
    }
    RecordTypeSpec *Saved = NamedAdvancedRecord;
    NamedAdvancedRecord = RS;
    Type *T = resolveType(RS, Name);
    NamedAdvancedRecord = Saved;
    Cur = SavedScope;
    TD->setType(T);
    declare(TD); // its methods' headings may name it
    Cur = &MemberScope;
    if (auto *RT = llvm::dyn_cast<RecordType>(T))
      defineRecordMembers(RT, RS);
    Cur = SavedScope;
    NestPrefix = SavedPrefix;
  }

  static bool isForwardClassDecl(Decl *D) {
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(D);
    auto *CS = TD ? llvm::dyn_cast_or_null<ClassTypeSpec>(TD->getTypeSpec())
                  : nullptr;
    return CS && CS->isForward();
  }

  /// A type declared in a class or advanced record (in Cur, its member
  /// scope).
  void declareNestedType(TypeDecl *TD) {
    if (auto *HS = llvm::dyn_cast<HelperTypeSpec>(TD->getTypeSpec())) {
      Diags.error(TD->getName().Loc,
                  llvm::Twine(HS->isRecordHelper() ? "record" : "type") +
                      " helpers cannot be declared in a class or record",
                  {TD->getName().getRange()});
      TD->setType(errorType());
      declare(TD);
      return;
    }
    if (llvm::isa<InterfaceTypeSpec>(TD->getTypeSpec())) {
      Diags.error(TD->getName().Loc,
                  "interfaces cannot be declared in a class or record yet",
                  {TD->getName().getRange()});
      TD->setType(errorType());
      declare(TD);
      return;
    }
    declareType(TD);
  }

  /// Members of Outer declared with TD: a nested class or record knows its
  /// outer one; an enumeration's values are members too.
  void addNestedMembers(ClassType *Outer, TypeDecl *TD,
                        llvm::SmallVectorImpl<Decl *> &Members,
                        llvm::StringMap<Decl *> &Own) {
    Type *T = TD->getType();
    if (auto *NC = llvm::dyn_cast_or_null<ClassType>(T))
      NC->setOuter(Outer);
    else if (ClassType *MT = memberTableOf(T))
      MT->setOuter(Outer);
    auto *E = llvm::dyn_cast_or_null<EnumType>(T);
    auto *ES = llvm::dyn_cast<EnumTypeSpec>(TD->getTypeSpec());
    if (!E || !ES || ES->isScoped()) // (a scoped one's: TOuter.TEnum.V)
      return;
    for (ConstDecl *V : E->getValues()) {
      V->setVisibility(TD->getVisibility());
      if (Own.try_emplace(V->getName().Name, V).second)
        Members.push_back(V);
    }
  }

  /// The member Name of the class or advanced record that Quals name
  /// (TOuter, TOuter.TInner, ...); null after a diagnostic.
  Decl *lookupMemberPath(llvm::ArrayRef<Identifier> Quals,
                         const Identifier &Name) {
    ClassType *C = nullptr;
    for (size_t I = 0; I <= Quals.size(); ++I) {
      const Identifier &Id = I < Quals.size() ? Quals[I] : Name;
      Decl *D;
      if (!C) {
        D = Cur->lookup(Id.Name);
        if (!D) {
          diagUndeclared(Id);
          return nullptr;
        }
        Used.insert(D);
      } else {
        const ClassType *Owner = nullptr;
        D = C->findMember(Id.Name, &Owner);
        if (!D) {
          Diags.error(Id.Loc,
                      "no member named '" + Id.Spelling + "' in " +
                          (C->getRecord() ? "record " : "class ") +
                          quoted(C) + didYouMeanMember(C, Id.Name),
                      {Id.getRange()});
          return nullptr;
        }
        checkMemberAccess(D, Owner, Id);
      }
      if (I == Quals.size())
        return D;
      auto *TD = llvm::dyn_cast<TypeDecl>(D);
      Type *T = TD ? TD->getType() : nullptr;
      C = llvm::dyn_cast_or_null<ClassType>(T);
      if (!C)
        C = memberTableOf(T);
      if (!C) {
        if (!T || !T->isError())
          Diags.error(Id.Loc,
                      "'" + Id.Spelling +
                          "' is not a class or record with members",
                      {Id.getRange()});
        return nullptr;
      }
    }
    return nullptr;
  }

  void defineRecordMembers(RecordType *RT, RecordTypeSpec *RS) {
    auto *C = Ctx.create<ClassType>();
    C->setDeclName(RT->getDeclName());
    C->setRecord(RT);
    RT->setMemberTable(C);
    llvm::SmallVector<Decl *, 16> Members;
    llvm::StringMap<Decl *> Own;
    for (FieldDecl *F : RT->getAllFields()) {
      Members.push_back(F);
      Own.try_emplace(F->getName().Name, F);
    }
    for (Decl *M : RS->getMembers()) {
      const Identifier &MN = M->getName();
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isClassInit()) {
        defineClassInit(C, R); // not a member
        continue;
      }
      if (llvm::isa<ConstDecl>(M) || llvm::isa<TypeDecl>(M)) {
        // (declared with the record: see declareAdvancedRecord)
        if (Decl *Prev = Own.lookup(MN.Name)) {
          Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                      {MN.getRange()});
          Diags.note(Prev->getLocation(), "previous definition is here");
          continue;
        }
        Own[MN.Name] = M;
        Members.push_back(M);
        if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
          addNestedMembers(C, NT, Members, Own);
        continue;
      }
      RoutineDecl *OverloadOf = nullptr;
      if (Decl *Prev = Own.lookup(MN.Name)) {
        OverloadOf = llvm::dyn_cast<RoutineDecl>(Prev);
        if (!OverloadOf || !llvm::isa<RoutineDecl>(M)) {
          Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                      {MN.getRange()});
          Diags.note(Prev->getLocation(), "previous definition is here");
          continue;
        }
      } else {
        Own[MN.Name] = M;
      }
      Members.push_back(M);
      if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // class var
        declareClassVar(C, V);
        continue;
      }
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
        defineRecordMethod(C, RT, R);
        if (OverloadOf && !addOverload(OverloadOf, R)) {
          R->setClass(nullptr); // rejected: not a member
          Members.pop_back();
        }
        continue;
      }
      auto *P = llvm::cast<PropertyDecl>(M);
      if (P->isDefault())
        Diags.error(P->getLocation(),
                    "a record cannot have a default property",
                    {P->getName().getRange()});
      definePropertyAccessors(C, P, Own, nullptr);
    }
    C->define(nullptr, Ctx.copyArray<Decl *>(Members), RT->getAllFields(), {},
              CurModule);
  }

  /// A class variable of class, advanced record or helper C (class var):
  /// static storage, one for C and its descendants.
  void declareClassVar(ClassType *C, VarDecl *V) {
    const Identifier &MN = V->getName();
    V->setType(resolveType(V->getTypeSpec()));
    V->setVarClass(C);
    if (containsFile(V->getType()))
      Diags.error(MN.Loc, "a class variable cannot be a file", {MN.getRange()});
    if (Expr *Init = V->getInit()) { // a typed constant
      Used.insert(V);
      ConstValue Folded;
      ConstInitWhat = "typed constant value";
      if (!V->getType()->isError() &&
          checkConstInit(Init, V->getType(), Folded))
        V->setInitValue(Folded);
    }
  }

  /// A method, class method (static) or class operator of record RT.
  void defineRecordMethod(ClassType *C, RecordType *RT, RoutineDecl *R) {
    const Identifier &Id = R->getName();
    R->setClass(C);
    R->setParent(nullptr);
    resolveSignature(R);
    if (R->isVirtual() || R->isOverride() || R->isAbstract())
      Diags.error(Id.Loc,
                  "the methods of a record cannot be 'virtual', 'override' "
                  "or 'abstract'",
                  {Id.getRange()});
    if (R->isClassMethod())
      R->setStatic(); // no Self
    else
      makeSelf(R, C);
    if (R->isConstructor() && R->getParams().empty())
      Diags.error(Id.Loc, "a record's constructor needs parameters",
                  {Id.getRange()});
    if (R->isOperator())
      checkOperatorHeading(R, RT);
  }

  /// A class operator's parameters: two (binary), one (unary - + not, and
  /// the conversions := and Explicit); no var parameters; the record among
  /// the parameter or result types.
  void checkOperatorHeading(RoutineDecl *R, RecordType *RT) {
    const Identifier &Id = R->getName();
    llvm::StringRef Sym = Id.Name.drop_front(strlen("operator"));
    size_t N = R->getParams().size();
    bool Unary = Sym == "not" || Sym == ":=" || Sym == "explicit";
    bool Either = Sym == "-" || Sym == "+";
    if (Unary ? N != 1 : Either ? N != 1 && N != 2 : N != 2) {
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' takes " +
                      (Unary ? "one parameter"
                       : Either ? "one or two parameters"
                                : "two parameters"),
                  {Id.getRange()});
      return;
    }
    bool HasRecord = R->getResultType() == RT;
    for (VarDecl *P : R->getParams()) {
      if (P->isVarParam())
        Diags.error(P->getLocation(),
                    "the parameters of an operator cannot be 'var'",
                    {P->getName().getRange()});
      HasRecord |= P->getType() == RT;
    }
    if (!HasRecord)
      Diags.error(Id.Loc,
                  "an operator of " + quoted(RT) +
                      " needs a parameter or result of that type",
                  {Id.getRange()});
  }

  static ClassType *memberTableOf(Type *T) {
    auto *RT = llvm::dyn_cast_or_null<RecordType>(T);
    return RT ? RT->getMemberTable() : nullptr;
  }

  /// A user-defined conversion of a value of type From to To: a `:=`
  /// operator (Explicit: an `Explicit` one first) of either record whose
  /// parameter takes From (exactly, else by assignment) and whose result is
  /// To. Null if there is none.
  RoutineDecl *findConversion(Type *From, Type *To, bool Explicit) {
    if (!LangOpts.ObjKeywords || !From || !To)
      return nullptr;
    for (llvm::StringRef Name :
         {llvm::StringRef("operatorexplicit"), llvm::StringRef("operator:=")}) {
      if (Name == "operatorexplicit" && !Explicit)
        continue;
      RoutineDecl *Best = nullptr;
      bool BestExact = false;
      for (Type *T : {From, To}) {
        ClassType *MT = memberTableOf(T);
        if (!MT || (T == To && To == From))
          continue;
        for (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(MT->findOwnMember(Name));
             R; R = R->getNextOverload()) {
          Type *PT = R->getParams().size() == 1 ? R->getParams()[0]->getType()
                                                : nullptr;
          if (!R->isOperator() || !PT || R->getResultType() != To)
            continue;
          bool Exact = PT == From;
          if (!Exact && !isAssignable(PT, From))
            continue;
          if (!Best || (Exact && !BestExact)) {
            Best = R;
            BestExact = Exact;
          }
        }
      }
      if (Best)
        return Best;
    }
    return nullptr;
  }

  /// Converts Value to To by a user-defined `:=` operator if there is one.
  bool convertImplicitly(Expr *Value, Type *To) {
    RoutineDecl *Op = findConversion(Value->getType(), To, false);
    if (!Op)
      return false;
    Value->setConversion(Op, To);
    return true;
  }

  /// The class operator Name of the records among Args' types that fits
  /// Args best (exact types, then assignable ones, then ones converted by a
  /// `:=` operator), with the conversions it needs annotated; null if none
  /// fits. Found: whether either record has an operator Name of that arity.
  RoutineDecl *resolveOperator(llvm::StringRef Name, llvm::ArrayRef<Expr *> Args,
                               bool &Found) {
    llvm::SmallVector<ClassType *, 2> Tables;
    for (Expr *A : Args)
      if (ClassType *MT = memberTableOf(A->getType()))
        if (!llvm::is_contained(Tables, MT))
          Tables.push_back(MT);
    RoutineDecl *Best = nullptr;
    int BestCost = 0;
    for (ClassType *MT : Tables)
      for (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(MT->findOwnMember(Name));
           R; R = R->getNextOverload()) {
        if (!R->isOperator() || R->getParams().size() != Args.size())
          continue;
        Found = true;
        int Cost = 0;
        bool OK = true;
        for (size_t I = 0; I < Args.size() && OK; ++I) {
          Type *PT = R->getParams()[I]->getType(), *AT = Args[I]->getType();
          if (!PT || PT->isError())
            OK = false;
          else if (PT == AT)
            continue;
          else if (isAssignable(PT, AT))
            Cost += 1;
          else if (findConversion(AT, PT, false))
            Cost += 100;
          else
            OK = false;
        }
        if (OK && (!Best || Cost < BestCost)) {
          Best = R;
          BestCost = Cost;
        }
      }
    if (Best)
      for (size_t I = 0; I < Args.size(); ++I) {
        Type *PT = Best->getParams()[I]->getType(), *AT = Args[I]->getType();
        if (PT != AT && !isAssignable(PT, AT))
          convertImplicitly(Args[I], PT);
      }
    return Best;
  }

  static const char *operatorSymbol(BinaryOp Op) {
    switch (Op) {
    case BinaryOp::Add:     return "+";
    case BinaryOp::Sub:     return "-";
    case BinaryOp::Mul:     return "*";
    case BinaryOp::RealDiv: return "/";
    case BinaryOp::IntDiv:  return "div";
    case BinaryOp::Mod:     return "mod";
    case BinaryOp::And:     return "and";
    case BinaryOp::Or:      return "or";
    case BinaryOp::Xor:     return "xor";
    case BinaryOp::Shl:     return "shl";
    case BinaryOp::Shr:     return "shr";
    case BinaryOp::Eq:      return "=";
    case BinaryOp::Ne:      return "<>";
    case BinaryOp::Lt:      return "<";
    case BinaryOp::Gt:      return ">";
    case BinaryOp::Le:      return "<=";
    case BinaryOp::Ge:      return ">=";
    case BinaryOp::In:      return "in";
    default:                return nullptr;
    }
  }

  /// L op R where an operand is an advanced record: its class operator
  /// (`<>` may use `=`, negated).
  Type *checkRecordOperator(BinaryExpr *E) {
    const char *Sym = operatorSymbol(E->getOp());
    Expr *Args[] = {E->getLHS(), E->getRHS()};
    Type *L = Args[0]->getType(), *R = Args[1]->getType();
    bool Found = false;
    RoutineDecl *Op =
        Sym ? resolveOperator(std::string("operator") + Sym, Args, Found)
            : nullptr;
    bool Negate = false;
    if (!Op && E->getOp() == BinaryOp::Ne) {
      Op = resolveOperator("operator=", Args, Found);
      Negate = Op != nullptr;
    }
    if (!Op) {
      Diags.error(E->getOpLoc(),
                  llvm::Twine("operator '") + getOpSpelling(E->getOp()) +
                      "' is not overloaded for " + quoted(L) + " and " +
                      quoted(R),
                  {Args[0]->getRange(), Args[1]->getRange()});
      return errorType();
    }
    E->setOperator(Op, Negate);
    if (Negate)
      return Ctx.getBooleanType();
    return Op->getResultType();
  }

  /// op X where X is an advanced record: its class operator.
  Type *checkRecordOperator(UnaryExpr *E) {
    const char *Sym = E->getOp() == UnaryOp::Not     ? "not"
                      : E->getOp() == UnaryOp::Minus ? "-"
                                                     : "+";
    Expr *Args[] = {E->getOperand()};
    bool Found = false;
    RoutineDecl *Op = resolveOperator(std::string("operator") + Sym, Args, Found);
    if (!Op) {
      Diags.error(E->getOpLoc(),
                  llvm::Twine("operator '") + getOpSpelling(E->getOp()) +
                      "' is not overloaded for " + quoted(Args[0]->getType()),
                  {Args[0]->getRange()});
      return errorType();
    }
    E->setOperator(Op);
    return Op->getResultType();
  }

  //===--------------------------------------------------------------------===//
  // Classes (objfpc)
  //===--------------------------------------------------------------------===//

  static std::string qualified(ClassType *C, const Identifier &Member) {
    return C->getName() + "." + Member.Spelling.str();
  }

  /// TFoo = class ... end;  /  TFoo = class;
  void declareClass(TypeDecl *TD, ClassTypeSpec *CS) {
    const Identifier &Id = TD->getName();
    if (CurRoutine) {
      Diags.error(Id.Loc,
                  "class '" + Id.Spelling +
                      "' must be declared at program or unit level",
                  {Id.getRange()});
      TD->setType(errorType());
      CS->setResolved(errorType());
      declare(TD);
      return;
    }
    // The definition of a class declared forward in this type section?
    if (!CS->isForward())
      for (auto It = PendingClasses.begin(); It != PendingClasses.end(); ++It)
        if (It->first->getName().Name == Id.Name &&
            Cur->lookupOwn(Id.Name) == It->first) {
          ClassType *C = It->second;
          PendingClasses.erase(It);
          TD->setType(C);
          CS->setResolved(C);
          defineClass(C, CS);
          return;
        }
    auto *C = Ctx.create<ClassType>();
    C->setDeclName(typeDeclName(Id));
    C->setModule(CurModule);
    TD->setType(C);
    CS->setResolved(C);
    declare(TD); // before the members, which may refer to the class
    if (CS->isForward()) {
      PendingClasses.push_back({TD, C});
      return;
    }
    defineClass(C, CS);
  }

  /// A forward class must be defined in the same type section.
  void checkPendingClasses() {
    for (auto &[TD, C] : PendingClasses) {
      Diags.error(TD->getName().Loc,
                  "class '" + TD->getName().Spelling +
                      "' is declared forward but never defined",
                  {TD->getName().getRange()});
      C->define(TObjectTy, {}, TObjectTy->getAllFields(),
                TObjectTy->getVTable(), CurModule);
    }
    PendingClasses.clear();
    for (auto &[TD, I] : PendingInterfaces) {
      Diags.error(TD->getName().Loc,
                  "interface '" + TD->getName().Spelling +
                      "' is declared forward but never defined",
                  {TD->getName().getRange()});
      I->define(nullptr, {}, {}, CurModule);
    }
    PendingInterfaces.clear();
  }

  //===--------------------------------------------------------------------===//
  // Interfaces (objfpc, CORBA style)
  //===--------------------------------------------------------------------===//

  /// IFoo = interface ... end;  /  IFoo = interface;
  void declareInterface(TypeDecl *TD, InterfaceTypeSpec *IS) {
    const Identifier &Id = TD->getName();
    if (CurRoutine) {
      Diags.error(Id.Loc,
                  "interface '" + Id.Spelling +
                      "' must be declared at program or unit level",
                  {Id.getRange()});
      TD->setType(errorType());
      IS->setResolved(errorType());
      declare(TD);
      return;
    }
    if (!IS->isForward())
      for (auto It = PendingInterfaces.begin(); It != PendingInterfaces.end(); ++It)
        if (It->first->getName().Name == Id.Name &&
            Cur->lookupOwn(Id.Name) == It->first) {
          InterfaceType *I = It->second;
          PendingInterfaces.erase(It);
          TD->setType(I);
          IS->setResolved(I);
          defineInterface(I, IS);
          return;
        }
    auto *I = Ctx.create<InterfaceType>();
    I->setDeclName(Id.Spelling);
    I->setCOM(!IS->isCorba());
    TD->setType(I);
    IS->setResolved(I);
    declare(TD);
    if (IS->isForward()) {
      PendingInterfaces.push_back({TD, I});
      return;
    }
    defineInterface(I, IS);
  }

  /// The interface named Id, or null after a diagnostic.
  InterfaceType *lookupInterface(const Identifier &Id) {
    Decl *D = Cur->lookup(Id.Name);
    if (!D) {
      Diags.error(Id.Loc,
                  "unknown interface name '" + Id.Spelling + "'" +
                      didYouMean(Id.Name),
                  {Id.getRange()});
      return nullptr;
    }
    auto *TD = llvm::dyn_cast<TypeDecl>(D);
    if (TD && TD->getType() && TD->getType()->isError())
      return nullptr;
    auto *I = TD ? llvm::dyn_cast_or_null<InterfaceType>(TD->getType()) : nullptr;
    if (!I) {
      Diags.error(Id.Loc, "'" + Id.Spelling + "' is not an interface type",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return nullptr;
    }
    if (!I->isDefined()) {
      Diags.error(Id.Loc,
                  "interface '" + Id.Spelling +
                      "' is not defined yet (only declared forward)",
                  {Id.getRange()});
      return nullptr;
    }
    return I;
  }

  /// The method Name of I or its nearest ancestor declaring it (the first
  /// of its overloads).
  static RoutineDecl *findInterfaceMethod(InterfaceType *I, llvm::StringRef Name) {
    for (; I; I = I->getParent())
      for (RoutineDecl *R : I->getMethods())
        if (R->getName().Name == Name)
          return R;
    return nullptr;
  }

  void defineInterface(InterfaceType *I, InterfaceTypeSpec *IS) {
    InterfaceType *Parent = nullptr;
    I->setCOM(!IS->isCorba());
    if (IS->getParent().isValid()) {
      const Identifier &PId = IS->getParent();
      if (PId.Name == I->getDeclName().lower())
        Diags.error(PId.Loc,
                    "interface '" + I->getName() + "' cannot inherit from itself",
                    {PId.getRange()});
      else
        Parent = lookupInterface(PId);
      if (Parent && Parent->isCOM() != I->isCOM()) {
        Diags.error(PId.Loc,
                    llvm::Twine("a ") + (I->isCOM() ? "COM" : "CORBA") +
                        " interface cannot inherit from the " +
                        (Parent->isCOM() ? "COM" : "CORBA") + " interface '" +
                        Parent->getName() + "'",
                    {PId.getRange()});
        Parent = nullptr;
      }
    }
    // A COM interface without a parent descends from IUnknown.
    if (!Parent && I->isCOM())
      Parent = IUnknownTy;
    llvm::SmallVector<RoutineDecl *, 8> Own, All;
    if (Parent)
      All.append(Parent->getAllMethods().begin(), Parent->getAllMethods().end());
    llvm::StringMap<RoutineDecl *> Seen;
    for (Decl *M : IS->getMembers()) {
      auto *R = llvm::cast<RoutineDecl>(M);
      const Identifier &Id = R->getName();
      R->setInterface(I);
      R->setParent(nullptr);
      resolveSignature(R);
      if (R->isVirtual() || R->isOverride() || R->isAbstract()) {
        Diags.error(Id.Loc,
                    "the methods of an interface cannot be 'virtual', "
                    "'override' or 'abstract'",
                    {Id.getRange()});
        continue;
      }
      if (RoutineDecl *Inh = Parent ? findInterfaceMethod(Parent, Id.Name) : nullptr) {
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' is already declared in interface '" +
                        Inh->getInterface()->getName() + "'",
                    {Id.getRange()});
        noteDeclaredHere(Inh);
        continue;
      }
      if (RoutineDecl *Prev = Seen.lookup(Id.Name)) {
        if (!addOverload(Prev, R))
          continue;
      } else {
        Seen[Id.Name] = R;
      }
      R->setVMTIndex(static_cast<int>(All.size()));
      All.push_back(R);
      Own.push_back(R);
    }
    I->define(Parent, Ctx.copyArray<RoutineDecl *>(Own),
              Ctx.copyArray<RoutineDecl *>(All), CurModule);
  }

  /// The methods of class C implementing the methods of interface I (by
  /// slot): methods (not class methods) of C or its ancestors with the same
  /// name and heading. Where: the interface's name in C's declaration, to
  /// report a missing one (null: inherited, already checked).
  llvm::ArrayRef<RoutineDecl *> implementInterface(ClassType *C, InterfaceType *I,
                                                   const Identifier *Where) {
    llvm::SmallVector<RoutineDecl *, 8> Impl;
    for (RoutineDecl *M : I->getAllMethods()) {
      RoutineDecl *Found = nullptr;
      for (const ClassType *X = C; X && !Found; X = X->getParent())
        for (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(
                 X->findOwnMember(M->getName().Name));
             R && !Found; R = R->getNextOverload())
          if (R->getMethodKind() == MethodKind::Method && !R->isClassMethod() &&
              R->getBuiltin() == TObjectMethod::None &&
              R->isFunction() == M->isFunction() && sameSignature(R, M) &&
              (!M->isFunction() || R->getResultType() == M->getResultType()) &&
              R->isCDecl() == M->isCDecl())
            Found = R;
      if (!Found && Where) {
        Diags.error(Where->Loc,
                    "class '" + C->getName() + "' does not implement '" +
                        M->getInterface()->getName() + "." +
                        M->getName().Spelling.str() + "'",
                    {Where->getRange()});
        noteDeclaredHere(M);
      }
      Impl.push_back(Found);
    }
    return Ctx.copyArray<RoutineDecl *>(Impl);
  }

  /// The class named Id (a parent class or a method's class), or null after
  /// a diagnostic. What: "parent class" / "class".
  ClassType *lookupClass(const Identifier &Id) {
    Decl *D = Cur->lookup(Id.Name);
    if (!D) {
      Diags.error(Id.Loc,
                  "unknown class name '" + Id.Spelling + "'" + didYouMean(Id.Name),
                  {Id.getRange()});
      return nullptr;
    }
    auto *TD = llvm::dyn_cast<TypeDecl>(D);
    if (TD && TD->getType() && TD->getType()->isError())
      return nullptr;
    auto *C = TD ? llvm::dyn_cast_or_null<ClassType>(TD->getType()) : nullptr;
    if (!C) {
      Diags.error(Id.Loc, "'" + Id.Spelling + "' is not a class type",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return nullptr;
    }
    if (!C->isDefined()) {
      Diags.error(Id.Loc,
                  "class '" + Id.Spelling + "' is not defined yet (only declared "
                  "forward)",
                  {Id.getRange()});
      return nullptr;
    }
    return C;
  }

  /// The hidden parameter Self of method R of class C.
  void makeSelf(RoutineDecl *R, ClassType *C) {
    Identifier Name = memberName("Self");
    // A record's method gets the record by reference, a type helper's the
    // value it extends.
    RecordType *Rec = C->getRecord();
    auto *Self = Ctx.create<VarDecl>(
        Name, nullptr,
        C->hasSelfByReference() ? VarDecl::VarParam : VarDecl::ValueParam);
    // In a class method, Self is the class (a class reference).
    Self->setType(Rec                    ? static_cast<Type *>(Rec)
                  : C->isHelper()        ? C->getHelperFor()
                  : R->isClassMethod() ? static_cast<Type *>(classRefOf(C))
                                       : C);
    Self->setOwner(R);
    R->setSelfVar(Self);
  }

  static bool sameHeading(RoutineDecl *A, RoutineDecl *B) {
    return A->getMethodKind() == B->getMethodKind() &&
           A->isClassMethod() == B->isClassMethod() &&
           A->isFunction() == B->isFunction() && sameSignature(A, B) &&
           (!A->isFunction() || A->getResultType() == B->getResultType());
  }

  void defineClass(ClassType *C, ClassTypeSpec *CS) {
    ClassType *Parent = TObjectTy;
    // class(TParent, I1, I2): the interfaces it implements (the first name
    // may be one too).
    llvm::SmallVector<std::pair<InterfaceType *, const Identifier *>, 2> Listed;
    auto AddInterface = [&](const Identifier &Id) {
      InterfaceType *I = lookupInterface(Id);
      if (!I)
        return;
      for (auto &[J, _] : Listed)
        if (J == I) {
          Diags.error(Id.Loc,
                      "interface '" + Id.Spelling + "' is listed twice",
                      {Id.getRange()});
          return;
        }
      Listed.push_back({I, &Id});
    };
    if (CS->getParent().isValid()) {
      const Identifier &PId = CS->getParent();
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(PId.Name));
      if (PId.Name == C->getDeclName().lower()) {
        Diags.error(PId.Loc,
                    "class '" + C->getName() + "' cannot inherit from itself",
                    {PId.getRange()});
      } else if (TD && llvm::isa_and_nonnull<InterfaceType>(TD->getType())) {
        AddInterface(PId);
      } else if (ClassType *P = lookupClass(PId)) {
        Parent = P;
      }
    }
    for (const Identifier &Id : CS->getInterfaces())
      AddInterface(Id);
    // The objects of COM interfaces are reference counted by
    // TInterfacedObject.
    for (auto &[I, Where] : Listed)
      if (I->isCOM() && !Parent->inheritsFrom(TInterfacedObjectTy)) {
        Diags.error(Where->Loc,
                    "class '" + C->getName() + "' implements the COM interface '" +
                        I->getName() +
                        "', so it must descend from TInterfacedObject",
                    {Where->getRange()});
        break;
      }

    llvm::SmallVector<Decl *, 16> Members;
    llvm::SmallVector<FieldDecl *, 16> Fields(Parent->getAllFields().begin(),
                                              Parent->getAllFields().end());
    llvm::SmallVector<RoutineDecl *, 8> VTable(Parent->getVTable().begin(),
                                               Parent->getVTable().end());
    llvm::StringMap<Decl *> Own;
    PropertyDecl *DefaultProp = nullptr;
    // Nested constants and types are visible to the members after them, and
    // so are the ancestors'.
    Scope Inherited = Scope::nestedIn(Cur, Parent);
    Scope MemberScope(&Inherited);
    Scope *SavedScope = Cur;
    llvm::StringRef SavedPrefix = NestPrefix;
    Cur = &MemberScope;
    NestPrefix = C->getDeclName();
    size_t PendingClassMark = PendingClasses.size();
    size_t PendingPointerMark = PendingPointers.size();
    for (Decl *M : CS->getMembers()) {
      const Identifier &MN = M->getName();
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isClassInit()) {
        defineClassInit(C, R); // not a member
        continue;
      }
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isGenericMethod()) {
        if (Decl *Prev = Own.lookup(R->getMemberKey())) {
          Diags.error(MN.Loc,
                      "redefinition of generic method '" + MN.Spelling + "'",
                      {MN.getRange()});
          Diags.note(Prev->getLocation(), "previous definition is here");
          continue;
        }
        defineGenericMethod(C, R);
        Own[R->getMemberKey()] = R;
        Members.push_back(R);
        continue;
      }
      RoutineDecl *OverloadOf = nullptr; // objfpc: an overloaded method
      if (Decl *Fwd = Own.lookup(MN.Name);
          isForwardClassDecl(Fwd) && llvm::isa<TypeDecl>(M)) {
        declareNestedType(llvm::cast<TypeDecl>(M)); // its definition
        Own[MN.Name] = M;
        std::replace(Members.begin(), Members.end(), Fwd, M);
        continue;
      }
      if (Decl *Prev = Own.lookup(MN.Name)) {
        OverloadOf = llvm::dyn_cast<RoutineDecl>(Prev);
        if (!LangOpts.ObjKeywords || !OverloadOf || !llvm::isa<RoutineDecl>(M)) {
          Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                      {MN.getRange()});
          Diags.note(Prev->getLocation(), "previous definition is here");
          continue;
        }
      }
      const ClassType *Owner = nullptr;
      Decl *Inherited = Parent->findMember(MN.Name, &Owner);
      if (Inherited &&
          !(llvm::isa<RoutineDecl>(M) && llvm::isa<RoutineDecl>(Inherited))) {
        Diags.error(MN.Loc,
                    "'" + MN.Spelling + "' is already declared in class '" +
                        Owner->getName() + "'",
                    {MN.getRange()});
        noteDeclaredHere(Inherited);
        continue;
      }
      if (!OverloadOf)
        Own[MN.Name] = M;
      Members.push_back(M);

      if (auto *CD = llvm::dyn_cast<ConstDecl>(M)) {
        declareConst(CD);
        continue;
      }
      if (auto *TD = llvm::dyn_cast<TypeDecl>(M)) {
        declareNestedType(TD);
        addNestedMembers(C, TD, Members, Own);
        continue;
      }
      if (auto *F = llvm::dyn_cast<FieldDecl>(M)) {
        F->setType(resolveType(F->getTypeSpec()));
        if (containsFile(F->getType()))
          Diags.error(MN.Loc, "a field of a class cannot be a file",
                      {MN.getRange()});
        if (F->getRace() == RaceQualifier::Isolated)
          Diags.error(MN.Loc,
                      "a field cannot be isolated (only variables can); its "
                      "object belongs to whoever has the object",
                      {MN.getRange()});
        else
          checkRaceDecl(F->getRace(), F->getType(), MN, "field");
        Fields.push_back(F);
        continue;
      }
      if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // class var: static storage
        declareClassVar(C, V);
        continue;
      }
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
        defineMethod(C, Parent, R,
                     llvm::dyn_cast_or_null<RoutineDecl>(Inherited), Owner,
                     VTable);
        if (OverloadOf && !addOverload(OverloadOf, R)) {
          R->setClass(nullptr); // rejected: not a member
          Members.pop_back();
        }
        continue;
      }
      auto *P = llvm::cast<PropertyDecl>(M);
      definePropertyAccessors(C, P, Own, Parent);
      if (P->isDefault()) {
        if (DefaultProp) {
          Diags.error(MN.Loc,
                      "class '" + C->getName() +
                          "' already has a default property",
                      {MN.getRange()});
          noteDeclaredHere(DefaultProp);
        }
        DefaultProp = P;
      }
    }
    // A forward class declared in the class must be defined in it; pointers
    // declared in it may point to its later types.
    for (size_t I = PendingClassMark; I < PendingClasses.size(); ++I) {
      auto &[TD, NC] = PendingClasses[I];
      Diags.error(TD->getName().Loc,
                  "class '" + TD->getName().Spelling +
                      "' is declared forward but never defined",
                  {TD->getName().getRange()});
      NC->define(TObjectTy, {}, TObjectTy->getAllFields(),
                 TObjectTy->getVTable(), CurModule);
    }
    PendingClasses.resize(PendingClassMark);
    resolvePendingPointersFrom(PendingPointerMark);
    Cur = SavedScope;
    NestPrefix = SavedPrefix;
    C->define(Parent, Ctx.copyArray<Decl *>(Members),
              Ctx.copyArray<FieldDecl *>(Fields),
              Ctx.copyArray<RoutineDecl *>(VTable), CurModule);
    // The interfaces: the parent's (with this class's overrides), then its own.
    llvm::SmallVector<InterfaceImpl, 4> Impls;
    for (const InterfaceImpl &PI : Parent->getInterfaces())
      Impls.push_back({PI.Intf, implementInterface(C, PI.Intf, nullptr)});
    for (auto &[I, Where] : Listed)
      if (!Parent->findInterface(I))
        Impls.push_back({I, implementInterface(C, I, Where)});
    C->setInterfaces(Ctx.copyArray<InterfaceImpl>(Impls));
  }

  /// generic function M<T>(...) in class C: a member found only by its key
  /// (M<N>), not checked; its specializations are made from its
  /// implementation (instantiateMethod). Not virtual: each specialization
  /// is a method of its own.
  void defineGenericMethod(ClassType *C, RoutineDecl *R) {
    const Identifier &Id = R->getName();
    R->setClass(C);
    R->setParent(nullptr);
    auto Params = R->getTypeParams();
    for (size_t I = 0; I < Params.size(); ++I)
      for (size_t J = 0; J < I; ++J)
        if (Params[I].Name == Params[J].Name)
          Diags.error(Params[I].Loc,
                      "duplicate type parameter '" + Params[I].Spelling + "'",
                      {Params[I].getRange()});
    const char *Bad =
        R->isVirtual() || R->isOverride() || R->isAbstract()
            ? "a generic method cannot be virtual, override or abstract (each "
              "specialization is a method of its own)"
        : R->isConstructor() || R->isDestructor()
            ? "a constructor or a destructor cannot be generic"
        : R->isOperator() ? "an operator cannot be generic"
                          : nullptr;
    if (Bad)
      Diags.error(Id.Loc, Bad, {Id.getRange()});
  }

  /// The class constructor or destructor R of class or advanced record C:
  /// one of each, without parameters or directives, in a type declared at
  /// program or unit level (as in Free Pascal); static, so without Self.
  void defineClassInit(ClassType *C, RoutineDecl *R) {
    const Identifier &Id = R->getName();
    bool Ctor = R->getClassInit() == ClassInitKind::Constructor;
    const char *Kind = Ctor ? "class constructor" : "class destructor";
    const char *What = C->getRecord() ? "record" : "class";
    R->setClass(C);
    R->setParent(nullptr);
    resolveSignature(R);
    if (CurRoutine) {
      Diags.error(Id.Loc,
                  llvm::Twine("a ") + Kind + " needs a " + What +
                      " declared at program or unit level",
                  {Id.getRange()});
      return;
    }
    if (RoutineDecl *Prev =
            Ctor ? C->getClassConstructor() : C->getClassDestructor()) {
      Diags.error(Id.Loc,
                  llvm::Twine("only one ") + Kind + " can be declared in " +
                      What + " '" + C->getName() + "'",
                  {Id.getRange()});
      noteDeclaredHere(Prev);
      return;
    }
    if (!R->getParams().empty())
      Diags.error(Id.Loc, llvm::Twine("a ") + Kind + " cannot have parameters",
                  {Id.getRange()});
    const char *Directive = R->isVirtual()               ? "virtual"
                            : R->isOverride()            ? "override"
                            : R->isAbstract()            ? "abstract"
                            : R->hasOverloadDirective()  ? "overload"
                            : R->isInline()              ? "inline"
                            : R->getCallConv() != CallConv::Default
                                ? callConvName(R->getCallConv())
                                : nullptr;
    if (Directive)
      Diags.error(Id.Loc,
                  llvm::Twine("'") + Directive + "' cannot be used with a " +
                      Kind,
                  {Id.getRange()});
    if (Ctor)
      C->setClassConstructor(R);
    else
      C->setClassDestructor(R);
  }

  void defineMethod(ClassType *C, ClassType *Parent, RoutineDecl *R,
                    RoutineDecl *Inherited,
                    const ClassType *Owner,
                    llvm::SmallVectorImpl<RoutineDecl *> &VTable) {
    const Identifier &Id = R->getName();
    R->setClass(C);
    R->setParent(nullptr);
    resolveSignature(R);
    // A static class method has no Self: it is called like a routine.
    if (R->isStatic()) {
      const char *Bad = !R->isClassMethod() ? "only class methods can be 'static'"
                        : R->isVirtual()    ? "'static' cannot be used with 'virtual'"
                        : R->isOverride()   ? "'static' cannot be used with 'override'"
                        : R->isAbstract()   ? "'static' cannot be used with 'abstract'"
                                            : nullptr;
      if (Bad) {
        Diags.error(Id.Loc, Bad, {Id.getRange()});
        R->setStatic(false);
      }
    }
    if (!R->isStatic())
      makeSelf(R, C);
    bool Virtual = R->isVirtual(), Override = R->isOverride();
    // Of overloaded inherited methods, the one with this heading.
    if (Inherited && Inherited->isOverloaded()) {
      RoutineDecl *Match = nullptr;
      for (RoutineDecl *X = Inherited; X; X = X->getNextOverload())
        if (sameSignature(X, R))
          Match = X;
      if (Match || Override)
        Inherited = Match;
    }
    if (Virtual && Override) {
      Diags.error(Id.Loc,
                  "method '" + Id.Spelling +
                      "' cannot be both 'virtual' and 'override'",
                  {Id.getRange()});
      return;
    }
    if (R->isAbstract() && !Virtual && !Override) {
      Diags.error(Id.Loc,
                  "abstract method '" + Id.Spelling + "' must be 'virtual'",
                  {Id.getRange()});
      return;
    }
    if (Override) {
      if (!Inherited || !Inherited->isDispatched()) {
        Diags.error(Id.Loc,
                    "method '" + Id.Spelling +
                        "' is marked 'override' but there is no virtual method '" +
                        Id.Spelling + "' to override in '" +
                        Parent->getName() + "'",
                    {Id.getRange()});
        if (Inherited)
          noteDeclaredHere(Inherited);
        return;
      }
      if (!sameHeading(Inherited, R)) {
        Diags.error(Id.Loc,
                    "heading of '" + Id.Spelling +
                        "' does not match the method it overrides in '" +
                        Owner->getName() + "'",
                    {Id.getRange()});
        noteDeclaredHere(Inherited);
        return;
      }
      if (R->getCallConv() == CallConv::Default) {
        if (Inherited->isCDecl())
          R->setInheritedCDecl(); // as in Free Pascal
      } else if (R->isCDecl() != Inherited->isCDecl()) {
        Diags.error(Id.Loc,
                    "calling convention of '" + Id.Spelling + "' is " +
                        callConvDesc(R->isCDecl()) +
                        " but the method it overrides in '" +
                        Owner->getName() + "' is " +
                        callConvDesc(Inherited->isCDecl()),
                    {Id.getRange()});
        noteDeclaredHere(Inherited);
        return;
      }
      R->setVMTIndex(Inherited->getVMTIndex());
      VTable[Inherited->getVMTIndex()] = R;
      return;
    }
    if (Virtual) {
      R->setVMTIndex(static_cast<int>(VTable.size()));
      VTable.push_back(R);
    }
  }

  /// property P[params]: T [index N] read R write W -- R: a field of type T
  /// (a plain property) or a function returning T; W: a field of type T or
  /// a procedure taking the value (a value or const parameter of type T).
  /// The methods first take the index parameters of an array property
  /// (same types and kinds), then N (an integer) for an indexed property.
  /// Both must be declared before the property.
  void definePropertyAccessors(ClassType *C, PropertyDecl *P,
                               const llvm::StringMap<Decl *> &Own,
                               ClassType *Parent) {
    Type *T = resolveType(P->getTypeSpec());
    P->setType(T);
    bool ParamsOK = true;
    for (VarDecl *Param : P->getParams()) {
      Param->setType(resolveType(Param->getTypeSpec()));
      if (Param->getType()->isError())
        ParamsOK = false;
      if (Param->isVarParam()) {
        Diags.error(Param->getLocation(),
                    "the index parameters of a property cannot be 'var'",
                    {Param->getName().getRange()});
        ParamsOK = false;
      }
    }
    if (Expr *E = P->getIndexSpec()) {
      Type *IT = check(E);
      if (!IT->isError() && (!IT->isIntegerLike() || !fold(E))) {
        Diags.error(E->getBeginLoc(),
                    "the index of a property must be an integer constant",
                    {E->getRange()});
        ParamsOK = false;
      }
    }
    if (Expr *E = P->getDefaultValue()) {
      Type *DT = check(E);
      if (!DT->isError() && !T->isError() && (!fold(E) || !isAssignable(T, DT)))
        Diags.error(E->getBeginLoc(),
                    "the default value of property '" + P->getName().Spelling +
                        "' must be a constant of type " + quoted(T),
                    {E->getRange()});
    }
    if (P->isDefault() && !P->isArrayProperty())
      Diags.error(P->getLocation(),
                  "only an array property can be the default property",
                  {P->getName().getRange()});
    if (!P->getReadName().isValid() && !P->getWriteName().isValid()) {
      Diags.error(P->getLocation(),
                  "property '" + P->getName().Spelling +
                      "' needs a 'read' or 'write' accessor",
                  {P->getName().getRange()});
      return;
    }
    auto Find = [&](const Identifier &Id) -> Decl * {
      if (Decl *D = Own.lookup(Id.Name))
        return D;
      Decl *D = Parent ? Parent->findMember(Id.Name) : nullptr;
      if (!D)
        Diags.error(Id.Loc,
                    "no field or method named '" + Id.Spelling + "' in '" +
                        C->getName() + "'",
                    {Id.getRange()});
      return D;
    };
    // An accessor method's leading parameters: the indexes, then N.
    bool Plain = !P->isArrayProperty() && !P->getIndexSpec();
    auto IndexParamsMatch = [&](RoutineDecl *R) {
      auto Params = R->getParams();
      size_t N = P->getParams().size();
      if (Params.size() < N + (P->getIndexSpec() ? 1 : 0))
        return false;
      for (size_t I = 0; I < N; ++I)
        if (Params[I]->getType() != P->getParams()[I]->getType() ||
            Params[I]->getVarKind() != P->getParams()[I]->getVarKind() ||
            Params[I]->isConstRefParam() != P->getParams()[I]->isConstRefParam())
          return false;
      if (P->getIndexSpec())
        return !Params[N]->isVarParam() && Params[N]->getType() &&
               Params[N]->getType()->isIntegerLike();
      return true;
    };
    size_t Leading = P->getParams().size() + (P->getIndexSpec() ? 1 : 0);
    auto Wanted = [&](bool IsRead) {
      std::string S;
      for (VarDecl *Param : P->getParams())
        S += std::string(S.empty() ? "" : "; ") +
             (Param->isConstRefParam() ? "constref "
              : Param->isConstParam() ? "const "
                                      : "") +
             Param->getName().Spelling.str() + ": " +
             Param->getType()->getName();
      if (P->getIndexSpec())
        S += std::string(S.empty() ? "" : "; ") + "index: integer";
      if (!IsRead)
        S += std::string(S.empty() ? "" : "; ") + "value: " + T->getName();
      std::string Kind = P->isClassProperty() ? "a static class " : "a ";
      return IsRead ? Kind + "function (" + S + "): " + T->getName()
                    : Kind + "procedure (" + S + ")";
    };
    // A class property's accessors: class variables or static class
    // methods (as in Free Pascal); an instance property's: fields or
    // methods.
    bool ClassProp = P->isClassProperty();
    const char *PropWord = ClassProp ? "class property '" : "property '";
    auto Storage = [&](Decl *D) -> Type * {
      if (ClassProp) {
        auto *V = llvm::dyn_cast<VarDecl>(D);
        return V && V->isClassVar() ? V->getType() : nullptr;
      }
      auto *F = llvm::dyn_cast<FieldDecl>(D);
      return F ? F->getType() : nullptr;
    };
    const char *StorageWord = ClassProp ? "a class variable" : "a field";
    const char *FnWord = ClassProp ? "a static class function" : "a function";
    const char *ProcWord = ClassProp ? "a static class procedure" : "a procedure";
    Decl *Read = nullptr, *Write = nullptr;
    if (P->getReadName().isValid() && (Read = Find(P->getReadName()))) {
      const Identifier &Id = P->getReadName();
      Type *FT = Storage(Read);
      auto *R = llvm::dyn_cast<RoutineDecl>(Read);
      auto Fits = [&](RoutineDecl *X) {
        return X->isFunction() && X->getParams().size() == Leading &&
               IndexParamsMatch(X) && X->getResultType() == T &&
               (!ClassProp || X->isStatic());
      };
      for (RoutineDecl *X = R; R && R->isOverloaded() && X; X = X->getNextOverload())
        if (Fits(X)) { // of overloaded methods, the one that fits
          Read = R = X;
          break;
        }
      bool OK = (Plain && FT == T) || (R && Fits(R));
      if (!OK && !T->isError() && ParamsOK) {
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' cannot be read as " + PropWord +
                        P->getName().Spelling + "' of type " + quoted(T) +
                        (Plain ? " (use " + std::string(StorageWord) +
                                     " of that type or " + FnWord +
                                     " without parameters returning it)"
                               : " (use " + Wanted(true) + ")"),
                    {Id.getRange()});
        Read = nullptr;
      }
    }
    if (P->getWriteName().isValid() && (Write = Find(P->getWriteName()))) {
      const Identifier &Id = P->getWriteName();
      Type *FT = Storage(Write);
      auto *R = llvm::dyn_cast<RoutineDecl>(Write);
      auto Fits = [&](RoutineDecl *X) {
        return X->getMethodKind() == MethodKind::Method && X->isProcedure() &&
               X->getParams().size() == Leading + 1 && IndexParamsMatch(X) &&
               !X->getParams()[Leading]->isVarParam() &&
               X->getParams()[Leading]->getType() == T &&
               (!ClassProp || X->isStatic());
      };
      for (RoutineDecl *X = R; R && R->isOverloaded() && X; X = X->getNextOverload())
        if (Fits(X)) { // of overloaded methods, the one that fits
          Write = R = X;
          break;
        }
      bool OK = (Plain && FT == T) || (R && Fits(R));
      if (!OK && !T->isError() && ParamsOK) {
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' cannot be written as " + PropWord +
                        P->getName().Spelling + "' of type " + quoted(T) +
                        (Plain ? " (use " + std::string(StorageWord) +
                                     " of that type or " + ProcWord +
                                     " with one parameter of it)"
                               : " (use " + Wanted(false) + ")"),
                    {Id.getRange()});
        Write = nullptr;
      }
    }
    P->setAccessors(Read, Write);
  }

  /// The default property of C or its nearest ancestor declaring one.
  static PropertyDecl *defaultProperty(const ClassType *C) {
    for (; C; C = C->getParent())
      for (Decl *M : C->getMembers())
        if (auto *P = llvm::dyn_cast<PropertyDecl>(M); P && P->isDefault())
          return P;
    return nullptr;
  }

  /// TS in the heading of a method of C: the nested types of C and of the
  /// classes around it are visible.
  Type *resolveTypeIn(ClassType *C, TypeSpec *TS) {
    llvm::SmallVector<ClassType *, 2> Chain;
    for (ClassType *O = C; O; O = O->getOuter())
      Chain.push_back(O);
    llvm::SmallVector<std::unique_ptr<Scope>, 2> Scopes;
    Scope *Saved = Cur;
    for (ClassType *O : llvm::reverse(Chain)) {
      Scopes.push_back(std::make_unique<Scope>(Scope::nestedIn(Cur, O)));
      Cur = Scopes.back().get();
    }
    Type *T = resolveType(TS);
    Cur = Saved;
    return T;
  }

  /// procedure TFoo.Bar ...; begin ... end;  -- the body of a method.
  void declareMethodBody(RoutineDecl *R) {
    const Identifier &CN = R->getClassName(), &Id = R->getName();
    if (CurRoutine) {
      Diags.error(CN.Loc,
                  "method '" + CN.Spelling + "." + Id.Spelling +
                      "' must be implemented at program or unit level",
                  {CN.getRange()});
      return;
    }
    ClassType *C = nullptr;
    if (!R->getOuterClassNames().empty()) { // TOuter.TInner.Bar
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
          lookupMemberPath(R->getOuterClassNames(), CN));
      Type *T = TD ? TD->getType() : nullptr;
      C = llvm::dyn_cast_or_null<ClassType>(T);
      if (!C)
        C = memberTableOf(T);
      if (!C) {
        if (TD && !T->isError())
          Diags.error(CN.Loc,
                      "'" + CN.Spelling + "' is not a class or record with members",
                      {CN.getRange()});
        return;
      }
    } else {
      if (auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(CN.Name)))
        C = memberTableOf(TD->getType()); // an advanced record
      if (!C)
        C = lookupClass(CN);
    }
    if (!C)
      return;
    const char *What = C->getRecord() ? "record" : "class";
    if (R->isClassInit()) { // class constructor TFoo.Create
      bool Ctor = R->getClassInit() == ClassInitKind::Constructor;
      RoutineDecl *M = Ctor ? C->getClassConstructor() : C->getClassDestructor();
      if (!M || M->getName().Name != Id.Name) {
        Diags.error(Id.Loc,
                    llvm::Twine(What) + " '" + C->getName() + "' has no " +
                        (Ctor ? "class constructor '" : "class destructor '") +
                        Id.Spelling + "'",
                    {Id.getRange()});
        if (M)
          noteDeclaredHere(M);
        return;
      }
      if (C->getModule() != CurModule) {
        Diags.error(Id.Loc,
                    "'" + qualified(C, M->getName()) +
                        "' must be implemented where its " + What +
                        " is declared",
                    {Id.getRange()});
        return;
      }
      if (M->getBody()) {
        Diags.error(Id.Loc, "redefinition of '" + qualified(C, M->getName()) + "'",
                    {Id.getRange()});
        return;
      }
      if (!R->getParams().empty())
        Diags.error(Id.Loc,
                    llvm::Twine("a ") +
                        (Ctor ? "class constructor" : "class destructor") +
                        " cannot have parameters",
                    {Id.getRange()});
      M->setBody(R->getBody());
      M->setResultName(R->hasResultName());
      M->setImplementation(R);
      R->setCanonical(M);
      checkRoutineBody(M);
      return;
    }
    auto *M = llvm::dyn_cast_or_null<RoutineDecl>(C->findOwnMember(Id.Name));
    if (!M || M->getBuiltin() != TObjectMethod::None) {
      Diags.error(Id.Loc,
                  llvm::Twine(What) + " '" + C->getName() + "' has no " +
                      (R->isOperator() ? "operator '" : "method '") +
                      Id.Spelling + "'",
                  {Id.getRange()});
      return;
    }
    if (M->isOverloaded()) { // objfpc: the method with this heading
      for (VarDecl *P : R->getParams())
        P->setType(resolveTypeIn(C, P->getTypeSpec()));
      Type *RT = isConversionOperator(M) && R->getResultTypeSpec()
                     ? resolveTypeIn(C, R->getResultTypeSpec())
                     : nullptr;
      RoutineDecl *Match = nullptr;
      for (RoutineDecl *X = M; X; X = X->getNextOverload())
        if (sameSignature(X, R) && !X->getBody() &&
            (!RT || RT == X->getResultType()))
          Match = X;
      if (!Match) {
        Diags.error(Id.Loc,
                    "heading of '" + qualified(C, M->getName()) +
                        "' does not match any of its overloads",
                    {Id.getRange()});
        for (RoutineDecl *X = M; X; X = X->getNextOverload())
          Diags.note(X->getLocation(), "candidate: " + routineHeading(X));
        return;
      }
      M = Match;
    }
    std::string QName = qualified(C, M->getName());
    if (C->getModule() != CurModule) {
      Diags.error(Id.Loc,
                  "method '" + QName + "' must be implemented where its " +
                      What + " is declared",
                  {Id.getRange()});
      return;
    }
    if (M->getMethodKind() != R->getMethodKind() ||
        (M->isFunction() || M->isAsync()) != R->isFunction() ||
        M->isClassMethod() != R->isClassMethod()) {
      Diags.error(Id.Loc,
                  "'" + QName + "' was declared as a " +
                      (M->isClassMethod() ? "class " : "") + M->getKindName(),
                  {Id.getRange()});
      noteDeclaredHere(M);
      return;
    }
    if (M->isAbstract()) {
      Diags.error(Id.Loc, "abstract method '" + QName + "' cannot have a body",
                  {Id.getRange()});
      noteDeclaredHere(M);
      return;
    }
    if (M->getBody()) {
      Diags.error(Id.Loc, "redefinition of '" + QName + "'", {Id.getRange()});
      return;
    }
    if (R->hasParamList() || R->getResultTypeSpec()) {
      for (VarDecl *P : R->getParams())
        P->setType(resolveTypeIn(C, P->getTypeSpec()));
      Type *RT = R->getResultTypeSpec() ? resolveTypeIn(C, R->getResultTypeSpec())
                                        : nullptr;
      Type *Declared = M->isAsync() ? M->getAsyncType() : M->getResultType();
      if (!sameSignature(M, R) || (RT && RT != Declared) ||
          !sameDefaults(M, R)) {
        Diags.error(Id.Loc,
                    "heading of '" + QName + "' does not match its declaration",
                    {Id.getRange()});
        noteDeclaredHere(M);
      }
    }
    if (SourceLocation SL = R->getStaticDirectiveLoc(); SL.isValid()) {
      if (!R->isClassMethod() || R->getMethodKind() != MethodKind::Method)
        Diags.error(SL, "'static' is only allowed on class methods");
      else if (!M->isStatic()) {
        Diags.error(SL, "'" + QName + "' was not declared 'static'");
        noteDeclaredHere(M);
      }
    }
    if (R->isCDecl() != M->isCDecl()) {
      Diags.error(Id.Loc,
                  "calling convention of '" + QName + "' is " +
                      callConvDesc(R->isCDecl()) + " but its declaration's is " +
                      callConvDesc(M->isCDecl()),
                  {Id.getRange()});
      noteDeclaredHere(M);
    }
    M->setBody(R->getBody());
    M->setResultName(R->hasResultName()); // (as where its body is)
    M->setImplementation(R);
    R->setCanonical(M);
    if (R->isInline())
      M->setInline();
    checkRoutineBody(M);
  }

  /// C's class constructor and destructor have bodies.
  void checkClassInitImplemented(ClassType *C) {
    for (RoutineDecl *R : {C->getClassConstructor(), C->getClassDestructor()})
      if (R && !R->getBody())
        Diags.error(R->getName().Loc,
                    llvm::Twine(R->getClassInit() == ClassInitKind::Constructor
                                    ? "class constructor '"
                                    : "class destructor '") +
                        qualified(C, R->getName()) +
                        "' is declared but never implemented",
                    {R->getName().getRange()});
  }

  /// Every method of the classes declared in B has a body.
  void checkMethodsImplemented(Block &B) {
    for (Decl *D : B.getDecls())
      if (auto *TD = llvm::dyn_cast<TypeDecl>(D))
        checkMethodsImplemented(TD);
  }

  /// ... of the class or advanced record TD, and of those it declares.
  void checkMethodsImplemented(TypeDecl *TD) {
    if (llvm::isa_and_nonnull<HelperTypeSpec>(TD->getTypeSpec())) {
      if (auto *C = llvm::dyn_cast_or_null<ClassType>(TD->getType()))
        for (Decl *M : C->getMembers())
          if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
              R && !R->getBody() &&
              !(R->isGenericMethod() && R->getGenericDefinition() != R))
            Diags.error(R->getName().Loc,
                        "method '" + qualified(C, R->getName()) +
                            "' is declared but never implemented",
                        {R->getName().getRange()});
      return;
    }
    if (ClassType *MT = memberTableOf(TD->getType())) {
      checkClassInitImplemented(MT);
      for (Decl *M : MT->getMembers()) { // an advanced record's
        if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && !R->getBody())
          Diags.error(R->getName().Loc,
                      "method '" + qualified(MT, R->getName()) +
                          "' is declared but never implemented",
                      {R->getName().getRange()});
        else if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
          checkMethodsImplemented(NT);
      }
      return;
    }
    auto *CS = llvm::dyn_cast_or_null<ClassTypeSpec>(TD->getTypeSpec());
    if (!CS || CS->isForward())
      return;
    auto *C = llvm::dyn_cast_or_null<ClassType>(TD->getType());
    if (!C)
      return;
    checkClassInitImplemented(C);
    for (Decl *M : CS->getMembers()) {
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
          R && R->getClass() == C && !R->isAbstract() && !R->getBody() &&
          !R->isClassInit() &&
          !(R->isGenericMethod() && R->getGenericDefinition() != R))
        Diags.error(R->getName().Loc,
                    "method '" + qualified(C, R->getName()) +
                        "' is declared but never implemented",
                    {R->getName().getRange()});
      else if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
        checkMethodsImplemented(NT);
    }
  }

  /// The method whose body (or a routine nested in it) is being checked.
  RoutineDecl *enclosingMethod() const {
    for (RoutineDecl *R = CurRoutine; R; R = R->getParent())
      if (R->isMethod())
        return R;
    return nullptr;
  }

  /// The class whose members a with context exposes (null for records):
  /// an object's, or in a class method Self's (a class reference).
  static ClassType *classOf(WithContext *W) {
    if (!W)
      return nullptr;
    if (auto *R = llvm::dyn_cast_or_null<ClassRefType>(W->Record->getType()))
      return R->getClass();
    return llvm::dyn_cast_or_null<ClassType>(W->Record->getType());
  }

  /// Does with context W expose methods (an object's, a class's in a class
  /// method, or an advanced record's)?
  static bool hasMembers(WithContext *W) {
    return W && (W->Helper || classOf(W) || memberTableOf(W->Record->getType()));
  }

  /// Is D an instance member (a field, a property, a method that is not a
  /// class method or constructor)? Class methods cannot use them.
  static bool isInstanceMember(Decl *D) {
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
      return !P->isClassProperty();
    if (llvm::isa<FieldDecl>(D))
      return true;
    auto *R = llvm::dyn_cast<RoutineDecl>(D);
    return R && R->isMethod() && !R->isClassMethod() && !R->isConstructor();
  }

  /// In a class method, Self is a class: a constructor called through it
  /// creates an object of that class (its static type: Self's class).
  static ClassType *constructedThroughSelf(RoutineDecl *R, WithContext *W) {
    if (!R->isConstructor() || !W || !W->IsSelf)
      return nullptr;
    auto *CR = llvm::dyn_cast_or_null<ClassRefType>(W->Record->getType());
    return CR ? CR->getClass() : nullptr;
  }

  /// Reports use of a private or protected member D of class Owner where it
  /// is not visible. Private members are visible in the program or unit
  /// declaring the class, protected ones also in methods of descendants;
  /// strict private members only in methods of the class, strict protected
  /// ones also in methods of descendants.
  void checkMemberAccess(Decl *D, const ClassType *Owner, const Identifier &Id) {
    Visibility V = D->getVisibility();
    if (V == Visibility::Public || V == Visibility::Published || !Owner)
      return;
    bool Strict = V == Visibility::StrictPrivate || V == Visibility::StrictProtected;
    if (!Strict && Owner->getModule() == CurModule)
      return;
    RoutineDecl *M = enclosingMethod();
    // (private, from another module: a specialization's own methods)
    if (M && (V == Visibility::StrictPrivate || V == Visibility::Private
                  ? M->getClass() == Owner
                  : M->getClass()->inheritsFrom(Owner)))
      return;
    const char *What = V == Visibility::Private           ? "private"
                       : V == Visibility::Protected       ? "protected"
                       : V == Visibility::StrictPrivate   ? "strict private"
                                                          : "strict protected";
    Diags.error(Id.Loc,
                "'" + Id.Spelling + "' is a " + What + " member of '" +
                    Owner->getName() + "'",
                {Id.getRange()});
    noteDeclaredHere(D);
  }

  /// Visibility of a member found by name through a class scope; in a
  /// class method (Self is a class), only class members are usable.
  void checkScopeAccess(Decl *D, WithContext *W, const Identifier &Id) {
    if (ClassType *C = classOf(W)) {
      const ClassType *Owner = nullptr;
      C->findMember(Id.Name, &Owner);
      checkMemberAccess(D, Owner, Id);
      if (W->IsSelf && W->Record->getType()->isClassRef() && isInstanceMember(D)) {
        Diags.error(Id.Loc,
                    "instance member '" + Id.Spelling +
                        "' cannot be used in a class method",
                    {Id.getRange()});
        noteDeclaredHere(D);
      }
    }
  }

  /// The variable that E, the target of an `absolute`, is in: E is a
  /// variable, a field of a record in one or an element at a constant
  /// index of an array in one. Null for anything else.
  VarDecl *absoluteRootOf(Expr *E) {
    if (isBitField(E))
      return nullptr;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
      auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
      return V && !N->getWith() && !V->getType()->isArrayOfConst() ? V : nullptr;
    }
    if (auto *F = llvm::dyn_cast<FieldExpr>(E))
      return F->getBase()->getType()->isRecord() &&
                     llvm::isa_and_nonnull<FieldDecl>(F->getMember())
                 ? absoluteRootOf(F->getBase())
                 : nullptr;
    if (auto *I = llvm::dyn_cast<IndexExpr>(E))
      return !I->getProperty() && llvm::isa<ArrayType>(I->getBase()->getType()) &&
                     fold(I->getIndex())
                 ? absoluteRootOf(I->getBase())
                 : nullptr;
    return nullptr;
  }

  /// `V: T absolute Target`, as in Free Pascal. Checked before V is
  /// declared, so that it cannot name itself.
  void checkAbsolute(VarDecl *V, Expr *Target) {
    // The name of an enclosing function is its result.
    auto *N = llvm::dyn_cast<NameRefExpr>(Target);
    auto *R = N ? llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(N->getName().Name))
                : nullptr;
    if (R && R->isFunction())
      for (RoutineDecl *Enc = CurRoutine; Enc; Enc = Enc->getParent())
        for (RoutineDecl *X = R; X; X = X->getNextOverload())
          if (Enc == X) {
            if (X != CurRoutine)
              X->getResultVar()->setCaptured();
            N->setDecl(X->getResultVar());
            N->setType(X->getResultType());
            ResultAssigned.insert(X);
            V->setAbsoluteRoot(X->getResultVar());
            return;
          }
    if (checkAllowingUntyped(Target)->isError())
      return;
    VarDecl *Root = absoluteRootOf(Target);
    if (!Root) {
      bool IsConstant = llvm::isa<IntegerLiteralExpr>(Target) ||
                        (N && llvm::isa_and_nonnull<ConstDecl>(N->getDecl()));
      Diags.error(Target->getBeginLoc(),
                  IsConstant ? "'absolute' with a memory address is not supported"
                             : "'absolute' needs a variable, a field of a record "
                               "variable or an element at a constant index",
                  {Target->getRange()});
      return;
    }
    V->setAbsoluteRoot(Root);
    if (Root->getVarKind() == VarDecl::Result) // objfpc `Result`
      ResultAssigned.insert(Root->getOwner());
  }

  /// x: T; [cvar;] external ['lib'] [name 'sym'];  -- a C variable, at
  /// program or unit level; the library and symbol: string constants.
  void checkExternalVar(VarDecl *V) {
    const Identifier &Id = V->getName();
    Used.insert(V);
    if (!V->isExternal()) {
      Diags.error(Id.Loc,
                  "'cvar' without 'external' (a variable for C code to use) "
                  "is not supported yet",
                  {Id.getRange()});
      return;
    }
    if (CurRoutine)
      Diags.error(Id.Loc,
                  "external variable '" + Id.Spelling +
                      "' must be declared at program or unit level",
                  {Id.getRange()});
    else if (V->getAbsolute() || V->getInit())
      Diags.error(Id.Loc,
                  "external variable '" + Id.Spelling + "' cannot be " +
                      (V->getAbsolute() ? "'absolute'" : "initialized"),
                  {Id.getRange()});
    if (Expr *L = V->getLibraryExpr())
      if (auto S = externalString(L, "library name", "variable"))
        V->setLibraryName(*S);
    if (Expr *N = V->getNameExpr())
      if (auto S = externalString(N, "symbol name", "variable"))
        V->setExternalName(*S);
  }

  void declareVar(VarDecl *V) {
    V->setType(resolveType(V->getTypeSpec()));
    V->setOwner(CurRoutine);
    checkOwnedDecl(V);
    if (V->isExternal() || V->isCVar())
      checkExternalVar(V);
    if (Expr *Target = V->getAbsolute())
      checkAbsolute(V, Target);
    declareVarInit(V);
  }

  /// `x: T owned`: a variable of a routine (or one declared among the
  /// statements), of a class type.
  void checkOwnedDecl(VarDecl *V) {
    if (V->isOwned()) {
      const Identifier &Id = V->getName();
      bool OK = false;
      if (!CurRoutine && !V->isInline())
        Diags.error(Id.Loc,
                    "'owned' is only allowed on the local variables of a "
                    "routine",
                    {Id.getRange()});
      else if (!V->getType()->isError() && !V->getType()->isClass())
        Diags.error(Id.Loc,
                    "owned variable '" + Id.Spelling +
                        "' must be of a class type, not " +
                        quoted(V->getType()),
                    {Id.getRange()});
      else if (V->getAbsolute() || V->getInit())
        Diags.error(Id.Loc,
                    "owned variable '" + Id.Spelling + "' cannot be " +
                        (V->getAbsolute() ? "'absolute'" : "initialized"),
                    {Id.getRange()});
      else
        OK = !V->getType()->isError();
      V->setOwned(OK);
      if (OK)
        markHasTry(); // freed in a finally part around the routine's body
    }
    checkRaceDecl(V->getRace(), V->getType(), V->getName(), "variable");
  }

  /// Can values of type T be shared between tasks? A task, a token, or an
  /// object of a class derived from TSharedObject (which synchronizes what
  /// it shares).
  bool isThreadSafe(Type *T) const {
    if (T->isTask() || T->isToken())
      return true;
    auto *C = llvm::dyn_cast<ClassType>(T);
    return C && TSharedObjectTy && C->inheritsFrom(TSharedObjectTy);
  }

  /// Does a value of type T give access to data that others may change (an
  /// object that is not thread-safe, a pointer, a dynamic array, an
  /// interface)?
  bool sharesMutableData(Type *T) const {
    if (isThreadSafe(T))
      return false;
    return T->isClass() || T->isPointer() || T->isDynArray() || T->isInterface();
  }

  /// `shared T`: T thread-safe; `isolated T`: T a reference (a class, a
  /// dynamic array or a pointer), which `move` hands over.
  void checkRaceDecl(RaceQualifier Q, Type *T, const Identifier &Id,
                     const char *What) {
    if (Q == RaceQualifier::None || T->isError())
      return;
    if (Q == RaceQualifier::Shared && !isThreadSafe(T))
      Diags.error(Id.Loc,
                  llvm::Twine("shared ") + What + " '" + Id.Spelling +
                      "' must be of a thread-safe type (a class derived from "
                      "TSharedObject, such as TAtomic<T>, TLocked<T> or "
                      "TChannel<T>; a task; a TCancelToken), not " + quoted(T),
                  {Id.getRange()});
    if (Q == RaceQualifier::Isolated && !T->isClass() && !T->isDynArray() &&
        !T->isPointer())
      Diags.error(Id.Loc,
                  llvm::Twine("isolated ") + What + " '" + Id.Spelling +
                      "' must be of a class, dynamic array or pointer type, "
                      "not " + quoted(T),
                  {Id.getRange()});
  }

  void declareVarInit(VarDecl *V) {
    if (Expr *Init = V->getInit()) {
      // Typed constants are static: they keep their value across calls.
      // (An initialized variable is set again whenever its routine runs.)
      if (V->isTypedConst()) {
        V->setOwner(nullptr);
        Used.insert(V);
      }
      ConstValue Folded;
      ConstInitWhat = V->isTypedConst() ? "typed constant value" : "initial value";
      Type *Unimaged = containsAlignedRecord(V->getType())
                           ? notByteImage(V->getType())
                           : nullptr;
      if (containsBitPacked(V->getType()))
        Diags.error(Init->getBeginLoc(),
                    std::string(V->isTypedConst() ? "typed constants"
                                                  : "initialized variables") +
                        " of bitpacked types are not supported yet",
                    {Init->getRange()});
      else if (Unimaged)
        Diags.error(Init->getBeginLoc(),
                    std::string(V->isTypedConst() ? "typed constants"
                                                  : "initialized variables") +
                        " of packed records or records under {$packrecords} "
                        "with values of type " +
                        quoted(Unimaged) + " are not supported yet",
                    {Init->getRange()});
      else if (!V->getType()->isError() &&
               checkConstInit(Init, V->getType(), Folded))
        V->setInitValue(Folded);
      ConstInitWhat = "typed constant value";
    }
    declare(V);
  }

  /// Is T an array of char (any index type) that a string constant of
  /// length N initializes (Turbo/FPC)?
  static bool isCharArrayOfLength(Type *T, uint64_t N) {
    auto *A = llvm::dyn_cast<ArrayType>(T);
    if (!A || !A->getElementType()->isChar())
      return false;
    OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
    return static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1) == N;
  }

  /// Checks the initial value of a typed constant of type T: a constant
  /// expression (folded into Folded) or, for arrays and records, an
  /// aggregate whose elements are checked recursively.
  bool checkConstInit(Expr *Init, Type *T, ConstValue &Folded) {
    if (auto *Agg = llvm::dyn_cast<AggregateExpr>(Init)) {
      Agg->setType(T);
      auto Elems = Agg->getElements();
      if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
        OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
        uint64_t Want = static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1);
        if (Agg->isRecordForm() || Elems.size() != Want) {
          Diags.error(Init->getBeginLoc(),
                      "initializer for " + quoted(T) + " must list " +
                          std::to_string(Want) + " element" +
                          (Want == 1 ? "" : "s") + ", separated by ','",
                      {Init->getRange()});
          return false;
        }
        bool OK = true;
        for (AggregateElement *El : Elems)
          OK &= checkConstInit(El->Value, A->getElementType(), El->Folded);
        return OK;
      }
      if (auto *R = llvm::dyn_cast<RecordType>(T)) {
        if (R->getFieldList()->Variant) {
          Diags.error(Init->getBeginLoc(),
                      "typed constants of records with variant parts are not "
                      "supported",
                      {Init->getRange()});
          return false;
        }
        // (1, 'a') for a tuple: its elements in turn, each then named.
        if (R->isTuple() && !Agg->isRecordForm()) {
          auto Fields = R->getAllFields();
          if (Elems.size() != Fields.size()) {
            Diags.error(Init->getBeginLoc(),
                        "initializer for " + quoted(T) + " must list " +
                            std::to_string(Fields.size()) +
                            " elements, separated by ','",
                        {Init->getRange()});
            return false;
          }
          for (size_t I = 0; I < Elems.size(); ++I) {
            Elems[I]->Field = Fields[I]->getName();
            Elems[I]->Field.Loc = Elems[I]->Value->getBeginLoc();
          }
        }
        if (!Agg->isRecordForm()) {
          Diags.error(Init->getBeginLoc(),
                      "initializer for record " + quoted(T) +
                          " must name its fields: (field: value; ...)",
                      {Init->getRange()});
          return false;
        }
        bool OK = true;
        llvm::StringSet<> Seen;
        for (AggregateElement *El : Elems) {
          FieldDecl *F = R->findField(El->Field.Name);
          if (!F) {
            Diags.error(El->Field.Loc,
                        "no field named '" + El->Field.Spelling + "' in " +
                            quoted(T),
                        {El->Field.getRange()});
            OK = false;
            continue;
          }
          if (!Seen.insert(El->Field.Name).second) {
            Diags.error(El->Field.Loc,
                        "field '" + El->Field.Spelling + "' is initialized twice",
                        {El->Field.getRange()});
            OK = false;
            continue;
          }
          OK &= checkConstInit(El->Value, F->getType(), El->Folded);
        }
        return OK;
      }
      if (Elems.size() == 1 && !Agg->isRecordForm()) // (1 + 2) for a scalar
        return checkConstInit(Elems[0]->Value, T, Folded);
      Diags.error(Init->getBeginLoc(),
                  "a parenthesized list cannot initialize " + quoted(T),
                  {Init->getRange()});
      return false;
    }

    if (T == TGuidTy) // '{01234567-89AB-CDEF-0123-456789ABCDEF}'
      return checkGuidInit(Init, Folded);
    if (auto *RT = llvm::dyn_cast<RoutineType>(T))
      return checkRoutineConstInit(Init, RT);
    if (T->isDynArray()) { // only nil
      if (llvm::isa<NilExpr>(Init)) {
        check(Init);
        return true;
      }
      Diags.error(Init->getBeginLoc(),
                  "typed constants of type " + quoted(T) +
                      " can only be initialized with nil",
                  {Init->getRange()});
      return false;
    }
    auto C = evaluateConstant(Init, ConstInitWhat);
    if (!C)
      return false;
    bool CharArray = C->Value.K == ConstValue::String &&
                     isCharArrayOfLength(T, C->Value.Str.size());
    if (!CharArray && !(T->isSimple() || T->isShortString() || T->isAnsiString() ||
                        T->isStringType())) {
      Diags.error(Init->getBeginLoc(),
                  "typed constants of type " + quoted(T) +
                      " need a parenthesized initializer",
                  {Init->getRange()});
      return false;
    }
    if (!CharArray && !isAssignable(T, C->Ty)) {
      Diags.error(Init->getBeginLoc(),
                  "cannot initialize " + quoted(T) + " with a value of type " +
                      quoted(C->Ty),
                  {Init->getRange()});
      return false;
    }
    if (C->Value.K == ConstValue::Ordinal && T->isOrdinal()) {
      if (!constantInRange(C->Value.Int, C->Ty, T)) {
        Diags.error(Init->getBeginLoc(),
                    "value " + formatValue(C->Ty, C->Value.Int) +
                        " is out of range for " + quoted(T),
                    {Init->getRange()});
        return false;
      }
    }
    checkWideToChar(T, Init);
    Folded = C->Value;
    if (C->Value.K == ConstValue::Ordinal && C->Ty->getBase()->isWideChar() &&
        T->getBase()->isChar())
      Folded.Int = wideToChar(Folded.Int);
    return true;
  }

  /// A typed constant (or initialized variable) of procedural type RT: nil,
  /// a routine (@F) declared at program or unit level or a static method,
  /// or a lambda that uses nothing of the routines around it and has no
  /// object.
  bool checkRoutineConstInit(Expr *Init, RoutineType *RT) {
    auto Bad = [&](const llvm::Twine &Why) {
      Diags.error(Init->getBeginLoc(),
                  llvm::Twine(ConstInitWhat) + " must be a constant: " + Why,
                  {Init->getRange()});
      return false;
    };
    if (llvm::isa<NilExpr>(Init)) {
      check(Init);
      return true;
    }
    auto *A = llvm::dyn_cast<AddrOfExpr>(Init);
    if (!A) {
      Diags.error(Init->getBeginLoc(),
                  "typed constants of type " + quoted(RT) +
                      " can only be initialized with nil, a routine or a "
                      "lambda",
                  {Init->getRange()});
      return false;
    }
    Type *VT = checkValueFor(RT, Init);
    if (VT->isError())
      return false;
    if (!isAssignable(RT, VT)) {
      Diags.error(Init->getBeginLoc(),
                  "cannot initialize " + quoted(RT) + " with a value of type " +
                      quoted(VT),
                  {Init->getRange()});
      return false;
    }
    if (A->getMethod())
      return Bad("the object of a method pointer is not");
    if (RoutineDecl *L = A->getLambda()) {
      if (L->getCapture().isValid())
        return Bad("this lambda uses '" + L->getCapture().Spelling +
                   "' of the routine around it");
      if (L->isMethodLambda() && L->getLambdaSelf())
        return Bad("this lambda's object is Self");
    }
    return true;
  }

  /// A TGUID typed constant (or initialized variable) from a string
  /// constant, as in Free Pascal: '{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}'
  /// (hexadecimal digits) or '' (all zeros). Folded receives its 16 bytes
  /// (Data1, Data2 and Data3 little-endian, then Data4).
  bool checkGuidInit(Expr *Init, ConstValue &Folded) {
    auto C = evaluateConstant(Init, ConstInitWhat);
    if (!C)
      return false;
    bool IsString = C->Value.K == ConstValue::String;
    bool IsChar = C->Value.K == ConstValue::Ordinal && C->Ty->getBase()->isChar();
    if (!IsString && !IsChar) {
      Diags.error(Init->getBeginLoc(),
                  "cannot initialize " + quoted(TGuidTy) +
                      " with a value of type " + quoted(C->Ty),
                  {Init->getRange()});
      return false;
    }
    std::string Bytes(16, '\0');
    if (IsString && !C->Value.Str.empty()) {
      llvm::StringRef S = C->Value.Str;
      auto Hex = [&](size_t Pos, size_t Digits, uint64_t &V) {
        V = 0;
        for (size_t I = Pos; I < Pos + Digits; ++I) {
          if (!llvm::isHexDigit(S[I]))
            return false;
          V = V << 4 | llvm::hexDigitValue(S[I]);
        }
        return true;
      };
      // Groups: position, digits, first byte, bytes, little-endian
      struct Group { size_t Pos, Digits, Byte; bool LE; };
      static const Group Groups[] = {{1, 8, 0, true},   {10, 4, 4, true},
                                     {15, 4, 6, true},  {20, 4, 8, false},
                                     {25, 12, 10, false}};
      bool OK = S.size() == 38 && S[0] == '{' && S[37] == '}' &&
                S[9] == '-' && S[14] == '-' && S[19] == '-' && S[24] == '-';
      for (const Group &G : Groups) {
        uint64_t V;
        if (!OK || !(OK = Hex(G.Pos, G.Digits, V)))
          break;
        size_t N = G.Digits / 2;
        for (size_t I = 0; I < N; ++I)
          Bytes[G.Byte + (G.LE ? I : N - 1 - I)] =
              static_cast<char>((V >> (8 * I)) & 0xff);
      }
      if (!OK) {
        Diags.error(Init->getBeginLoc(),
                    "improper GUID syntax: expected "
                    "'{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}' (hexadecimal "
                    "digits) or ''",
                    {Init->getRange()});
        return false;
      }
    } else if (IsChar) {
      Diags.error(Init->getBeginLoc(),
                  "improper GUID syntax: expected "
                  "'{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}' (hexadecimal "
                  "digits) or ''",
                  {Init->getRange()});
      return false;
    }
    Folded = ConstValue::string(Ctx.getStrings().intern(Bytes));
    return true;
  }

  /// Is E, a checked expression, a constant (nil, a set of constants, a
  /// concatenation of string constants)?
  bool isConstantDefault(Expr *E) {
    if (llvm::isa<NilExpr>(E))
      return true;
    if (auto *Sum = llvm::dyn_cast<BinaryExpr>(E);
        Sum && Sum->getOp() == BinaryOp::Add && !Sum->getOperator() &&
        (E->getType()->isShortString() || E->getType()->isAnsiString()))
      return isConstantDefault(Sum->getLHS()) && isConstantDefault(Sum->getRHS());
    if (auto *S = llvm::dyn_cast<SetCtorExpr>(E)) {
      for (const SetElement &El : S->getElements())
        if (!fold(El.Lo) || (El.Hi && !fold(El.Hi)))
          return false;
      return true;
    }
    return fold(E).has_value();
  }

  /// Checks the default value of parameter P, of a resolved type: a
  /// constant that can be passed to it.
  void checkParamDefault(VarDecl *P) {
    Expr *E = P->getDefault();
    if (!E || P->getType()->isError())
      return;
    Type *T = P->getType(), *AT = check(E);
    if (AT->isError())
      return;
    if (!isConstantDefault(E)) {
      Diags.error(E->getBeginLoc(), "default parameter value must be a constant",
                  {E->getRange()});
      return;
    }
    if (!isAssignable(T, AT) && !isPCharLiteral(T, E)) {
      Diags.error(E->getBeginLoc(),
                  "cannot pass a default value of type " + quoted(AT) +
                      " to parameter '" + P->getName().Spelling + "' of type " +
                      quoted(T),
                  {E->getRange()});
      return;
    }
    if (auto C = fold(E); C && C->Value.K == ConstValue::Ordinal && T->isOrdinal()) {
      if (!constantInRange(C->Value.Int, C->Ty, T))
        Diags.error(E->getBeginLoc(),
                    "value " + formatValue(C->Ty, C->Value.Int) +
                        " is out of range for " + quoted(T),
                    {E->getRange()});
    }
  }

  /// The elements of the constant set S.
  std::bitset<256> constantSetBits(SetCtorExpr *S) {
    std::bitset<256> Bits;
    for (const SetElement &El : S->getElements()) {
      int64_t Lo = fold(El.Lo)->Value.Int;
      int64_t Hi = El.Hi ? fold(El.Hi)->Value.Int : Lo;
      for (int64_t V = std::max<int64_t>(Lo, 0); V <= std::min<int64_t>(Hi, 255); ++V)
        Bits.set(static_cast<size_t>(V));
    }
    return Bits;
  }

  /// Are the constant default values A and B of a parameter equal?
  bool sameDefaultValue(Expr *A, Expr *B) {
    if (llvm::isa<NilExpr>(A) || llvm::isa<NilExpr>(B))
      return llvm::isa<NilExpr>(A) && llvm::isa<NilExpr>(B);
    auto *SA = llvm::dyn_cast<SetCtorExpr>(A);
    auto *SB = llvm::dyn_cast<SetCtorExpr>(B);
    if (SA || SB)
      return SA && SB && constantSetBits(SA) == constantSetBits(SB);
    auto CA = fold(A), CB = fold(B);
    if (!CA || !CB)
      return false;
    const ConstValue &VA = CA->Value, &VB = CB->Value;
    if (VA.K != VB.K)
      return false;
    return VA.K == ConstValue::Ordinal  ? VA.Int == VB.Int
           : VA.K == ConstValue::String ? VA.Str == VB.Str
                                        : VA.RealVal == VB.RealVal;
  }

  /// Do the default values of Impl, the implementation of Heading (with
  /// the same signature), fit? It may leave them out, or repeat them.
  bool sameDefaults(RoutineDecl *Heading, RoutineDecl *Impl) {
    for (size_t I = 0; I < Impl->getParams().size(); ++I) {
      VarDecl *P = Impl->getParams()[I];
      if (!P->getDefault())
        continue;
      Expr *HD = Heading->getParams()[I]->getDefault();
      if (!HD)
        return false;
      unsigned Errors = Diags.getNumErrors();
      checkParamDefault(P);
      if (Diags.getNumErrors() != Errors) // reported
        continue;
      if (!sameDefaultValue(HD, P->getDefault()))
        return false;
    }
    return true;
  }

  /// Resolves a routine's parameter and result types.
  void resolveSignature(RoutineDecl *R) {
    for (VarDecl *P : R->getParams()) {
      P->setOwner(R);
      P->setType(resolveType(P->getTypeSpec()));
      checkParamDefault(P);
      if (!P->isVarParam() && containsFile(P->getType()))
        Diags.error(P->getLocation(),
                    "file parameter '" + P->getName().Spelling +
                        "' must be a 'var' parameter",
                    {P->getName().getRange()});
    }
    if (!R->isFunction())
      return;
    Type *ResultTy = errorType();
    if (!R->getResultTypeSpec()) {
      Diags.error(R->getName().Loc,
                  "function '" + R->getName().Spelling + "' needs a result type",
                  {R->getName().getRange()});
    } else {
      ResultTy = resolveType(R->getResultTypeSpec());
      // ISO 7185 6.6.2: the result type is a simple type (or a pointer).
      // Turbo/FPC also return strings and records (without files).
      if (!ResultTy->isError() && !ResultTy->isValidResultType() &&
          !ResultTy->isShortString() && !isMethodPointer(ResultTy) &&
          !(!isISO() && ResultTy->isRecord() && !containsFile(ResultTy))) {
        Diags.error(R->getResultTypeSpec()->getBeginLoc(),
                    "function result type must be a simple or pointer type, not " +
                        quoted(ResultTy),
                    {R->getResultTypeSpec()->getRange()});
        ResultTy = errorType();
      } else if (!ResultTy->isError() && ResultTy->isRecord() &&
                 containsInterface(ResultTy)) {
        Diags.error(R->getResultTypeSpec()->getBeginLoc(),
                    "a function cannot return a record containing COM "
                    "interfaces",
                    {R->getResultTypeSpec()->getRange()});
        ResultTy = errorType();
      }
    }
    // async T / async: an async routine, whose body is a function's
    // returning T or a procedure's.
    if (auto *TT = llvm::dyn_cast<TaskType>(ResultTy)) {
      R->setAsync(TT);
      if (R->isExternal())
        Diags.error(R->getName().Loc,
                    "an external routine cannot be async",
                    {R->getName().getRange()});
      if (!TT->getResult()) {
        R->setFunction(false);
        return;
      }
      ResultTy = TT->getResult();
      if (!ResultTy->isError() && !ResultTy->isValidResultType() &&
          !ResultTy->isShortString() &&
          !(ResultTy->isRecord() && !containsFile(ResultTy))) {
        Diags.error(R->getResultTypeSpec()->getBeginLoc(),
                    "the result of a task cannot be " + quoted(ResultTy),
                    {R->getResultTypeSpec()->getRange()});
        ResultTy = errorType();
      }
    }
    R->setResultType(ResultTy);
    auto *Result = Ctx.create<VarDecl>(R->getName(), R->getResultTypeSpec(),
                                       VarDecl::Result);
    Result->setType(ResultTy);
    Result->setOwner(R);
    R->setResultVar(Result);
  }

  /// An async routine's restrictions: its task may outlive its caller, so
  /// its arguments are copied (no var, out or constref parameters, no open
  /// or untyped ones), and it is not nested (but a spawn block, which its
  /// scope waits for) nor a virtual method, constructor or destructor.
  void checkAsyncRoutine(RoutineDecl *R) {
    const Identifier &Id = R->getName();
    if (R->getParent() && !R->isSpawnBlock() && !R->isLambda())
      Diags.error(Id.Loc,
                  "async routine '" + Id.Spelling +
                      "' must be declared at program or unit level, or be a "
                      "method (its task may outlive the routine around it)",
                  {Id.getRange()});
    if (R->isMethod()) {
      const char *Why = nullptr;
      if (R->isConstructor() || R->isDestructor())
        Why = "a constructor or a destructor cannot be async";
      else if (R->isVirtual() || R->isAbstract() || R->isOverride() ||
               R->isDispatched())
        Why = "an async method cannot be virtual (not supported yet)";
      else if (R->getClass() && R->getClass()->hasSelfByReference())
        Why = "a record's or helper's method cannot be async (its Self is "
              "passed by reference)";
      if (Why)
        Diags.error(Id.Loc, Why, {Id.getRange()});
    }
    for (VarDecl *P : R->getParams()) {
      Type *T = P->getType();
      const char *Kind = P->isOutParam()        ? "an 'out'"
                         : P->isVarParam()      ? "a 'var'"
                         : P->isConstRefParam() ? "a 'constref'"
                         : T && (T->isOpenArray() || T->isArrayOfConst())
                             ? "an open array"
                         : T && T->isUntyped() ? "an untyped"
                                               : nullptr;
      if (Kind)
        Diags.error(P->getLocation(),
                    "parameter '" + P->getName().Spelling + "' of async routine '" +
                        Id.Spelling + "' cannot be " + Kind +
                        " parameter: its task may outlive the caller's "
                        "variable; pass a value",
                    {P->getName().getRange()});
    }
  }

  /// CompareOut false: `out` and `var` parameters count as the same, and
  /// `constref` and `const` ones, as
  /// when looking for routines that overloads cannot tell apart.
  static bool sameSignature(RoutineDecl *A, RoutineDecl *B, bool CompareOut = true) {
    if (A->getParams().size() != B->getParams().size())
      return false;
    for (size_t I = 0; I < A->getParams().size(); ++I) {
      VarDecl *P = A->getParams()[I], *Q = B->getParams()[I];
      if (P->getVarKind() != Q->getVarKind() || P->getType() != Q->getType() ||
          (CompareOut && (P->isOutParam() != Q->isOutParam() ||
                          P->isConstRefParam() != Q->isConstRefParam())))
        return false;
    }
    return true;
  }

  /// The word for a by-reference parameter, for diagnostics.
  static const char *paramKeyword(VarDecl *P) {
    return P->isOutParam() ? "out" : "var";
  }

  /// The scope key of a generic routine: its name and type parameter count
  /// (F<1>), so it does not clash with routines named F.
  llvm::StringRef genericKey(llvm::StringRef Name, size_t N) {
    return Ctx.getStrings().intern((Name + "<" + llvm::Twine(N) + ">").str());
  }

  /// generic function F<T>...: declared (under its key), not checked.
  void declareGeneric(RoutineDecl *R) {
    const Identifier &Id = R->getName();
    if (R->getClassName().isValid() && !CurRoutine)
      return declareGenericMethodBody(R);
    if (CurRoutine) {
      Diags.error(Id.Loc,
                  "generic routines must be declared at program or unit level",
                  {Id.getRange()});
      return;
    }
    auto Params = R->getTypeParams();
    for (size_t I = 0; I < Params.size(); ++I)
      for (size_t J = 0; J < I; ++J)
        if (Params[I].Name == Params[J].Name)
          Diags.error(Params[I].Loc,
                      "duplicate type parameter '" + Params[I].Spelling + "'",
                      {Params[I].getRange()});
    llvm::StringRef Key = genericKey(Id.Name, Params.size());
    auto *Prev = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookupOwn(Key));
    if (!Prev && CurUnitInterface)
      Prev = llvm::dyn_cast_or_null<RoutineDecl>(CurUnitInterface->lookupOwn(Key));
    R->setGenericScope(Cur);
    Used.insert(R);
    if (Prev && !Prev->getBody() && Prev->getGenericDefinition() == Prev &&
        R->getBody()) { // the definition of a forward or interface heading
      Prev->setGenericDefinition(R);
      Prev->setGenericScope(Cur);
      return;
    }
    if (Prev) {
      Diags.error(Id.Loc,
                  "redefinition of generic " + llvm::Twine(R->getKindName()) +
                      " '" + Id.Spelling + "'",
                  {Id.getRange()});
      Diags.note(Prev->getLocation(), "previous definition is here");
      return;
    }
    Cur->insert(Key, R);
  }

  /// generic function TFoo.M<T>(...) begin ... end: the definition of the
  /// generic method M<N> of TFoo; its heading must match the declaration's
  /// in its shape (type parameters, parameters' kinds and number, function
  /// or procedure), the types being checked in each specialization.
  void declareGenericMethodBody(RoutineDecl *R) {
    const Identifier &CN = R->getClassName(), &Id = R->getName();
    ClassType *C = nullptr;
    if (!R->getOuterClassNames().empty()) {
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
          lookupMemberPath(R->getOuterClassNames(), CN));
      C = TD ? llvm::dyn_cast<ClassType>(TD->getType()) : nullptr;
    } else {
      C = lookupClass(CN);
    }
    if (!C)
      return;
    auto Params = R->getTypeParams();
    auto *M = llvm::dyn_cast_or_null<RoutineDecl>(
        C->findOwnMember(genericKey(Id.Name, Params.size())));
    if (!M) {
      Diags.error(Id.Loc,
                  "class '" + C->getName() + "' has no generic method '" +
                      Id.Spelling + "' with " + llvm::Twine(Params.size()) +
                      " type parameter" + (Params.size() == 1 ? "" : "s"),
                  {Id.getRange()});
      return;
    }
    std::string QName = qualified(C, M->getName());
    if (C->getModule() != CurModule) {
      Diags.error(Id.Loc,
                  "method '" + QName +
                      "' must be implemented where its class is declared",
                  {Id.getRange()});
      return;
    }
    if (M->getGenericDefinition() != M) {
      Diags.error(Id.Loc, "redefinition of '" + QName + "'", {Id.getRange()});
      return;
    }
    bool Same = M->isFunction() == R->isFunction() &&
                M->isClassMethod() == R->isClassMethod() &&
                M->getParams().size() == R->getParams().size();
    for (size_t I = 0; Same && I < Params.size(); ++I)
      Same = Params[I].Name == M->getTypeParams()[I].Name;
    for (size_t I = 0; Same && I < R->getParams().size(); ++I) {
      VarDecl *A = M->getParams()[I], *B = R->getParams()[I];
      Same = A->getVarKind() == B->getVarKind() &&
             A->isOutParam() == B->isOutParam() &&
             A->isConstRefParam() == B->isConstRefParam() &&
             A->getName().Name == B->getName().Name;
    }
    if (!Same) {
      Diags.error(Id.Loc,
                  "heading of '" + QName + "' does not match its declaration",
                  {Id.getRange()});
      noteDeclaredHere(M);
      return;
    }
    R->setGenericScope(Cur);
    R->setClass(C);
    M->setGenericDefinition(R);
    M->setGenericScope(Cur);
    Used.insert(R);
  }

  /// The specialization of generic method G (of its class) for type
  /// arguments Types: its implementation parsed again, named M<types>, a
  /// method of the class (not a member) with G's directives; its heading
  /// resolved now, its body checked later. Null after a diagnostic.
  RoutineDecl *instantiateMethod(const Identifier &Name, RoutineDecl *G,
                                 llvm::ArrayRef<Type *> Types) {
    Used.insert(G);
    ClassType *C = G->getClass();
    RoutineDecl *Def = G->getGenericDefinition();
    if (Def == G) {
      Diags.error(Name.Loc,
                  "generic method '" + qualified(C, G->getName()) +
                      "' is used before its implementation",
                  {Name.getRange()});
      noteDeclaredHere(G);
      return nullptr;
    }
    auto Key = std::make_pair(G, std::vector<Type *>(Types.begin(), Types.end()));
    if (auto It = Instances.find(Key); It != Instances.end())
      return It->second;
    RoutineDecl *I = Reparse ? Reparse(Def) : nullptr;
    if (!I) {
      if (!Reparse)
        Diags.error(Name.Loc, "generic methods cannot be specialized here",
                    {Name.getRange()});
      return nullptr;
    }
    std::string N = (G->getName().Name + "<").str();
    std::string S = (G->getName().Spelling + "<").str();
    for (size_t K = 0; K < Types.size(); ++K) {
      if (K) {
        N += ",";
        S += ", ";
      }
      N += llvm::StringRef(Types[K]->getName()).lower();
      S += Types[K]->getName();
    }
    N += ">";
    S += ">";
    I->setName(Identifier{Ctx.getStrings().intern(N), Ctx.getStrings().intern(S),
                          Def->getName().Loc});
    Instances[Key] = I;
    // Its scope: the implementation's, where the type parameters name the
    // types.
    auto Sc = std::make_unique<Scope>(static_cast<Scope *>(Def->getGenericScope()));
    for (size_t K = 0; K < Types.size(); ++K) {
      const Identifier &P = Def->getTypeParams()[K];
      auto *TD = Ctx.create<TypeDecl>(P.Loc, P, Types[K]);
      Used.insert(TD);
      Sc->insert(P.Name, TD);
    }
    Scope *InstScope = Sc.get();
    InstanceScopes.push_back(std::move(Sc));
    Scope *SavedScope = Cur;
    RoutineDecl *SavedRoutine = CurRoutine;
    llvm::StringRef SavedPrefix = NestPrefix;
    Cur = InstScope;
    CurRoutine = nullptr;
    NestPrefix = {};
    unsigned Errors = Diags.getNumErrors();
    I->setParent(nullptr);
    I->setClass(C);
    I->setMethodKind(MethodKind::Method);
    if (G->isClassMethod())
      I->setClassMethod();
    if (G->isStatic())
      I->setStatic(true);
    I->setVisibility(G->getVisibility());
    resolveSignature(I);
    if (!I->isStatic())
      makeSelf(I, C);
    if (Diags.getNumErrors() > Errors)
      Diags.note(Name.Loc, "in the specialization '" + qualified(C, I->getName()) +
                               "' requested here");
    Cur = SavedScope;
    CurRoutine = SavedRoutine;
    NestPrefix = SavedPrefix;
    Used.insert(I);
    PendingInstances.push_back({I, InstScope, Name.Loc});
    ModuleInstances.push_back(I);
    return I;
  }

  /// The generic methods named Name of class C or its ancestors (the
  /// nearest of each number of type parameters).
  llvm::SmallVector<RoutineDecl *, 2> genericMethodsNamed(const ClassType *C,
                                                          llvm::StringRef Name) {
    llvm::SmallVector<RoutineDecl *, 2> Found;
    for (size_t N = 1; N <= 16; ++N)
      if (auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
              C->findMember(genericKey(Name, N))))
        Found.push_back(G);
    return Found;
  }

  /// Is TS, the type generic helper TD extends, a pattern that gives all
  /// its type parameters (and not a bare parameter: a helper for every
  /// type)?
  bool genericHelperPatternOK(TypeDecl *TD, TypeSpec *TS) {
    auto Params = TD->getTypeParams();
    llvm::SmallVector<bool, 2> Seen(Params.size(), false);
    std::function<bool(TypeSpec *)> Walk = [&](TypeSpec *X) -> bool {
      if (auto *N = llvm::dyn_cast<NamedTypeSpec>(X)) {
        if (N->getQualifiers().empty())
          for (size_t K = 0; K < Params.size(); ++K)
            if (Params[K].Name == N->getName().Name)
              Seen[K] = true;
        return true;
      }
      if (auto *DS = llvm::dyn_cast<DynArrayTypeSpec>(X))
        return Walk(DS->getElement());
      if (auto *SS = llvm::dyn_cast<SpecializeTypeSpec>(X)) {
        for (TypeSpec *A : SS->getArgs())
          if (!Walk(A))
            return false;
        return true;
      }
      return false;
    };
    if (auto *N = llvm::dyn_cast<NamedTypeSpec>(TS); N && N->getQualifiers().empty())
      for (const Identifier &P : Params)
        if (P.Name == N->getName().Name)
          return false; // (a helper for every type)
    return Walk(TS) && !llvm::is_contained(Seen, false);
  }

  /// Is Name the name of a generic (type or routine), with any number of
  /// type parameters?
  bool isGenericName(llvm::StringRef Name) {
    for (size_t N = 1; N <= 16; ++N)
      if (Cur->lookup(genericKey(Name, N)))
        return true;
    return false;
  }

  /// generic TList<T> = ...: declared (under its key), not checked.
  void declareGenericType(TypeDecl *TD) {
    if (auto *HS = llvm::dyn_cast<HelperTypeSpec>(TD->getTypeSpec());
        HS && !CurRoutine) { // a generic helper: also in the chain
      if (!genericHelperPatternOK(TD, HS->getExtended()))
        Diags.error(HS->getExtended()->getBeginLoc(),
                    "a generic helper must extend a type built from all its "
                    "type parameters, such as 'array of T' or 'specialize "
                    "TGen<T>'",
                    {HS->getExtended()->getRange()});
      else {
        TD->setPrevGenericHelper(
            llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(genericHelpersKey())));
        Cur->set(genericHelpersKey(), TD);
      }
    }
    const Identifier &Id = TD->getName();
    TD->setType(errorType());
    if (CurRoutine) {
      Diags.error(Id.Loc,
                  "generic types must be declared at program or unit level",
                  {Id.getRange()});
      return;
    }
    auto Params = TD->getTypeParams();
    for (size_t I = 0; I < Params.size(); ++I)
      for (size_t J = 0; J < I; ++J)
        if (Params[I].Name == Params[J].Name)
          Diags.error(Params[I].Loc,
                      "duplicate type parameter '" + Params[I].Spelling + "'",
                      {Params[I].getRange()});
    llvm::StringRef Key = genericKey(Id.Name, Params.size());
    if (Decl *Prev = Cur->lookupOwn(Key)) {
      Diags.error(Id.Loc, "redefinition of generic '" + Id.Spelling + "'",
                  {Id.getRange()});
      Diags.note(Prev->getLocation(), "previous definition is here");
      return;
    }
    TD->setGenericScope(Cur);
    Used.insert(TD);
    Cur->insert(Key, TD);
  }

  /// specialize Name<Args>, a type: the declaration of the type made of
  /// the generic type Name for those type arguments, parsed again from the
  /// generic's tokens on first use and declared like any type (named
  /// Name<integer>), in the generic's scope plus the type parameters as
  /// type aliases; its methods' implementations are parsed again too, and
  /// their bodies checked later. Null after a diagnostic.
  TypeDecl *specializeType(const Identifier &Name,
                           llvm::ArrayRef<TypeSpec *> Args) {
    llvm::SmallVector<Type *, 2> Types;
    for (TypeSpec *TS : Args) {
      Type *T = resolveType(TS);
      if (T->isError())
        return nullptr;
      Types.push_back(T);
    }
    auto *G = llvm::dyn_cast_or_null<TypeDecl>(
        Cur->lookup(genericKey(Name.Name, Args.size())));
    if (!G || !G->isGeneric()) {
      Diags.error(Name.Loc,
                  "'" + Name.Spelling + "' is not a generic type with " +
                      llvm::Twine(Args.size()) + " type parameter" +
                      (Args.size() == 1 ? "" : "s"),
                  {Name.getRange()});
      return nullptr;
    }
    return specializeTypeFor(Name, G, Types);
  }

  /// The specialization of generic type G for Types (asked for at Name),
  /// made on first use (see specializeType).
  TypeDecl *specializeTypeFor(const Identifier &Name, TypeDecl *G,
                              llvm::ArrayRef<Type *> Types) {
    Used.insert(G);
    auto Key = std::make_pair(G, std::vector<Type *>(Types.begin(), Types.end()));
    if (auto It = TypeInstances.find(Key); It != TypeInstances.end())
      return It->second;
    llvm::SmallVector<Decl *, 8> Methods;
    TypeDecl *I = ReparseType ? ReparseType(G, Methods) : nullptr;
    if (!I) {
      if (!ReparseType)
        Diags.error(Name.Loc, "generic types cannot be specialized here",
                    {Name.getRange()});
      return nullptr;
    }
    // TList<integer, string>; it is still found as TList, in its scope.
    std::string S = (G->getName().Spelling + "<").str();
    for (size_t K = 0; K < Types.size(); ++K)
      S += (K ? ", " : "") + Types[K]->getName();
    S += ">";
    I->setName(Identifier{G->getName().Name, Ctx.getStrings().intern(S),
                          G->getName().Loc});
    TypeInstances[Key] = I;
    auto Sc = std::make_unique<Scope>(static_cast<Scope *>(G->getGenericScope()));
    for (size_t K = 0; K < Types.size(); ++K) {
      const Identifier &P = G->getTypeParams()[K];
      auto *TD = Ctx.create<TypeDecl>(P.Loc, P, Types[K]);
      Used.insert(TD);
      Sc->insert(P.Name, TD);
    }
    Scope *InstScope = Sc.get();
    InstanceScopes.push_back(std::move(Sc));
    InstanceScopesOf[G].push_back(InstScope);
    Scope *SavedScope = Cur;
    RoutineDecl *SavedRoutine = CurRoutine;
    llvm::StringRef SavedPrefix = NestPrefix;
    std::vector<RoutineDecl *> *SavedDeferred = DeferredBodies;
    std::vector<RoutineDecl *> Bodies;
    Cur = InstScope;
    CurRoutine = nullptr;
    NestPrefix = {};
    DeferredBodies = &Bodies;
    unsigned Errors = Diags.getNumErrors();
    size_t PointerMark = PendingPointers.size();
    declareType(I);
    resolvePendingPointersFrom(PointerMark);
    for (Decl *M : Methods)
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M))
        declareRoutine(R);
    checkMethodsImplemented(I);
    if (Diags.getNumErrors() > Errors)
      Diags.note(Name.Loc, "in the specialization '" + S + "' requested here");
    Cur = SavedScope;
    CurRoutine = SavedRoutine;
    NestPrefix = SavedPrefix;
    DeferredBodies = SavedDeferred;
    Used.insert(I);
    for (RoutineDecl *R : Bodies)
      PendingInstances.push_back({R, InstScope, Name.Loc});
    // The type and its methods' implementations are the module's.
    ModuleInstances.push_back(I);
    for (Decl *M : Methods)
      ModuleInstances.push_back(M);
    return I;
  }

  /// A forward or interface heading without its definition (yet).
  static bool isUndefinedHeading(RoutineDecl *R) {
    return !R->getBody() &&
           !(R->isGeneric() && R->getGenericDefinition()->getBody());
  }

  /// specialize Name<Args>: the routine for those type arguments, made
  /// from the generic's declaration on first use (its heading resolved now,
  /// its body checked later); null after a diagnostic.
  /// (W: the with context the generic was found through, a generic
  /// method's object: Self in a method.)
  RoutineDecl *specialize(const Identifier &Name, llvm::ArrayRef<TypeSpec *> Args,
                          WithContext **W = nullptr) {
    llvm::SmallVector<Type *, 2> Types;
    for (TypeSpec *TS : Args) {
      Type *T = resolveType(TS);
      if (T->isError())
        return nullptr;
      Types.push_back(T);
    }
    auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
        Cur->lookup(genericKey(Name.Name, Args.size()), W));
    bool OtherGeneric = false; // F with another number of type parameters
    for (size_t N = 1; N <= 16 && !G; ++N)
      OtherGeneric |= Cur->lookup(genericKey(Name.Name, N)) != nullptr;
    if (G && G->isGenericMethod()) // specialize M<T>(args) in a method
      return instantiateMethod(Name, G, Types);
    if (!G) {
      if (Cur->lookup(Name.Name) || OtherGeneric)
        Diags.error(Name.Loc,
                    "'" + Name.Spelling + "' is not a generic routine with " +
                        llvm::Twine(Args.size()) + " type parameter" +
                        (Args.size() == 1 ? "" : "s"),
                    {Name.getRange()});
      else
        diagUndeclared(Name);
      return nullptr;
    }
    return instantiate(Name, G, Types);
  }

  /// The specialization of the generic routine G for type arguments Types
  /// (see specialize).
  RoutineDecl *instantiate(const Identifier &Name, RoutineDecl *G,
                           llvm::ArrayRef<Type *> Types) {
    Used.insert(G);
    RoutineDecl *Def = G->getGenericDefinition();
    if (!Def->getBody()) {
      Diags.error(Name.Loc,
                  "generic " + llvm::Twine(G->getKindName()) + " '" +
                      Name.Spelling + "' is used before its definition",
                  {Name.getRange()});
      Diags.note(G->getLocation(), "declared here");
      return nullptr;
    }
    auto Key = std::make_pair(G, std::vector<Type *>(Types.begin(), Types.end()));
    if (auto It = Instances.find(Key); It != Instances.end())
      return It->second;
    RoutineDecl *I = Reparse ? Reparse(Def) : nullptr;
    if (!I) {
      if (!Reparse)
        Diags.error(Name.Loc, "generic routines cannot be specialized here",
                    {Name.getRange()});
      return nullptr;
    }
    // F<integer, string>
    std::string N = (Def->getName().Name + "<").str();
    std::string S = (Def->getName().Spelling + "<").str();
    for (size_t K = 0; K < Types.size(); ++K) {
      if (K) {
        N += ",";
        S += ", ";
      }
      N += llvm::StringRef(Types[K]->getName()).lower();
      S += Types[K]->getName();
    }
    N += ">";
    S += ">";
    I->setName(Identifier{Ctx.getStrings().intern(N), Ctx.getStrings().intern(S),
                          Def->getName().Loc});
    Instances[Key] = I;
    // Its scope: the generic's, where the type parameters name the types.
    auto Sc = std::make_unique<Scope>(static_cast<Scope *>(Def->getGenericScope()));
    for (size_t K = 0; K < Types.size(); ++K) {
      const Identifier &P = Def->getTypeParams()[K];
      auto *TD = Ctx.create<TypeDecl>(P.Loc, P, Types[K]);
      Used.insert(TD);
      Sc->insert(P.Name, TD);
    }
    Scope *InstScope = Sc.get();
    InstanceScopes.push_back(std::move(Sc));
    Scope *SavedScope = Cur;
    RoutineDecl *SavedRoutine = CurRoutine;
    llvm::StringRef SavedPrefix = NestPrefix;
    Cur = InstScope;
    CurRoutine = nullptr;
    NestPrefix = {};
    unsigned Errors = Diags.getNumErrors();
    I->setParent(nullptr);
    resolveSignature(I);
    if (Diags.getNumErrors() > Errors)
      Diags.note(Name.Loc, "in the specialization '" + S + "' requested here");
    Cur = SavedScope;
    CurRoutine = SavedRoutine;
    NestPrefix = SavedPrefix;
    Used.insert(I);
    PendingInstances.push_back({I, InstScope, Name.Loc});
    ModuleInstances.push_back(I);
    return I;
  }

  /// The index of the type parameter of Def that TS names, or -1.
  static int typeParamNamed(RoutineDecl *Def, TypeSpec *TS) {
    auto *N = llvm::dyn_cast_or_null<NamedTypeSpec>(TS);
    if (!N || !N->getQualifiers().empty())
      return -1;
    auto Params = Def->getTypeParams();
    for (size_t K = 0; K < Params.size(); ++K)
      if (Params[K].Name == N->getName().Name)
        return static_cast<int>(K);
    return -1;
  }

  /// Lambda L (A), the argument for parameter P of generic Def, whose type
  /// is a specialization of a generic procedural type G (specialize
  /// TFunc<T, R>): the type arguments of Def it gives (into Bound, through
  /// Give). A type written in L gives the type argument there; L's
  /// parameters without one take the types known; then, if L's result
  /// is a type argument still unknown, L is completed for those parameter
  /// types and its body gives it.
  template <typename GiveFn>
  void inferThroughLambda(RoutineDecl *Def, VarDecl *P, AddrOfExpr *A,
                          RoutineDecl *L, llvm::SmallVectorImpl<Type *> &Bound,
                          bool &OK, GiveFn &Give) {
    auto *SS = llvm::dyn_cast_or_null<SpecializeTypeSpec>(P->getTypeSpec());
    auto *GS = static_cast<Scope *>(Def->getGenericScope());
    if (!SS || !GS)
      return;
    auto *G = llvm::dyn_cast_or_null<TypeDecl>(
        GS->lookup(genericKey(SS->getName().Name, SS->getArgs().size())));
    auto *RS = G && G->isGeneric()
                   ? llvm::dyn_cast_or_null<RoutineTypeSpec>(G->getTypeSpec())
                   : nullptr;
    if (!RS || RS->isFunction() != L->isFunction() ||
        RS->getParams().size() != L->getParams().size())
      return;
    // The type parameter of G that TS names, or -1.
    auto GParam = [&](TypeSpec *TS) -> int {
      auto *N = llvm::dyn_cast_or_null<NamedTypeSpec>(TS);
      if (!N || !N->getQualifiers().empty())
        return -1;
      auto Ps = G->getTypeParams();
      for (size_t K = 0; K < Ps.size(); ++K)
        if (Ps[K].Name == N->getName().Name)
          return static_cast<int>(K);
      return -1;
    };
    // The type of G's type parameter J as Def gives it now: a type argument
    // of Def (null while unknown), or a type named where Def is declared.
    auto TypeOfArg = [&](int J, int &K) -> Type * {
      TypeSpec *TA = SS->getArgs()[J];
      K = typeParamNamed(Def, TA);
      if (K >= 0)
        return Bound[K];
      auto *N = llvm::dyn_cast<NamedTypeSpec>(TA);
      if (!N)
        return nullptr;
      Scope *Saved = Cur;
      Cur = GS;
      Type *T = resolveType(TA);
      Cur = Saved;
      return T->isError() ? nullptr : T;
    };
    llvm::SmallVector<VarDecl *, 4> WantParams;
    bool Known = true;
    for (size_t I = 0; I < RS->getParams().size() && OK; ++I) {
      VarDecl *RP = RS->getParams()[I], *LP = L->getParams()[I];
      int J = GParam(RP->getTypeSpec()), K = -1;
      Type *T = J >= 0 ? TypeOfArg(J, K) : nullptr;
      if (LP->getTypeSpec()) { // written: it gives the type argument
        Type *W = resolveType(LP->getTypeSpec());
        if (K >= 0)
          Give(K, W, A);
        T = W->isError() ? nullptr : W;
      }
      if (!T)
        Known = false;
      auto *V = Ctx.create<VarDecl>(RP->getName(), nullptr, RP->getVarKind());
      if (RP->isOutParam())
        V->setOutParam();
      if (RP->isConstRefParam())
        V->setConstRefParam();
      V->setType(T);
      WantParams.push_back(V);
    }
    if (!OK || !L->isFunction())
      return;
    int J = GParam(RS->getResult()), K = -1;
    if (J < 0 || (TypeOfArg(J, K), K < 0))
      return;
    if (L->getResultTypeSpec()) { // written
      Give(K, resolveType(L->getResultTypeSpec()), A);
      return;
    }
    if (Bound[K] || !Known)
      return;
    auto *Want = Ctx.create<RoutineType>(
        Ctx.copyArray<VarDecl *>(WantParams), nullptr,
        RS->getCallConv() == CallConv::CDecl, RS->isOfObject(), RS->isReference());
    completeLambda(A, Want);
    Type *RT = L->getResultType();
    if (L->isLambdaBad() || !RT || RT->isError()) {
      OK = false;
      return;
    }
    Give(K, RT, A);
  }

  /// Name(args), where Name is not declared but generic routines Name<...>
  /// are: the specialization for the type arguments that the arguments
  /// give (a parameter of type T or array of T gives T the type of its
  /// argument; several arguments for one T, their common type), and for
  /// those still unknown the type Want wanted of the result, if the result
  /// is a T. Found: there is such a generic. Null without one, and after a
  /// diagnostic.
  RoutineDecl *inferSpecialization(const Identifier &Name,
                                   llvm::ArrayRef<Expr *> Args, Type *Want,
                                   bool &Found, WithContext **W = nullptr) {
    Found = false;
    llvm::SmallVector<RoutineDecl *, 2> Generics;
    for (size_t N = 1; N <= 16; ++N) {
      WithContext *GW = nullptr;
      if (auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
              Cur->lookup(genericKey(Name.Name, N), &GW))) {
        if (Generics.empty() && W) // (M(args) in a method: Self.M)
          *W = GW;
        Generics.push_back(G);
      }
    }
    if (Generics.empty())
      return nullptr;
    Found = true;
    return inferFor(Name, Generics, Args, Want);
  }

  /// Of Generics (routines or methods named Name), the one whose
  /// parameters take Args, specialized for the type arguments they (and
  /// Want, the type wanted of the result) give; null after a diagnostic.
  RoutineDecl *inferFor(const Identifier &Name,
                        llvm::SmallVectorImpl<RoutineDecl *> &Generics,
                        llvm::ArrayRef<Expr *> Args, Type *Want) {
    if (Generics.size() > 1) { // those that take these arguments
      llvm::SmallVector<RoutineDecl *, 2> Fit;
      for (RoutineDecl *G : Generics) {
        auto Params = G->getGenericDefinition()->getParams();
        llvm::SmallVector<unsigned, 8> Map;
        if (hasNamedArgs(Args) ? mapArgs(Params, Args, Map, nullptr)
                               : acceptsArgCount(Params, Args.size()))
          Fit.push_back(G);
      }
      if (Fit.size() != 1) {
        Diags.error(Name.Loc,
                    "cannot tell which generic '" + Name.Spelling +
                        "' is called; give the types: specialize " +
                        Name.Spelling + "<...>",
                    {Name.getRange()});
        return nullptr;
      }
      Generics = Fit;
    }
    RoutineDecl *G = Generics[0];
    RoutineDecl *Def = G->getGenericDefinition();
    auto TypeParams = Def->getTypeParams();
    llvm::SmallVector<Type *, 2> Bound(TypeParams.size(), nullptr);
    Type *Str = Ctx.getAnsiStringType();
    bool OK = true;
    // T (number K) is also the type Ty of the value A.
    auto Give = [&](int K, Type *Ty, Expr *A) {
      if (Ty->isError()) {
        OK = false;
        return;
      }
      if (Ty->isStringLiteral() || isCharLiteral(A))
        Ty = Str;
      if (!isInferable(Ty))
        return; // (nil, []: says nothing)
      Type *Joined = Bound[K] ? commonType(Bound[K], Ty, Str) : Ty;
      if (!Joined) {
        Diags.error(A->getBeginLoc(),
                    "cannot infer the type parameter '" +
                        TypeParams[K].Spelling + "' of '" + Name.Spelling +
                        "': the arguments give both " + quoted(Bound[K]) +
                        " and " + quoted(Ty) + "; give the type: specialize " +
                        Name.Spelling + "<...>",
                    {A->getRange()});
        OK = false;
        return;
      }
      Bound[K] = Joined;
    };
    for (size_t I = 0; I < Args.size() && OK; ++I) {
      VarDecl *P = paramOfArg(Def->getParams(), Args, I);
      Expr *A = argValue(Args[I]);
      if (!P || llvm::isa<FormattedExpr>(A))
        continue;
      TypeSpec *TS = P->getTypeSpec();
      if (int K = typeParamNamed(Def, TS); K >= 0) {
        if (!isRoutineName(A))
          Give(K, check(A), A);
        continue;
      }
      TypeSpec *Elem = nullptr;
      if (auto *OS = llvm::dyn_cast_or_null<OpenArrayTypeSpec>(TS))
        Elem = OS->getElement();
      else if (auto *DS = llvm::dyn_cast_or_null<DynArrayTypeSpec>(TS))
        Elem = DS->getElement();
      if (int K = typeParamNamed(Def, Elem); K >= 0) {
        if (auto *SC = llvm::dyn_cast<SetCtorExpr>(A)) { // [a, b, c]
          for (const SetElement &El : SC->getElements())
            if (!El.Hi && OK)
              Give(K, check(El.Lo), El.Lo);
          continue;
        }
        Type *AT = check(A);
        if (AT->isError())
          OK = false;
        else if (auto *AA = llvm::dyn_cast<ArrayType>(AT))
          Give(K, AA->getElementType(), A);
        else if (auto *DA = llvm::dyn_cast<DynArrayType>(AT))
          Give(K, DA->getElementType(), A);
        else if (auto *OA = llvm::dyn_cast<OpenArrayType>(AT))
          Give(K, OA->getElementType(), A);
        continue;
      }
    }
    if (!OK)
      return nullptr;
    // Lambdas passed for a parameter of a generic procedural type
    // (specialize TFunc<T, R>): the types written in them give type
    // arguments; those of their parameters not written come from the type
    // arguments known (T in), and a result not written from the body,
    // checked with them (R out).
    for (size_t I = 0; I < Args.size() && OK; ++I) {
      VarDecl *P = paramOfArg(Def->getParams(), Args, I);
      auto *A = llvm::dyn_cast<AddrOfExpr>(argValue(Args[I]));
      RoutineDecl *L = A ? A->getLambda() : nullptr;
      if (P && L && !L->isLambdaDone())
        inferThroughLambda(Def, P, A, L, Bound, OK, Give);
    }
    if (!OK)
      return nullptr;
    // The type wanted of the result, for a result of type T.
    if (int K = typeParamNamed(Def, Def->getResultTypeSpec());
        K >= 0 && !Bound[K] && Want && !Want->isError() && isInferable(Want))
      Bound[K] = Want;
    for (size_t K = 0; K < Bound.size(); ++K)
      if (!Bound[K]) {
        Diags.error(Name.Loc,
                    "cannot infer the type parameter '" +
                        TypeParams[K].Spelling + "' of '" + Name.Spelling +
                        "' in this call: no argument gives it; give the "
                        "type: specialize " + Name.Spelling + "<...>",
                    {Name.getRange()});
        noteDeclaredHere(G);
        return nullptr;
      }
    if (G->isGenericMethod())
      return instantiateMethod(Name, G, Bound);
    return instantiate(Name, G, Bound);
  }

  /// Checks the bodies of the specializations made so far (and those they
  /// make), then adds the module's to its block.
  void checkInstances(Block &ModuleBlock) {
    for (size_t K = 0; K < PendingInstances.size() && !Diags.shouldStop(); ++K) {
      PendingInstance P = PendingInstances[K];
      Scope *SavedScope = Cur;
      Cur = P.S;
      unsigned Errors = Diags.getNumErrors();
      checkRoutineBody(P.R);
      if (Diags.getNumErrors() > Errors)
        Diags.note(P.Use,
                   "in the specialization '" +
                       (P.R->getClass()
                            ? qualified(P.R->getClass(), P.R->getName())
                            : P.R->getName().Spelling.str()) +
                       "' requested here");
      Cur = SavedScope;
    }
    PendingInstances.clear();
    if (ModuleInstances.empty())
      return;
    llvm::SmallVector<Decl *, 16> All(ModuleBlock.getDecls().begin(),
                                      ModuleBlock.getDecls().end());
    All.append(ModuleInstances.begin(), ModuleInstances.end());
    ModuleBlock.setDecls(Ctx.copyArray<Decl *>(All));
    ModuleInstances.clear();
  }

  void declareRoutine(RoutineDecl *R) {
    if (R->isLambda()) // completed where it is used (completeLambda)
      return;
    if (R->isSpawnBlock()) // completed where it is spawned (visitSpawnExpr)
      return;
    if (R->isTemplateMember()) { // of a generic type: see specializeType
      // Its specializations see what is declared up to here (the unit's
      // implementation, for a generic type of its interface).
      auto *G = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(
          genericKey(R->getClassName().Name, R->getTypeParams().size())));
      if (G && G->isGeneric() && !CurRoutine && G->getGenericScope() != Cur) {
        G->setGenericScope(Cur);
        for (Scope *S : InstanceScopesOf[G])
          S->setParent(Cur);
      }
      return;
    }
    R->setParent(CurRoutine);
    if (R->isGeneric())
      return declareGeneric(R);
    if (R->getClassName().isValid())
      return declareMethodBody(R);
    const Identifier &Id = R->getName();
    if (R->isClassMethod())
      Diags.error(Id.Loc,
                  "only a method can be a class " + std::string(R->getKindName()) +
                      "; implement it as '" + R->getKindName() + " TClass." +
                      Id.Spelling.str() + "'",
                  {Id.getRange()});

    // The definition of an earlier `forward` declaration?
    auto *Fwd = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookupOwn(Id.Name));
    bool InInterface = false;
    if (!Fwd && CurUnitInterface && !CurRoutine) { // an interface heading
      Fwd = llvm::dyn_cast_or_null<RoutineDecl>(
          CurUnitInterface->lookupOwn(Id.Name));
      InInterface = Fwd != nullptr;
    }
    // objfpc: of overloaded routines, the one with this heading.
    if (Fwd && Fwd->isOverloaded() && R->getBody()) {
      for (VarDecl *P : R->getParams())
        P->setType(resolveType(P->getTypeSpec()));
      RoutineDecl *Match = nullptr;
      for (RoutineDecl *X = Fwd; X; X = X->getNextOverload())
        if (X->isForward() && !X->getBody() && sameSignature(X, R))
          Match = X;
      Fwd = Match;
      if (!Match && InInterface) {
        Diags.error(Id.Loc,
                    "overloads of '" + Id.Spelling +
                        "' must all be declared in the interface",
                    {Id.getRange()});
        return;
      }
    }
    if (Fwd && Fwd->isForward() && !Fwd->getBody() && R->getBody()) {
      if ((Fwd->isFunction() || Fwd->isAsync()) != R->isFunction()) {
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' was declared forward as a " +
                        Fwd->getKindName(),
                    {Id.getRange()});
        Diags.note(Fwd->getLocation(), "forward declaration is here");
        return;
      }
      // ISO omits the parameter list here; if repeated, it must match.
      if (R->hasParamList() || R->getResultTypeSpec()) {
        for (VarDecl *P : R->getParams())
          P->setType(resolveType(P->getTypeSpec()));
        Type *RT = R->getResultTypeSpec() ? resolveType(R->getResultTypeSpec())
                                          : nullptr;
        Type *Declared = Fwd->isAsync() ? Fwd->getAsyncType() : Fwd->getResultType();
        if (!sameSignature(Fwd, R) || (RT && RT != Declared) ||
            !sameDefaults(Fwd, R)) {
          Diags.error(Id.Loc,
                      "heading of '" + Id.Spelling +
                          "' does not match its forward declaration",
                      {Id.getRange()});
          Diags.note(Fwd->getLocation(), "forward declaration is here");
        }
      }
      if (R->isCDecl() != Fwd->isCDecl()) {
        Diags.error(Id.Loc,
                    "calling convention of '" + Id.Spelling + "' is " +
                        callConvDesc(R->isCDecl()) +
                        " but its forward declaration's is " +
                        callConvDesc(Fwd->isCDecl()),
                    {Id.getRange()});
        Diags.note(Fwd->getLocation(), "forward declaration is here");
      }
      Fwd->setBody(R->getBody());
      Fwd->setResultName(R->hasResultName()); // (as where its body is)
      R->setCanonical(Fwd);
      if (R->isInline())
        Fwd->setInline();
      checkRoutineBody(Fwd);
      return;
    }

    resolveSignature(R);
    if (InInterface && LangOpts.ObjKeywords) { // a different heading
      Diags.error(Id.Loc,
                  "overloads of '" + Id.Spelling +
                      "' must all be declared in the interface",
                  {Id.getRange()});
      return;
    }
    if (!declareRoutineName(R))
      return;
    if (R->isExternal())
      checkExternal(R);
    if (R->getBody())
      checkRoutineBody(R);
  }

  /// Declares R; in objfpc, another routine of its name in the same scope
  /// makes it an overload (with other parameters). False if R was rejected
  /// as such (its body is not checked then).
  bool declareRoutineName(RoutineDecl *R) {
    if (LangOpts.ObjKeywords)
      if (auto *First = llvm::dyn_cast_or_null<RoutineDecl>(
              Cur->lookupOwn(R->getName().Name)))
        return addOverload(First, R);
    declare(R);
    return true;
  }

  /// R (resolved) joins the overloads First...: its parameters must differ.
  bool addOverload(RoutineDecl *First, RoutineDecl *R) {
    for (RoutineDecl *X = First; X; X = X->getNextOverload())
      if (sameSignature(X, R, /*CompareOut=*/false) &&
          !(isConversionOperator(R) && X->getResultType() != R->getResultType())) {
        const Identifier &Id = R->getName();
        Diags.error(Id.Loc,
                    "redefinition of '" + Id.Spelling +
                        "' (overloads must differ in their parameters)",
                    {Id.getRange()});
        Diags.note(X->getLocation(), "previous definition is here");
        return false;
      }
    First->addOverload(R);
    return true;
  }

  /// A record's `:=` or `Explicit` operator: its overloads may differ in the
  /// result type alone.
  static bool isConversionOperator(RoutineDecl *R) {
    return R->isOperator() && (R->getName().Name == "operator:=" ||
                               R->getName().Name == "operatorexplicit");
  }

  //===--------------------------------------------------------------------===//
  // Overload resolution (objfpc)
  //===--------------------------------------------------------------------===//

  /// The routines a call of First's name may mean: First's overloads; for a
  /// method with the `overload` directive also those of its ancestors that
  /// it does not override.
  void collectOverloads(RoutineDecl *First,
                        llvm::SmallVectorImpl<RoutineDecl *> &Out) {
    bool Directive = false;
    size_t Start = Out.size();
    for (RoutineDecl *X = First; X; X = X->getNextOverload()) {
      Out.push_back(X);
      Directive |= X->hasOverloadDirective();
    }
    if (!Directive || !First->isMethod() || !First->getClass())
      return;
    ClassType *Parent = First->getClass()->getParent();
    auto *Up = Parent ? llvm::dyn_cast_or_null<RoutineDecl>(
                            Parent->findMember(First->getName().Name))
                      : nullptr;
    if (!Up)
      return;
    llvm::SmallVector<RoutineDecl *, 4> Inherited;
    collectOverloads(Up, Inherited);
    for (RoutineDecl *Y : Inherited)
      if (llvm::none_of(llvm::ArrayRef<RoutineDecl *>(Out).drop_front(Start),
                        [&](RoutineDecl *X) { return sameSignature(X, Y); }))
        Out.push_back(Y);
  }

  /// How far an argument of type A is from a parameter of type P (lower
  /// is better): an exact type, a subrange of it, a wider integer type
  /// (integer before int64), a same-size or narrower one, real, an
  /// ancestor class (the nearest first), anything else assignable.
  static unsigned conversionCost(Type *P, Type *A) {
    if (P == A)
      return 0;
    Type *PB = P->getBase(), *AB = A->getBase();
    if (PB == AB)
      return 1;
    if (PB->isInteger() && AB->isInteger()) {
      unsigned PBits = llvm::cast<IntegerType>(PB)->getBits();
      unsigned ABits = llvm::cast<IntegerType>(AB)->getBits();
      if (PBits > ABits)
        return PBits == 64 ? 3 : 2;
      return PBits == ABits ? 4 : 5;
    }
    if (P->isReal() && AB->isInteger())
      return 6;
    if (P->isShortString() &&
        (A->isShortString() || A->isStringLiteral() || A->isAnsiString()))
      return 1;
    if (P->isShortString() && AB->isChar())
      return 2;
    if (P->isAnsiString() && (A->isShortString() || A->isStringLiteral()))
      return 2;
    if (P->isAnsiString() && AB->isChar())
      return 3;
    auto Distance = [](const ClassType *From, const ClassType *To) {
      unsigned D = 0;
      for (; From && From != To; From = From->getParent())
        ++D;
      return D;
    };
    if (auto *PC = llvm::dyn_cast<ClassType>(P))
      if (auto *AC = llvm::dyn_cast<ClassType>(A))
        return 1 + Distance(AC, PC);
    if (auto *PR = llvm::dyn_cast<ClassRefType>(P))
      if (auto *AR = llvm::dyn_cast<ClassRefType>(A))
        return 1 + Distance(AR->getClass(), PR->getClass());
    if (A->isNil())
      return 1;
    return 7;
  }

  /// Breaks ties between costs: widening an integer to 64 bits of the
  /// other signedness (byte to int64 rather than qword, longint to qword
  /// rather than int64), as Free Pascal prefers the same signedness.
  static unsigned signPenalty(Type *P, Type *A) {
    auto *PI = llvm::dyn_cast<IntegerType>(P->getBase());
    auto *AI = llvm::dyn_cast<IntegerType>(A->getBase());
    return PI && AI && PI->getBits() == 64 && AI->getBits() < 64 &&
                   PI->isSigned() != AI->isSigned()
               ? 1
               : 0;
  }

  /// Is Arg the name of a routine (passed for a procedural parameter)?
  bool isRoutineName(Expr *Arg) {
    auto *N = llvm::dyn_cast<NameRefExpr>(Arg);
    return N && llvm::isa_and_nonnull<RoutineDecl>(Cur->lookup(N->getName().Name));
  }

  /// Can parameters Params take N arguments (the rest from default values)?
  static bool acceptsArgCount(llvm::ArrayRef<VarDecl *> Params, size_t N) {
    if (N > Params.size())
      return false;
    for (size_t I = N; I < Params.size(); ++I)
      if (!Params[I]->getDefault())
        return false;
    return true;
  }

  /// The value of an argument: of Name := value, the value.
  static Expr *argValue(Expr *Arg) {
    if (auto *NA = llvm::dyn_cast<NamedArgExpr>(Arg))
      return NA->getValue();
    return Arg;
  }

  static bool hasNamedArgs(llvm::ArrayRef<Expr *> Args) {
    for (Expr *A : Args)
      if (llvm::isa<NamedArgExpr>(A))
        return true;
    return false;
  }

  /// The parameter of Params that argument I of Args is for (by its
  /// position, or by its name), or null.
  static VarDecl *paramOfArg(llvm::ArrayRef<VarDecl *> Params,
                             llvm::ArrayRef<Expr *> Args, size_t I) {
    if (auto *NA = llvm::dyn_cast<NamedArgExpr>(Args[I])) {
      for (VarDecl *P : Params)
        if (P->getName().Name == NA->getName().Name)
          return P;
      return nullptr;
    }
    return I < Params.size() ? Params[I] : nullptr;
  }

  /// Where the arguments of a call with named ones go: Map[I] is the index
  /// in Params of the parameter of argument I. Arguments by position come
  /// first and are for the first parameters; each parameter is given once;
  /// one that is not given has a default value. False if Args do not fit,
  /// reported if Callee is given.
  bool mapArgs(llvm::ArrayRef<VarDecl *> Params, llvm::ArrayRef<Expr *> Args,
               llvm::SmallVectorImpl<unsigned> &Map,
               const Identifier *Callee) {
    llvm::SmallVector<Expr *, 8> Given(Params.size(), nullptr);
    bool Named = false;
    for (size_t I = 0; I < Args.size(); ++I) {
      Expr *A = Args[I];
      auto *NA = llvm::dyn_cast<NamedArgExpr>(A);
      size_t At = I;
      if (NA) {
        Named = true;
        for (At = 0; At < Params.size(); ++At)
          if (Params[At]->getName().Name == NA->getName().Name)
            break;
        if (At == Params.size()) {
          if (Callee)
            Diags.error(NA->getName().Loc,
                        "'" + Callee->Spelling + "' has no parameter named '" +
                            NA->getName().Spelling + "'",
                        {NA->getName().getRange()});
          return false;
        }
      } else if (Named) {
        if (Callee)
          Diags.error(A->getBeginLoc(),
                      "an argument without a name cannot follow a named one",
                      {A->getRange()});
        return false;
      } else if (At >= Params.size()) {
        if (Callee)
          Diags.error(A->getBeginLoc(),
                      "too many arguments in call to '" + Callee->Spelling +
                          "': it has " + llvm::Twine(Params.size()) +
                          (Params.size() == 1 ? " parameter" : " parameters"),
                      {A->getRange()});
        return false;
      }
      if (Given[At]) {
        if (Callee) {
          Diags.error(A->getBeginLoc(),
                      "parameter '" + Params[At]->getName().Spelling +
                          "' is given twice",
                      {A->getRange()});
          Diags.note(Given[At]->getBeginLoc(), "its other argument is here");
        }
        return false;
      }
      Given[At] = A;
      Map.push_back(static_cast<unsigned>(At));
    }
    for (size_t I = 0; I < Params.size(); ++I)
      if (!Given[I] && !Params[I]->getDefault()) {
        if (Callee)
          Diags.error(Callee->Loc,
                      "no argument for parameter '" +
                          Params[I]->getName().Spelling + "' in call to '" +
                          Callee->Spelling + "'",
                      {Callee->getRange()});
        return false;
      }
    return true;
  }

  /// The arguments of the call N to a routine with Params, in the order of
  /// the parameters: a call with named arguments (Name := value) is
  /// rewritten to one by position, where a parameter left out before the
  /// last one given takes its default value. After an error ArgsFailed is
  /// set, for checkCallArgs.
  template <class NodeT>
  llvm::ArrayRef<Expr *> orderedArgs(NodeT *N, llvm::ArrayRef<VarDecl *> Params,
                                     const Identifier &Callee) {
    llvm::ArrayRef<Expr *> Args = N->getArgs();
    if (!hasNamedArgs(Args))
      return Args;
    llvm::SmallVector<unsigned, 8> Map;
    if (!mapArgs(Params, Args, Map, &Callee)) {
      ArgsFailed = true;
      return Args;
    }
    unsigned Count = 0;
    for (unsigned At : Map)
      Count = std::max(Count, At + 1);
    llvm::SmallVector<Expr *, 8> Ordered(Count, nullptr);
    for (size_t I = 0; I < Args.size(); ++I)
      Ordered[Map[I]] = argValue(Args[I]);
    for (unsigned I = 0; I < Count; ++I)
      if (!Ordered[I])
        Ordered[I] = Params[I]->getDefault();
    N->setArgs(Ctx.copyArray<Expr *>(Ordered));
    return N->getArgs();
  }

  /// The cost of calling R with Args (types ArgTys; null: a routine's
  /// name), or -1 if they do not fit. Penalty receives the sign penalties
  /// (which break ties).
  int callCost(RoutineDecl *R, llvm::ArrayRef<Expr *> Args,
               llvm::ArrayRef<Type *> ArgTys, unsigned &Penalty) {
    Penalty = 0;
    auto Params = R->getParams();
    llvm::SmallVector<unsigned, 8> Map;
    if (hasNamedArgs(Args)) {
      if (!mapArgs(Params, Args, Map, nullptr))
        return -1;
    } else if (!acceptsArgCount(Params, Args.size())) {
      return -1;
    }
    int Cost = 0;
    for (size_t I = 0; I < Args.size(); ++I) {
      VarDecl *P = Params[Map.empty() ? I : Map[I]];
      Expr *Arg = argValue(Args[I]);
      Type *PT = P->getType();
      Type *AT = ArgTys[I];
      if (!PT || PT->isError())
        return -1;
      if (auto *SC = llvm::dyn_cast<SetCtorExpr>(Arg);
          SC && AT == ArrayOfConstTy) {
        int C = setCtorCost(SC, P);
        if (C < 0)
          return -1;
        Cost += C;
        continue;
      }
      if (auto *OA = llvm::dyn_cast<OpenArrayType>(PT)) {
        if (!AT || !fitsOpenArray(Arg, AT, OA))
          return -1;
        Cost += AT == OA ? 0 : 1;
        continue;
      }
      if (RoutineDecl *L = pendingLambda(Arg)) { // its shape
        int C = lambdaCost(L, PT);
        if (C < 0)
          return -1;
        Cost += C;
        continue;
      }
      if (!AT) { // a routine's name: for a procedural parameter
        if (!llvm::isa<RoutineType>(PT))
          return -1;
        continue;
      }
      if (PT->isUntyped()) { // any variable, after the typed fits
        if (!isVariableReference(Arg))
          return -1;
        Cost += 100;
        continue;
      }
      if (AT->isUntyped())
        return -1;
      if (P->isVarParam()) {
        if (AT != PT || !isVariableReference(Arg))
          return -1;
        continue;
      }
      if (!isAssignable(PT, AT) && !isPCharLiteral(PT, Arg)) {
        if (!findConversion(AT, PT, false))
          return -1;
        Cost += 1000; // a record's := operator
        continue;
      }
      Cost += static_cast<int>(conversionCost(PT, AT));
      Penalty += signPenalty(PT, AT);
    }
    return Cost;
  }

  /// The routine of First's name that Args fit best: First itself unless
  /// it is overloaded. Null after a diagnostic (no fit, or a tie).
  static bool hasConstArrayParam(llvm::ArrayRef<RoutineDecl *> Cands,
                                 llvm::ArrayRef<Expr *> Args, size_t I) {
    for (RoutineDecl *R : Cands)
      if (VarDecl *P = paramOfArg(R->getParams(), Args, I);
          P && P->getType() &&
          (P->getType()->isArrayOfConst() || P->getType()->isOpenArray()))
        return true;
    return false;
  }

  static bool hasProceduralParam(llvm::ArrayRef<RoutineDecl *> Cands,
                                 llvm::ArrayRef<Expr *> Args, size_t I) {
    for (RoutineDecl *R : Cands)
      if (VarDecl *P = paramOfArg(R->getParams(), Args, I);
          P && P->getType() && llvm::isa<RoutineType>(P->getType()))
        return true;
    return false;
  }

  /// The parameters of R that a call with N arguments leaves to their
  /// default values.
  static unsigned defaultsUsed(RoutineDecl *R, size_t N) {
    size_t Count = R->getParams().size();
    return Count > N ? static_cast<unsigned>(Count - N) : 0;
  }

  /// Does an expression of type A fit an open array of Elem without being
  /// an array constructor: the same open array, an array of exactly Elem,
  /// or a string constant for chars?
  bool fitsOpenArray(Expr *Arg, Type *A, OpenArrayType *OA) {
    Type *Elem = OA->getElementType();
    if (A == OA)
      return true;
    if (auto *AT = llvm::dyn_cast<ArrayType>(A)) // (not a bitpacked layout)
      return AT->getElementType() == Elem && !AT->hasBitElements();
    if (auto *DA = llvm::dyn_cast<DynArrayType>(A))
      return DA->getElementType() == Elem;
    return Elem->isChar() &&
           (A->isStringLiteral() || (A->getBase()->isChar() && fold(Arg)));
  }

  /// The cost of passing [...] for a parameter of type PT (-1: it cannot
  /// be): as an array of const first, then a set, then an open array, as in
  /// Free Pascal.
  int setCtorCost(SetCtorExpr *E, VarDecl *P) {
    Type *PT = P->getType();
    if (PT->isArrayOfConst())
      return 0;
    int Cost = 0;
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(PT)) {
      if (P->isVarParam())
        return -1;
      Cost = 2;
      for (const SetElement &El : E->getElements()) {
        if (El.Hi)
          return -1;
        Type *T = check(El.Lo);
        if (T->isError() ||
            (!isAssignable(OA->getElementType(), T) &&
             !isPCharLiteral(OA->getElementType(), El.Lo)))
          return -1;
        Cost += conversionCost(OA->getElementType(), T);
      }
      return Cost;
    }
    auto *ST = llvm::dyn_cast<SetType>(PT);
    if (!ST || P->isVarParam())
      return -1;
    for (const SetElement &El : E->getElements())
      for (Expr *X : {El.Lo, El.Hi}) {
        if (!X)
          continue;
        Type *T = check(X);
        if (T->isError() || !T->isOrdinal() ||
            !haveSameOrdinalHost(ST->getElementType(), T))
          return -1;
      }
    return 1;
  }

  void checkOpenArrayArg(Expr *Arg, VarDecl *P, OpenArrayType *OA) {
    const Identifier &PName = P->getName();
    Type *Elem = OA->getElementType();
    if (auto *SC = llvm::dyn_cast<SetCtorExpr>(Arg)) {
      if (P->isVarParam()) {
        Diags.error(Arg->getBeginLoc(),
                    "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + PName.Spelling +
                        "' must be a variable",
                    {Arg->getRange()});
        return;
      }
      for (const SetElement &El : SC->getElements()) {
        if (El.Hi) {
          Diags.error(El.Hi->getBeginLoc(),
                      "a range cannot be an element of an open array",
                      {El.Hi->getRange()});
          continue;
        }
        Type *T = check(El.Lo);
        if (T->isError())
          continue;
        if (!isAssignable(Elem, T) && !isPCharLiteral(Elem, El.Lo) &&
            !convertImplicitly(El.Lo, Elem))
          Diags.error(El.Lo->getBeginLoc(),
                      "a value of type " + quoted(T) +
                          " cannot be an element of an open array of " +
                          quoted(Elem),
                      {El.Lo->getRange()});
      }
      SC->setType(OA);
      return;
    }
    bool Slice = isSliceCall(Arg);
    AllowSlice = Slice;
    Type *AT = check(Arg);
    AllowSlice = false;
    if (AT->isError())
      return;
    if (!fitsOpenArray(Arg, AT, OA)) {
      Diags.error(Arg->getBeginLoc(),
                  "cannot pass a value of type " + quoted(AT) + " to parameter '" +
                      PName.Spelling + "' of type " + quoted(OA),
                  {Arg->getRange()});
      return;
    }
    if (P->isVarParam()) {
      Expr *Target = Slice ? llvm::cast<CallExpr>(Arg)->getArgs()[0] : Arg;
      if (isBitField(Target))
        diagBitFieldAddress(Target);
      else if (!isVariableReference(Target))
        Diags.error(Arg->getBeginLoc(),
                    "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + PName.Spelling +
                        "' must be a variable",
                    {Arg->getRange()});
      else
        checkModifiable(Target);
    }
  }

  /// [a, b, ...] passed for an array of const: any values it can hold.
  void checkConstArray(SetCtorExpr *E, Type *PT) {
    for (const SetElement &El : E->getElements()) {
      if (El.Hi) {
        Diags.error(El.Hi->getBeginLoc(),
                    "a range cannot be an element of an array of const",
                    {El.Hi->getRange()});
        continue;
      }
      Type *T = check(El.Lo);
      if (T->isError())
        continue;
      if (T->getBase()->isWideChar() ||
          !(T->isIntegerLike() || T->isReal() || T->getBase()->isChar() ||
            T->getBase()->isBoolean() || T->isStringish() || T->isPointer() ||
            T->isNil() || T->isClass() || T->isClassRef()))
        Diags.error(El.Lo->getBeginLoc(),
                    "a value of type " + quoted(T) +
                        " cannot be an element of an array of const",
                    {El.Lo->getRange()});
    }
    E->setType(PT);
  }

  /// E is a lambda not completed yet: its routine.
  static RoutineDecl *pendingLambda(Expr *E) {
    auto *A = llvm::dyn_cast<AddrOfExpr>(E);
    RoutineDecl *L = A ? A->getLambda() : nullptr;
    if (auto *C = llvm::dyn_cast<CallExpr>(E)) // one that copies
      L = C->getCopyLambda();
    return L && !L->isLambdaDone() ? L : nullptr;
  }

  /// The types written in lambda L (of its parameters and result), resolved
  /// once while overloads are chosen (kept as their types until
  /// completeLambda sets them). False after a diagnostic.
  bool resolveLambdaSpecs(RoutineDecl *L) {
    for (VarDecl *P : L->getParams())
      if (P->getTypeSpec() && !P->getType()) {
        Type *T = resolveType(P->getTypeSpec());
        if (T->isError())
          return false;
        P->setType(T);
      }
    if (L->isFunction() && L->getResultTypeSpec() && !L->getResultType()) {
      Type *T = resolveType(L->getResultTypeSpec());
      if (T->isError())
        return false;
      L->setResultType(T);
    }
    return true;
  }

  /// The cost of passing lambda L for a parameter of type PT, by its
  /// shape (-1: it does not fit): a procedural type of the same kind
  /// (function or procedure), with as many parameters, of the types and
  /// kinds L writes, and the result type it writes. A parameter written
  /// with a type but without a kind takes the kind wanted, but fits a value
  /// parameter best (cost 1 for each other); one without either fits any
  /// parameter alike (so overloads that differ there are ambiguous).
  int lambdaCost(RoutineDecl *L, Type *PT) {
    auto *RT = llvm::dyn_cast<RoutineType>(PT);
    if (!RT || RT->isFunction() != L->isFunction() ||
        RT->getParams().size() != L->getParams().size() ||
        (L->isCopied() && !RT->isReference())) // (copies: a closure)
      return -1;
    int Cost = 0;
    for (size_t I = 0; I < RT->getParams().size(); ++I) {
      VarDecl *P = L->getParams()[I], *WP = RT->getParams()[I];
      if (P->getTypeSpec() && P->getType() != WP->getType())
        return -1;
      bool WantsValue = WP->getVarKind() == VarDecl::ValueParam &&
                        !WP->isOutParam() && !WP->isConstRefParam();
      if (P->getVarKind() == VarDecl::ValueParam)
        Cost += WantsValue || !P->getTypeSpec() ? 0 : 1;
      else if (P->getVarKind() != WP->getVarKind() ||
               P->isOutParam() != WP->isOutParam() ||
               P->isConstRefParam() != WP->isConstRefParam())
        return -1;
    }
    if (L->isFunction() && L->getResultTypeSpec() &&
        L->getResultType() != RT->getResult())
      return -1;
    return Cost;
  }

  RoutineDecl *resolveOverload(RoutineDecl *First, llvm::ArrayRef<Expr *> Args,
                               const Identifier &Callee, SourceRange Range) {
    llvm::SmallVector<RoutineDecl *, 4> Cands;
    collectOverloads(First, Cands);
    if (Cands.size() < 2)
      return First;
    llvm::SmallVector<Type *, 4> ArgTys;
    for (Expr *Written : Args) {
      Expr *A = argValue(Written);
      if (llvm::isa<FormattedExpr>(A))
        return First; // reported with the call's arguments
      // A lambda: chosen by its shape (lambdaFits), completed afterwards
      // with the parameter's type.
      if (RoutineDecl *L = pendingLambda(A)) {
        if (!resolveLambdaSpecs(L))
          return nullptr;
        ArgTys.push_back(nullptr);
        continue;
      }
      // [...] for an open array need not be a valid set.
      if (llvm::isa<SetCtorExpr>(A) &&
          hasConstArrayParam(Cands, Args, ArgTys.size())) {
        ArgTys.push_back(ArrayOfConstTy);
        continue;
      }
      AllowSlice = isSliceCall(A);
      // A routine's name: for a procedural parameter if a candidate has
      // one here, else a call (a function without parameters).
      Type *T = isRoutineName(A) &&
                        hasProceduralParam(Cands, Args, ArgTys.size())
                    ? nullptr
                    : checkAllowingUntyped(A);
      AllowSlice = false;
      if (T && T->isError())
        return nullptr;
      ArgTys.push_back(T);
    }
    // The best conversions of the arguments written decide; then the
    // fewest parameters left to their default values (a routine that takes
    // just these arguments is preferred); a tie is an error.
    RoutineDecl *Best = nullptr;
    int BestCost = -1;
    unsigned BestPenalty = 0, BestDefaults = 0;
    bool Tie = false;
    for (RoutineDecl *R : Cands) {
      unsigned Penalty;
      int Cost = callCost(R, Args, ArgTys, Penalty);
      if (Cost < 0)
        continue;
      unsigned Defaults = defaultsUsed(R, Args.size());
      if (!Best || Cost < BestCost ||
          (Cost == BestCost && Penalty < BestPenalty) ||
          (Cost == BestCost && Penalty == BestPenalty &&
           Defaults < BestDefaults)) {
        Best = R;
        BestCost = Cost;
        BestPenalty = Penalty;
        BestDefaults = Defaults;
        Tie = false;
      } else if (Cost == BestCost && Penalty == BestPenalty &&
                 Defaults == BestDefaults) {
        Tie = true;
      }
    }
    if (Best && !Tie)
      return Best;
    std::string Types;
    for (size_t I = 0; I < ArgTys.size(); ++I) {
      Types += I ? ", " : "";
      if (auto *NA = llvm::dyn_cast<NamedArgExpr>(Args[I]))
        Types += NA->getName().Spelling.str() + " := ";
      Types += ArgTys[I]                        ? ArgTys[I]->getName()
               : pendingLambda(argValue(Args[I])) ? lambdaShape(
                                                        pendingLambda(argValue(Args[I])))
                                                  : std::string("routine");
    }
    Diags.error(Callee.Loc,
                (Best ? "ambiguous call to overloaded '"
                      : "no overload of '") +
                    Callee.Spelling +
                    (Best ? "' with arguments (" : "' takes arguments (") +
                    Types + ")",
                {Range});
    for (RoutineDecl *R : Cands) {
      unsigned Penalty;
      if (!Best || (callCost(R, Args, ArgTys, Penalty) == BestCost &&
                    Penalty == BestPenalty &&
                    defaultsUsed(R, Args.size()) == BestDefaults))
        Diags.note(R->getLocation(), "candidate: " + routineHeading(R));
    }
    return nullptr;
  }

  /// A lambda's shape for diagnostics: "function(integer; ?): ?" (? for
  /// types left out).
  std::string lambdaShape(RoutineDecl *L) {
    std::string S = L->isFunction() ? "lambda function(" : "lambda procedure(";
    auto Params = L->getParams();
    for (size_t I = 0; I < Params.size(); ++I) {
      Type *T = Params[I]->getTypeSpec() ? Params[I]->getType() : nullptr;
      S += std::string(I ? "; " : "") +
           (Params[I]->isOutParam()        ? "out "
            : Params[I]->isVarParam()      ? "var "
            : Params[I]->isConstRefParam() ? "constref "
            : Params[I]->isConstParam()    ? "const "
                                           : "") +
           (T ? T->getName() : std::string("?"));
    }
    S += ")";
    if (L->isFunction()) {
      Type *T = L->getResultTypeSpec() ? L->getResultType() : nullptr;
      S += ": " + (T ? T->getName() : std::string("?"));
    }
    return S;
  }

  /// "f(integer; var string): real" for diagnostics.
  static std::string routineHeading(RoutineDecl *R) {
    std::string S = R->getName().Spelling.str() + "(";
    auto Params = R->getParams();
    for (size_t I = 0; I < Params.size(); ++I)
      S += std::string(I ? "; " : "") +
           (Params[I]->isOutParam()     ? "out "
            : Params[I]->isVarParam()   ? "var "
            : Params[I]->isConstRefParam() ? "constref "
            : Params[I]->isConstParam() ? "const "
                                        : "") +
           (Params[I]->getType() ? Params[I]->getType()->getName()
                                 : std::string("?"));
    S += ")";
    if (R->isFunction() && R->getResultType())
      S += ": " + R->getResultType()->getName();
    return S;
  }

  /// The string constant E, the library or symbol name (What) of an
  /// external routine; nullopt after a diagnostic.
  std::optional<llvm::StringRef> externalString(Expr *E, const char *What,
                                                const char *Of = "routine") {
    Type *T = check(E);
    if (T->isError())
      return std::nullopt;
    std::optional<Constant> C = fold(E);
    if (C && C->Value.K == ConstValue::String)
      return C->Value.Str;
    if (C && C->Value.K == ConstValue::Ordinal && T->getBase()->isChar()) {
      char Ch = static_cast<char>(C->Value.Int);
      return llvm::StringRef(Ctx.copyArray<char>(llvm::ArrayRef<char>(&Ch, 1)).data(), 1);
    }
    Diags.error(E->getBeginLoc(),
                llvm::Twine("the ") + What + " of an external " + Of +
                    " must be a string constant",
                {E->getRange()});
    return std::nullopt;
  }

  /// A type with a C equivalent, for external routines.
  static bool isCType(Type *T) {
    return T->isError() || T->isReal() || T->isPointer() ||
           T->isToken() || // (the runtime's token, not counted by the call)
           (T->isOrdinal() && !T->getBase()->isBoolean());
  }

  /// A procedural type C code can call: cdecl, with parameters and result
  /// of C types.
  static bool isCCallback(RoutineType *RT) {
    if (!RT->isCDecl() || (RT->getResult() && !isCType(RT->getResult())))
      return false;
    for (VarDecl *P : RT->getParams())
      if (P->getType() && !P->isVarParam() && !isCType(P->getType()))
        return false;
    return true;
  }

  /// External (C) routines: declared at the outermost level, with
  /// parameters and result of types that have a C equivalent; the library
  /// and symbol names: string constants.
  void checkExternal(RoutineDecl *R) {
    if (CurRoutine)
      Diags.error(R->getName().Loc,
                  "external routine '" + R->getName().Spelling +
                      "' must be declared at program or unit level",
                  {R->getName().getRange()});
    if (Expr *L = R->getLibraryExpr())
      if (auto S = externalString(L, "library name"))
        R->setLibraryName(*S);
    if (Expr *N = R->getNameExpr())
      if (auto S = externalString(N, "symbol name"))
        R->setExternalName(*S);
    if (R->isVarArgs() && !R->isCDecl())
      Diags.error(R->getName().Loc,
                  "'varargs' routine '" + R->getName().Spelling +
                      "' must be 'cdecl'",
                  {R->getName().getRange()});
    for (VarDecl *P : R->getParams()) {
      // (an untyped parameter: a pointer, `void *` in C)
      if (!P->getType() || P->isVarParam() || P->getType()->isUntyped())
        continue;
      if (auto *RT = llvm::dyn_cast<RoutineType>(P->getType())) {
        if (!isCCallback(RT))
          Diags.error(P->getLocation(),
                      "parameter '" + P->getName().Spelling +
                          "' of external routine '" + R->getName().Spelling +
                          "' has type " + quoted(P->getType()) +
                          "; a callback needs a 'cdecl' procedural type with "
                          "integer, real, char, pointer or var parameters",
                      {P->getName().getRange()});
        continue;
      }
      if (!isCType(P->getType()))
        Diags.error(P->getLocation(),
                    "parameter '" + P->getName().Spelling + "' of external routine '" +
                        R->getName().Spelling + "' has type " +
                        quoted(P->getType()) +
                        "; use integer, real, char, pointer, procedural or "
                        "var parameters",
                    {P->getName().getRange()});
    }
    if (R->isFunction() && !isCType(R->getResultType()))
      Diags.error(R->getName().Loc,
                  "external function '" + R->getName().Spelling +
                      "' cannot return " + quoted(R->getResultType()),
                  {R->getName().getRange()});
  }

  /// A string constant passed where a pchar is expected (Turbo/FPC): the
  /// compiler supplies a NUL-terminated copy.
  bool isPCharLiteral(Type *Target, Expr *Value) {
    auto *P = llvm::dyn_cast<PointerType>(Target);
    if (isISO() || !P || P->isUntyped() || !P->getPointee() ||
        !P->getPointee()->isChar())
      return false;
    if (llvm::isa<StringLiteralExpr>(Value))
      return true;
    auto *N = llvm::dyn_cast<NameRefExpr>(Value);
    auto *C = N ? llvm::dyn_cast_or_null<ConstDecl>(N->getDecl()) : nullptr;
    return C && (C->getValue().K == ConstValue::String ||
                 (C->getType() && C->getType()->getBase()->isChar()));
  }

  void checkRoutineBody(RoutineDecl *R) {
    if (DeferredBodies) { // of a specialization being declared: later
      DeferredBodies->push_back(R);
      return;
    }
    if (R->isAsync())
      checkAsyncRoutine(R);
    // The method of a nested class or record also sees the constants and
    // types of the classes around it.
    llvm::SmallVector<std::unique_ptr<Scope>, 2> OuterScopes;
    Scope *Base = Cur;
    if (ClassType *RC = R->getClass()) {
      llvm::SmallVector<ClassType *, 2> Outers;
      for (ClassType *O = RC->getOuter(); O; O = O->getOuter())
        Outers.push_back(O);
      for (ClassType *O : llvm::reverse(Outers)) {
        OuterScopes.push_back(std::make_unique<Scope>(Scope::nestedIn(Base, O)));
        Base = OuterScopes.back().get();
      }
    }
    // A method sees the members of its class through Self.
    std::unique_ptr<Scope> RecordScope, ClassScope, FieldsScope;
    // (a method-pointer lambda outside methods: a Self no one sees)
    bool SelfSeen = R->hasSelf() && R->getClass();
    if (SelfSeen) {
      auto *SelfRef = Ctx.create<NameRefExpr>(R->getSelfVar()->getName());
      SelfRef->setDecl(R->getSelfVar());
      SelfRef->setType(R->getSelfVar()->getType());
      auto *W = Ctx.create<WithContext>(SelfRef, /*IsSelf=*/true);
      W->Routine = R;
      auto *RT = llvm::dyn_cast_or_null<RecordType>(
          R->getClass()->getHelperFor());
      if (R->getClass()->isHelper())
        W->Helper = R->getClass();
      // A helper's method for an advanced record also sees the record's
      // methods and properties (the helper's own members first).
      if (ClassType *MT = memberTableOf(RT)) {
        RecordScope = std::make_unique<Scope>(Base, MT, W);
        Base = RecordScope.get();
      }
      ClassScope = std::make_unique<Scope>(Base, R->getClass(), W);
      // A record helper's method also sees the record's fields.
      if (RT)
        FieldsScope = std::make_unique<Scope>(ClassScope.get(), RT, W);
    } else if (R->isStatic()) { // a record's static method: its statics
      ClassScope = std::make_unique<Scope>(Base, R->getClass(), nullptr);
    }
    Scope RoutineScope(FieldsScope  ? FieldsScope.get()
                       : ClassScope ? ClassScope.get()
                                    : Base);
    (RecordScope  ? RecordScope.get()
     : ClassScope ? ClassScope.get()
                  : &RoutineScope)
        ->setRoutineBoundary();
    Scope *SavedScope = Cur;
    RoutineDecl *SavedRoutine = CurRoutine;
    Cur = &RoutineScope;
    CurRoutine = R;

    if (SelfSeen)
      Cur->insert("self", R->getSelfVar());
    for (VarDecl *P : R->getParams())
      if (Decl *Prev = Cur->insert(P->getName().Name, P)) {
        Diags.error(P->getLocation(),
                    "duplicate parameter name '" + P->getName().Spelling + "'",
                    {P->getName().getRange()});
        if (Prev->isPredeclared())
          Diags.note(P->getLocation(), "'Self' is the object a method is "
                                       "called on");
        else
          Diags.note(Prev->getLocation(), "previous parameter is here");
      }
    // objfpc: `Result` names the function result.
    if (R->hasResultName() && R->isFunction() && R->getResultVar())
      Cur->insert("result", R->getResultVar());

    checkBlock(*R->getBody());

    if (R->isFunction() && !ResultAssigned.count(R) && R->getResultType() &&
        !R->getResultType()->isError())
      Diags.warning(R->getName().Loc,
                    "function '" + R->getName().Spelling +
                        "' never assigns its result",
                    {R->getName().getRange()});

    Cur = SavedScope;
    CurRoutine = SavedRoutine;
  }

  /// Resolves the pointer types since Mark whose targets are visible now
  /// (declared in a class); the others stay pending.
  void resolvePendingPointersFrom(size_t Mark) {
    for (size_t I = Mark; I < PendingPointers.size();) {
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
          Cur->lookup(PendingPointers[I].first->getTarget().Name));
      if (TD && TD->getType()) {
        PendingPointers[I].second->setPointee(TD->getType());
        PendingPointers.erase(PendingPointers.begin() + I);
      } else {
        ++I;
      }
    }
  }

  void resolvePendingPointers() {
    for (auto &[PS, P] : PendingPointers) {
      const Identifier &Id = PS->getTarget();
      Decl *D = Cur->lookup(Id.Name);
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(D);
      if (TD && TD->getType()) {
        P->setPointee(TD->getType());
        continue;
      }
      if (D)
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' is a " + D->getKindName() +
                        ", not a type",
                    {Id.getRange()});
      else
        Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'" +
                        didYouMean(Id.Name),
                    {Id.getRange()});
      P->setPointee(errorType());
    }
    PendingPointers.clear();
  }

  void checkBlock(Block &B) {
    declareAll(B);
    finishBlock(B, /*WarnUnused=*/true);
  }

  void declareAll(Block &B) {
    for (Decl *D : B.getDecls()) {
      if (Diags.shouldStop())
        return;
      if (!llvm::isa<TypeDecl>(D)) {
        resolvePendingPointers();
        checkPendingClasses();
      }
      switch (D->getKind()) {
      case Decl::DK_Const:   declareConst(llvm::cast<ConstDecl>(D)); break;
      case Decl::DK_Type:    declareType(llvm::cast<TypeDecl>(D)); break;
      case Decl::DK_Var:     declareVar(llvm::cast<VarDecl>(D)); break;
      case Decl::DK_Routine: declareRoutine(llvm::cast<RoutineDecl>(D)); break;
      case Decl::DK_Label: {
        auto *L = llvm::cast<LabelDecl>(D);
        L->setOwner(CurRoutine);
        if (Decl *Prev = Cur->insert(labelKey(L->getName().Name), L)) {
          Diags.error(L->getLocation(),
                      "label " + L->getName().Name + " is declared twice",
                      {L->getName().getRange()});
          Diags.note(Prev->getLocation(), "previous declaration is here");
        }
        break;
      }
      default: llvm_unreachable("unexpected declaration in block");
      }
    }
    resolvePendingPointers();
    checkPendingClasses();
  }

  void finishBlock(Block &B, bool WarnUnused) {
    // ISO 7185 6.1.4: every forward declaration needs a definition in the
    // same block.
    for (Decl *D : B.getDecls())
      if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
          R && R->isForward() && isUndefinedHeading(R))
        Diags.error(R->getName().Loc,
                    "'" + R->getName().Spelling +
                        "' is declared forward but never defined",
                    {R->getName().getRange()});
    checkMethodsImplemented(B);
    CompoundStmt *SavedBody = RoutineBody;
    RoutineBody = B.getBody();
    unsigned SavedHandlers = HandlerDepth;
    int SavedFinally = FinallyLoopDepth;
    HandlerDepth = 0;
    FinallyLoopDepth = -1;
    OwnedFlow *SavedFlow = Flow;
    OwnedFlow BlockFlow;
    BlockFlow.S.Valid = LiveBit;
    Flow = &BlockFlow;
    for (Decl *D : B.getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->isOwned())
        ownedDeclare(V);
    std::vector<Decl *> *SavedInline = InlineVars;
    std::vector<Decl *> BlockInline;
    InlineVars = &BlockInline;
    visitStmt(B.getBody());
    InlineVars = SavedInline;
    Flow = SavedFlow;
    if (!BlockInline.empty()) { // they are the block's too, after the others
      BlockInline.insert(BlockInline.begin(), B.getDecls().begin(),
                         B.getDecls().end());
      B.setDecls(Ctx.copyArray<Decl *>(BlockInline));
    }
    HandlerDepth = SavedHandlers;
    FinallyLoopDepth = SavedFinally;
    RoutineBody = SavedBody;

    for (Decl *D : B.getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D); WarnUnused && V && !Used.count(V))
        Diags.warning("unused-variable", V->getLocation(),
                      "variable '" + V->getName().Spelling +
                          "' is declared but never used",
                      {V->getName().getRange()});

    // ISO 7185 6.8.1: every declared label prefixes exactly one statement.
    for (Decl *D : B.getDecls())
      if (auto *L = llvm::dyn_cast<LabelDecl>(D);
          L && !L->getTarget() &&
          Cur->lookupLocal(labelKey(L->getName().Name)) == L) // not a duplicate
        Diags.error(L->getLocation(),
                    "label " + L->getName().Name +
                        " is declared but does not label any statement",
                    {L->getName().getRange()});
  }

  void checkGotos() {
    for (PendingGoto &G : PendingGotos) {
      LabelDecl *L = G.S->getDecl();
      if (!L || !L->getTarget())
        continue;
      bool OK;
      if (L->getOwner() == G.From) {
        OK = llvm::is_contained(G.Containers, L->getTarget()) ||
             (L->getSequence() &&
              llvm::is_contained(G.Containers, L->getSequence()));
      } else {
        // A goto out of a nested routine may only target the outermost
        // statement sequence of the enclosing block.
        OK = L->isTopLevel();
        if (OK) {
          L->setNonLocal();
          if (!L->getIndex())
            L->setIndex(++NonLocalLabelCount[L->getOwner()]);
        }
      }
      if (!OK) {
        Diags.error(G.S->getBeginLoc(),
                    "'goto " + L->getName().Name +
                        "' jumps into a statement that does not contain it",
                    {G.S->getRange()});
        Diags.note(L->getTarget()->getBeginLoc(),
                   "label " + L->getName().Name +
                       " is here; it can only be reached from its own "
                       "statement sequence");
      }
    }
  }

  void checkProgramParams(ProgramDecl &P) {
    llvm::StringMap<SourceLocation> Seen;
    for (const Identifier &Param : P.getParams()) {
      auto [It, Inserted] = Seen.try_emplace(Param.Name, Param.Loc);
      if (!Inserted) {
        Diags.error(Param.Loc,
                    "duplicate program parameter '" + Param.Spelling + "'",
                    {Param.getRange()});
        Diags.note(It->second, "previous occurrence is here");
        continue;
      }
    }
  }

  /// ISO 7185 6.10: program parameters other than input/output must be
  /// declared as file variables in the program block. They are bound to the
  /// command-line arguments in order.
  void checkProgramParamDecls(ProgramDecl &P) {
    for (const Identifier &Param : P.getParams()) {
      if (Param.Name == "input" || Param.Name == "output")
        continue;
      Decl *D = Cur->lookupLocal(Param.Name);
      auto *V = llvm::dyn_cast_or_null<VarDecl>(D);
      if (V)
        Used.insert(V); // bound to a command-line argument
      if (V && V->getType() && (V->getType()->isFile() || V->getType()->isError()))
        continue;
      Diags.error(Param.Loc,
                  "program parameter '" + Param.Spelling +
                      "' must be declared as a file variable in the program",
                  {Param.getRange()});
    }
  }

  //===--------------------------------------------------------------------===//
  // Statement helpers
  //===--------------------------------------------------------------------===//

  void checkCondition(Expr *Cond, llvm::StringRef Keyword) {
    Type *T = check(Cond);
    if (!T->isError() && !T->getBase()->isBoolean())
      Diags.error(Cond->getBeginLoc(),
                  "condition of '" + Keyword + "' must be of type 'boolean', "
                  "not " + quoted(T),
                  {Cond->getRange()});
  }

  /// Reports an assignment (or read / var argument) to the control variable
  /// of an enclosing for statement. Returns true if one was reported.
  bool checkNotForControlVar(Expr *Target) {
    auto *N = llvm::dyn_cast<NameRefExpr>(Target);
    if (!N || !N->getDecl())
      return false;
    for (const ActiveFor &F : ActiveFors) {
      if (F.Var != N->getDecl())
        continue;
      Diags.error(N->getBeginLoc(),
                  "cannot assign to '" + N->getName().Spelling +
                      "' inside a 'for' loop that it controls",
                  {N->getRange()});
      Diags.note(F.Loc, "'for' loop is here");
      return true;
    }
    return false;
  }

  /// The order of label value V of a selector of type SelTy: qword's
  /// values compare unsigned.
  static int64_t labelKey(int64_t V, Type *SelTy) {
    return isQWordValue(SelTy) ? V ^ INT64_MIN : V;
  }

  /// Checks one case or variant label against the selector/tag type.
  bool checkLabel(CaseLabel *L, Type *SelTy, llvm::StringRef What) {
    bool OK = true;
    Type *LoTy = nullptr, *HiTy = nullptr;
    for (Expr *E : {L->Lo, L->Hi}) {
      if (!E)
        continue;
      auto C = evaluateConstant(E, What);
      if (!C) {
        OK = false;
        continue;
      }
      if (!SelTy->isError() && !areCompatible(C->Ty, SelTy)) {
        Diags.error(E->getBeginLoc(),
                    What + " of type " + quoted(C->Ty) + " does not match " +
                        (What == "case label" ? "selector" : "tag") +
                        " type " + quoted(SelTy),
                    {E->getRange()});
        OK = false;
        continue;
      }
      (E == L->Lo ? L->LoValue : L->HiValue) = C->Value.Int;
      (E == L->Lo ? LoTy : HiTy) = C->Ty;
    }
    if (!L->Hi) {
      L->HiValue = L->LoValue;
      HiTy = LoTy;
    }
    if (OK && labelKey(L->LoValue, SelTy) > labelKey(L->HiValue, SelTy)) {
      Diags.error(L->Lo->getBeginLoc(), "case label range is empty",
                  {L->getRange()});
      OK = false;
    }
    if (OK && !SelTy->isError()) {
      if (!constantInRange(L->LoValue, LoTy, SelTy) ||
          !constantInRange(L->HiValue, HiTy, SelTy)) {
        Diags.error(L->Lo->getBeginLoc(),
                    What + " is outside the range of type " + quoted(SelTy),
                    {L->getRange()});
        OK = false;
      }
    }
    return OK;
  }

  /// ISO 7185 6.6.3.6: a procedure/function passed as an argument must have
  /// congruent parameter lists (same kinds, identical types) and result.
  static bool congruent(llvm::ArrayRef<VarDecl *> A, Type *AResult,
                        llvm::ArrayRef<VarDecl *> B, Type *BResult) {
    if (A.size() != B.size() || (AResult == nullptr) != (BResult == nullptr) ||
        (AResult && AResult != BResult))
      return false;
    for (size_t I = 0; I < A.size(); ++I) {
      if (A[I]->getVarKind() != B[I]->getVarKind() ||
          A[I]->isOutParam() != B[I]->isOutParam() ||
          A[I]->isConstRefParam() != B[I]->isConstRefParam())
        return false;
      Type *TA = A[I]->getType(), *TB = B[I]->getType();
      auto *RA = llvm::dyn_cast_or_null<RoutineType>(TA);
      auto *RB = llvm::dyn_cast_or_null<RoutineType>(TB);
      if (RA && RB) {
        if (RA->isCDecl() != RB->isCDecl() ||
            RA->isOfObject() != RB->isOfObject() ||
            RA->isReference() != RB->isReference() ||
            !congruent(RA->getParams(), RA->getResult(), RB->getParams(),
                       RB->getResult()))
          return false;
      } else if (TA != TB) {
        return false;
      }
    }
    return true;
  }

  /// An argument for a procedural/functional parameter: the name of a
  /// routine (or of another procedural parameter) with a congruent heading.
  void checkRoutineArg(Expr *Arg, VarDecl *P, RoutineType *Formal) {
    auto *LA = llvm::dyn_cast<AddrOfExpr>(Arg);
    if (LA) { // a lambda: its types
      completeLambda(LA, Formal);
      if (LA->getLambda() && LA->getLambda()->isLambdaBad())
        return;
    }
    // @obj.M, or a method pointer; a closure (any value of the type)
    if (Formal->isOfObject() || Formal->isReference()) {
      Type *AT = checkValueFor(Formal, Arg);
      if (AT->isError())
        return;
      if (!isAssignable(Formal, AT))
        Diags.error(Arg->getBeginLoc(),
                    "cannot pass a value of type " + quoted(AT) + " to parameter '" +
                        P->getName().Spelling + "' of type " + quoted(Formal),
                    {Arg->getRange()});
      else if (P->isVarParam() && isBitField(Arg))
        diagBitFieldAddress(Arg);
      else if (P->isVarParam() && !isVariableReference(Arg))
        Diags.error(Arg->getBeginLoc(),
                    "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + P->getName().Spelling +
                        "' must be a variable",
                    {Arg->getRange()});
      else if (P->isVarParam())
        checkModifiable(Arg);
      return;
    }
    const char *Kind = Formal->isFunction() ? "function" : "procedure";
    // Turbo/FPC: @f, as objfpc requires (the operand: a routine's name).
    auto *A = isISO() ? nullptr : llvm::dyn_cast<AddrOfExpr>(Arg);
    if (A)
      A->setType(P->getType());
    if (A && llvm::isa<FieldExpr>(A->getOperand())) { // @TFoo.M (static)
      const Identifier &Name = llvm::cast<FieldExpr>(A->getOperand())->getField();
      RoutineType *SavedTarget = AddrOfTarget;
      AddrOfTarget = Formal;
      A->setType(nullptr);
      Type *T = check(A);
      AddrOfTarget = SavedTarget;
      A->setType(P->getType());
      if (T->isError())
        return;
      RoutineDecl *R = A->getCode();
      if (!R) {
        Diags.error(Arg->getBeginLoc(),
                    llvm::Twine("argument for ") + Kind + " parameter '" +
                        P->getName().Spelling + "' must be a " + Kind +
                        " or a static class method",
                    {Arg->getRange()});
        return;
      }
      if (R->isCDecl() != Formal->isCDecl() ||
          !congruent(Formal->getParams(), Formal->getResult(), R->getParams(),
                     R->isFunction() ? R->getResultType() : nullptr))
        Diags.error(Arg->getBeginLoc(),
                    "'" + Name.Spelling +
                        "' does not match the heading of parameter '" +
                        P->getName().Spelling + "' (" + quoted(Formal) + ")",
                    {Arg->getRange()});
      return;
    }
    auto *N = llvm::dyn_cast<NameRefExpr>(A ? A->getOperand() : Arg);
    Decl *D = N ? Cur->lookup(N->getName().Name) : nullptr;
    llvm::ArrayRef<VarDecl *> Params;
    Type *Result = nullptr;
    bool Found = false, CDecl = false;
    if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D); R && !R->isMethod()) {
      // objfpc: of overloaded routines, the one whose heading fits
      for (RoutineDecl *X = R; R->isOverloaded() && X; X = X->getNextOverload())
        if (X->isCDecl() == Formal->isCDecl() &&
            congruent(Formal->getParams(), Formal->getResult(), X->getParams(),
                      X->isFunction() ? X->getResultType() : nullptr)) {
          D = R = X;
          break;
        }
      Params = R->getParams();
      Result = R->isFunction() ? R->getResultType() : nullptr;
      CDecl = R->isCDecl();
      Found = true;
    } else if (auto *V = llvm::dyn_cast_or_null<VarDecl>(D)) {
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType())) {
        Params = RT->getParams();
        Result = RT->getResult();
        CDecl = RT->isCDecl();
        Found = true;
      }
    }
    if (!Found) {
      Diags.error(Arg->getBeginLoc(),
                  llvm::Twine("argument for ") + Kind + " parameter '" +
                      P->getName().Spelling + "' must be the name of a " +
                      Kind +
                      (llvm::isa_and_nonnull<BuiltinDecl>(D)
                           ? " (required procedures and functions cannot be "
                             "passed)"
                           : ""),
                  {Arg->getRange()});
      return;
    }
    N->setDecl(D);
    N->setType(P->getType());
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
      std::string Why = notProceduralValue(R, !Formal->isCDecl());
      if (!Why.empty()) {
        Diags.error(Arg->getBeginLoc(),
                    "'" + N->getName().Spelling + "': " + Why,
                    {Arg->getRange()});
        return;
      }
    }
    if (CDecl != Formal->isCDecl() ||
        !congruent(Formal->getParams(), Formal->getResult(), Params, Result))
      Diags.error(Arg->getBeginLoc(),
                  (LA && LA->getLambda()
                       ? std::string("this lambda")
                       : "'" + N->getName().Spelling.str() + "'") +
                      " does not match the heading of parameter '" +
                      P->getName().Spelling + "' (" + quoted(Formal) + ")",
                  {Arg->getRange()});
  }

  /// An argument (number I, from 0) beyond the parameters of a 'varargs'
  /// external routine: a value C can take (ordinals are passed as int at
  /// least, reals as double, string constants as C strings).
  void checkVarArg(Expr *Arg, size_t I, const Identifier &Callee) {
    if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
      Diags.error(F->getWidth()->getBeginLoc(),
                  "field widths are only allowed in 'write' and 'writeln'",
                  {F->getWidth()->getRange()});
      return;
    }
    Type *T = check(Arg);
    auto *N = llvm::dyn_cast<NameRefExpr>(Arg);
    bool StringConst = T->isStringLiteral() &&
                       (llvm::isa<StringLiteralExpr>(Arg) ||
                        (N && llvm::isa_and_nonnull<ConstDecl>(N->getDecl())));
    if (T->isError() || T->isOrdinal() || T->isReal() || T->isPointer() ||
        T->isClass() || T->isNil() || StringConst)
      return;
    Diags.error(Arg->getBeginLoc(),
                "argument " + llvm::Twine(I + 1) + " of '" + Callee.Spelling +
                    "' has type " + quoted(T) +
                    "; a 'varargs' argument must be an integer, real, char, "
                    "boolean, pointer or string constant",
                {Arg->getRange()});
  }

  /// Why routine R cannot be a procedural value (empty: it can). Nested
  /// routines can be passed to ISO procedural parameters (AllowNested).
  static std::string notProceduralValue(RoutineDecl *R, bool AllowNested) {
    if (R->getParent() && !AllowNested && !R->isLambda())
      return "only routines declared at program or unit level can be "
             "procedural values";
    if (!R->isExternal())
      return "";
    if (!R->isCDecl())
      return "an external routine must be 'cdecl' to be a procedural value";
    if (R->isVarArgs())
      return "a 'varargs' routine cannot be a procedural value";
    for (VarDecl *P : R->getParams())
      if (llvm::isa_and_nonnull<RoutineType>(P->getType()))
        return "an external routine with procedural parameters cannot be a "
               "procedural value";
    return "";
  }

  /// Checks the arguments of a call to a user routine.
  void checkCallArgs(RoutineDecl *R, const Identifier &Callee,
                     llvm::ArrayRef<Expr *> Args, SourceRange CallRange) {
    checkCallArgs(R->getParams(), R, Callee, Args, CallRange);
    if (R->isAsync()) // (a task's arguments are copies, which it keeps)
      for (Expr *A : Args)
        checkIsolatedCopy(A, "passed to a task");
  }

  void checkCallArgs(llvm::ArrayRef<VarDecl *> Params, Decl *CalleeDecl,
                     const Identifier &Callee, llvm::ArrayRef<Expr *> Args,
                     SourceRange CallRange) {
    Decl *R = CalleeDecl;
    if (ArgsFailed) { // named arguments that do not fit: reported
      ArgsFailed = false;
      for (Expr *A : Args)
        if (!llvm::isa<FormattedExpr>(argValue(A)))
          check(argValue(A));
      return;
    }
    // A call whose result is discarded still holds it for a while.
    if (auto *RD = llvm::dyn_cast_or_null<RoutineDecl>(CalleeDecl);
        RD && RD->isFunction() && RD->getResultType() &&
        isManaged(RD->getResultType()))
      markUsesManaged();
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(CalleeDecl))
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
          RT && RT->getResult() && isManaged(RT->getResult()))
        markUsesManaged();
    auto *RD = llvm::dyn_cast_or_null<RoutineDecl>(CalleeDecl);
    bool VarArgs = RD && RD->isVarArgs();
    if (!acceptsArgCount(Params, Args.size()) &&
        !(VarArgs && Args.size() > Params.size())) {
      Diags.error(Callee.Loc,
                  llvm::Twine(Args.size() < Params.size() ? "too few"
                                                          : "too many") +
                      " arguments in call to '" + Callee.Spelling +
                      "': expected " + llvm::Twine(Params.size()) + ", got " +
                      llvm::Twine(Args.size()),
                  {CallRange});
      if (R)
        noteDeclaredHere(R);
      for (Expr *A : Args)
        if (!llvm::isa<FormattedExpr>(A))
          check(A);
      return;
    }

    for (size_t I = 0; I < Args.size(); ++I) {
      Expr *Arg = Args[I];
      if (I >= Params.size()) { // varargs
        checkVarArg(Arg, I, Callee);
        continue;
      }
      VarDecl *P = Params[I];
      if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
        Diags.error(F->getWidth()->getBeginLoc(),
                    "field widths are only allowed in 'write' and 'writeln'",
                    {F->getWidth()->getRange()});
        continue;
      }
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(P->getType())) {
        checkRoutineArg(Arg, P, RT);
        continue;
      }
      if (auto *OA = llvm::dyn_cast_or_null<OpenArrayType>(P->getType())) {
        if (RD && RD->isCDecl())
          Diags.error(Arg->getBeginLoc(),
                      "an open array cannot be passed to a 'cdecl' routine",
                      {Arg->getRange()});
        else
          checkOpenArrayArg(Arg, P, OA);
        continue;
      }
      if (P->getType() && P->getType()->isArrayOfConst()) {
        if (RD && RD->isCDecl()) {
          Diags.error(Arg->getBeginLoc(),
                      "an array of const cannot be passed to a 'cdecl' routine",
                      {Arg->getRange()});
          continue;
        }
        if (auto *SC = llvm::dyn_cast<SetCtorExpr>(Arg)) {
          checkConstArray(SC, P->getType());
          continue;
        }
        Type *AT = check(Arg);
        if (!AT->isError() && !AT->isArrayOfConst())
          Diags.error(Arg->getBeginLoc(),
                      "cannot pass a value of type " + quoted(AT) +
                          " to parameter '" + P->getName().Spelling +
                          "' of type 'array of const'",
                      {Arg->getRange()});
        continue;
      }
      if (P->getType() && P->getType()->isUntyped()) {
        // Turbo/FPC: any variable, passed by reference
        if (checkAllowingUntyped(Arg)->isError())
          continue;
        if (!isVariableReference(Arg))
          Diags.error(Arg->getBeginLoc(),
                      "argument for untyped parameter '" +
                          P->getName().Spelling + "' must be a variable",
                      {Arg->getRange()});
        else if (P->isVarParam())
          checkModifiable(Arg);
        continue;
      }
      Type *PT = P->getType();
      Type *AT = PT && (llvm::isa<AddrOfExpr>(Arg) ||
                        llvm::isa<TupleExpr>(Arg) ||
                        (PT->isDynArray() && llvm::isa<SetCtorExpr>(Arg)))
                     ? checkValueFor(PT, Arg)
                     : check(Arg);
      if (AT->isError() || !PT || PT->isError())
        continue;
      const Identifier &PName = P->getName();

      if (P->isVarParam()) {
        if (isBitField(Arg)) {
          diagBitFieldAddress(Arg);
          continue;
        }
        if (!isVariableReference(Arg)) {
          Diags.error(Arg->getBeginLoc(),
                      "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + PName.Spelling +
                          "' must be a variable",
                      {Arg->getRange()});
          continue;
        }
        // ISO 7185 6.6.3.3: actual and formal var parameters have the
        // *same* type.
        if (!checkModifiable(Arg))
          continue;
        if (AT != PT)
          Diags.error(Arg->getBeginLoc(),
                      "argument of type " + quoted(AT) + " for '" +
                          llvm::Twine(paramKeyword(P)) + "' parameter '" +
                          PName.Spelling + "' must have exactly "
                      "type " + quoted(PT),
                      {Arg->getRange()});
        else
          checkNotForControlVar(Arg);
        continue;
      }
      checkWideToChar(PT, Arg);
      if (!isAssignable(PT, AT) && !isPCharLiteral(PT, Arg) &&
          !convertImplicitly(Arg, PT))
        Diags.error(Arg->getBeginLoc(),
                    "cannot pass a value of type " + quoted(AT) +
                        " to parameter '" + PName.Spelling + "' of type " +
                        quoted(PT),
                    {Arg->getRange()});
      else if (P->isConstRefParam() && AT == PT && !Arg->getConversion() &&
               isConstRefAddressable(Arg))
        Arg->setConstRefAddress(); // its address (else a temporary's)
    }
  }

  /// Can E's address be passed for a `constref` parameter: a variable (not
  /// a bit field or a property)?
  static bool isConstRefAddressable(Expr *E) {
    if (auto *F = llvm::dyn_cast<FieldExpr>(E);
        F && !llvm::isa_and_nonnull<FieldDecl>(F->getMember()) &&
        !llvm::isa_and_nonnull<VarDecl>(F->getMember()))
      return false;
    return isVariableReference(E);
  }

  //===--------------------------------------------------------------------===//
  // Built-in procedure calls
  //===--------------------------------------------------------------------===//

  static bool isWritable(Type *T) {
    return T->isIntegerLike() || T->isReal() || T->isCharLike() ||
           T->getBase()->isBoolean() || T->isStringType() || T->isShortString() ||
           T->isAnsiString();
  }

  /// If the first argument of read/write/... is a file, checks and returns
  /// it; otherwise returns null.
  FileType *checkFileArg(ProcCallStmt *S) {
    if (S->getArgs().empty() || llvm::isa<FormattedExpr>(S->getArgs()[0]))
      return nullptr;
    Expr *First = S->getArgs()[0];
    Type *T = check(First);
    auto *F = llvm::dyn_cast<FileType>(T);
    if (F && !isVariableReference(First)) {
      Diags.error(First->getBeginLoc(), "file argument must be a variable",
                  {First->getRange()});
      return nullptr;
    }
    return F;
  }

  /// write/read of an untyped file: an error (they use blockwrite and
  /// blockread). True if reported.
  bool untypedFileIO(ProcCallStmt *S, FileType *File, bool Write) {
    if (!File || !File->isUntyped())
      return false;
    Diags.error(S->getCallee().Loc,
                "'" + S->getCallee().Spelling + "' cannot be used on an untyped " +
                    "file; use '" + (Write ? "blockwrite" : "blockread") + "'",
                {S->getCallee().getRange()});
    return true;
  }

  void checkWrite(ProcCallStmt *S, bool IsWriteln) {
    const Identifier &Callee = S->getCallee();
    FileType *File = checkFileArg(S);
    if (untypedFileIO(S, File, /*Write=*/true))
      return;
    auto Args = S->getArgs().drop_front(File ? 1 : 0);
    if (!IsWriteln && Args.empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});
    if (File && !File->isText()) {
      if (IsWriteln)
        Diags.error(Callee.Loc, "'writeln' requires a text file",
                    {Callee.getRange()});
      for (Expr *Arg : Args) {
        if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
          Diags.error(F->getWidth()->getBeginLoc(),
                      "field widths are only allowed for text files",
                      {F->getWidth()->getRange()});
          continue;
        }
        Type *T = check(Arg);
        if (!T->isError() && !isAssignable(File->getElementType(), T))
          Diags.error(Arg->getBeginLoc(),
                      "cannot write a value of type " + quoted(T) +
                          " to a " + quoted(File),
                      {Arg->getRange()});
      }
      return;
    }

    for (Expr *Arg : Args)
      checkWritten(Arg, /*Template=*/false);
  }

  /// A value written to a text file, or put in a template string, with its
  /// field width and precision if it has them (a FormattedExpr).
  void checkWritten(Expr *Arg, bool Template) {
    Expr *Value = Arg, *Width = nullptr, *Precision = nullptr;
    auto *F = llvm::dyn_cast<FormattedExpr>(Arg);
    if (F) {
      Value = F->getValue();
      Width = F->getWidth();
      Precision = F->getPrecision();
    }

    Type *T = check(Value);
    if (F)
      F->setType(T);
    // Turbo/FPC: an enumeration's value is written as its name, a pchar
    // as the C string it points to.
    if (!T->isError() && !isWritable(T) &&
        (isISO() || !(T->getBase()->isEnum() || isPChar(T)))) {
      if (Template)
        Diags.error(Value->getBeginLoc(),
                    "a value of type " + quoted(T) +
                        " cannot be put in a template string",
                    {Value->getRange()});
      else
        Diags.error(Value->getBeginLoc(),
                    "cannot write a value of type " + quoted(T),
                    {Value->getRange()});
    }

    for (Expr *W : {Width, Precision}) {
      if (!W)
        continue;
      Type *WT = check(W);
      if (!WT->isError() && !WT->isIntegerLike())
        Diags.error(W->getBeginLoc(),
                    llvm::Twine(W == Width ? "field width" : "precision") +
                        " must be an integer, not " + quoted(WT),
                    {W->getRange()});
    }
    // ISO 6.9.3.4: only real values take a fraction-digits argument.
    if (Precision && !T->isError() && !T->isReal())
      Diags.error(Precision->getBeginLoc(),
                  "a precision (':n') is only allowed for real values",
                  {Precision->getRange()});
  }

  void checkRead(ProcCallStmt *S, bool IsReadln) {
    const Identifier &Callee = S->getCallee();
    FileType *File = checkFileArg(S);
    if (untypedFileIO(S, File, /*Write=*/false))
      return;
    auto Args = S->getArgs().drop_front(File ? 1 : 0);
    if (!IsReadln && Args.empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});
    if (File && !File->isText()) {
      if (IsReadln)
        Diags.error(Callee.Loc, "'readln' requires a text file",
                    {Callee.getRange()});
      for (Expr *Arg : Args) {
        Type *T = check(Arg);
        if (T->isError())
          continue;
        if (!isVariableReference(Arg))
          Diags.error(Arg->getBeginLoc(),
                      "argument to '" + Callee.Spelling + "' must be a variable",
                      {Arg->getRange()});
        else if (!isAssignable(T, File->getElementType()))
          Diags.error(Arg->getBeginLoc(),
                      "cannot read a component of " + quoted(File) +
                          " into a variable of type " + quoted(T),
                      {Arg->getRange()});
        else
          checkNotForControlVar(Arg);
      }
      return;
    }

    for (Expr *Arg : Args) {
      if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
        Diags.error(F->getWidth()->getBeginLoc(),
                    "field widths are only allowed in 'write' and 'writeln'",
                    {F->getWidth()->getRange()});
        continue;
      }
      Type *T = check(Arg);
      if (T->isError())
        continue;
      if (!isVariableReference(Arg)) {
        Diags.error(Arg->getBeginLoc(),
                    "argument to '" + Callee.Spelling + "' must be a variable",
                    {Arg->getRange()});
        continue;
      }
      if (!T->isIntegerLike() && !T->getBase()->isChar() && !T->isReal() &&
          !T->isShortString() && !T->isAnsiString())
        Diags.error(Arg->getBeginLoc(),
                    "cannot read a value of type " + quoted(T) +
                        " (only integer, real and char)",
                    {Arg->getRange()});
      else if (checkModifiable(Arg))
        checkNotForControlVar(Arg);
    }
  }

  /// reset/rewrite/get/put(f) and page[(f)].
  /// reset, rewrite (an untyped file's: with an optional record size),
  /// get, put, page, close, closefile, flush.
  void checkFileProc(ProcCallStmt *S, BuiltinKind K) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    bool IsPage = K == BuiltinKind::Page;
    if (IsPage && Args.empty())
      return; // page(output)
    bool Sized = K == BuiltinKind::Reset || K == BuiltinKind::Rewrite;
    if (Args.size() != 1 && !(Sized && Args.size() == 2)) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 file argument",
                  {S->getRange()});
      return;
    }
    Type *T = check(Args[0]);
    if (T->isError())
      return;
    auto *F = llvm::dyn_cast<FileType>(T);
    if (!F || !isVariableReference(Args[0]))
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects a file variable, not a " +
                      "value of type " + quoted(T),
                  {Args[0]->getRange()});
    else if ((IsPage || K == BuiltinKind::Flush) && !T->isTextFile())
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' requires a text file",
                  {Args[0]->getRange()});
    else if ((K == BuiltinKind::Get || K == BuiltinKind::Put) && F->isUntyped())
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' cannot be used on an untyped file",
                  {Args[0]->getRange()});
    else if (Args.size() == 2 && !F->isUntyped())
      Diags.error(Args[1]->getBeginLoc(),
                  "only an untyped file has a record size",
                  {Args[1]->getRange()});
    else if (Args.size() == 2)
      checkIntegerArg(Args[1], Callee, "record size");
  }

  /// An integer argument of built-in Callee (What: its role).
  void checkIntegerArg(Expr *E, const Identifier &Callee, llvm::StringRef What) {
    Type *T = check(E);
    if (!T->isError() && !T->isIntegerLike())
      Diags.error(E->getBeginLoc(),
                  "the " + What + " of '" + Callee.Spelling +
                      "' must be an integer, not " + quoted(T),
                  {E->getRange()});
  }

  /// A file variable argument of Callee: the file type, or null after a
  /// diagnostic. NotText: text files are not allowed.
  FileType *checkFileVarArg(Expr *E, const Identifier &Callee,
                            bool NotText = false, bool Untyped = false) {
    Type *T = check(E);
    if (T->isError())
      return nullptr;
    auto *F = llvm::dyn_cast<FileType>(T);
    if (!F || !isVariableReference(E)) {
      Diags.error(E->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects " +
                      (Untyped ? "an untyped " : "a ") + "file variable, not a " +
                      "value of type " + quoted(T),
                  {E->getRange()});
      return nullptr;
    }
    if ((Untyped && !F->isUntyped()) || (NotText && F->isText())) {
      Diags.error(E->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects " +
                      (Untyped ? "an untyped file" : "a typed or untyped file") +
                      ", not " + quoted(T),
                  {E->getRange()});
      return nullptr;
    }
    return F;
  }

  /// assign(f, name) / assignfile: f any file variable, name a string, a
  /// char or a pchar.
  void checkAssignFile(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.size() != 2) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects a file variable and a name",
                  {S->getRange()});
      return;
    }
    checkFileVarArg(Args[0], Callee);
    Type *T = check(Args[1]);
    if (!T->isError() && !T->isStringish() && T != PCharTy)
      Diags.error(Args[1]->getBeginLoc(),
                  "the file name of '" + Callee.Spelling +
                      "' must be a string, not " + quoted(T),
                  {Args[1]->getRange()});
  }

  /// blockread(f, buf, count [, done]) / blockwrite: f an untyped file, buf
  /// any variable, count an integer, done an integer variable.
  void checkBlockIO(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.size() != 3 && Args.size() != 4) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects 3 or 4 arguments (file, "
                  "buffer, count [, count done])",
                  {S->getRange()});
      return;
    }
    checkFileVarArg(Args[0], Callee, /*NotText=*/true, /*Untyped=*/true);
    if (!checkAllowingUntyped(Args[1])->isError() &&
        !isVariableReference(Args[1]))
      Diags.error(Args[1]->getBeginLoc(),
                  "the buffer of '" + Callee.Spelling + "' must be a variable",
                  {Args[1]->getRange()});
    else if (isBitField(Args[1]))
      diagBitFieldAddress(Args[1]);
    else if (S->getCallee().Name == "blockread")
      checkModifiable(Args[1]);
    checkIntegerArg(Args[2], Callee, "count");
    if (Args.size() == 4) {
      Type *T = check(Args[3]);
      if (T->isError())
        return;
      if (!isVariableReference(Args[3]))
        Diags.error(Args[3]->getBeginLoc(),
                    "the count done by '" + Callee.Spelling +
                        "' goes in a variable",
                    {Args[3]->getRange()});
      else if (!T->isIntegerLike())
        Diags.error(Args[3]->getBeginLoc(),
                    "the count done by '" + Callee.Spelling +
                        "' goes in an integer variable, not " + quoted(T),
                    {Args[3]->getRange()});
      else
        checkModifiable(Args[3]);
    }
  }

  /// seek(f, n): f a typed or untyped file, n an integer.
  void checkSeek(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.size() != 2) {
      Diags.error(Callee.Loc,
                  "'seek' expects a file variable and a record number",
                  {S->getRange()});
      return;
    }
    checkFileVarArg(Args[0], Callee, /*NotText=*/true);
    checkIntegerArg(Args[1], Callee, "record number");
  }

  /// pack(a, i, z): z := a[i..]   unpack(z, a, i): a[i..] := z
  void checkPackUnpack(ProcCallStmt *S, bool IsPack) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.size() != 3) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 3 arguments",
                  {S->getRange()});
      return;
    }
    Expr *Unpacked = IsPack ? Args[0] : Args[1];
    Expr *Index = IsPack ? Args[1] : Args[2];
    Expr *Packed = IsPack ? Args[2] : Args[0];
    Type *UT = check(Unpacked), *IT = check(Index), *PT = check(Packed);
    if (UT->isError() || IT->isError() || PT->isError())
      return;
    auto *UA = llvm::dyn_cast<ArrayType>(UT);
    auto *PA = llvm::dyn_cast<ArrayType>(PT);
    if (!UA || UA->isPacked() || !isVariableReference(Unpacked)) {
      Diags.error(Unpacked->getBeginLoc(), "expected an unpacked array variable",
                  {Unpacked->getRange()});
      return;
    }
    if (!PA || !PA->isPacked() || !isVariableReference(Packed)) {
      Diags.error(Packed->getBeginLoc(), "expected a packed array variable",
                  {Packed->getRange()});
      return;
    }
    if (UA->getElementType() != PA->getElementType())
      Diags.error(Packed->getBeginLoc(),
                  "packed and unpacked arrays must have the same component type",
                  {Packed->getRange()});
    if (!areCompatible(IT, UA->getIndexType()))
      Diags.error(Index->getBeginLoc(),
                  "index of type " + quoted(IT) + " does not match " +
                      quoted(UA->getIndexType()),
                  {Index->getRange()});
  }

  /// new(p [, tag constants]) / dispose(p [, tag constants]). The tags
  /// select record variants (ISO 6.6.5.3); storage is always allocated for
  /// the largest variant, so they are only checked.
  /// delete(var s, index, count) / insert(source, var s, index)
  void checkDeleteInsert(ProcCallStmt *S, bool IsInsert) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.size() != 3) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 3 arguments",
                  {S->getRange()});
      return;
    }
    Expr *Str = IsInsert ? Args[1] : Args[0];
    for (Expr *A : Args) {
      Type *T = check(A);
      if (T->isError())
        continue;
      if (A == Str) {
        if (!(T->isShortString() || T->isAnsiString()) || !isVariableReference(A))
          Diags.error(A->getBeginLoc(), "expected a string variable",
                      {A->getRange()});
        else
          checkModifiable(A);
      } else if (IsInsert && A == Args[0]) {
        if (!T->isStringish())
          Diags.error(A->getBeginLoc(), "expected a string", {A->getRange()});
      } else if (!T->isIntegerLike()) {
        Diags.error(A->getBeginLoc(), "expected an integer", {A->getRange()});
      }
    }
  }

  /// setlength(a, n, ...): a a dynamic array variable, one length for each
  /// level of arrays of arrays that is set.
  void checkDynSetLength(ProcCallStmt *S) {
    auto Args = S->getArgs();
    Type *T = Args[0]->getType();
    if (!isVariableReference(Args[0]))
      Diags.error(Args[0]->getBeginLoc(), "expected a dynamic array variable",
                  {Args[0]->getRange()});
    else
      checkModifiable(Args[0]);
    if (Args.size() < 2) {
      Diags.error(S->getCallee().Loc,
                  "'" + S->getCallee().Spelling + "' expects a length",
                  {S->getRange()});
      return;
    }
    for (size_t I = 1; I < Args.size(); ++I) {
      Type *N = check(Args[I]);
      if (!N->isError() && !N->isIntegerLike())
        Diags.error(Args[I]->getBeginLoc(), "expected an integer",
                    {Args[I]->getRange()});
      if (!T->isDynArray()) {
        Diags.error(Args[I]->getBeginLoc(),
                    "more lengths than levels of dynamic arrays",
                    {Args[I]->getRange()});
        return;
      }
      T = llvm::cast<DynArrayType>(T)->getElementType();
    }
  }

  /// setlength(s, n): s a string variable, n an integer.
  void checkSetLength(ProcCallStmt *S) {
    auto Args = S->getArgs();
    if (!Args.empty() && check(Args[0])->isDynArray())
      return checkDynSetLength(S);
    if (Args.size() != 2) {
      Diags.error(S->getCallee().Loc,
                  "'" + S->getCallee().Spelling + "' expects 2 arguments",
                  {S->getRange()});
      return;
    }
    Type *T = check(Args[0]);
    Type *N = check(Args[1]);
    if (!T->isError()) {
      if (!(T->isShortString() || T->isAnsiString()) ||
          !isVariableReference(Args[0]))
        Diags.error(Args[0]->getBeginLoc(), "expected a string variable",
                    {Args[0]->getRange()});
      else
        checkModifiable(Args[0]);
    }
    if (!N->isError() && !N->isIntegerLike())
      Diags.error(Args[1]->getBeginLoc(), "expected an integer",
                  {Args[1]->getRange()});
  }

  /// inc(x [, n]) / dec(x [, n]): x an ordinal variable (Turbo/FPC: or a
  /// pointer, moved by n of what it points to), n an integer.
  void checkIncDec(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.empty() || Args.size() > 2) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 or 2 arguments",
                  {S->getRange()});
      return;
    }
    Type *T = check(Args[0]);
    if (!T->isError() &&
        (!(T->isOrdinal() || T->isPointer()) || !isVariableReference(Args[0])))
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects an ordinal or pointer variable",
                  {Args[0]->getRange()});
    else if (!T->isError() && checkModifiable(Args[0]))
      checkNotForControlVar(Args[0]);
    if (Args.size() == 2) {
      Type *N = check(Args[1]);
      if (!N->isError() && !N->isIntegerLike())
        Diags.error(Args[1]->getBeginLoc(), "step must be an integer",
                    {Args[1]->getRange()});
    }
  }

  void checkNewDispose(ProcCallStmt *S, bool IsNew) {
    const Identifier &Callee = S->getCallee();
    if (S->getArgs().empty()) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects a pointer argument",
                  {S->getRange()});
      return;
    }
    Expr *Arg = S->getArgs()[0];
    Type *T = check(Arg);
    if (!T->isError()) {
      if (!T->isPointer())
        Diags.error(Arg->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects a pointer " +
                        (IsNew ? "variable" : "") + ", not a value of type " +
                        quoted(T),
                    {Arg->getRange()});
      else if (IsNew && !isVariableReference(Arg))
        Diags.error(Arg->getBeginLoc(), "argument to 'new' must be a variable",
                    {Arg->getRange()});
      else if (IsNew)
        checkModifiable(Arg);
    }
    for (Expr *Tag : S->getArgs().drop_front())
      evaluateConstant(Tag, "variant selector");
  }

  //===--------------------------------------------------------------------===//
  // Built-in function calls
  //===--------------------------------------------------------------------===//

  Type *checkStringFunction(CallExpr *E, BuiltinDecl *BD,
                            llvm::ArrayRef<Type *> ArgTypes) {
    const Identifier &Callee = E->getCallee();
    auto Args = E->getArgs();
    for (Type *T : ArgTypes)
      if (T->isError())
        return errorType();
    auto Bad = [&](Expr *A, llvm::StringRef What) {
      Diags.error(A->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects " + What, {A->getRange()});
      return errorType();
    };
    auto Arity = [&](size_t N) {
      if (Args.size() == N)
        return true;
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects " + llvm::Twine(N) +
                      " argument" + (N == 1 ? "" : "s"),
                  {E->getRange()});
      return false;
    };
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Length:
      if (!Arity(1))
        return errorType();
      return ArgTypes[0]->isStringish() || ArgTypes[0]->isArrayOfConst() ||
                     ArgTypes[0]->isOpenArray() || ArgTypes[0]->isDynArray()
                 ? Ctx.getIntegerType()
                 : Bad(Args[0], "a string");
    case BuiltinKind::UpCase:
      if (!Arity(1))
        return errorType();
      if (ArgTypes[0]->getBase()->isWideChar())
        return Ctx.getWideCharType(); // only 'a'..'z' change
      return ArgTypes[0]->getBase()->isChar() ? Ctx.getCharType()
                                              : Bad(Args[0], "a char");
    case BuiltinKind::Pos:
      if (!Arity(2))
        return errorType();
      for (size_t I = 0; I < 2; ++I)
        if (!ArgTypes[I]->isStringish())
          return Bad(Args[I], "strings");
      return Ctx.getIntegerType();
    case BuiltinKind::Copy:
      if (!ArgTypes.empty() && ArgTypes[0]->isDynArray()) {
        // copy(a), copy(a, start), copy(a, start, count)
        if (Args.size() > 3) {
          Diags.error(Callee.Loc, "'copy' expects at most 3 arguments", {E->getRange()});
          return errorType();
        }
        for (size_t I = 1; I < Args.size(); ++I)
          if (!ArgTypes[I]->isIntegerLike())
            return Bad(Args[I], "integer index and count");
        return ArgTypes[0];
      }
      if (!Arity(3))
        return errorType();
      if (!ArgTypes[0]->isStringish())
        return Bad(Args[0], "a string");
      if (!ArgTypes[1]->isIntegerLike() || !ArgTypes[2]->isIntegerLike())
        return Bad(Args[1], "integer index and count");
      return ArgTypes[0]->isAnsiString() ? Ctx.getAnsiStringType()
                                         : Ctx.getShortStringType();
    default: // Concat
      if (Args.empty()) {
        Diags.error(Callee.Loc, "'concat' expects strings", {E->getRange()});
        return errorType();
      }
      for (size_t I = 0; I < Args.size(); ++I)
        if (!ArgTypes[I]->isStringish())
          return Bad(Args[I], "strings");
      for (Type *T : ArgTypes)
        if (T->isAnsiString())
          return Ctx.getAnsiStringType();
      return Ctx.getShortStringType();
    }
  }

  /// slice(a, n), for an open array parameter only.
  bool AllowSlice = false;

  bool isSliceCall(Expr *E) {
    auto *C = llvm::dyn_cast<CallExpr>(E);
    return C && C->getCallee().Name == "slice" &&
           llvm::isa_and_nonnull<BuiltinDecl>(Cur->lookup("slice"));
  }

  Type *checkSlice(CallExpr *E, llvm::ArrayRef<Type *> ArgTypes) {
    auto Args = E->getArgs();
    const Identifier &Callee = E->getCallee();
    bool Allowed = AllowSlice;
    AllowSlice = false;
    if (Args.size() != 2) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 2 arguments",
                  {E->getRange()});
      return errorType();
    }
    if (ArgTypes[0]->isError() || ArgTypes[1]->isError())
      return errorType();
    if (!Allowed) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling +
                      "' can only be passed to an open array parameter",
                  {E->getRange()});
      return errorType();
    }
    Type *Elem = nullptr;
    if (auto *A = llvm::dyn_cast<ArrayType>(ArgTypes[0]))
      Elem = A->getElementType();
    else if (auto *O = llvm::dyn_cast<OpenArrayType>(ArgTypes[0]))
      Elem = O->getElementType();
    if (!Elem) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects an array, not " +
                      quoted(ArgTypes[0]),
                  {Args[0]->getRange()});
      return errorType();
    }
    if (!ArgTypes[1]->isIntegerLike()) {
      Diags.error(Args[1]->getBeginLoc(), "expected an integer",
                  {Args[1]->getRange()});
      return errorType();
    }
    return Ctx.getOpenArrayType(Elem);
  }

  Type *checkBuiltinFunction(CallExpr *E, BuiltinDecl *BD,
                             llvm::ArrayRef<Type *> ArgTypes) {
    if (BD->getBuiltinKind() == BuiltinKind::Slice)
      return checkSlice(E, ArgTypes);
    const Identifier &Callee = E->getCallee();
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Length: case BuiltinKind::Copy: case BuiltinKind::Pos:
    case BuiltinKind::Concat: case BuiltinKind::UpCase:
      return checkStringFunction(E, BD, ArgTypes);
    default:
      break;
    }
    if (ArgTypes.size() != 1) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects 1 argument, got " +
                      llvm::Twine(ArgTypes.size()),
                  {E->getRange()});
      return errorType();
    }
    Type *T = ArgTypes[0];
    if (T->isError())
      return errorType();
    Expr *Arg = E->getArgs()[0];

    if (BD->getBuiltinKind() == BuiltinKind::FilePos ||
        BD->getBuiltinKind() == BuiltinKind::FileSize) {
      if (!checkFileVarArg(Arg, Callee, /*NotText=*/true))
        return errorType();
      return Ctx.getInt64Type(); // (in records)
    }
    if (BD->getBuiltinKind() == BuiltinKind::GetFileHandle) {
      // any file variable, text too; a THandle (longint, as in FPC on Unix)
      if (!checkFileVarArg(Arg, Callee))
        return errorType();
      return Ctx.getIntegerType();
    }
    if (BD->getBuiltinKind() == BuiltinKind::Eof ||
        BD->getBuiltinKind() == BuiltinKind::Eoln) {
      bool IsEoln = BD->getBuiltinKind() == BuiltinKind::Eoln;
      if (!T->isFile() || !isVariableReference(Arg) ||
          (IsEoln && !T->isTextFile())) {
        Diags.error(Arg->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects a " +
                        (IsEoln ? "text " : "") + "file variable, not " +
                        quoted(T),
                    {Arg->getRange()});
        return errorType();
      }
      return Ctx.getBooleanType();
    }

    auto Expect = [&](bool OK, llvm::StringRef What) {
      if (!OK)
        Diags.error(Arg->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects " + What +
                        " argument, not " + quoted(T),
                    {Arg->getRange()});
      return OK;
    };

    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Abs:
    case BuiltinKind::Sqr:
      if (!Expect(T->isNumeric(), "a numeric"))
        return errorType();
      return T->isReal() ? Ctx.getRealType() : promoteInteger(T);
    case BuiltinKind::Odd:
      return Expect(T->isIntegerLike(), "an integer") ? Ctx.getBooleanType()
                                                      : errorType();
    case BuiltinKind::Ord:
      if (!Expect(T->isOrdinal(), "an ordinal"))
        return errorType();
      return T->isIntegerLike() ? promoteInteger(T) : Ctx.getIntegerType();
    case BuiltinKind::Chr:
      return Expect(T->isIntegerLike(), "an integer") ? Ctx.getCharType()
                                                      : errorType();
    case BuiltinKind::Succ:
    case BuiltinKind::Pred:
      return Expect(T->isOrdinal(), "an ordinal") ? T->getBase() : errorType();
    case BuiltinKind::Sin: case BuiltinKind::Cos: case BuiltinKind::Exp:
    case BuiltinKind::Ln: case BuiltinKind::Sqrt: case BuiltinKind::Arctan:
      return Expect(T->isNumeric(), "a numeric") ? Ctx.getRealType()
                                                 : errorType();
    case BuiltinKind::Trunc:
    case BuiltinKind::Round:
      if (!Expect(T->isNumeric(), "a real"))
        return errorType();
      // Turbo/FPC: the result is an int64.
      return isISO() ? Ctx.getIntegerType() : Ctx.getInt64Type();
    default:
      llvm_unreachable("not a builtin function");
    }
  }

  /// T(x) (Turbo/FPC): between ordinal types (integer types wrap, nothing
  /// is range-checked) and between pointer types.
  Type *checkTypecast(CallExpr *E, TypeDecl *TD) {
    Type *T = TD->getType() ? TD->getType() : errorType();
    auto Args = E->getArgs();
    if (Args.size() != 1) {
      Diags.error(E->getCallee().Loc,
                  "a typecast to " + quoted(T) + " takes exactly one value",
                  {E->getRange()});
      for (Expr *A : Args)
        check(A);
      return errorType();
    }
    Type *AT = T == TMethodTy ? checkComparand(Args[0])
                              : checkAllowingUntyped(Args[0]);
    if (T->isError() || AT->isError())
      return errorType();
    if (T == TMethodTy && isMethodPointer(AT)) { // its code and object
      E->setMethodCast(isVariableReference(Args[0]));
      return T;
    }
    if (isMethodPointer(T) && AT == TMethodTy) // TEv(m)
      return T;
    if (AT->isUntyped()) { // Turbo/FPC: the untyped parameter seen as a T
      E->setVarCast();
      return T;
    }
    if (T->isAnsiString() && (AT->isStringish() || AT->isPointer()))
      return T; // an untyped pointer is taken to be an ansistring
    if (T->isShortString() && AT->isStringish())
      return T;
    if ((T->isOrdinal() && AT->isOrdinal()) ||
        (T->isPointer() &&
         (AT->isPointer() || AT->isNil() || AT->isClass() || AT->isAnsiString() ||
          AT->isArrayOfConst() || AT->isDynArray())) ||
        (T->isDynArray() && (AT->isDynArray() || AT->isPointer() || AT->isNil())) ||
        (T->isClass() && (AT->isClass() || AT->isPointer() || AT->isNil())) ||
        (T->isClassRef() && (AT->isClassRef() || AT->isNil())) ||
        (T->isInterface() &&
         (AT->isInterface() || AT->isNil() ||
          (AT->isClass() &&
           llvm::cast<ClassType>(AT)->findInterface(llvm::cast<InterfaceType>(T))))))
      return T;
    if (RoutineDecl *Op = findConversion(AT, T, /*Explicit=*/true)) {
      Args[0]->setConversion(Op, T); // a record's Explicit or := operator
      return T;
    }
    Diags.error(E->getBeginLoc(),
                "invalid typecast from " + quoted(AT) + " to " + quoted(T) +
                    " (only between ordinal types and between pointer types)",
                {E->getRange()});
    return errorType();
  }

  /// A type name or an expression as the argument of sizeof/low/high:
  /// returns its type (also stored on the argument).
  Type *checkTypeOrValue(Expr *Arg) {
    if (llvm::isa<FieldExpr>(Arg)) // TOuter.TInner
      if (TypeDecl *TD = typeNamedBy(Arg)) {
        Type *T = TD->getType() ? TD->getType() : errorType();
        Arg->setType(T);
        return T;
      }
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Arg))
      if (auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(N->getName().Name))) {
        N->setDecl(TD);
        Type *T = TD->getType() ? TD->getType() : errorType();
        N->setType(T);
        return T;
      }
    return check(Arg);
  }

  /// Built-in functions whose arguments are not ordinary values (type
  /// names, procedural variables) or that take no arguments. Returns
  /// nullopt for the others.
  std::optional<Type *> checkSpecialBuiltin(CallExpr *E, BuiltinDecl *BD) {
    const Identifier &Callee = E->getCallee();
    auto Args = E->getArgs();
    auto Arity = [&](size_t N) {
      if (Args.size() == N)
        return true;
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects " + llvm::Twine(N) +
                      " argument" + (N == 1 ? "" : "s"),
                  {E->getRange()});
      for (Expr *A : Args)
        check(A);
      return false;
    };
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Wait:
      return checkWait(Args, Callee, E->getRange(), /*NeedValue=*/true);
    case BuiltinKind::WhenAll:
    case BuiltinKind::WhenAny: {
      if (!Arity(1))
        return errorType();
      TaskType *TT = checkTaskArray(Args[0], Callee);
      if (!TT)
        return errorType();
      if (BD->getBuiltinKind() == BuiltinKind::WhenAny)
        return Ctx.getTaskType(Ctx.getIntegerType());
      return Ctx.getTaskType(TT->getResult() ? Ctx.getDynArrayType(TT->getResult())
                                             : nullptr);
    }
    case BuiltinKind::AddChecked:
    case BuiltinKind::SubChecked:
    case BuiltinKind::MulChecked: { // (a op b, ok) in the type of a op b
      if (!Arity(2))
        return errorType();
      Type *L = check(Args[0]), *R = check(Args[1]);
      if (L->isError() || R->isError())
        return errorType();
      if (!L->isIntegerLike() || !R->isIntegerLike()) {
        Diags.error(Callee.Loc,
                    "'" + Callee.Spelling + "' expects integer arguments, not " +
                        quoted(L) + " and " + quoted(R),
                    {E->getRange()});
        return errorType();
      }
      Type *T = integerOperandType(Args[0], L, Args[1], R);
      return getTupleType({T, Ctx.getBooleanType()}, {});
    }
    case BuiltinKind::SizeOf: {
      if (!Arity(1))
        return errorType();
      Type *T = checkTypeOrValue(Args[0]);
      if (T->isError())
        return errorType();
      if (containsFile(T) || T->isStringLiteral() || T->isNil() ||
          (llvm::isa<SetType>(T) && llvm::cast<SetType>(T)->isEmptySet())) {
        Diags.error(Args[0]->getBeginLoc(),
                    "'sizeof' cannot be applied to " + quoted(T),
                    {Args[0]->getRange()});
        return errorType();
      }
      return Ctx.getInt64Type();
    }
    case BuiltinKind::Low:
    case BuiltinKind::High: {
      if (!Arity(1))
        return errorType();
      Type *T = checkTypeOrValue(Args[0]);
      if (T->isError())
        return errorType();
      if (T->isDynArray()) { // of a variable: its length is known at run time
        auto *N = llvm::dyn_cast<NameRefExpr>(Args[0]);
        if (N && llvm::isa_and_nonnull<TypeDecl>(Cur->lookup(N->getName().Name))) {
          Diags.error(Args[0]->getBeginLoc(),
                      "'" + Callee.Spelling + "' needs a dynamic array variable, not a type",
                      {Args[0]->getRange()});
          return errorType();
        }
        return Ctx.getIntegerType();
      }
      if (T->isShortString() || T->isArrayOfConst() || T->isOpenArray())
        return Ctx.getIntegerType();
      if (auto *A = llvm::dyn_cast<ArrayType>(T))
        return A->getIndexType();
      if (T->isOrdinal())
        return T;
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling +
                      "' expects an ordinal type, an array or a string, not " +
                      quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    case BuiltinKind::Assigned: {
      if (!Arity(1))
        return errorType();
      Type *T;
      // A procedural variable is tested, not called.
      auto *N = llvm::dyn_cast<NameRefExpr>(Args[0]);
      auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(Cur->lookup(N->getName().Name))
                  : nullptr;
      if (V && llvm::isa_and_nonnull<RoutineType>(V->getType())) {
        N->setDecl(V);
        N->setType(V->getType());
        Used.insert(V);
        T = V->getType();
      } else {
        T = check(Args[0]);
      }
      if (T->isError())
        return errorType();
      if (!T->isPointer() && !T->isClass() && !T->isInterface() &&
          !T->isDynArray() && !llvm::isa<RoutineType>(T)) {
        Diags.error(Args[0]->getBeginLoc(),
                    "'assigned' expects a pointer or a procedural variable, not " +
                        quoted(T),
                    {Args[0]->getRange()});
        return errorType();
      }
      return Ctx.getBooleanType();
    }
    case BuiltinKind::ParamCount:
      if (!Arity(0))
        return errorType();
      return Ctx.getIntegerType();
    case BuiltinKind::ExceptAddr: // FPC's CodePointer
      if (!Arity(0))
        return errorType();
      return Ctx.getUntypedPointerType();
    case BuiltinKind::ParamStr: {
      if (!Arity(1))
        return errorType();
      Type *T = check(Args[0]);
      if (!T->isError() && !T->isIntegerLike()) {
        Diags.error(Args[0]->getBeginLoc(),
                    "'paramstr' expects an integer, not " + quoted(T),
                    {Args[0]->getRange()});
        return errorType();
      }
      return Ctx.getShortStringType();
    }
    default:
      return std::nullopt;
    }
  }

  /// getmem(p, n) freemem(p) reallocmem(p, n) fillchar(x, n, v)
  /// move(src, dst, n) halt[(code)]  (Turbo/FPC)
  void checkMemoryProc(ProcCallStmt *S, BuiltinKind K) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    size_t Want = K == BuiltinKind::FreeMem ? 1
                  : K == BuiltinKind::GetMem || K == BuiltinKind::ReallocMem ? 2
                                                                            : 3;
    if (K == BuiltinKind::Halt) {
      if (Args.size() > 1)
        Diags.error(Callee.Loc, "'halt' expects at most 1 argument",
                    {S->getRange()});
      else if (Args.size() == 1) {
        Type *T = check(Args[0]);
        if (!T->isError() && !T->isIntegerLike())
          Diags.error(Args[0]->getBeginLoc(),
                      "'halt' expects an integer exit code, not " + quoted(T),
                      {Args[0]->getRange()});
      }
      return;
    }
    if (Args.size() != Want) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects " + llvm::Twine(Want) +
                      " argument" + (Want == 1 ? "" : "s"),
                  {S->getRange()});
      for (Expr *A : Args)
        check(A);
      return;
    }
    auto Expect = [&](Expr *A, bool OK, llvm::StringRef What) {
      if (!OK)
        Diags.error(A->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects " + What,
                    {A->getRange()});
    };
    llvm::SmallVector<Type *, 3> T;
    for (size_t I = 0; I < Args.size(); ++I) // (variables: untyped ones too)
      T.push_back((K == BuiltinKind::FillChar && I == 0) ||
                          (K == BuiltinKind::Move && I < 2)
                      ? checkAllowingUntyped(Args[I])
                      : check(Args[I]));
    for (Type *X : T)
      if (X->isError())
        return;
    switch (K) {
    case BuiltinKind::GetMem:
    case BuiltinKind::ReallocMem:
      Expect(Args[0], T[0]->isPointer() && isVariableReference(Args[0]),
             "a pointer variable");
      Expect(Args[1], T[1]->isIntegerLike(), "an integer size");
      if (T[0]->isPointer() && isVariableReference(Args[0]))
        checkModifiable(Args[0]);
      return;
    case BuiltinKind::FreeMem:
      Expect(Args[0], T[0]->isPointer(), "a pointer");
      return;
    case BuiltinKind::FillChar:
      Expect(Args[0], isVariableReference(Args[0]) && !containsFile(T[0]),
             "a variable");
      Expect(Args[1], T[1]->isIntegerLike(), "an integer count");
      Expect(Args[2], T[2]->isOrdinal(), "an ordinal fill value");
      if (isVariableReference(Args[0]))
        checkModifiable(Args[0]);
      return;
    default: // Move
      Expect(Args[0], isVariableReference(Args[0]) && !containsFile(T[0]),
             "a source variable");
      Expect(Args[1], isVariableReference(Args[1]) && !containsFile(T[1]),
             "a destination variable");
      Expect(Args[2], T[2]->isIntegerLike(), "an integer count");
      if (isVariableReference(Args[1]))
        checkModifiable(Args[1]);
      return;
    }
  }

public:
  Checker(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LO,
          Sema::ReparseFn Reparse = nullptr,
          Sema::ReparseTypeFn ReparseType = nullptr)
      : Ctx(Ctx), Diags(Diags), LangOpts(LO), Reparse(std::move(Reparse)),
        ReparseType(std::move(ReparseType)) {
    declareStandardScope();
  }

  std::vector<std::string> standardNames() const {
    std::vector<std::string> Names;
    Standard.forEachName([&](llvm::StringRef N) { Names.push_back(N.str()); });
    return Names;
  }

  /// Makes the interfaces of the units in Uses visible in S.
  void importUnits(Scope &S, llvm::ArrayRef<Identifier> Uses) {
    for (const Identifier &U : Uses) {
      if (Scope *I = UnitInterfaces.lookup(U.Name))
        S.addImport(I);
      // Missing units are reported by the driver.
    }
  }

  /// Checks unit U. InterfaceOnly: U was compiled separately (S11), so
  /// only its interface is declared, for the units and program using it.
  void checkUnit(UnitDecl &U, bool InterfaceOnly = false) {
    CurModule = &U;
    auto Iface = std::make_unique<Scope>(&Standard);
    importUnits(*Iface, U.getInterfaceUses());
    Cur = Iface.get();
    declareAll(*U.getInterface());
    if (InterfaceOnly) {
      for (Decl *D : U.getInterface()->getDecls())
        Used.insert(D);
      // The definitions of its generic routines, which the units and the
      // program using it specialize; they see the unit's interface only.
      auto Impl = std::make_unique<Scope>(Iface.get());
      importUnits(*Impl, U.getImplementationUses());
      Cur = Impl.get();
      CurUnitInterface = Iface.get();
      for (Decl *D : U.getImplementation()->getDecls())
        if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isGeneric())
          declareRoutine(R);
      CurUnitInterface = nullptr;
      UnitInterfaces[U.getName().Name] = Iface.get();
      UnitScopes.push_back(std::move(Iface));
      UnitScopes.push_back(std::move(Impl));
      Cur = &Standard;
      return;
    }

    auto Impl = std::make_unique<Scope>(Iface.get());
    importUnits(*Impl, U.getImplementationUses());
    Cur = Impl.get();
    CurUnitInterface = Iface.get();
    declareAll(*U.getImplementation());
    for (Decl *D : U.getInterface()->getDecls())
      if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
          R && isUndefinedHeading(R) && !R->isExternal())
        Diags.error(R->getName().Loc,
                    "'" + R->getName().Spelling +
                        "' is declared in the interface of unit '" +
                        U.getName().Spelling + "' but not implemented",
                    {R->getName().getRange()});
    checkMethodsImplemented(*U.getInterface());
    finishBlock(*U.getImplementation(), /*WarnUnused=*/true);
    checkInstances(*U.getImplementation());
    CurUnitInterface = nullptr;

    // Interface variables may be used by other units: never "unused".
    for (Decl *D : U.getInterface()->getDecls())
      Used.insert(D);
    UnitInterfaces[U.getName().Name] = Iface.get();
    UnitScopes.push_back(std::move(Iface));
    UnitScopes.push_back(std::move(Impl));
    Cur = &Standard;
  }

  void checkProgram(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                    bool SeparateUnits) {
    for (UnitDecl *U : Units)
      checkUnit(*U, SeparateUnits);
    checkProgramParams(P);
    CurModule = &P;
    Scope ProgramScope(&Standard);
    importUnits(ProgramScope, P.getUses());
    Cur = &ProgramScope;
    checkBlock(*P.getBlock());
    checkInstances(*P.getBlock());
    checkProgramParamDecls(P);
    checkGotos();
    Cur = &Standard;
  }

  /// A unit compiled on its own: the units it uses were compiled separately.
  void checkSeparateUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps) {
    for (UnitDecl *D : Deps)
      checkUnit(*D, /*InterfaceOnly=*/true);
    checkUnit(U);
    checkGotos();
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *) {}

  void visitSequence(const void *Seq, llvm::ArrayRef<Stmt *> Body) {
    Containers.push_back(Seq);
    std::unique_ptr<Scope> Declared; // of the variables declared in it
    Scope *SavedScope = Cur;
    for (Stmt *Child : Body) {
      if (Diags.shouldStop())
        break;
      DirectSequence = Seq;
      SeqScope = &Declared;
      visitStmt(Child);
    }
    DirectSequence = nullptr;
    SeqScope = nullptr;
    if (Declared)
      Cur = SavedScope;
    Containers.pop_back();
  }

  /// The type of a variable declared without one, from its value's type T.
  /// Is E a quoted character ('a')? Where a type is inferred it is a
  /// string, like any quoted text.
  static bool isCharLiteral(Expr *E) {
    while (auto *P = llvm::dyn_cast<ParenExpr>(E))
      E = P->getInner();
    auto *L = llvm::dyn_cast<StringLiteralExpr>(E);
    return L && L->isCharLiteral();
  }

  /// The type that values of types A and B have in common where one type
  /// is inferred for both (the elements of [a, b], the arguments for one
  /// type parameter), or null: the same type; the wider integer type; real
  /// for a real and an integer (nothing is lost); Str for strings and
  /// characters; else the one the other can be assigned to.
  Type *commonType(Type *A, Type *B, Type *Str) {
    if (A == B)
      return A;
    bool IA = llvm::isa<IntegerType>(A->getBase());
    bool IB = llvm::isa<IntegerType>(B->getBase());
    if (IA && IB) {
      Type *PA = promoteInteger(A), *PB = promoteInteger(B);
      return PA == PB ? PA : Ctx.getInt64Type();
    }
    if ((A->isReal() && (IB || B->isReal())) || (B->isReal() && IA))
      return Ctx.getRealType();
    if (A->isStringish() && B->isStringish())
      return Str;
    if (isAssignable(A, B))
      return A;
    if (isAssignable(B, A))
      return B;
    return nullptr;
  }

  /// Can a variable (an element, a type argument) have the type T of a
  /// value, when it is given no type of its own?
  static bool isInferable(Type *T) {
    auto *ST = llvm::dyn_cast<SetType>(T);
    return !(T->isNil() || T->isUntyped() || T->isOpenArray() ||
             T->isArrayOfConst() || containsFile(T) ||
             (ST && ST->isEmptySet()));
  }

  /// var x := [a, b, c]: the dynamic array type of the value, an array
  /// written out. Null if Init is not one: [], a list with a range, or one
  /// whose elements are all enumeration values, characters or booleans (a
  /// set, as ever). The error type after a diagnostic.
  Type *inferArrayLiteral(Expr *Init, bool LongStr, const Identifier &Name) {
    auto *SC = llvm::dyn_cast<SetCtorExpr>(Init);
    if (!SC || SC->getElements().empty())
      return nullptr;
    for (const SetElement &El : SC->getElements())
      if (El.Hi)
        return nullptr;
    Type *Str = LongStr ? static_cast<Type *>(Ctx.getAnsiStringType())
                        : Ctx.getShortStringType(255);
    Type *Elem = nullptr;
    bool OK = true, SetLike = true;
    for (const SetElement &El : SC->getElements()) {
      Type *T = check(El.Lo);
      if (T->isError()) {
        OK = false;
        continue;
      }
      SetLike &= T->isOrdinal() && !llvm::isa<IntegerType>(T->getBase());
      if (T->isStringLiteral())
        T = Str;
      if (!isInferable(T)) {
        Diags.error(El.Lo->getBeginLoc(),
                    "cannot tell the element type of the array '" +
                        Name.Spelling + "' from " +
                        (T->isNil() ? std::string("nil")
                                    : "a value of type " + quoted(T)) +
                        "; give the variable a type",
                    {El.Lo->getRange()});
        OK = false;
        continue;
      }
      Type *Joined = Elem ? commonType(Elem, T, Str) : T;
      if (!Joined) {
        Diags.error(El.Lo->getBeginLoc(),
                    "the elements of the array '" + Name.Spelling +
                        "' have no type in common: " + quoted(Elem) + " and " +
                        quoted(T) + "; give the variable a type",
                    {El.Lo->getRange()});
        OK = false;
        continue;
      }
      Elem = Joined;
    }
    if (!OK)
      return errorType();
    if (SetLike)
      return nullptr;
    return Ctx.getDynArrayType(Elem);
  }

  Type *inferVarType(VarDeclStmt *S, VarDecl *V, Type *T) {
    if (T->isError())
      return T;
    if (T->isStringLiteral() || isCharLiteral(S->getInit()))
      return S->hasLongStrings() ? Ctx.getAnsiStringType()
                                 : Ctx.getShortStringType(255);
    auto *ST = llvm::dyn_cast<SetType>(T);
    if (T->isNil() || T->isUntyped() || T->isOpenArray() ||
        T->isArrayOfConst() || T->isFile() || (ST && ST->isEmptySet())) {
      Diags.error(S->getInit()->getBeginLoc(),
                  "cannot tell the type of '" + V->getName().Spelling +
                      "' from " +
                      (T->isNil() ? std::string("nil")
                       : ST       ? std::string("an empty set")
                                  : "a value of type " + quoted(T)) +
                      "; give the variable a type",
                  {S->getInit()->getRange()});
      return errorType();
    }
    return T;
  }

  /// var x: T;  var x: T := e;  var x := e;  -- among the statements of a
  /// sequence, to whose end the variables are visible.
  void visitVarDeclStmt(VarDeclStmt *S) {
    const void *Seq = DirectSequence;
    std::unique_ptr<Scope> *Declared = SeqScope;
    DirectSequence = nullptr;
    Expr *Init = S->getInit();
    VarDecl *First = S->getVars()[0];
    if (!Seq || !Declared) {
      Diags.error(S->getBeginLoc(),
                  "a variable can only be declared in a statement sequence "
                  "(between 'begin' and 'end')",
                  {S->getRange()});
      if (Init)
        check(Init);
      return;
    }
    // The value is checked first: it cannot use the variable.
    Type *T;
    Type *ValueTy = nullptr;
    if (First->getTypeSpec()) {
      T = resolveType(First->getTypeSpec());
      if (Init)
        ValueTy = checkValueFor(T, Init);
    } else if (Type *AT = inferArrayLiteral(Init, S->hasLongStrings(),
                                            First->getName())) {
      T = AT; // var a := [1, 2, 3]: a dynamic array
      ValueTy = AT->isError() ? AT : checkValueFor(AT, Init);
    } else {
      ValueTy = check(Init);
      T = inferVarType(S, First, ValueTy);
    }
    if (!*Declared) {
      *Declared = std::make_unique<Scope>(Cur);
      Cur = Declared->get();
    }
    for (VarDecl *V : S->getVars()) {
      V->setType(T);
      V->setOwner(CurRoutine);
      checkOwnedDecl(V);
      declare(V);
      if (InlineVars)
        InlineVars->push_back(V);
      if (V->isOwned())
        ownedDeclare(V);
    }
    if (!Init)
      return;
    Identifier Id = First->getName();
    auto *Target = Ctx.create<NameRefExpr>(Id);
    Target->setDecl(First);
    Target->setType(T);
    Used.insert(First); // (as an assignment does)
    auto *Assign = Ctx.create<AssignStmt>(
        SourceRange(Id.Loc, Init->getRange().getEnd()), Target,
        S->getAssignLoc(), Init);
    S->setInitAssign(Assign);
    finishAssign(Assign, T, ValueTy);
    if (S->isConst()) // from here on
      First->setReadOnly();
  }

  /// (a, b) := e;  var (a, b) := e;  -- e, a tuple, is kept in a hidden
  /// variable (a tuple written out is built in one), then each target is
  /// assigned its element, with the checks of an assignment.
  void visitDestructureStmt(DestructureStmt *S) {
    const void *Seq = DirectSequence;
    std::unique_ptr<Scope> *Declared = SeqScope;
    DirectSequence = nullptr;
    Expr *Value = S->getValue();
    bool IsVar = !S->getVars().empty();
    size_t N = IsVar ? S->getVars().size() : S->getTargets().size();
    auto Fail = [&] {
      if (IsVar)
        return;
      for (Expr *T : S->getTargets())
        check(T);
    };
    if (IsVar && (!Seq || !Declared)) {
      Diags.error(S->getBeginLoc(),
                  "a variable can only be declared in a statement sequence "
                  "(between 'begin' and 'end')",
                  {S->getRange()});
      check(Value);
      return;
    }
    // (a, b) := ('x', 1): the targets have types, so 'x' stays a char,
    // which a char and a string can take.
    KeepCharLiterals = !IsVar;
    Type *VT = check(Value);
    KeepCharLiterals = false;
    auto *RT = llvm::dyn_cast<RecordType>(VT);
    if (!VT->isError() &&
        (!RT || !RT->isTuple() || RT->getAllFields().size() != N)) {
      Diags.error(Value->getBeginLoc(),
                  std::to_string(N) + " variables cannot take the elements of " +
                      (RT && RT->isTuple() ? "the tuple " : "a value of type ") +
                      quoted(VT),
                  {Value->getRange()});
      VT = errorType();
    }
    if (IsVar) { // declared also after an error, with the error type
      if (!*Declared) {
        *Declared = std::make_unique<Scope>(Cur);
        Cur = Declared->get();
      }
      llvm::SmallVector<Expr *, 4> Targets;
      for (size_t I = 0; I < N; ++I) {
        VarDecl *V = S->getVars()[I];
        Type *T = VT->isError() ? VT : RT->getAllFields()[I]->getType();
        V->setType(T);
        V->setOwner(CurRoutine);
        declare(V);
        if (InlineVars)
          InlineVars->push_back(V);
        auto *Ref = Ctx.create<NameRefExpr>(V->getName());
        Targets.push_back(Ref);
      }
      S->setTargets(Ctx.copyArray<Expr *>(Targets));
    }
    if (VT->isError())
      return Fail();
    // The variable the elements are read from.
    VarDecl *Temp;
    if (auto *TE = llvm::dyn_cast<TupleExpr>(Value)) {
      Temp = TE->getTemp();
    } else {
      Temp = makeHiddenVar(VT, Value->getBeginLoc());
      if (!Temp) // (not among the statements of a block: unreachable)
        return Fail();
      auto *Ref = Ctx.create<NameRefExpr>(Temp->getName());
      Ref->setDecl(Temp);
      Ref->setType(VT);
      S->setInit(Ctx.create<AssignStmt>(Value->getRange(), Ref,
                                        S->getAssignLoc(), Value));
    }
    llvm::SmallVector<AssignStmt *, 4> Assigns;
    for (size_t I = 0; I < N; ++I) {
      FieldDecl *F = RT->getAllFields()[I];
      Expr *Target = S->getTargets()[I];
      Identifier TempId = Temp->getName();
      TempId.Loc = Value->getBeginLoc();
      auto *Ref = Ctx.create<NameRefExpr>(TempId);
      Ref->setDecl(Temp);
      Ref->setType(VT);
      Identifier FieldId = F->getName();
      FieldId.Loc = Value->getBeginLoc();
      auto *Elem = Ctx.create<FieldExpr>(Value->getRange(), Ref, FieldId);
      Elem->setMember(F);
      Elem->setType(F->getType());
      auto *A = Ctx.create<AssignStmt>(
          SourceRange(Target->getBeginLoc(), Value->getRange().getEnd()),
          Target, S->getAssignLoc(), Elem);
      visitAssignStmt(A);
      Assigns.push_back(A);
    }
    S->setAssigns(Ctx.copyArray<AssignStmt *>(Assigns));
  }

  void visitCompoundStmt(CompoundStmt *S) { visitSequence(S, S->getBody()); }

  void visitLabeledStmt(LabeledStmt *S) {
    const void *Seq = DirectSequence;
    DirectSequence = nullptr;
    const Identifier &Id = S->getLabel();
    auto *L = llvm::dyn_cast_or_null<LabelDecl>(Cur->lookup(labelKey(Id.Name)));
    if (!L || L->getOwner() != CurRoutine) {
      Diags.error(Id.Loc,
                  "label " + Id.Name + " is not declared in this block",
                  {Id.getRange()});
    } else if (L->getTarget()) {
      Diags.error(Id.Loc, "label " + Id.Name + " labels more than one statement",
                  {Id.getRange()});
      Diags.note(L->getTarget()->getBeginLoc(), "previous use is here");
    } else {
      S->setDecl(L);
      L->setTarget(S, Seq, Seq && Seq == RoutineBody);
    }
    Containers.push_back(S);
    visitStmt(S->getBody());
    Containers.pop_back();
  }

  void visitGotoStmt(GotoStmt *S) {
    DirectSequence = nullptr;
    const Identifier &Id = S->getLabel();
    auto *L = llvm::dyn_cast_or_null<LabelDecl>(Cur->lookup(labelKey(Id.Name)));
    if (!L) {
      Diags.error(Id.Loc, "use of undeclared label " + Id.Name, {Id.getRange()});
      return;
    }
    S->setDecl(L);
    PendingGotos.push_back({S, Containers, CurRoutine});
  }

  /// The procedural type of the field or property D, or null.
  static RoutineType *proceduralMember(Decl *D) {
    Type *T = nullptr;
    if (auto *F = llvm::dyn_cast<FieldDecl>(D))
      T = F->getType();
    else if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
      T = P->getType();
    return llvm::dyn_cast_or_null<RoutineType>(T);
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    WithContext *W = nullptr;
    Decl *D = S->getTypeArgs().empty() ? Cur->lookup(Callee.Name, &W)
                                       : specialize(Callee, S->getTypeArgs(), &W);
    noteLambdaUse(D, W, Callee);
    bool Generic = false; // F(args) for a generic F: its types inferred
    if (!D && S->getTypeArgs().empty())
      D = inferSpecialization(Callee, S->getArgs(), nullptr, Generic, &W);
    if (!D) {
      if (S->getTypeArgs().empty() && !Generic) {
        diagUndeclared(Callee);
        return;
      }
      for (Expr *Arg : S->getArgs())
        check(argValue(Arg));
      return;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) { // objfpc: overloads
      D = resolveOverload(R, S->getArgs(), Callee, S->getRange());
      if (!D)
        return;
    }
    S->setDecl(D);
    warnHints(D, Callee);
    if (hasMembers(W)) { // a method of an object or advanced record
      S->setWith(W);
      checkScopeAccess(D, W, Callee);
    }

    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D);
        BD && BD->getBuiltinKind() == BuiltinKind::Wait) {
      checkWait(S->getArgs(), Callee, S->getRange(), /*NeedValue=*/false);
      return;
    }
    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D); BD && BD->isProcedure()) {
      switch (BD->getBuiltinKind()) {
      case BuiltinKind::Write:   return checkWrite(S, /*IsWriteln=*/false);
      case BuiltinKind::Writeln: return checkWrite(S, /*IsWriteln=*/true);
      case BuiltinKind::Read:    return checkRead(S, /*IsReadln=*/false);
      case BuiltinKind::Readln:  return checkRead(S, /*IsReadln=*/true);
      case BuiltinKind::New:     return checkNewDispose(S, /*IsNew=*/true);
      case BuiltinKind::Dispose: return checkNewDispose(S, /*IsNew=*/false);
      case BuiltinKind::Reset: case BuiltinKind::Rewrite:
      case BuiltinKind::Get: case BuiltinKind::Put: case BuiltinKind::Page:
      case BuiltinKind::Close: case BuiltinKind::CloseFile:
      case BuiltinKind::Flush:
        return checkFileProc(S, BD->getBuiltinKind());
      case BuiltinKind::Assign: case BuiltinKind::AssignFile:
        return checkAssignFile(S);
      case BuiltinKind::BlockRead: case BuiltinKind::BlockWrite:
        return checkBlockIO(S);
      case BuiltinKind::Seek:
        return checkSeek(S);
      case BuiltinKind::Break: case BuiltinKind::Continue:
        if (!S->getArgs().empty())
          Diags.error(Callee.Loc, "'" + Callee.Spelling + "' takes no arguments",
                      {S->getRange()});
        else if (!LoopDepth)
          Diags.error(Callee.Loc,
                      "'" + Callee.Spelling + "' is only allowed inside a loop",
                      {Callee.getRange()});
        else if (FinallyLoopDepth >= 0 &&
                 LoopDepth == static_cast<unsigned>(FinallyLoopDepth))
          Diags.error(Callee.Loc,
                      "'" + Callee.Spelling +
                          "' cannot leave a 'finally' part",
                      {Callee.getRange()});
        else
          ownedBreak(BD->getBuiltinKind() == BuiltinKind::Continue);
        return;
      case BuiltinKind::Exit: {
        auto Args = S->getArgs();
        if (FinallyLoopDepth >= 0)
          Diags.error(Callee.Loc,
                      "'" + Callee.Spelling +
                          "' is not allowed in a 'finally' part",
                      {Callee.getRange()});
        if (Args.empty()) {
          ownedDead();
          return;
        }
        // exit(value) (Turbo/FPC): sets the function result, then exits.
        if (Args.size() > 1 || !CurRoutine || !CurRoutine->isFunction()) {
          Diags.error(Callee.Loc,
                      Args.size() > 1
                          ? "'exit' takes at most one argument"
                          : "'exit' with a value is only allowed in a function",
                      {S->getRange()});
          for (Expr *A : Args)
            check(A);
          ownedDead();
          return;
        }
        Type *T = llvm::isa<TupleExpr>(Args[0]) || llvm::isa<CallExpr>(Args[0])
                      ? checkValueFor(CurRoutine->getResultType(), Args[0])
                      : check(Args[0]);
        if (VarDecl *OV = ownedVarOf(Args[0]))
          diagOwnedEscape(OV, Args[0], /*Returned=*/true);
        checkLambdaEscape(Args[0], /*Local=*/false);
        ownedDead();
        Type *RT = CurRoutine->getResultType();
        ResultAssigned.insert(CurRoutine);
        if (!T->isError() && !RT->isError() && !isAssignable(RT, T) &&
            !convertImplicitly(Args[0], RT))
          Diags.error(Args[0]->getBeginLoc(),
                      "cannot return a value of type " + quoted(T) +
                          " from a function of type " + quoted(RT),
                      {Args[0]->getRange()});
        return;
      }
      case BuiltinKind::Inc: case BuiltinKind::Dec:
        return checkIncDec(S);
      case BuiltinKind::Delete: case BuiltinKind::Insert:
        return checkDeleteInsert(S, BD->getBuiltinKind() == BuiltinKind::Insert);
      case BuiltinKind::SetLength:
        return checkSetLength(S);
      case BuiltinKind::Pack: case BuiltinKind::Unpack:
        return checkPackUnpack(S, BD->getBuiltinKind() == BuiltinKind::Pack);
      case BuiltinKind::GetMem: case BuiltinKind::FreeMem:
      case BuiltinKind::ReallocMem: case BuiltinKind::FillChar:
      case BuiltinKind::Move: case BuiltinKind::Halt:
        return checkMemoryProc(S, BD->getBuiltinKind());
      default: llvm_unreachable("not a builtin procedure");
      }
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isAsync()) {
      checkCallArgs(R, Callee, orderedArgs(S, R->getParams(), Callee),
                    S->getRange());
      warnDanglingTask(Callee, S->getRange());
      return;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
        R && (R->isProcedure() || !isISO())) // TP: discard a function result
      return checkCallArgs(R, Callee, orderedArgs(S, R->getParams(), Callee),
                           S->getRange());
    if (auto *V = llvm::dyn_cast<VarDecl>(D))
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
          RT && !RT->isFunction()) {
        Used.insert(V); // (called: used)
        return checkCallArgs(RT->getParams(), V, Callee,
                             orderedArgs(S, RT->getParams(), Callee),
                             S->getRange());
      }
    if (RoutineType *RT = proceduralMember(D)) {
      // FOnEvent(x) in a method or a with statement: through the member
      auto *Ref = Ctx.create<NameRefExpr>(Callee);
      if (check(Ref)->isError())
        return;
      S->setCalleeValue(Ref);
      return checkCallArgs(RT->getParams(), nullptr, Callee,
                           orderedArgs(S, RT->getParams(), Callee),
                           S->getRange());
    }

    if (llvm::isa<BuiltinDecl>(D) || llvm::isa<RoutineDecl>(D))
      Diags.error(Callee.Loc,
                  "function '" + Callee.Spelling +
                      "' cannot be called as a statement; use its result",
                  {Callee.getRange()});
    else
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' is a " + D->getKindName() +
                      ", not a procedure",
                  {Callee.getRange()});
    noteDeclaredHere(D);
  }

  /// Resolves an assignment target that is a plain name: a variable, a field
  /// (inside with), or the result of an enclosing function.
  Type *checkNameTarget(NameRefExpr *Target) {
    const Identifier &Id = Target->getName();
    WithContext *W = nullptr;
    Decl *D = Cur->lookup(Id.Name, &W);
    // (a function's name: its result, noted below)
    auto *FR = llvm::dyn_cast_or_null<RoutineDecl>(D);
    if (!FR || !FR->isFunction())
      noteLambdaUse(D, W, Id);
    if (!D) {
      diagUndeclared(Id);
      return errorType();
    }
    checkScopeAccess(D, W, Id);

    if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isFunction()) {
      for (RoutineDecl *Enc = CurRoutine; Enc; Enc = Enc->getParent())
        for (RoutineDecl *X = R; X; X = X->getNextOverload()) // (overloads)
          if (Enc == X) {
            if (X != CurRoutine) // from a nested routine
              X->getResultVar()->setCaptured();
            noteLambdaUse(X->getResultVar(), nullptr, Id);
            Target->setDecl(X->getResultVar());
            ResultAssigned.insert(X);
            return X->getResultType();
          }
      Diags.error(Id.Loc,
                  "cannot assign to function '" + Id.Spelling +
                      "' outside its body",
                  {Id.getRange()});
      return errorType();
    }

    Target->setDecl(D);
    Target->setWith(W);
    Used.insert(D);
    warnHints(D, Id);
    if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
      checkNotForControlVar(Target);
      if (V->getVarKind() == VarDecl::Result) // objfpc `Result`
        ResultAssigned.insert(V->getOwner());
      return V->getType() ? V->getType() : errorType();
    }
    if (auto *F = llvm::dyn_cast<FieldDecl>(D))
      return F->getType() ? F->getType() : errorType();
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D)) {
      if (!checkArrayPropertyUse(P, nullptr, Id))
        return errorType();
      return checkPropertyWrite(P, Id);
    }

    Diags.error(Id.Loc,
                "cannot assign to " + llvm::Twine(D->getKindName()) + " '" +
                    Id.Spelling + "'",
                {Id.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }

  void visitIndirectCallStmt(IndirectCallStmt *S) {
    DirectSequence = nullptr;
    IndirectCallExpr *Call = S->getCall();
    if (isMethodCallee(Call->getCallee())) {
      Type *T = checkMethodCall(Call);
      if (T)
        Call->setType(T);
      if (T && T->isTask() && isAsyncCall(Call))
        if (auto *F = llvm::dyn_cast<FieldExpr>(Call->getCallee()))
          warnDanglingTask(F->getField(), Call->getRange());
      return;
    }
    if (RoutineType *RT = checkIndirectCallee(Call))
      Call->setType(RT->isFunction() ? RT->getResult() : RT);
    else
      Call->setType(errorType());
  }

  /// Type of a property assigned to; it needs a write accessor.
  Type *checkPropertyWrite(PropertyDecl *P, const Identifier &Id) {
    if (ClassType *H =
            helperOfMember(P->getWrite() ? P->getWrite() : P->getRead())) {
      Diags.error(Id.Loc,
                  "writing a property of a " + H->helperKindName() +
                      " is not supported yet",
                  {Id.getRange()});
      return errorType();
    }
    if (P->getWrite() || P->getType()->isError() ||
        (!P->getRead() && P->getWriteName().isValid()))
      return P->getType();
    Diags.error(Id.Loc, "property '" + Id.Spelling + "' is read-only",
                {Id.getRange()});
    noteDeclaredHere(P);
    return errorType();
  }

  /// Is the target of an assignment a property (written by its accessor)?
  static PropertyDecl *assignedProperty(Expr *Target) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target))
      return llvm::dyn_cast_or_null<PropertyDecl>(N->getDecl());
    if (auto *F = llvm::dyn_cast<FieldExpr>(Target))
      return llvm::dyn_cast_or_null<PropertyDecl>(F->getMember());
    if (auto *I = llvm::dyn_cast<IndexExpr>(Target))
      return I->getProperty();
    return nullptr;
  }

  /// The name an array property is used by (obj.P / P), or for a default
  /// property, its name at the indexed object.
  static Identifier propertyName(IndexExpr *E) {
    Expr *Root = E->getPropertyRoot();
    if (auto *F = llvm::dyn_cast<FieldExpr>(Root); F && F->getMember() == E->getProperty())
      return F->getField();
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Root); N && N->getDecl() == E->getProperty())
      return N->getName();
    Identifier Id = E->getProperty()->getName();
    Id.Loc = Root->getBeginLoc();
    return Id;
  }

  /// An array property used without its indexes (outside IndexBase).
  bool checkArrayPropertyUse(PropertyDecl *P, Expr *E, const Identifier &Id) {
    if (!P->isArrayProperty())
      return true;
    if (E && E == IndexBase) {
      PendingIndexes[E] = {P, E, {}};
      return true;
    }
    Diags.error(Id.Loc,
                "array property '" + Id.Spelling + "' needs " +
                    (P->getParams().size() == 1
                         ? std::string("an index")
                         : std::to_string(P->getParams().size()) + " indexes"),
                {Id.getRange()});
    noteDeclaredHere(P);
    return false;
  }

  void visitAssignStmt(AssignStmt *S) {
    Expr *Target = S->getTarget();
    // A lambda's body, $lambda := e, where no result type is known: the
    // type of e (a quoted constant: a string).
    if (CurRoutine && CurRoutine->isLambda() && CurRoutine->isFunction() &&
        !CurRoutine->getResultType())
      if (auto *N = llvm::dyn_cast<NameRefExpr>(Target);
          N && N->getName().Name == CurRoutine->getName().Name) {
        Type *T = check(S->getValue());
        if (!T->isError() && (T->isStringLiteral() || isCharLiteral(S->getValue())))
          T = Ctx.getAnsiStringType();
        if (!T->isError() && !isInferable(T)) {
          Diags.error(S->getValue()->getBeginLoc(),
                      "cannot tell the result type of this lambda from " +
                          (T->isNil() ? std::string("nil")
                                      : "a value of type " + quoted(T)) +
                          "; give it one: function(...): T => ...",
                      {S->getValue()->getRange()});
          T = errorType();
        }
        CurRoutine->setResultType(T);
        CurRoutine->getResultVar()->setType(T);
      }
    Type *TargetTy;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target)) {
      TargetTy = checkNameTarget(N);
      N->setType(TargetTy);
    } else {
      Expr *SavedTarget = CurAssignTarget;
      CurAssignTarget = Target;
      TargetTy = check(Target);
      CurAssignTarget = SavedTarget;
      // r.f := v / Result.f := v: assigns the result (a record) in part
      Expr *Root = Target;
      while (true) {
        if (auto *FE = llvm::dyn_cast<FieldExpr>(Root))
          Root = FE->getBase();
        else if (auto *IE = llvm::dyn_cast<IndexExpr>(Root))
          Root = IE->getBase();
        else
          break;
      }
      if (auto *RN = llvm::dyn_cast<NameRefExpr>(Root))
        if (auto *V = llvm::dyn_cast_or_null<VarDecl>(RN->getDecl());
            V && V->getVarKind() == VarDecl::Result)
          ResultAssigned.insert(V->getOwner());
      if (auto *P = assignedProperty(Target); P && !TargetTy->isError()) {
        auto *I = llvm::dyn_cast<IndexExpr>(Target);
        TargetTy = checkPropertyWrite(
            P, I ? propertyName(I) : llvm::cast<FieldExpr>(Target)->getField());
        Target->setType(TargetTy);
      } else if (!TargetTy->isError() && !isVariableReference(Target) &&
                 !isBitField(Target)) {
        Diags.error(Target->getBeginLoc(), "expression is not assignable",
                    {Target->getRange()});
        TargetTy = errorType();
      }
    }

    // x += e (Free Pascal's C operators): Value is x + (e), x read again.
    auto *Sum = S->isCompound() ? llvm::cast<BinaryExpr>(S->getValue())
                                : nullptr;
    if (Sum && !TargetTy->isError() && assignedProperty(Target)) {
      Diags.error(S->getAssignLoc(),
                  "'" + llvm::Twine(getOpSpelling(Sum->getOp())) +
                      "=' cannot assign to a property; use ':='",
                  {Target->getRange()});
      TargetTy = errorType();
    }
    if (Sum && TargetTy->isError()) { // reported once: x is not read again
      check(Sum->getRHS());
      return;
    }
    if (TargetTy->isUntyped()) {
      Diags.error(Target->getBeginLoc(),
                  "cannot assign to untyped parameter '" +
                      llvm::cast<NameRefExpr>(Target)->getName().Spelling +
                      "'; assign to a typecast such as longint(" +
                      llvm::cast<NameRefExpr>(Target)->getName().Spelling + ")",
                  {Target->getRange()});
      check(Sum ? Sum->getRHS() : S->getValue());
      return;
    }
    if (Sum)
      if (auto *N = llvm::dyn_cast<NameRefExpr>(Target))
        if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
            V && V->getVarKind() == VarDecl::Result &&
            llvm::isa_and_nonnull<RoutineDecl>(Cur->lookup(N->getName().Name))) {
          CompoundResultRef = llvm::dyn_cast<NameRefExpr>(Sum->getLHS());
          CompoundResultVar = V;
        }
    Type *ValueTy = checkValueFor(TargetTy, S->getValue());
    CompoundResultRef = nullptr;
    finishAssign(S, TargetTy, ValueTy);
  }

  /// The rest of the checks of Target := Value, both already checked.
  void finishAssign(AssignStmt *S, Type *TargetTy, Type *ValueTy) {
    Expr *Target = S->getTarget();
    checkIsolatedCopy(S->getValue(), "assigned");
    if (auto *TN = llvm::dyn_cast<NameRefExpr>(Target))
      if (auto *TV = llvm::dyn_cast_or_null<VarDecl>(TN->getDecl());
          TV && TV->isShared() && !TN->getWith() && spawnOutside(TV->getOwner()))
        Diags.error(TN->getName().Loc,
                    "a spawned task cannot assign shared variable '" +
                        TN->getName().Spelling +
                        "', which other tasks use; change what it holds "
                        "through its methods",
                    {TN->getRange()});
    // An owned variable's object stays its own unless moved: only a local
    // variable can refer to it too. Assigning to the variable gives it an
    // object again.
    VarDecl *OwnedV = ownedVarOf(Target);
    if (VarDecl *OV = ownedVarOf(S->getValue()); OV && !ValueTy->isError()) {
      auto *TN = llvm::dyn_cast<NameRefExpr>(Target);
      auto *TV = TN ? llvm::dyn_cast_or_null<VarDecl>(TN->getDecl()) : nullptr;
      bool Local = TV && !TN->getWith() && !TV->isOwned() && CurRoutine &&
                   TV->getOwner() == CurRoutine &&
                   (TV->getVarKind() == VarDecl::Variable ||
                    TV->getVarKind() == VarDecl::ValueParam) &&
                   !TV->isTypedConst() && !TV->isClassVar();
      if (!Local)
        diagOwnedEscape(OV, S->getValue(),
                        TV && TV->getVarKind() == VarDecl::Result);
    }
    if (OwnedV)
      ownedAssign(OwnedV);
    if (llvm::isa<AddrOfExpr>(S->getValue())) { // a lambda: where it is kept
      auto *TN = llvm::dyn_cast<NameRefExpr>(Target);
      auto *TV = TN ? llvm::dyn_cast_or_null<VarDecl>(TN->getDecl()) : nullptr;
      checkLambdaEscape(S->getValue(),
                        TV && !TN->getWith() && CurRoutine &&
                            TV->getOwner() == CurRoutine &&
                            TV->getVarKind() == VarDecl::Variable &&
                            !TV->isTypedConst() && !TV->isClassVar());
    }
    if (TargetTy->isError() || ValueTy->isError())
      return;
    Expr *SavedOwnedTarget = OwnedTarget;
    OwnedTarget = Target;
    bool Modifiable = checkModifiable(Target);
    OwnedTarget = SavedOwnedTarget;
    if (!Modifiable)
      return;
    if (containsFile(TargetTy)) {
      Diags.error(S->getAssignLoc(), "files cannot be assigned",
                  {Target->getRange()});
      return;
    }
    checkWideToChar(TargetTy, S->getValue());
    if (!isAssignable(TargetTy, ValueTy) &&
        !convertImplicitly(S->getValue(), TargetTy))
      Diags.error(S->getAssignLoc(),
                  "assigning to " + quoted(TargetTy) +
                      " from incompatible type " + quoted(ValueTy),
                  {Target->getRange(), S->getValue()->getRange()});
  }

  void visitTryStmt(TryStmt *S) {
    markHasTry();
    DirectSequence = nullptr;
    OwnedTry Protected;
    ownedTryBegin(Protected);
    visitSequence(S, S->getBody());
    ownedTryEnd(Protected);
    OwnedState BodyEnd = ownedGet();
    if (S->isFinally()) {
      // The finally part runs after any of the body's states; what it
      // leaves alone is as the body left it.
      ownedSet(Protected.Seen);
      unsigned Tick = Flow ? Flow->Tick : 0;
      int Saved = FinallyLoopDepth;
      FinallyLoopDepth = static_cast<int>(LoopDepth);
      visitSequence(S, S->getFinallyBody());
      FinallyLoopDepth = Saved;
      if (Flow) {
        OwnedState Out = Flow->S;
        if (!BodyEnd.isLive() || !Out.isLive()) {
          ownedDead();
        } else {
          for (unsigned I = 0; I < Flow->N; ++I)
            if (Flow->Changed[I] > Tick) {
              uint64_t Bit = uint64_t(1) << I;
              BodyEnd.Valid = (BodyEnd.Valid & ~Bit) | (Out.Valid & Bit);
              BodyEnd.Moved = (BodyEnd.Moved & ~Bit) | (Out.Moved & Bit);
              BodyEnd.Entry[I] = Out.Entry[I];
            }
          Flow->S = BodyEnd;
        }
      }
      return;
    }
    ++HandlerDepth;
    for (ExceptHandler *H : S->getHandlers()) {
      if (Diags.shouldStop())
        break;
      ownedSet(Protected.Seen);
      checkHandler(H);
      BodyEnd.join(ownedGet());
    }
    ownedSet(Protected.Seen);
    visitSequence(S, S->getExceptBody());
    if (S->catchesAll())
      BodyEnd.join(ownedGet());
    ownedSet(BodyEnd);
    --HandlerDepth;
  }

  /// The object of the owned variable OV would be kept (or returned)
  /// through E without `move`.
  void diagOwnedEscape(VarDecl *OV, Expr *E, bool Returned) {
    Diags.error(E->getBeginLoc(),
                "the object of owned variable '" + OV->getName().Spelling +
                    "' can only be " + (Returned ? "returned" : "stored") +
                    " with 'move " + OV->getName().Spelling + "'",
                {E->getRange()});
  }

  /// on [E:] TFoo do Body: E is a variable of type TFoo visible in Body.
  void checkHandler(ExceptHandler *H) {
    ClassType *C = lookupClass(H->getClassName());
    H->setClass(C);
    Scope HandlerScope(Cur);
    if (H->getVarName().isValid()) {
      auto *V = Ctx.create<VarDecl>(H->getVarName(), nullptr);
      V->setType(C ? static_cast<Type *>(C) : errorType());
      V->setOwner(CurRoutine);
      H->setVar(V);
      Used.insert(V);
      HandlerScope.insert(V->getName().Name, V);
    }
    Scope *Saved = Cur;
    Cur = &HandlerScope;
    DirectSequence = nullptr;
    visitStmt(H->getBody());
    Cur = Saved;
  }

  void visitRaiseStmt(RaiseStmt *S) {
    DirectSequence = nullptr;
    if (!S->getObject()) {
      if (!HandlerDepth)
        Diags.error(S->getBeginLoc(),
                    "'raise' without an object (a re-raise) is only allowed "
                    "in an exception handler",
                    {S->getRange()});
      ownedDead();
      return;
    }
    Type *T = check(S->getObject());
    if (!T->isError() && !llvm::isa<ClassType>(T))
      Diags.error(S->getObject()->getBeginLoc(),
                  "'raise' needs an object of a class type, not " + quoted(T),
                  {S->getObject()->getRange()});
    // raise ... at Address, Frame: pointers (FPC's CodePointer and Pointer)
    for (Expr *P : {S->getAddress(), S->getFrame()}) {
      if (!P)
        continue;
      Type *Ptr = Ctx.getUntypedPointerType();
      Type *PT = checkValueFor(Ptr, P);
      if (!PT->isError() && !isAssignable(Ptr, PT))
        Diags.error(P->getBeginLoc(),
                    "the " +
                        llvm::Twine(P == S->getAddress() ? "address"
                                                         : "frame") +
                        " of a raised exception must be a pointer, not " +
                        quoted(PT),
                    {P->getRange()});
    }
    ownedDead();
  }

  void visitIfStmt(IfStmt *S) {
    DirectSequence = nullptr;
    checkCondition(S->getCond(), "if");
    OwnedState Before = ownedGet();
    visitStmt(S->getThen());
    OwnedState Then = ownedGet();
    ownedSet(Before);
    if (S->getElse())
      visitStmt(S->getElse());
    ownedJoin(Then);
  }

  void visitWhileStmt(WhileStmt *S) {
    DirectSequence = nullptr;
    OwnedLoop Loop;
    ownedLoopBegin(Loop);
    checkCondition(S->getCond(), "while");
    OwnedState Before = ownedGet();
    ++LoopDepth;
    visitStmt(S->getBody());
    --LoopDepth;
    ownedLoopEnd(Loop, Before);
  }

  void visitRepeatStmt(RepeatStmt *S) {
    OwnedLoop Loop;
    ownedLoopBegin(Loop);
    ++LoopDepth;
    visitSequence(S, S->getBody());
    --LoopDepth;
    ownedJoin(Loop.Continues); // continue goes to the condition
    Loop.Continues = OwnedState();
    checkCondition(S->getCond(), "until");
    ownedLoopEnd(Loop, OwnedState());
  }

  /// `for var x [: T] ...`: declares x in Loop, the scope of the loop. T is
  /// its type if it has none written (null: not known yet).
  void declareLoopVar(VarDecl *V, Type *T, Scope &Loop) {
    if (V->getTypeSpec())
      T = resolveType(V->getTypeSpec());
    V->setType(T);
    V->setOwner(CurRoutine);
    Cur = &Loop;
    declare(V);
    if (InlineVars)
      InlineVars->push_back(V);
  }

  void visitForStmt(ForStmt *S) {
    DirectSequence = nullptr;
    NameRefExpr *VarRef = S->getVar();
    const Identifier &Id = VarRef->getName();
    Type *VarTy = errorType();
    bool Usable = false;
    Scope LoopVars(Cur);
    Scope *OuterScope = Cur;
    if (VarDecl *IV = S->getInlineVar()) // (its values cannot use it)
      declareLoopVar(IV, IV->getTypeSpec() ? nullptr : check(S->getInit()),
                     LoopVars);

    if (Decl *D = Cur->lookup(Id.Name)) {
      VarRef->setDecl(D);
      Used.insert(D);
      auto *V = llvm::dyn_cast<VarDecl>(D);
      if (!V) {
        Diags.error(Id.Loc,
                    "'for' loop control variable must be a variable, but '" +
                        Id.Spelling + "' is a " + D->getKindName(),
                    {Id.getRange()});
        noteDeclaredHere(D);
      } else if (V->getType() && !V->getType()->isError()) {
        VarTy = V->getType();
        if (!VarTy->isOrdinal()) {
          Diags.error(Id.Loc,
                      "'for' loop control variable must be of an ordinal "
                      "type, not " + quoted(VarTy),
                      {Id.getRange()});
        } else if (isISO() && (V->getOwner() != CurRoutine || V->isVarParam())) {
          // ISO 7185 6.8.3.9: declared in the closest enclosing block.
          Diags.error(Id.Loc,
                      "'for' loop control variable '" + Id.Spelling +
                          "' must be a local variable of the enclosing block",
                      {Id.getRange()});
          noteDeclaredHere(V);
        } else if (!checkNotForControlVar(VarRef)) {
          Usable = true;
        }
      }
    } else {
      diagUndeclared(Id);
    }
    VarRef->setType(VarTy);

    for (Expr *Bound : {S->getInit(), S->getFinal()}) {
      Type *T = check(Bound);
      if (Usable && !T->isError() && !areCompatible(T, VarTy))
        Diags.error(Bound->getBeginLoc(),
                    "'for' loop " +
                        llvm::Twine(Bound == S->getInit() ? "initial"
                                                          : "final") +
                        " value of type " + quoted(T) +
                        " does not match control variable of type " +
                        quoted(VarTy),
                    {Bound->getRange()});
    }

    if (Usable)
      ActiveFors.push_back({VarRef->getDecl(), S->getBeginLoc()});
    OwnedState Before = ownedGet();
    OwnedLoop Loop;
    ownedLoopBegin(Loop);
    ++LoopDepth;
    visitStmt(S->getBody());
    --LoopDepth;
    ownedLoopEnd(Loop, Before);
    if (Usable)
      ActiveFors.pop_back();
    Cur = OuterScope;
  }

  /// A hidden variable of a for-in loop (code generation binds it), named
  /// in Sc only: its name is no Pascal identifier.
  VarDecl *forInVar(llvm::StringRef Name, Type *T, SourceLocation Loc,
                    Scope &Sc) {
    llvm::StringRef N = Ctx.getStrings().intern(Name);
    auto *V = Ctx.create<VarDecl>(Identifier{N, N, Loc}, nullptr);
    V->setType(T);
    V->setOwner(CurRoutine);
    Used.insert(V);
    Sc.insert(N, V);
    return V;
  }

  NameRefExpr *forInRef(VarDecl *V, SourceLocation Loc) {
    Identifier Id = V->getName();
    Id.Loc = Loc;
    return Ctx.create<NameRefExpr>(Id);
  }

  /// Base.Name (Name as written in Free Pascal), at Loc.
  FieldExpr *forInMember(Expr *Base, llvm::StringRef Spelling,
                         SourceLocation Loc) {
    Identifier Id{Ctx.getStrings().intern(llvm::StringRef(Spelling).lower()),
                  Ctx.getStrings().intern(Spelling), Loc};
    return Ctx.create<FieldExpr>(Id.getRange(), Base, Id);
  }

  /// The member Name of a class, advanced record or interface, or null.
  static Decl *findEnumMember(Type *T, llvm::StringRef Name) {
    if (ClassType *MT = memberTableOf(T))
      T = MT;
    if (auto *C = llvm::dyn_cast<ClassType>(T))
      return C->findMember(Name);
    if (auto *I = llvm::dyn_cast<InterfaceType>(T))
      return findInterfaceMethod(I, Name);
    return nullptr;
  }

  /// for Var in Collection do Body (Turbo/FPC), as in Free Pascal: Var
  /// takes the elements of an array or string, the members of a set in
  /// order, the values of an ordinal type, or the values of an enumerator.
  /// Var := element is an ordinary assignment (Step).
  void visitForInStmt(ForInStmt *S) {
    DirectSequence = nullptr;
    NameRefExpr *VarRef = S->getVar();
    const Identifier &Id = VarRef->getName();
    Type *VarTy = errorType();
    bool Usable = false;
    Scope LoopVars(Cur);
    Scope *OuterScope = Cur;
    // for var x in ...: without a type, x has the type of the elements,
    // known when the first one is assigned to it (MakeStep).
    VarDecl *IV = S->getInlineVar();
    VarDecl *Untyped = IV && !IV->getTypeSpec() ? IV : nullptr;
    if (IV) {
      checkTypeOrValue(S->getCollection()); // (it cannot use x)
      declareLoopVar(IV, nullptr, LoopVars);
    }
    if (Untyped) {
      VarRef->setDecl(Untyped);
      Used.insert(Untyped);
      Usable = true;
    } else if (Decl *D = Cur->lookup(Id.Name)) {
      VarRef->setDecl(D);
      Used.insert(D);
      auto *V = llvm::dyn_cast<VarDecl>(D);
      if (!V) {
        Diags.error(Id.Loc,
                    "'for' loop control variable must be a variable, but '" +
                        Id.Spelling + "' is a " + D->getKindName(),
                    {Id.getRange()});
        noteDeclaredHere(D);
      } else if (V->getType() && !V->getType()->isError()) {
        VarTy = V->getType();
        Usable = !checkNotForControlVar(VarRef);
      }
    } else {
      diagUndeclared(Id);
    }
    VarRef->setType(VarTy);

    Expr *C = S->getCollection();
    SourceLocation CLoc = C->getBeginLoc();
    Type *CT = checkTypeOrValue(C);
    auto *TypeRef = llvm::dyn_cast<NameRefExpr>(C);
    auto *TypeField = llvm::dyn_cast<FieldExpr>(C);
    bool IsType =
        (TypeRef && llvm::isa_and_nonnull<TypeDecl>(TypeRef->getDecl())) ||
        (TypeField && llvm::isa_and_nonnull<TypeDecl>(TypeField->getMember()));
    ForInStmt::InKind Kind = ForInStmt::Invalid;
    Type *Elem = nullptr;
    if (CT->isError()) {
    } else if (IsType) {
      if (CT->isOrdinal()) {
        Kind = ForInStmt::Ordinals;
        Elem = CT;
      } else {
        Diags.error(CLoc,
                    "'for ... in' over a type needs an ordinal type, not " +
                        quoted(CT),
                    {C->getRange()});
      }
    } else if (auto *A = llvm::dyn_cast<ArrayType>(CT)) {
      Kind = ForInStmt::Array;
      Elem = A->getElementType();
    } else if (auto *OA = llvm::dyn_cast<OpenArrayType>(CT)) {
      Kind = ForInStmt::OpenArray;
      Elem = OA->getElementType();
    } else if (auto *DA = llvm::dyn_cast<DynArrayType>(CT)) {
      Kind = ForInStmt::DynArray;
      Elem = DA->getElementType();
      markUsesManaged(); // the array is held during the loop
    } else if (CT->isAnsiString()) {
      Kind = ForInStmt::AnsiString;
      Elem = Ctx.getCharType();
      markUsesManaged(); // the string is held during the loop
    } else if (CT->isShortString()) {
      Kind = ForInStmt::ShortString;
      Elem = Ctx.getCharType();
    } else if (CT->isStringLiteral()) {
      Kind = ForInStmt::StringConst;
      Elem = Ctx.getCharType();
    } else if (auto *ST = llvm::dyn_cast<SetType>(CT)) {
      Kind = ForInStmt::Set;
      Elem = ST->getElementType(); // null for []: no elements
    } else if (CT->isClass() || CT->isInterface() || memberTableOf(CT)) {
      Kind = ForInStmt::Enumerator;
    }
    if (Kind == ForInStmt::Invalid && !CT->isError() && !IsType)
      Diags.error(CLoc,
                  "'for ... in' cannot enumerate a value of type " + quoted(CT) +
                      " (an array, string, set, ordinal type, or an object or "
                      "record with a GetEnumerator method)",
                  {C->getRange()});

    Scope InScope(Cur);
    Scope *Saved = Cur;
    auto MakeStep = [&](Expr *Value) {
      if (Untyped && !Untyped->getType()) {
        Cur = &InScope;
        Type *ET = check(Value);
        Cur = Saved;
        Untyped->setType(ET);
        VarRef->setType(ET);
      }
      auto *Target = Ctx.create<NameRefExpr>(Id);
      auto *Step = Ctx.create<AssignStmt>(
          SourceRange(Id.Loc, C->getRange().getEnd()), Target, S->getInLoc(),
          Value);
      Cur = &InScope;
      visitAssignStmt(Step);
      Cur = Saved;
      return Step;
    };
    if (Kind == ForInStmt::Enumerator) {
      if (!checkEnumerator(S, CT, Usable, InScope, MakeStep))
        Kind = ForInStmt::Invalid;
    } else if (Kind != ForInStmt::Invalid) {
      VarDecl *ElemV =
          Elem ? forInVar("in.elem", Elem, S->getInLoc(), InScope) : nullptr;
      AssignStmt *Step =
          Usable && Elem ? MakeStep(forInRef(ElemV, CLoc)) : nullptr;
      S->setLowering(Kind, Elem, ElemV, Step);
    }
    if (Untyped && !Untyped->getType()) {
      if (Kind != ForInStmt::Invalid)
        Diags.error(Id.Loc,
                    "cannot tell the type of '" + Id.Spelling +
                        "' from an empty set; give the variable a type",
                    {Id.getRange()});
      Untyped->setType(errorType());
      Usable = false;
    }

    if (Usable)
      ActiveFors.push_back({VarRef->getDecl(), S->getBeginLoc()});
    OwnedState Before = ownedGet();
    OwnedLoop Loop;
    ownedLoopBegin(Loop);
    ++LoopDepth;
    visitStmt(S->getBody());
    --LoopDepth;
    ownedLoopEnd(Loop, Before);
    if (Usable)
      ActiveFors.pop_back();
    Cur = OuterScope;
  }

  /// A for-in loop over CT through Collection.GetEnumerator: a function
  /// without arguments whose result has a function MoveNext: boolean and a
  /// property Current. False after an error.
  template <typename StepFn>
  bool checkEnumerator(ForInStmt *S, Type *CT, bool Usable, Scope &InScope,
                       StepFn MakeStep) {
    Expr *C = S->getCollection();
    SourceLocation CLoc = C->getBeginLoc();
    auto *Get = llvm::dyn_cast_or_null<RoutineDecl>(
        findEnumMember(CT, "getenumerator"));
    if (!Get || !Get->isFunction() || !acceptsArgCount(Get->getParams(), 0) ||
        !Get->getResultType()) {
      Diags.error(CLoc,
                  "'for ... in' cannot enumerate a value of type " + quoted(CT) +
                      ": it has no function GetEnumerator without arguments",
                  {C->getRange()});
      return false;
    }
    Type *ET = Get->getResultType();
    if (ET->isError())
      return false;
    auto *Next =
        llvm::dyn_cast_or_null<RoutineDecl>(findEnumMember(ET, "movenext"));
    if (!Next || !Next->isFunction() || !Next->getResultType() ||
        !Next->getResultType()->getBase()->isBoolean() ||
        !acceptsArgCount(Next->getParams(), 0)) {
      Diags.error(CLoc,
                  "enumerator " + quoted(ET) +
                      " has no function MoveNext: boolean without arguments",
                  {C->getRange()});
      noteDeclaredHere(Get);
      return false;
    }
    auto *Current =
        llvm::dyn_cast_or_null<PropertyDecl>(findEnumMember(ET, "current"));
    if (!Current || Current->isArrayProperty()) {
      Diags.error(CLoc, "enumerator " + quoted(ET) + " has no property Current",
                  {C->getRange()});
      noteDeclaredHere(Get);
      return false;
    }

    VarDecl *CollV = forInVar("in.coll", CT, S->getInLoc(), InScope);
    VarDecl *EnumV = forInVar("in.enum", ET, S->getInLoc(), InScope);
    Scope *Saved = Cur;
    Cur = &InScope;
    Expr *GetE = forInMember(forInRef(CollV, CLoc), "GetEnumerator", CLoc);
    Expr *NextE = forInMember(forInRef(EnumV, CLoc), "MoveNext", CLoc);
    bool OK = !check(GetE)->isError() && !check(NextE)->isError();
    Cur = Saved;
    if (!OK)
      return false;
    AssignStmt *Step =
        Usable ? MakeStep(forInMember(forInRef(EnumV, CLoc), "Current", CLoc))
               : nullptr;
    S->setEnumerator(CollV, EnumV, GetE, NextE, Step);
    if (ET->isClass())
      markHasTry(); // freed in a finally part
    return true;
  }

  void visitCaseStmt(CaseStmt *S) {
    DirectSequence = nullptr;
    Type *SelTy = check(S->getSelector());
    if (!SelTy->isError() && !SelTy->isOrdinal()) {
      Diags.error(S->getSelector()->getBeginLoc(),
                  "'case' selector must be of an ordinal type, not " +
                      quoted(SelTy),
                  {S->getSelector()->getRange()});
      SelTy = errorType();
    }
    // Labels are checked against the selector's host type: a subrange
    // selector may be compared with any value of its host.
    Type *LabelTy = SelTy->isError() ? SelTy : SelTy->getBase();

    // ISO 7185 6.8.3.5: no value may appear in more than one label.
    llvm::SmallVector<CaseLabel *, 16> Seen;
    OwnedState Before = ownedGet(), After;
    for (CaseArm *Arm : S->getArms()) {
      for (CaseLabel *L : Arm->getLabels()) {
        if (!checkLabel(L, LabelTy, "case label") || SelTy->isError())
          continue;
        for (CaseLabel *Prev : Seen) {
          auto K = [&](int64_t V) { return labelKey(V, LabelTy); };
          if (K(L->LoValue) > K(Prev->HiValue) || K(L->HiValue) < K(Prev->LoValue))
            continue;
          int64_t Dup = K(L->LoValue) > K(Prev->LoValue) ? L->LoValue
                                                          : Prev->LoValue;
          Diags.error(L->Lo->getBeginLoc(),
                      "duplicate case label value " + formatValue(SelTy, Dup),
                      {L->getRange()});
          Diags.note(Prev->Lo->getBeginLoc(), "previous case label is here");
          break;
        }
        Seen.push_back(L);
      }
      ownedSet(Before);
      visitStmt(Arm->getBody());
      After.join(ownedGet());
    }
    ownedSet(Before); // (no else part: no arm may run)
    visitSequence(S->getElseBody().data(), S->getElseBody());
    ownedJoin(After);
  }

  void visitWithStmt(WithStmt *S) {
    DirectSequence = nullptr;
    // Each record opens a scope; they nest left to right.
    llvm::SmallVector<std::unique_ptr<Scope>, 2> Scopes;
    Scope *Saved = Cur;
    for (WithContext *W : S->getRecords()) {
      W->Routine = CurRoutine;
      Type *T = check(W->Record);
      if (T->isError())
        continue;
      if (auto *CT = llvm::dyn_cast<ClassType>(T)) { // objfpc: an object
        Scopes.push_back(std::make_unique<Scope>(Cur, CT, W));
        Cur = Scopes.back().get();
        continue;
      }
      auto *RT = llvm::dyn_cast<RecordType>(T);
      if (!RT) {
        Diags.error(W->Record->getBeginLoc(),
                    "'with' requires a record, not a value of type " +
                        quoted(T),
                    {W->Record->getRange()});
        continue;
      }
      if (!isVariableReference(W->Record)) {
        Diags.error(W->Record->getBeginLoc(),
                    "'with' requires a record variable",
                    {W->Record->getRange()});
        continue;
      }
      if (ClassType *MT = RT->getMemberTable()) // its methods, properties too
        Scopes.push_back(std::make_unique<Scope>(Cur, MT, W));
      else
        Scopes.push_back(std::make_unique<Scope>(Cur, RT, W));
      Cur = Scopes.back().get();
      if (ClassType *H = helperFor(RT)) { // its helper's members (first)
        W->Helper = H;
        Scopes.push_back(std::make_unique<Scope>(Cur, H, W));
        Cur = Scopes.back().get();
      }
    }
    visitStmt(S->getBody());
    Cur = Saved;
  }

  //===--------------------------------------------------------------------===//
  // Expressions (each returns the expression's type)
  //===--------------------------------------------------------------------===//

  Type *visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    if (E->getValue() <= static_cast<uint64_t>(PascalMaxInt))
      return Ctx.getIntegerType();
    // Turbo/FPC: larger literals are int64 constants; beyond int64, qword
    // ($FFFFFFFFFFFFFFFF and the like: the bits of an int64, as in FPC).
    if (!isISO() && (E->getValue() <= static_cast<uint64_t>(INT64_MAX) ||
                     E->isBased()))
      return Ctx.getInt64Type();
    if (!isISO())
      return Ctx.getQWordType();
    Diags.error(E->getBeginLoc(), "integer literal exceeds maxint (2147483647)",
                {E->getRange()});
    return Ctx.getIntegerType();
  }

  Type *visitRealLiteralExpr(RealLiteralExpr *) { return Ctx.getRealType(); }

  Type *visitStringLiteralExpr(StringLiteralExpr *E) {
    if (E->isWideCharLiteral())
      return Ctx.getWideCharType();
    return E->isCharLiteral() ? Ctx.getCharType()
                              : Ctx.getStringLiteralType(E->getValue().size());
  }

  Type *visitNameRefExpr(NameRefExpr *E) {
    if (E == CompoundResultRef) {
      E->setDecl(CompoundResultVar);
      return CompoundResultVar->getType();
    }
    const Identifier &Id = E->getName();
    WithContext *W = nullptr;
    Decl *D = Cur->lookup(Id.Name, &W);
    noteLambdaUse(D, W, Id);
    if (!D) {
      diagUndeclared(Id);
      return errorType();
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) { // objfpc: overloads
      D = resolveOverload(R, {}, Id, Id.getRange());
      if (!D)
        return errorType();
    }
    E->setDecl(D);
    E->setWith(W);
    Used.insert(D);
    warnHints(D, Id);
    checkScopeAccess(D, W, Id);

    switch (D->getKind()) {
    case Decl::DK_Var: {
      Type *T = llvm::cast<VarDecl>(D)->getType();
      if (llvm::cast<VarDecl>(D)->isOwned())
        ownedUse(llvm::cast<VarDecl>(D), Id.getRange());
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(T)) {
        // A functional parameter without arguments is a call.
        if (RT->isFunction() && acceptsArgCount(RT->getParams(), 0))
          return RT->getResult();
        if (RT->isOfObject()) // nothing else it could be: the value
          return T;
        Diags.error(Id.Loc,
                    RT->isFunction()
                        ? "missing arguments in call to '" + Id.Spelling + "'"
                        : "procedure '" + Id.Spelling +
                              "' does not return a value",
                    {Id.getRange()});
        return errorType();
      }
      return T ? T : errorType();
    }
    case Decl::DK_Field: {
      Type *T = llvm::cast<FieldDecl>(D)->getType();
      return T ? T : errorType();
    }
    case Decl::DK_Const: {
      Type *T = llvm::cast<ConstDecl>(D)->getType();
      return T ? T : errorType();
    }
    case Decl::DK_Property:
      if (!checkArrayPropertyUse(llvm::cast<PropertyDecl>(D), E, Id))
        return errorType();
      return checkPropertyRead(llvm::cast<PropertyDecl>(D), E, Id);
    case Decl::DK_Routine: {
      auto *R = llvm::cast<RoutineDecl>(D);
      if (ClassType *C = constructedThroughSelf(R, W);
          C && acceptsArgCount(R->getParams(), 0))
        return C;
      if (R->isAsync() && acceptsArgCount(R->getParams(), 0))
        return R->getAsyncType(); // its task
      if (R->isProcedure()) {
        Diags.error(Id.Loc,
                    "procedure '" + Id.Spelling + "' does not return a value",
                    {Id.getRange()});
        return errorType();
      }
      if (!acceptsArgCount(R->getParams(), 0)) {
        Diags.error(Id.Loc,
                    "missing arguments in call to '" + Id.Spelling + "'",
                    {Id.getRange()});
        noteDeclaredHere(R);
        return errorType();
      }
      return R->getResultType(); // a call without arguments
    }
    case Decl::DK_Type:
      // objfpc: a class name as a value is that class (a class reference).
      if (auto *C = llvm::dyn_cast_or_null<ClassType>(
              llvm::cast<TypeDecl>(D)->getType());
          C && LangOpts.ObjKeywords)
        return classRefOf(C);
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a value",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    case Decl::DK_Builtin:
      // eof / eoln without arguments test `input`.
      if (auto K = llvm::cast<BuiltinDecl>(D)->getBuiltinKind();
          K == BuiltinKind::Eof || K == BuiltinKind::Eoln)
        return Ctx.getBooleanType();
      if (llvm::cast<BuiltinDecl>(D)->getBuiltinKind() == BuiltinKind::ParamCount)
        return Ctx.getIntegerType();
      if (llvm::cast<BuiltinDecl>(D)->getBuiltinKind() == BuiltinKind::ExceptAddr)
        return Ctx.getUntypedPointerType();
      if (llvm::cast<BuiltinDecl>(D)->isFunction()) {
        Diags.error(Id.Loc, "missing argument in call to '" + Id.Spelling + "'",
                    {Id.getRange()});
        return errorType();
      }
      Diags.error(Id.Loc,
                  "procedure '" + Id.Spelling + "' does not return a value",
                  {Id.getRange()});
      return errorType();
    default:
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a value",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }
  }

  Type *visitParenExpr(ParenExpr *E) { return check(E->getInner()); }

  /// move p: the object of the owned variable p of this routine, which
  /// gives it up.
  Type *visitMoveExpr(MoveExpr *E) {
    NameRefExpr *N = E->getOperand();
    const Identifier &Id = N->getName();
    WithContext *W = nullptr;
    Decl *D = Cur->lookup(Id.Name, &W);
    noteLambdaUse(D, W, Id);
    if (!D) {
      diagUndeclared(Id);
      return errorType();
    }
    auto *V = llvm::dyn_cast<VarDecl>(D);
    if (!V || !(V->isOwned() || V->isIsolated()) || W) {
      Diags.error(Id.Loc,
                  "'move' needs an owned or isolated variable, and '" +
                      Id.Spelling + "' is not one",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }
    N->setDecl(V);
    N->setType(V->getType());
    Used.insert(V);
    if (!V->isOwned()) // isolated: its data, the variable left empty
      return V->getType()->isError() ? errorType() : V->getType();
    if (V->getOwner() != CurRoutine) {
      Diags.error(Id.Loc,
                  "owned variable '" + Id.Spelling +
                      "' can only be moved by the routine it belongs to",
                  {Id.getRange()});
      noteDeclaredHere(V);
      return errorType();
    }
    ownedUse(V, Id.getRange());
    ownedMove(V, E->getBeginLoc());
    return V->getType();
  }

  Type *visitUnaryExpr(UnaryExpr *E) {
    Type *T = check(E->getOperand());
    if (T->isError())
      return errorType();
    if (memberTableOf(T)) // an advanced record's operator
      return checkRecordOperator(E);
    if (E->getOp() == UnaryOp::Not) {
      if (T->getBase()->isBoolean())
        return Ctx.getBooleanType();
      if (!isISO() && T->isIntegerLike()) // Turbo/FPC: bitwise not, same type
        return T->getBase();
    } else if (T->isNumeric()) {
      if (T->isReal())
        return Ctx.getRealType();
      Type *P = promoteInteger(T);
      // -cardinal and -qword can be negative (FPC: int64).
      return E->getOp() == UnaryOp::Minus &&
                     (P == Ctx.getCardinalType() || isQWord(P))
                 ? Ctx.getInt64Type()
                 : P;
    }
    Diags.error(E->getOpLoc(),
                llvm::Twine("invalid operand to '") + getOpSpelling(E->getOp()) +
                    "' (" + quoted(T) + ")",
                {E->getOperand()->getRange()});
    return errorType();
  }

  /// obj is TFoo / obj as TFoo (objfpc); the left operand may also be a
  /// COM interface (its object).
  Type *checkIsAs(BinaryExpr *E) {
    Type *L = check(E->getLHS());
    auto *LI = llvm::dyn_cast<InterfaceType>(L);
    bool COMObject = LI && LI->isCOM();
    const char *Op = getOpSpelling(E->getOp());
    ClassType *C = nullptr;
    Expr *RHS = E->getRHS();
    auto *N = llvm::dyn_cast<NameRefExpr>(RHS);
    TypeDecl *TD =
        N ? llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(N->getName().Name))
          : llvm::isa<FieldExpr>(RHS) ? typeNamedBy(RHS) // TOuter.TInner
                                      : nullptr;
    if (TD) {
      Type *T = TD->getType() ? TD->getType() : errorType();
      if (N)
        N->setDecl(TD);
      RHS->setType(T);
      if (T->isError())
        return errorType();
      C = llvm::dyn_cast<ClassType>(T);
      // obj is IFoo / obj as IFoo: does obj's class implement the interface?
      if (auto *I = llvm::dyn_cast<InterfaceType>(T)) {
        if (L->isError())
          return errorType();
        if (!L->isClass() && !COMObject) {
          Diags.error(E->getOpLoc(),
                      llvm::Twine("invalid operands to '") + Op + "' (" +
                          quoted(L) + " and " + quoted(I) +
                          "): the left operand must be an object or a COM "
                          "interface",
                      {E->getLHS()->getRange(), RHS->getRange()});
          return errorType();
        }
        return E->getOp() == BinaryOp::Is ? Ctx.getBooleanType() : I;
      }
    } else if (check(RHS)->isError()) {
      return errorType();
    }
    if (!C) {
      Diags.error(RHS->getBeginLoc(),
                  llvm::Twine("the right operand of '") + Op +
                      "' must be the name of a class or interface type",
                  {RHS->getRange()});
      return errorType();
    }
    if (L->isError())
      return errorType();
    if (COMObject) // its object's class is not known
      return E->getOp() == BinaryOp::Is ? Ctx.getBooleanType() : C;
    auto *LC = llvm::dyn_cast<ClassType>(L);
    if (!LC) {
      Diags.error(E->getOpLoc(),
                  llvm::Twine("invalid operands to '") + Op + "' (" + quoted(L) +
                      " and " + quoted(C) + ")",
                  {E->getLHS()->getRange(), RHS->getRange()});
      return errorType();
    }
    if (!C->inheritsFrom(LC) && !LC->inheritsFrom(C)) {
      Diags.error(E->getOpLoc(),
                  llvm::Twine("invalid operands to '") + Op + "': " + quoted(LC) +
                      " and " + quoted(C) + " are unrelated classes",
                  {E->getLHS()->getRange(), RHS->getRange()});
      return errorType();
    }
    return E->getOp() == BinaryOp::Is ? Ctx.getBooleanType() : C;
  }

  static bool isMethodPointer(Type *T) {
    auto *RT = llvm::dyn_cast<RoutineType>(T);
    return RT && RT->isOfObject();
  }

  /// An operand of = or <>: a method pointer variable is compared, not called.
  Type *checkComparand(Expr *E) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
      auto *V = llvm::dyn_cast_or_null<VarDecl>(Cur->lookup(N->getName().Name));
      if (V && V->getType() && isMethodPointer(V->getType())) {
        N->setDecl(V);
        N->setType(V->getType());
        Used.insert(V);
        return V->getType();
      }
    }
    return check(E);
  }

  Type *visitBinaryExpr(BinaryExpr *E) {
    if (E->getOp() == BinaryOp::Is || E->getOp() == BinaryOp::As)
      return checkIsAs(E);
    bool Equality = E->getOp() == BinaryOp::Eq || E->getOp() == BinaryOp::Ne;
    Type *L = Equality ? checkComparand(E->getLHS()) : check(E->getLHS());
    Type *R = Equality ? checkComparand(E->getRHS()) : check(E->getRHS());
    if (L->isError() || R->isError())
      return errorType();
    if (Equality && (isMethodPointer(L) || isMethodPointer(R)) &&
        (L->isNil() || R->isNil() || isAssignable(L, R)))
      return Ctx.getBooleanType();
    if (memberTableOf(L) || memberTableOf(R)) // an advanced record's operator
      return checkRecordOperator(E);

    BinaryOp Op = E->getOp();
    // Turbo/FPC strings: + concatenates, comparisons work for any lengths.
    if (!isISO() && L->isStringish() && R->isStringish()) {
      if (Op == BinaryOp::Add && !(L->isNumeric() || R->isNumeric()))
        return L->isAnsiString() || R->isAnsiString() ? Ctx.getAnsiStringType()
                                                      : Ctx.getShortStringType();
      if (isRelational(Op) && Op != BinaryOp::In && needsStringCompare(L, R))
        return Ctx.getBooleanType();
    }
    if (L->isSet() && R->isSet() && Op != BinaryOp::In) {
      if (!areCompatible(L, R)) {
        Diags.error(E->getOpLoc(),
                    "incompatible set types " + quoted(L) + " and " + quoted(R),
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      switch (Op) {
      case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
        // The operand with the larger representation (its values hold the
        // other's); the left one of equals, the other one of [].
        if (llvm::cast<SetType>(L)->isEmptySet())
          return R;
        if (llvm::cast<SetType>(R)->isEmptySet())
          return L;
        return llvm::cast<SetType>(R)->getStorageBits() >
                       llvm::cast<SetType>(L)->getStorageBits()
                   ? R
                   : L;
      case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Le: case BinaryOp::Ge:
        return Ctx.getBooleanType(); // <= is "subset of", >= "superset of"
      default:
        break;
      }
    }
    if ((Op == BinaryOp::Eq || Op == BinaryOp::Ne) &&
        (L->isPointer() || L->isNil()) && (R->isPointer() || R->isNil()) &&
        (areCompatible(L, R) || (L->isNil() && R->isNil())))
      return Ctx.getBooleanType();
    if ((Op == BinaryOp::Eq || Op == BinaryOp::Ne) &&
        (L->isClass() || R->isClass() || L->isClassRef() || R->isClassRef() ||
         L->isInterface() || R->isInterface() || L->isDynArray() || R->isDynArray()) &&
        areCompatible(L, R))
      return Ctx.getBooleanType(); // objects and classes (objfpc)

    if (!isISO() && (L->isPointer() || R->isPointer())) {
      // Turbo/FPC pointer arithmetic: p + n, n + p and p - n move p by n of
      // what it points to (bytes, for an untyped pointer); p - q counts
      // them; pointers are ordered by address.
      if ((Op == BinaryOp::Add || Op == BinaryOp::Sub) && L->isPointer() &&
          R->isIntegerLike())
        return L;
      if (Op == BinaryOp::Add && L->isIntegerLike() && R->isPointer())
        return R;
      if (L->isPointer() && R->isPointer() && areCompatible(L, R)) {
        if (Op == BinaryOp::Sub)
          return Ctx.getInt64Type();
        if (Op == BinaryOp::Lt || Op == BinaryOp::Le || Op == BinaryOp::Gt ||
            Op == BinaryOp::Ge)
          return Ctx.getBooleanType();
      }
    }

    bool BothIntegers = L->isIntegerLike() && R->isIntegerLike();
    auto IntegerOp = [&]() {
      Type *T = integerOperandType(E->getLHS(), L, E->getRHS(), R);
      E->setOperandType(T);
      return T;
    };
    switch (Op) {
    case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
      if (BothIntegers)
        return IntegerOp();
      if (L->isNumeric() && R->isNumeric())
        return Ctx.getRealType();
      break;
    case BinaryOp::RealDiv:
      if (L->isNumeric() && R->isNumeric())
        return Ctx.getRealType();
      break;
    case BinaryOp::IntDiv: case BinaryOp::Mod:
      if (BothIntegers)
        return IntegerOp();
      break;
    case BinaryOp::And: case BinaryOp::Or: case BinaryOp::Xor:
      if (L->getBase()->isBoolean() && R->getBase()->isBoolean() &&
          (Op != BinaryOp::Xor || !isISO()))
        return Ctx.getBooleanType();
      // Turbo/FPC: bitwise on integers.
      if (!isISO() && BothIntegers)
        return IntegerOp();
      break;
    case BinaryOp::Shl: case BinaryOp::Shr:
      if (BothIntegers) { // the left operand's (promoted) type
        E->setOperandType(promoteInteger(L));
        return E->getOperandType();
      }
      break;
    case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
    case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
      if (BothIntegers) {
        IntegerOp();
        return Ctx.getBooleanType();
      }
      if (L->isNumeric() && R->isNumeric())
        return Ctx.getBooleanType();
      if (L->isOrdinal() && R->isOrdinal() && areCompatible(L, R)) {
        if (L->getBase() != R->getBase() && L->isCharLike()) // char, widechar
          E->setOperandType(Ctx.getWideCharType());
        return Ctx.getBooleanType();
      }
      if (L->isStringType() && R->isStringType()) {
        if (areCompatible(L, R))
          return Ctx.getBooleanType();
        Diags.error(E->getOpLoc(),
                    "cannot compare strings of different lengths (" +
                        std::to_string(L->getStringLength()) + " and " +
                        std::to_string(R->getStringLength()) + ")",
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      break;
    case BinaryOp::Is: case BinaryOp::As:
      break; // checkIsAs
    case BinaryOp::In:
      if (L->isOrdinal() && R->isSet() &&
          (llvm::cast<SetType>(R)->isEmptySet() ||
           haveSameOrdinalHost(llvm::cast<SetType>(R)->getElementType(), L)))
        return Ctx.getBooleanType();
      break;
    }

    Diags.error(E->getOpLoc(),
                llvm::Twine("invalid operands to '") + getOpSpelling(Op) +
                    "' (" + quoted(L) + " and " + quoted(R) + ")",
                {E->getLHS()->getRange(), E->getRHS()->getRange()});
    return errorType();
  }

  Type *visitCallExpr(CallExpr *E) {
    // A lambda that copies what it uses: its factory, declared now.
    if (auto *F = llvm::dyn_cast_or_null<RoutineDecl>(E->getDecl());
        F && F->isCopyFactory()) {
      if (!F->isLambdaDone() &&
          !completeCopyFactory(F, E, WantedCall == E ? WantedType : nullptr))
        return errorType();
      if (F->isLambdaBad())
        return errorType();
    }
    const Identifier &Callee = E->getCallee();
    WithContext *W = nullptr;
    Decl *D = E->getTypeArgs().empty() ? Cur->lookup(Callee.Name, &W)
                                       : specialize(Callee, E->getTypeArgs(), &W);
    noteLambdaUse(D, W, Callee);
    bool Generic = false; // F(args) for a generic F: its types inferred
    if (!D && E->getTypeArgs().empty())
      D = inferSpecialization(Callee, E->getArgs(),
                              WantedCall == E ? WantedType : nullptr, Generic, &W);
    if (!D) {
      if (E->getTypeArgs().empty() && !Generic)
        diagUndeclared(Callee);
      for (Expr *Arg : E->getArgs())
        check(argValue(Arg));
      return errorType();
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) { // objfpc: overloads
      D = resolveOverload(R, E->getArgs(), Callee, E->getRange());
      if (!D)
        return errorType();
    }
    E->setDecl(D);
    warnHints(D, Callee);
    if (hasMembers(W)) { // a method of an object or advanced record
      E->setWith(W);
      checkScopeAccess(D, W, Callee);
    }

    if (RoutineType *RT = proceduralMember(D)) {
      // FCalc(x) in a method or a with statement: through the member
      auto *Ref = Ctx.create<NameRefExpr>(Callee);
      if (check(Ref)->isError())
        return errorType();
      E->setCalleeValue(Ref);
      checkCallArgs(RT->getParams(), nullptr, Callee,
                    orderedArgs(E, RT->getParams(), Callee), E->getRange());
      if (RT->isFunction())
        return RT->getResult();
      Diags.error(Callee.Loc,
                  "procedure '" + Callee.Spelling + "' does not return a value",
                  {Callee.getRange()});
      return errorType();
    }

    if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
      if (R->isAsync()) { // its task
        checkCallArgs(R, Callee, orderedArgs(E, R->getParams(), Callee),
                      E->getRange());
        return R->getAsyncType();
      }
      if (R->isFunction()) {
        checkCallArgs(R, Callee, orderedArgs(E, R->getParams(), Callee),
                      E->getRange());
        return R->getResultType();
      }
      if (ClassType *C = constructedThroughSelf(R, W)) {
        checkCallArgs(R, Callee, orderedArgs(E, R->getParams(), Callee),
                      E->getRange());
        return C;
      }
      for (Expr *Arg : E->getArgs())
        check(argValue(Arg));
      Diags.error(Callee.Loc,
                  "procedure '" + Callee.Spelling + "' does not return a value",
                  {Callee.getRange()});
      return errorType();
    }

    if (auto *V = llvm::dyn_cast<VarDecl>(D))
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
          RT && RT->isFunction()) {
        Used.insert(V); // (called: used)
        checkCallArgs(RT->getParams(), V, Callee,
                      orderedArgs(E, RT->getParams(), Callee), E->getRange());
        return RT->getResult();
      }

    if (auto *TD = llvm::dyn_cast<TypeDecl>(D); TD && !isISO())
      return checkTypecast(E, TD);
    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D))
      if (auto T = checkSpecialBuiltin(E, BD))
        return *T;

    llvm::SmallVector<Type *, 4> ArgTypes;
    for (Expr *Arg : E->getArgs())
      ArgTypes.push_back(check(Arg));

    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D)) {
      if (BD->isFunction())
        return checkBuiltinFunction(E, BD, ArgTypes);
      Diags.error(Callee.Loc,
                  "procedure '" + Callee.Spelling +
                      "' does not return a value",
                  {Callee.getRange()});
      return errorType();
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && V->getType() && V->getType()->isArray())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' is an array; use '[' ']' to "
                  "index it",
                  {Callee.getRange()});
    else
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' is a " + D->getKindName() +
                      ", not a function",
                  {Callee.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }

  /// `a ${x} b`: an ansistring; each value is one `write` can write, with
  /// the field width and precision it takes there.
  Type *visitTemplateExpr(TemplateExpr *E) {
    llvm::ArrayRef<Expr *> Parts = E->getParts();
    for (size_t I = 0; I < Parts.size(); ++I) {
      if (I % 2 == 0)
        check(Parts[I]);
      else
        checkWritten(Parts[I], /*Template=*/true);
    }
    return Ctx.getAnsiStringType();
  }

  /// Name := value where the parameters have no names to give (a standard
  /// routine, a typecast).
  Type *visitNamedArgExpr(NamedArgExpr *E) {
    check(E->getValue());
    Diags.error(E->getName().Loc,
                "a named argument is only allowed in a call to a declared "
                "routine",
                {E->getName().getRange()});
    return errorType();
  }

  Type *visitFormattedExpr(FormattedExpr *E) {
    check(E->getValue());
    Diags.error(E->getWidth()->getBeginLoc(),
                "field widths are only allowed in 'write' and 'writeln'",
                {E->getWidth()->getRange()});
    return errorType();
  }

  Type *visitIndexExpr(IndexExpr *E) {
    Expr *SavedBase = IndexBase;
    IndexBase = E->getBase();
    Type *BT = check(E->getBase());
    IndexBase = SavedBase;
    if (BT->isError())
      return errorType();
    if (LangOpts.ObjKeywords) {
      // An array property: obj.P[i], P[i], obj[i] (the default property).
      auto It = PendingIndexes.find(E->getBase());
      if (It != PendingIndexes.end()) {
        PendingIndex Pend = std::move(It->second);
        PendingIndexes.erase(It);
        return checkPropertyIndex(E, std::move(Pend));
      }
      if (auto *C = llvm::dyn_cast<ClassType>(BT)) {
        PropertyDecl *P = defaultProperty(C);
        if (!P) {
          Diags.error(E->getIndex()->getBeginLoc(),
                      "class " + quoted(C) + " has no default property",
                      {E->getBase()->getRange()});
          return errorType();
        }
        const ClassType *Owner = nullptr;
        C->findMember(P->getName().Name, &Owner);
        Identifier Id = P->getName();
        Id.Loc = E->getBase()->getBeginLoc();
        checkMemberAccess(P, Owner, Id);
        return checkPropertyIndex(E, {P, E->getBase(), {}});
      }
    }
    Type *IT = check(E->getIndex());
    if (IT->isError())
      return errorType();
    if (auto *DA = llvm::dyn_cast<DynArrayType>(BT)) { // a[0..high(a)]
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "array index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      return DA->getElementType();
    }
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(BT)) { // a[0..high(a)]
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "array index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      return OA->getElementType();
    }
    if (auto *AC = llvm::dyn_cast<ArrayOfConstType>(BT)) { // a[0..high(a)]
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "array index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      return AC->getElementType();
    }
    if (BT->isAnsiString()) { // s[1..length(s)]
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "string index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      return Ctx.getCharType();
    }
    if (auto *SS = llvm::dyn_cast<ShortStringType>(BT)) {
      // s[0] is the length byte, s[1..N] the characters.
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "string index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      if (auto C = fold(E->getIndex());
          C && (C->Value.Int < 0 || C->Value.Int > SS->getMaxLength()))
        Diags.error(E->getIndex()->getBeginLoc(),
                    "string index " + std::to_string(C->Value.Int) +
                        " is out of bounds 0.." +
                        std::to_string(SS->getMaxLength()),
                    {E->getIndex()->getRange()});
      return Ctx.getCharType();
    }
    if (auto *SL = llvm::dyn_cast<StringLiteralType>(BT); SL && !isISO()) {
      // Turbo/FPC: a string constant can be indexed, 1..N.
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "string index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      if (auto C = fold(E->getIndex());
          C && (C->Value.Int < 1 ||
                static_cast<uint64_t>(C->Value.Int) > SL->getLength()))
        Diags.error(E->getIndex()->getBeginLoc(),
                    "string index " + std::to_string(C->Value.Int) +
                        " is out of bounds 1.." + std::to_string(SL->getLength()),
                    {E->getIndex()->getRange()});
      return Ctx.getCharType();
    }
    if (auto *PT = llvm::dyn_cast<PointerType>(BT);
        PT && !isISO() && !PT->isUntyped() && PT->getPointee()) {
      // Turbo/FPC: p[i] is (p + i)^.
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(),
                    "pointer index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      return PT->getPointee();
    }
    auto *AT = llvm::dyn_cast<ArrayType>(BT);
    if (!AT) {
      Diags.error(E->getIndex()->getBeginLoc(),
                  "subscripted value is not an array (type " + quoted(BT) +
                      ")",
                  {E->getBase()->getRange()});
      return errorType();
    }
    Type *Idx = AT->getIndexType();
    if (!areCompatible(IT, Idx)) {
      Diags.error(E->getIndex()->getBeginLoc(),
                  "array index of type " + quoted(IT) +
                      " does not match index type " + quoted(Idx),
                  {E->getIndex()->getRange()});
      return errorType();
    }
    // Constant indices are checked at compile time.
    if (auto C = fold(E->getIndex())) {
      OrdinalBounds B = getOrdinalBounds(Idx);
      if (C->Value.Int < B.Lo || C->Value.Int > B.Hi)
        Diags.error(E->getIndex()->getBeginLoc(),
                    "array index " + formatValue(Idx, C->Value.Int) +
                        " is out of bounds " + formatValue(Idx, B.Lo) + ".." +
                        formatValue(Idx, B.Hi),
                    {E->getIndex()->getRange()});
    }
    return AT->getElementType();
  }

  /// One more index for the array property of Pend: while indexes are
  /// missing, E itself is pending (if more follow); with all of them, they
  /// are checked like arguments and E is the property access.
  Type *checkPropertyIndex(IndexExpr *E, PendingIndex Pend) {
    PropertyDecl *P = Pend.Prop;
    Pend.Args.push_back(E->getIndex());
    size_t N = P->getParams().size();
    if (Pend.Args.size() < N) {
      if (E == IndexBase) {
        PendingIndexes[E] = std::move(Pend);
        return P->getType();
      }
      E->setProperty(P, Pend.Root, Ctx.copyArray<Expr *>(Pend.Args));
      Identifier Id = propertyName(E);
      Diags.error(Id.Loc,
                  "array property '" + Id.Spelling + "' needs " +
                      std::to_string(N) + " indexes, got " +
                      std::to_string(Pend.Args.size()),
                  {E->getRange()});
      noteDeclaredHere(P);
      for (Expr *A : Pend.Args)
        check(A);
      return errorType();
    }
    E->setProperty(P, Pend.Root, Ctx.copyArray<Expr *>(Pend.Args));
    Identifier Id = propertyName(E);
    checkCallArgs(P->getParams(), P, Id, E->getPropertyArgs(), E->getRange());
    if (E == CurAssignTarget)
      return P->getType(); // checked by the assignment
    return checkPropertyRead(P, E, Id);
  }

  Type *visitNilExpr(NilExpr *) { return Ctx.getNilType(); }

  /// Checks a value assigned to a target of type TargetTy. A procedural
  /// variable named as the value of a procedural target is copied, not
  /// called.
  /// [a, b, c] given to a dynamic array of type T.
  Type *checkDynArrayCtor(SetCtorExpr *S, DynArrayType *T) {
    Type *Elem = T->getElementType();
    for (const SetElement &El : S->getElements()) {
      if (El.Hi) {
        Diags.error(El.Hi->getBeginLoc(),
                    "a range cannot be an element of a dynamic array",
                    {El.Hi->getRange()});
        continue;
      }
      Type *AT = checkValueFor(Elem, El.Lo);
      if (AT->isError())
        continue;
      if (!isAssignable(Elem, AT) && !isPCharLiteral(Elem, El.Lo) &&
          !convertImplicitly(El.Lo, Elem))
        Diags.error(El.Lo->getBeginLoc(),
                    "a value of type " + quoted(AT) +
                        " cannot be an element of a dynamic array of " + quoted(Elem),
                    {El.Lo->getRange()});
    }
    S->setType(T);
    return T;
  }

  Type *checkValueFor(Type *TargetTy, Expr *Value) {
    // A call: the type wanted of its result can give a generic routine a
    // type argument (inferSpecialization).
    if (llvm::isa<CallExpr>(Value)) {
      WantedCall = Value;
      WantedType = TargetTy;
    }
    // if c then a else b: both branches are values for the type wanted.
    if (auto *CE = llvm::dyn_cast<CondExpr>(Value); CE && !CE->getType()) {
      Type *T = checkCond(CE, TargetTy);
      CE->setType(T);
      if (isManaged(T))
        markUsesManaged();
      return T;
    }
    // A tuple written out takes the element types of the tuple wanted.
    if (auto *TE = llvm::dyn_cast<TupleExpr>(Value)) {
      auto *Want = llvm::dyn_cast<RecordType>(TargetTy);
      if (Want && Want->isTuple() &&
          Want->getAllFields().size() == TE->getElements().size()) {
        Type *T = checkTuple(TE, Want);
        TE->setType(T);
        if (isManaged(T))
          markUsesManaged();
        return T;
      }
    }
    if (auto *DA = llvm::dyn_cast<DynArrayType>(TargetTy))
      if (auto *S = llvm::dyn_cast<SetCtorExpr>(Value))
        return checkDynArrayCtor(S, DA);
    if (auto *TP = llvm::dyn_cast<PointerType>(TargetTy);
        TP && TP->isUntyped() && llvm::isa<AddrOfExpr>(Value)) {
      AddrOfWantsCode = true;
      Type *T = check(Value);
      AddrOfWantsCode = false;
      return T;
    }
    if (auto *RT = llvm::dyn_cast<RoutineType>(TargetTy);
        RT && llvm::isa<AddrOfExpr>(Value)) { // @p picks an overload by RT
      RoutineType *Saved = AddrOfTarget;
      AddrOfTarget = RT;
      Type *T = check(Value);
      AddrOfTarget = Saved;
      return T;
    }
    if (llvm::isa<RoutineType>(TargetTy))
      if (auto *N = llvm::dyn_cast<NameRefExpr>(Value)) {
        auto *V = llvm::dyn_cast_or_null<VarDecl>(Cur->lookup(N->getName().Name));
        if (V && llvm::isa_and_nonnull<RoutineType>(V->getType())) {
          N->setDecl(V);
          N->setType(V->getType());
          Used.insert(V);
          return V->getType();
        }
      }
    return check(Value);
  }

  /// The procedural type of a call through a designator, or null after a
  /// diagnostic.
  RoutineType *checkIndirectCallee(IndirectCallExpr *E) {
    Type *T = check(E->getCallee());
    if (T->isError())
      return nullptr;
    auto *RT = llvm::dyn_cast<RoutineType>(T);
    if (!RT) {
      Diags.error(E->getCallee()->getBeginLoc(),
                  "called value of type " + quoted(T) +
                      " is not a procedure or function",
                  {E->getCallee()->getRange()});
      for (Expr *A : E->getArgs())
        check(A);
      return nullptr;
    }
    Identifier Name;
    Name.Spelling = "procedural value";
    Name.Loc = E->getBeginLoc();
    checkCallArgs(RT->getParams(), nullptr, Name,
                  orderedArgs(E, RT->getParams(), Name), E->getRange());
    return RT;
  }

  Type *visitIndirectCallExpr(IndirectCallExpr *E) {
    if (isMethodCallee(E->getCallee())) {
      Type *T = checkMethodCall(E);
      if (T)
        return T;
      Diags.error(E->getBeginLoc(), "procedure does not return a value",
                  {E->getRange()});
      return errorType();
    }
    RoutineType *RT = checkIndirectCallee(E);
    if (!RT)
      return errorType();
    if (!RT->isFunction()) {
      Diags.error(E->getBeginLoc(), "procedure does not return a value",
                  {E->getRange()});
      return errorType();
    }
    return RT->getResult();
  }

  Type *visitTupleExpr(TupleExpr *E) { return checkTuple(E, nullptr); }

  Type *visitCondExpr(CondExpr *E) { return checkCond(E, nullptr); }

  /// if c then a else b, a value. Want: the type it is a value for, which
  /// both branches then take; where they cannot, and without Want, its
  /// type is the common type of the branches. It becomes an if statement
  /// that assigns the branch chosen to a hidden variable.
  Type *checkCond(CondExpr *E, Type *Want) {
    checkCondition(E->getCond(), "if");
    Type *A = Want ? checkValueFor(Want, E->getThen()) : check(E->getThen());
    Type *B = Want ? checkValueFor(Want, E->getElse()) : check(E->getElse());
    if (A->isError() || B->isError())
      return errorType();
    Type *R = nullptr;
    if (Want && !Want->isError() && isAssignable(Want, A) &&
        isAssignable(Want, B))
      R = Want;
    if (!R) {
      Type *Str = E->hasLongStrings()
                      ? static_cast<Type *>(Ctx.getAnsiStringType())
                      : Ctx.getShortStringType(255);
      Type *TA = A->isStringLiteral() ? Str : A;
      Type *TB = B->isStringLiteral() ? Str : B;
      if (isInferable(TA) && isInferable(TB))
        R = commonType(TA, TB, Str);
      else if (isInferable(TA) && isAssignable(TA, TB))
        R = TA; // (nil, [] in the other branch)
      else if (isInferable(TB) && isAssignable(TB, TA))
        R = TB;
      if (!R) {
        Diags.error(E->getBeginLoc(),
                    "the branches of this conditional expression have no "
                    "type in common: " + quoted(A) + " and " + quoted(B),
                    {E->getThen()->getRange(), E->getElse()->getRange()});
        return errorType();
      }
    }
    VarDecl *Tmp = makeHiddenVar(R, E->getBeginLoc());
    if (!Tmp) {
      Diags.error(E->getBeginLoc(),
                  "a conditional expression can only be written in the "
                  "statements of a routine or program",
                  {E->getRange()});
      return errorType();
    }
    auto Ref = [&](SourceLocation Loc) {
      Identifier Id = Tmp->getName();
      Id.Loc = Loc;
      auto *N = Ctx.create<NameRefExpr>(Id);
      N->setDecl(Tmp);
      N->setType(R);
      return N;
    };
    auto Assign = [&](Expr *V, Type *VT) {
      auto *S = Ctx.create<AssignStmt>(V->getRange(), Ref(V->getBeginLoc()),
                                       V->getBeginLoc(), V);
      finishAssign(S, R, VT);
      return S;
    };
    AssignStmt *Then = Assign(E->getThen(), A);
    AssignStmt *Else = Assign(E->getElse(), B);
    E->setLowered(Ctx.create<IfStmt>(E->getRange(), E->getCond(), Then, Else),
                  Ref(E->getBeginLoc()));
    return R;
  }

  /// (a, b): a tuple value. Want: the tuple type it is a value for (with
  /// as many elements), whose element types its elements then take; where
  /// they cannot, and without Want, each element has the type a variable
  /// declared with its value would have.
  Type *checkTuple(TupleExpr *E, RecordType *Want) {
    llvm::SmallVector<Type *, 4> Types;
    bool OK = true;
    for (size_t I = 0; I < E->getElements().size(); ++I) {
      Expr *X = E->getElements()[I];
      Type *T = Want ? checkValueFor(Want->getAllFields()[I]->getType(), X)
                     : check(X);
      OK &= !T->isError();
      Types.push_back(T);
    }
    if (!OK)
      return errorType();
    Type *Result = Want;
    for (size_t I = 0; Result && I < Types.size(); ++I) {
      Type *FT = Want->getAllFields()[I]->getType();
      Expr *X = E->getElements()[I];
      if (isAssignable(FT, Types[I]) || convertImplicitly(X, FT))
        checkWideToChar(FT, X);
      else
        Result = nullptr;
    }
    if (!Result) {
      for (size_t I = 0; I < Types.size(); ++I) {
        Type *T = Types[I];
        Expr *X = E->getElements()[I];
        auto *ST = llvm::dyn_cast<SetType>(T);
        if (T->isStringLiteral() || (isCharLiteral(X) && !KeepCharLiterals)) {
          T = E->hasLongStrings() ? Ctx.getAnsiStringType()
                                  : Ctx.getShortStringType(255);
        } else if (T->isNil() || T->isUntyped() || T->isOpenArray() ||
                   T->isArrayOfConst() || containsFile(T) ||
                   (ST && ST->isEmptySet())) {
          Diags.error(X->getBeginLoc(),
                      "cannot tell the type of element " +
                          std::to_string(I + 1) + " of the tuple from " +
                          (T->isNil() ? std::string("nil")
                           : ST       ? std::string("an empty set")
                                      : "a value of type " + quoted(T)) +
                          "; assign the tuple to a variable of a tuple type",
                      {X->getRange()});
          return errorType();
        }
        Types[I] = T;
      }
      Result = getTupleType(Types, {});
    }
    if (VarDecl *V = E->getTemp()) {
      V->setType(Result);
    } else if (VarDecl *New = makeHiddenVar(Result, E->getBeginLoc())) {
      E->setTemp(New);
    } else {
      Diags.error(E->getBeginLoc(),
                  "a tuple can only be written in the statements of a "
                  "routine or program",
                  {E->getRange()});
      return errorType();
    }
    return Result;
  }

  Type *visitAggregateExpr(AggregateExpr *E) {
    Diags.error(E->getBeginLoc(),
                "a parenthesized list is only allowed as the value of a typed "
                "constant",
                {E->getRange()});
    return errorType();
  }

  /// @obj.M, @TFoo.M, or @M in a method: a method pointer. Null if the
  /// operand is not a method.
  Type *checkMethodAddr(AddrOfExpr *E) {
    bool WantsCode = AddrOfWantsCode;
    AddrOfWantsCode = false;
    if (!LangOpts.ObjKeywords)
      return nullptr;
    Expr *Op = E->getOperand();
    RoutineDecl *R = nullptr;
    Identifier Name;
    bool ViaClass = false;
    auto *F = llvm::dyn_cast<FieldExpr>(Op);
    auto *N = llvm::dyn_cast<NameRefExpr>(Op);
    if (F) {
      if (!isMethodCallee(F))
        return nullptr;
      ClassType *Named = classNamedBy(F->getBase());
      ViaClass = Named != nullptr;
      const ClassType *Owner = nullptr;
      auto *M = Named ? llvm::dyn_cast_or_null<RoutineDecl>(
                            Named->findMember(F->getField().Name, &Owner))
                      : nullptr;
      if (M && (!M->isClassMethod() || WantsCode)) { // @TFoo.M: its code
        const Identifier &Field = F->getField();
        const char *Why = nullptr;
        if (M->isConstructor() || M->isDestructor())
          Why = "constructors and destructors have no code address here";
        else if (M->getBuiltin() != TObjectMethod::None)
          Why = "the methods of TObject have no code address";
        else if (M->isAbstract())
          Why = "abstract methods have no code";
        if (Why) {
          Diags.error(Field.Loc, "'@" + Field.Spelling + "': " + Why,
                      {Field.getRange()});
          return errorType();
        }
        checkMemberAccess(M, Owner, Field);
        F->setMember(M);
        Used.insert(M);
        warnHints(M, Field);
        E->setCode(M);
        return Ctx.getUntypedPointerType();
      }
      Decl *D = resolveMember(F);
      if (!D)
        return errorType();
      R = llvm::dyn_cast<RoutineDecl>(D);
      Name = F->getField();
      if (R && F->getBase()->getType() && F->getBase()->getType()->isInterface()) {
        Diags.error(Name.Loc, "'@" + Name.Spelling +
                                  "': interface methods cannot be method pointers",
                    {Name.getRange()});
        return errorType();
      }
    } else if (N) {
      WithContext *W = nullptr;
      R = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(N->getName().Name, &W));
      if (R && R->isMethod()) {
        N->setWith(W);
        Name = N->getName();
        checkScopeAccess(R, W, Name);
      } else {
        R = nullptr;
      }
    }
    if (!R)
      return nullptr;
    if (R->isOverloaded()) {
      RoutineDecl *Pick = nullptr;
      for (RoutineDecl *X = R; AddrOfTarget && X; X = X->getNextOverload())
        if (!Pick && AddrOfTarget->isOfObject() &&
            congruent(AddrOfTarget->getParams(), AddrOfTarget->getResult(),
                      X->getParams(), X->isFunction() ? X->getResultType() : nullptr))
          Pick = X;
      if (!Pick) {
        Diags.error(Name.Loc,
                    "'@" + Name.Spelling + "': '" + Name.Spelling +
                        "' is overloaded; assign it to a method pointer of the "
                        "heading wanted",
                    {Name.getRange()});
        return errorType();
      }
      R = Pick;
    }
    const char *Why = nullptr;
    if (R->isConstructor() || R->isDestructor())
      Why = "constructors and destructors cannot be method pointers";
    else if (R->getBuiltin() != TObjectMethod::None)
      Why = "the methods of TObject cannot be method pointers";
    if (Why) {
      Diags.error(Name.Loc, "'@" + Name.Spelling + "': " + Why, {Name.getRange()});
      return errorType();
    }
    if (R->isStatic()) { // a routine without Self: a procedural value
      if (F)
        F->setMember(R);
      else
        N->setDecl(R);
      Used.insert(R);
      warnHints(R, Name);
      E->setCode(R);
      return Ctx.create<RoutineType>(R->getParams(),
                                     R->isFunction() ? R->getResultType() : nullptr,
                                     R->isCDecl(), false,
                                     AddrOfTarget && AddrOfTarget->isReference());
    }
    if (F)
      F->setMember(R);
    else
      N->setDecl(R);
    Used.insert(R);
    warnHints(R, Name);
    E->setMethod(R);
    return Ctx.create<RoutineType>(R->getParams(),
                                   R->isFunction() ? R->getResultType() : nullptr,
                                   R->isCDecl(), /*OfObject=*/true);
  }

  /// The lambda A (function(x) => e), used where a value of type Want is
  /// wanted (null: none): its hidden routine is declared here, nested in
  /// the current routine and in the current scope. Parameter types left
  /// out are Want's; the result type left out is Want's, or else the type
  /// of the body (set by visitAssignStmt). Then A is @routine.
  void completeLambda(AddrOfExpr *A, RoutineType *Want) {
    RoutineDecl *R = A->getLambda();
    if (!R || R->isLambdaDone())
      return;
    R->setLambdaDone();
    if (Want && Want->isReference())
      R->setClosure();
    if (Want && Want->isOfObject() && !makeMethodLambda(R, A))
      return;
    auto Params = R->getParams();
    if (Want && Want->getParams().size() != Params.size()) {
      Diags.error(A->getBeginLoc(),
                  "this lambda has " + llvm::Twine(Params.size()) +
                      (Params.size() == 1 ? " parameter" : " parameters") +
                      ", but " + quoted(Want) + " is wanted here",
                  {A->getRange()});
      R->setLambdaBad();
    }
    for (size_t I = 0; I < Params.size(); ++I) {
      VarDecl *P = Params[I];
      P->setOwner(R);
      Type *T;
      if (P->getTypeSpec()) {
        T = resolveType(P->getTypeSpec());
      } else if (Want && I < Want->getParams().size() &&
                 Want->getParams()[I]->getType()) {
        VarDecl *WP = Want->getParams()[I];
        T = WP->getType();
        if (P->getVarKind() == VarDecl::ValueParam) { // (x): as wanted
          P->setVarKind(WP->getVarKind());
          if (WP->isOutParam())
            P->setOutParam();
          if (WP->isConstRefParam())
            P->setConstRefParam();
        }
      } else {
        R->setLambdaBad();
        if (!Want)
          Diags.error(P->getLocation(),
                      "cannot infer the type of parameter '" +
                          P->getName().Spelling +
                          "' in this lambda: there is no expected type; add "
                          "a type, for example function(" +
                          P->getName().Spelling + ": Integer) => ...",
                      {P->getName().getRange()});
        T = errorType();
      }
      P->setType(T);
    }
    if (R->isFunction()) {
      Type *ResultTy = nullptr; // (not known yet: the body's)
      if (R->getResultTypeSpec())
        ResultTy = resolveType(R->getResultTypeSpec());
      else if (Want && Want->getResult())
        ResultTy = Want->getResult();
      R->setResultType(ResultTy);
      auto *Result = Ctx.create<VarDecl>(R->getName(), R->getResultTypeSpec(),
                                         VarDecl::Result);
      Result->setType(ResultTy);
      Result->setOwner(R);
      R->setResultVar(Result);
    }
    declareAndCheckLambda(R);
    if (R->isFunction() && !R->getResultType()) {
      R->setResultType(errorType());
      R->getResultVar()->setType(errorType());
    }
  }

  /// Lambda R (or a copy factory), its types known: declared here, nested
  /// in the current routine, and its body checked now, in the middle of a
  /// statement.
  void declareAndCheckLambda(RoutineDecl *R) {
    R->setParent(CurRoutine);
    Used.insert(R);
    // In the innermost scope of declarations (not that of a with statement
    // or of a class's members, where it would not be found).
    Scope *Saved = Cur;
    while (Cur->isMemberScope() && Cur->getParent())
      Cur = Cur->getParent();
    declare(R);
    Cur = Saved;
    unsigned SavedLoop = LoopDepth;
    const void *SavedSeq = DirectSequence;
    std::unique_ptr<Scope> *SavedSeqScope = SeqScope;
    Expr *SavedAssign = CurAssignTarget, *SavedOwned = OwnedTarget;
    Expr *SavedIndex = IndexBase, *SavedWanted = WantedCall;
    RoutineType *SavedAddr = AddrOfTarget;
    bool SavedKeep = KeepCharLiterals;
    LoopDepth = 0;
    DirectSequence = nullptr;
    SeqScope = nullptr;
    CurAssignTarget = OwnedTarget = IndexBase = WantedCall = nullptr;
    AddrOfTarget = nullptr;
    KeepCharLiterals = false;
    checkRoutineBody(R);
    LoopDepth = SavedLoop;
    DirectSequence = SavedSeq;
    SeqScope = SavedSeqScope;
    CurAssignTarget = SavedAssign;
    OwnedTarget = SavedOwned;
    IndexBase = SavedIndex;
    WantedCall = SavedWanted;
    AddrOfTarget = SavedAddr;
    KeepCharLiterals = SavedKeep;
  }

  /// The copy factory F of a lambda that copies what it uses, called by E
  /// where Want is wanted: a closure must be. Its parameters, the copies,
  /// take the types of the variables they copy (read-only); its result is
  /// Want, its body makes the lambda (completed for Want). False after a
  /// diagnostic.
  bool completeCopyFactory(RoutineDecl *F, CallExpr *E, Type *Want) {
    F->setLambdaDone();
    auto *RT = llvm::dyn_cast_or_null<RoutineType>(Want);
    if (!RT || !RT->isReference()) {
      Diags.error(E->getBeginLoc(),
                  "a lambda that copies what it uses must be a closure: a "
                  "'reference to' type must be wanted here",
                  {E->getRange()});
      F->setLambdaBad();
      return false;
    }
    if (F->isCopyAll()) { // the names that are variables of routines
      llvm::SmallVector<VarDecl *, 4> Kept;
      llvm::SmallVector<Expr *, 4> KeptArgs;
      for (size_t I = 0; I < F->getParams().size(); ++I) {
        WithContext *W = nullptr;
        auto *V = llvm::dyn_cast_or_null<VarDecl>(
            Cur->lookup(F->getParams()[I]->getName().Name, &W));
        Type *T = V ? V->getType() : nullptr;
        if (V && !W && V->getOwner() && !V->isTypedConst() &&
            !V->isClassVar() && T && !T->isOpenArray() &&
            !T->isArrayOfConst() && !T->isUntyped() && !containsFile(T)) {
          Kept.push_back(F->getParams()[I]);
          KeptArgs.push_back(E->getArgs()[I]);
        }
      }
      F->setParams(Ctx.copyArray<VarDecl *>(Kept));
      E->setArgs(Ctx.copyArray<Expr *>(KeptArgs));
    }
    bool OK = true;
    for (VarDecl *P : F->getParams()) {
      const Identifier &Id = P->getName();
      Decl *D = Cur->lookup(Id.Name);
      Type *T = nullptr;
      if (auto *V = llvm::dyn_cast_or_null<VarDecl>(D))
        T = V->getType();
      else if (auto *Fd = llvm::dyn_cast_or_null<FieldDecl>(D))
        T = Fd->getType(); // (an object's field, through Self or a with)
      const char *Why = nullptr;
      if (!T)
        Why = "a variable, a parameter or a field";
      else if (T->isOpenArray() || T->isArrayOfConst() || T->isUntyped())
        Why = "of a type that can be copied, not an open or untyped parameter";
      else if (containsFile(T))
        Why = "of a type that can be copied, not a file";
      if (Why) {
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' in the list of what a lambda copies "
                    "must be " + Why,
                    {Id.getRange()});
        OK = false;
        T = errorType();
      }
      P->setType(T);
      P->setOwner(F);
      P->setReadOnly();
    }
    if (!OK) {
      F->setLambdaBad();
      return false;
    }
    F->setResultType(RT);
    auto *Result = Ctx.create<VarDecl>(F->getName(), nullptr, VarDecl::Result);
    Result->setType(RT);
    Result->setOwner(F);
    F->setResultVar(Result);
    declareAndCheckLambda(F);
    return true;
  }

  /// Lambda R (at A) wanted as a method pointer: it gets a Self of its own,
  /// of the class of the method it is written in (the nearest routine
  /// around it with a Self, which must be an object), and that method's
  /// Self is its object. Reported (and R bad) if there is none.
  bool makeMethodLambda(RoutineDecl *R, AddrOfExpr *A) {
    RoutineDecl *M = CurRoutine;
    while (M && !M->hasSelf())
      M = M->getParent();
    VarDecl *OuterSelf = M ? M->getSelfVar() : nullptr;
    if (!M) { // outside methods: no object (nil), no members
      auto *Self = Ctx.create<VarDecl>(memberName("Self"), nullptr,
                                       VarDecl::ValueParam);
      Self->setType(Ctx.getUntypedPointerType());
      Self->setOwner(R);
      R->setSelfVar(Self);
      R->setMethodLambda(nullptr);
      return true;
    }
    if (!OuterSelf || !OuterSelf->getType() || !OuterSelf->getType()->isClass()) {
      Diags.error(A->getBeginLoc(),
                  "a lambda can be a method pointer ('of object') only in "
                  "an object's method, whose Self is its object, or outside "
                  "methods (its object is nil)",
                  {A->getRange()});
      R->setLambdaBad();
      return false;
    }
    auto *C = llvm::cast<ClassType>(OuterSelf->getType());
    R->setClass(C);
    makeSelf(R, C);
    R->setMethodLambda(OuterSelf);
    // Its object: M's Self, where the lambda is made.
    if (M != CurRoutine) {
      OuterSelf->setCaptured();
      Identifier Id = memberName("Self");
      Id.Loc = A->getBeginLoc();
      noteLambdaUse(OuterSelf, nullptr, Id);
    }
    return true;
  }

  /// A use, in a lambda, of D (named Id, found through W): if D belongs to
  /// a routine around the lambda (a variable, a member through Self or a
  /// with statement, a nested routine), the lambdas up to that routine
  /// must not outlive it (RoutineDecl::getCapture).
  ///
  /// A closure (a lambda made a `reference to` value) may outlive them:
  /// their frames it uses are then kept on the heap (setHeapFrame), and
  /// it must not use what does not live in a frame (closureCannotUse).
  void noteLambdaUse(Decl *D, WithContext *W, const Identifier &Id) {
    noteTaskUse(D, W, Id);
    if (!CurRoutine || !CurRoutine->isLambda() || !D)
      return;
    RoutineDecl *Owner = nullptr, *Nested = nullptr;
    VarDecl *Var = nullptr;
    if (W) {
      if (W->Routine == CurRoutine)
        return;
      if (!W->IsSelf) {
        // Its address is the enclosing routine's to compute, not the
        // lambda's.
        Diags.error(Id.Loc,
                    "this lambda uses '" + Id.Spelling +
                        "' through a 'with' statement outside it; name the "
                        "record instead",
                    {Id.getRange()});
        return;
      }
      Owner = W->Routine;
      Var = Owner ? Owner->getSelfVar() : nullptr;
    } else if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
      if (!V->getOwner() || V->getOwner() == CurRoutine || V->isTypedConst() ||
          V->isClassVar())
        return;
      Owner = V->getOwner();
      Var = V;
    } else if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
      if (!R->getParent() || R == CurRoutine)
        return;
      Owner = R->getParent();
      Nested = R;
    } else {
      return;
    }
    for (RoutineDecl *L = CurRoutine; L && L != Owner; L = L->getParent()) {
      if (!L->isLambda())
        continue;
      if (L->isMethodLambda()) { // called on its object, without a frame
        Diags.error(Id.Loc,
                    "this method-pointer lambda uses '" + Id.Spelling +
                        "' of the routine around it; it can use only its "
                        "object's members, its parameters and what is "
                        "declared at program or unit level",
                    {Id.getRange()});
        return;
      }
      if (!L->getCapture().isValid())
        L->setCapture(Id);
      if (!L->isClosure())
        continue;
      if (Nested && !Nested->isLambda()) {
        Diags.error(Id.Loc,
                    "this closure uses '" + Id.Spelling +
                        "', a routine nested in '" + Owner->getName().Spelling +
                        "'; a closure can only use routines declared at "
                        "program or unit level",
                    {Id.getRange()});
        return;
      }
      if (std::string Why = Var ? closureCannotUse(Var) : ""; !Why.empty()) {
        Diags.error(Id.Loc,
                    "this closure uses '" + Id.Spelling + "', " + Why +
                        ", which may not exist when the closure runs; copy it "
                        "to a local variable first",
                    {Id.getRange()});
        return;
      }
      for (RoutineDecl *F = L->getParent(); F; F = F->getParent()) {
        F->setHeapFrame();
        if (F == Owner)
          break;
      }
    }
  }

  /// The innermost spawn block around the code being checked, if R (a
  /// routine; null: the program) is outside it: the task that would share
  /// R's variables. Null otherwise.
  RoutineDecl *spawnOutside(RoutineDecl *R) const {
    for (RoutineDecl *X = CurRoutine; X; X = X->getParent()) {
      if (X == R)
        return nullptr;
      if (X->isSpawnBlock())
        return X;
    }
    return nullptr;
  }

  /// D (used through W) in a spawn block: an isolated variable of another
  /// task is an error; under {$modeswitch racechecks}, so is anything
  /// mutable that is not shared: a variable of the routines around it or
  /// of the program, Self's members (unless its class is thread-safe), a
  /// routine nested in another (which may use its variables).
  void noteTaskUse(Decl *D, WithContext *W, const Identifier &Id) {
    if (!D || !CurRoutine)
      return;
    if (W) {
      if (!W->Routine)
        return;
      RoutineDecl *S = spawnOutside(W->Routine);
      if (!S || !S->isRaceChecked())
        return;
      if (W->IsSelf) {
        ClassType *C = W->Routine->getClass();
        auto *F = llvm::dyn_cast<FieldDecl>(D);
        if ((C && isThreadSafe(C)) || (F && F->isShared()))
          return;
        Diags.error(Id.Loc,
                    "this spawned task uses '" + Id.Spelling +
                        "' of Self, which other tasks may use at the same "
                        "time; make it a shared field, or derive the class "
                        "from TSharedObject",
                    {Id.getRange()});
      } else {
        Diags.error(Id.Loc,
                    "this spawned task uses '" + Id.Spelling +
                        "' through a 'with' statement outside it, which "
                        "other tasks may change at the same time",
                    {Id.getRange()});
      }
      return;
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
      RoutineDecl *S = spawnOutside(V->getOwner());
      if (!S)
        return;
      if (V->isIsolated()) {
        Diags.error(Id.Loc,
                    "isolated variable '" + Id.Spelling +
                        "' belongs to one task; a spawned task cannot use it "
                        "(hand it to an async routine with 'move " +
                        Id.Spelling + "')",
                    {Id.getRange()});
        noteDeclaredHere(V);
        return;
      }
      if (!S->isRaceChecked() || V->isShared() || V->getType()->isError())
        return;
      bool IsSelf = V->getOwner() && V == V->getOwner()->getSelfVar();
      bool ReadOnly = IsSelf || V->isReadOnly() ||
                      (V->isConstParam() && !V->isConstRefParam());
      if (ReadOnly && !sharesMutableData(V->getType()))
        return;
      Diags.error(Id.Loc,
                  "this spawned task uses '" + Id.Spelling +
                      "', which is not shared: another task may change it at "
                      "the same time; declare it shared (of a thread-safe "
                      "type), or pass its value to an async routine",
                  {Id.getRange()});
      noteDeclaredHere(V);
      return;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
      if (!R->getParent() || R->isLambda() || R->isSpawnBlock())
        return;
      RoutineDecl *S = spawnOutside(R->getParent());
      if (S && S->isRaceChecked())
        Diags.error(Id.Loc,
                    "this spawned task calls '" + Id.Spelling +
                        "', a routine nested in '" +
                        R->getParent()->getName().Spelling +
                        "', which may use that routine's variables while "
                        "other tasks change them",
                    {Id.getRange()});
    }
  }

  /// E an isolated variable (by name): it cannot be copied, only moved.
  void checkIsolatedCopy(Expr *E, const char *Where) {
    while (auto *P = llvm::dyn_cast<ParenExpr>(E))
      E = P->getInner();
    auto *N = llvm::dyn_cast<NameRefExpr>(E);
    auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
    if (!V || !V->isIsolated())
      return;
    Diags.error(N->getName().Loc,
                "isolated variable '" + N->getName().Spelling +
                    "' cannot be copied (" + Where + "): its data belongs to "
                    "one task; write 'move " + N->getName().Spelling +
                    "' to hand it over",
                {N->getRange()});
  }

  /// Why a closure cannot use V, a variable of a routine around it (empty:
  /// it can): it is not kept in that routine's frame, or is not counted
  /// there.
  static std::string closureCannotUse(VarDecl *V) {
    std::string Of = V->getOwner()->isLambda()
                         ? std::string("the lambda around it")
                         : "'" + V->getOwner()->getName().Spelling.str() + "'";
    if (V->getVarKind() == VarDecl::Result)
      return "the result of " + Of;
    Type *T = V->getType();
    if (V == V->getOwner()->getSelfVar())
      return T && (T->isClass() || T->isClassRef())
                 ? ""
                 : "through the Self of " + Of + " (a record, passed by reference)";
    bool Param = V->getVarKind() != VarDecl::Variable;
    if (!Param || !T)
      return "";
    if (T->isOpenArray() || T->isArrayOfConst() || T->isUntyped())
      return "an open or untyped parameter of " + Of;
    if (V->isOutParam())
      return "an 'out' parameter of " + Of;
    if (V->isVarParam())
      return "a 'var' parameter of " + Of;
    if ((V->isConstParam() || V->isConstRefParam()) &&
        (T->isStructured() || isManaged(T) || V->isConstRefParam()))
      return std::string("a '") + (V->isConstRefParam() ? "constref" : "const") +
             "' parameter of " + Of;
    return "";
  }

  //===--------------------------------------------------------------------===//
  // Tasks (objfpc): async routines, await, spawn, scope, detached
  //===--------------------------------------------------------------------===//

  /// Nesting of scope blocks around the code being checked (spawn needs
  /// one).
  unsigned ScopeDepth = 0;

  /// Is the code being checked in an async routine (or a lambda in one)?
  bool inAsyncRoutine() const {
    RoutineDecl *R = CurRoutine;
    while (R && R->isLambda() && !R->isAsync())
      R = R->getParent();
    return R && R->isAsync();
  }

  /// Is E (checked) the call of an async routine?
  static bool isAsyncCall(Expr *E) {
    Decl *D = nullptr;
    if (auto *C = llvm::dyn_cast<CallExpr>(E))
      D = C->getDecl();
    else if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
      D = N->getDecl();
    else if (auto *F = llvm::dyn_cast<FieldExpr>(E))
      D = F->getMember();
    else if (auto *I = llvm::dyn_cast<IndirectCallExpr>(E))
      if (auto *F = llvm::dyn_cast<FieldExpr>(I->getCallee()))
        D = F->getMember();
    auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
    return R && R->isAsync();
  }

  void warnDanglingTask(const Identifier &Callee, SourceRange Range) {
    Diags.warning(Callee.Loc,
                  "dangling task: '" + Callee.Spelling +
                      "' is never awaited; write 'await " + Callee.Spelling +
                      "' to wait for it, keep it in a variable to await "
                      "later, or 'detached " + Callee.Spelling +
                      "' to let it run on its own",
                  {Range});
  }

  /// await e: e a task; its result (NeedValue: one is needed).
  Type *checkAwait(AwaitExpr *E, bool NeedValue) {
    Type *T = check(E->getOperand());
    if (T->isError())
      return errorType();
    auto *TT = llvm::dyn_cast<TaskType>(T);
    if (!TT) {
      Diags.error(E->getBeginLoc(),
                  "'await' waits for a task (the call of an async routine), "
                  "not a value of type " + quoted(T),
                  {E->getRange()});
      return errorType();
    }
    if (!inAsyncRoutine())
      Diags.error(E->getBeginLoc(),
                  "'await' is allowed only in an async routine; elsewhere, "
                  "Wait(t) waits for a task",
                  {E->getRange()});
    if (!TT->getResult()) {
      if (NeedValue) {
        Diags.error(E->getBeginLoc(),
                    "this task has no value (its routine is 'async' without "
                    "a type)",
                    {E->getRange()});
        return errorType();
      }
      return T;
    }
    return TT->getResult();
  }

  Type *visitAwaitExpr(AwaitExpr *E) { return checkAwait(E, true); }

  /// The hidden async procedure of spawn begin ... end: declared and
  /// checked where it is spawned (it uses the variables declared so far).
  void completeSpawnBlock(RoutineDecl *R) {
    R->setLambdaDone();
    resolveSignature(R);
    unsigned SavedDepth = ScopeDepth;
    ScopeDepth = 0; // (its task has scopes of its own)
    declareAndCheckLambda(R);
    ScopeDepth = SavedDepth;
  }

  Type *visitSpawnExpr(SpawnExpr *E) {
    Expr *C = E->getCall();
    if (auto *CE = llvm::dyn_cast<CallExpr>(C))
      if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(CE->getDecl());
          R && R->isSpawnBlock() && !R->isLambdaDone())
        completeSpawnBlock(R);
    Type *T = check(C);
    if (T->isError())
      return errorType();
    const char *Word = E->isPoolRun() ? "Pool.Run" : "spawn";
    if (!T->isTask() || !isAsyncCall(C)) {
      Diags.error(C->getBeginLoc(),
                  llvm::Twine("'") + Word +
                      "' runs the call of an async routine, not a value of type " +
                      quoted(T),
                  {C->getRange()});
      return errorType();
    }
    if (!E->isPoolRun() && ScopeDepth == 0)
      Diags.error(E->getBeginLoc(),
                  "'spawn' is allowed only inside a 'scope ... end' block, "
                  "which waits for the tasks it starts; Pool.Run starts one "
                  "on its own",
                  {E->getRange()});
    return T;
  }

  void visitExprStmt(ExprStmt *S) {
    DirectSequence = nullptr;
    Expr *E = S->getExpr();
    Type *T;
    if (auto *A = llvm::dyn_cast<AwaitExpr>(E))
      T = checkAwait(A, /*NeedValue=*/false);
    else
      T = check(E);
    E->setType(T);
    if (auto *SE = llvm::dyn_cast<SpawnExpr>(E); SE && SE->isPoolRun() && !T->isError())
      Diags.warning(E->getBeginLoc(),
                    "dangling task: the task Pool.Run starts is never awaited; "
                    "keep it in a variable to await later",
                    {E->getRange()});
  }

  void visitScopeStmt(ScopeStmt *S) {
    DirectSequence = nullptr;
    if (Expr *Tok = S->getToken()) {
      Type *T = check(Tok);
      if (!T->isError() && !T->isToken() && !T->isNil())
        Diags.error(Tok->getBeginLoc(),
                    "the token of a scope must be a TCancelToken, not a value "
                    "of type " + quoted(T),
                    {Tok->getRange()});
    }
    markHasTry(); // its end waits however the block ends
    ++ScopeDepth;
    visitSequence(S, S->getBody());
    --ScopeDepth;
  }

  void visitDetachedStmt(DetachedStmt *S) {
    DirectSequence = nullptr;
    Expr *C = S->getCall();
    Type *T = check(C);
    if (!T->isError() && (!T->isTask() || !isAsyncCall(C)))
      Diags.error(C->getBeginLoc(),
                  "'detached' runs the call of an async routine, not a value "
                  "of type " + quoted(T),
                  {C->getRange()});
    if (Expr *H = S->getHandler()) {
      // procedure(E: Exception): a routine (or procedural value)
      auto *EParam = Ctx.create<VarDecl>(memberName("E"), nullptr,
                                         VarDecl::ValueParam);
      EParam->setType(ExceptionTy);
      auto *Formal = Ctx.create<RoutineType>(
          Ctx.copyArray<VarDecl *>({EParam}), nullptr);
      auto *P = Ctx.create<VarDecl>(memberName("handler"), nullptr,
                                    VarDecl::ValueParam);
      P->setType(Formal);
      checkRoutineArg(H, P, Formal);
      checkLambdaEscape(H, /*Local=*/false);
      if (!H->getType())
        H->setType(Formal);
    }
  }

  /// Wait(t) (also as a statement: NeedValue false), WhenAll(tasks),
  /// WhenAny(tasks).
  Type *checkWait(llvm::ArrayRef<Expr *> Args, const Identifier &Callee,
                  SourceRange Range, bool NeedValue) {
    if (Args.size() != 1) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 argument",
                  {Range});
      for (Expr *A : Args)
        check(A);
      return errorType();
    }
    Type *T = check(Args[0]);
    if (T->isError())
      return errorType();
    auto *TT = llvm::dyn_cast<TaskType>(T);
    if (!TT) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'Wait' waits for a task (the call of an async routine), "
                  "not a value of type " + quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    if (!TT->getResult()) {
      if (NeedValue) {
        Diags.error(Args[0]->getBeginLoc(),
                    "this task has no value (its routine is 'async' without "
                    "a type)",
                    {Args[0]->getRange()});
        return errorType();
      }
      return T;
    }
    return TT->getResult();
  }

  /// The tasks of WhenAll / WhenAny: an array of tasks (static, dynamic or
  /// open); their task type, or null after a diagnostic.
  TaskType *checkTaskArray(Expr *A, const Identifier &Callee) {
    Type *T = check(A);
    if (T->isError())
      return nullptr;
    Type *Elem = nullptr;
    if (auto *AT = llvm::dyn_cast<ArrayType>(T))
      Elem = AT->getElementType();
    else if (auto *DT = llvm::dyn_cast<DynArrayType>(T))
      Elem = DT->getElementType();
    else if (auto *OT = llvm::dyn_cast<OpenArrayType>(T))
      Elem = OT->getElementType();
    auto *TT = llvm::dyn_cast_or_null<TaskType>(Elem);
    if (!TT) {
      Diags.error(A->getBeginLoc(),
                  "'" + Callee.Spelling + "' takes an array of tasks, not a "
                  "value of type " + quoted(T),
                  {A->getRange()});
      return nullptr;
    }
    return TT;
  }

  /// A lambda that uses what belongs to the routines around it must not
  /// outlive them: reported if Value is one and is stored (Local: in a
  /// local variable of the current routine, which is allowed).
  void checkLambdaEscape(Expr *Value, bool Local) {
    auto *A = llvm::dyn_cast<AddrOfExpr>(Value);
    RoutineDecl *R = A ? A->getLambda() : nullptr;
    if (!R || !R->getCapture().isValid() || Local || R->isClosure())
      return;
    Diags.error(Value->getBeginLoc(),
                "this lambda uses '" + R->getCapture().Spelling +
                    "' of the routine it is written in, so it must not "
                    "outlive it: it can be passed as an argument or kept in "
                    "a local variable, not stored here",
                {Value->getRange()});
  }

  Type *visitAddrOfExpr(AddrOfExpr *E) {
    completeLambda(E, AddrOfTarget);
    if (E->getLambda() && E->getLambda()->isLambdaBad())
      return errorType();
    if (Type *T = checkMethodAddr(E))
      return T;
    // @routine (Turbo/FPC): a procedural value.
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E->getOperand()))
      if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(N->getName().Name))) {
        if (R->isOverloaded()) { // objfpc: the overload the target wants
          RoutineDecl *Pick = nullptr;
          for (RoutineDecl *X = R; AddrOfTarget && X; X = X->getNextOverload())
            if (!Pick && X->isCDecl() == AddrOfTarget->isCDecl() &&
                congruent(AddrOfTarget->getParams(),
                          AddrOfTarget->getResult(), X->getParams(),
                          X->isFunction() ? X->getResultType() : nullptr))
              Pick = X;
          if (!Pick) {
            Diags.error(N->getBeginLoc(),
                        "'@" + N->getName().Spelling +
                            "': '" + N->getName().Spelling +
                            "' is overloaded; assign it to a procedural "
                            "variable of the heading wanted",
                        {N->getRange()});
            return errorType();
          }
          R = Pick;
        }
        N->setDecl(R);
        if (R->isMethod()) {
          Diags.error(N->getBeginLoc(),
                      "'@" + N->getName().Spelling +
                          "': methods cannot be procedural values",
                      {N->getRange()});
          return errorType();
        }
        if (std::string Why = notProceduralValue(R, false); !Why.empty()) {
          Diags.error(N->getBeginLoc(), "'@" + N->getName().Spelling + "': " + Why,
                      {N->getRange()});
          return errorType();
        }
        // A closure wanted: a lambda (made one by completeLambda) or a
        // routine declared at program or unit level (no frame to keep).
        // A method pointer: a lambda made one by completeLambda.
        bool Reference = AddrOfTarget && AddrOfTarget->isReference();
        auto *T = Ctx.create<RoutineType>(
            R->getParams(), R->isFunction() ? R->getResultType() : nullptr,
            R->isCDecl(), R->isMethodLambda(), Reference);
        N->setType(T);
        return T;
      }
    Type *T = checkAllowingUntyped(E->getOperand()); // (@x of an untyped x too)
    if (T->isError())
      return errorType();
    if (isBitField(E->getOperand())) {
      diagBitFieldAddress(E->getOperand());
      return errorType();
    }
    if (!isVariableReference(E->getOperand())) {
      Diags.error(E->getOperand()->getBeginLoc(),
                  "'@' requires a variable", {E->getOperand()->getRange()});
      return errorType();
    }
    if (VarDecl *OV = ownedVarOf(E->getOperand())) {
      Diags.error(E->getOperand()->getBeginLoc(),
                  "cannot take the address of owned variable '" +
                      OV->getName().Spelling + "'",
                  {E->getOperand()->getRange()});
      return errorType();
    }
    return Ctx.getUntypedPointerType();
  }

  Type *visitDerefExpr(DerefExpr *E) {
    Type *T = check(E->getBase());
    if (T->isError())
      return errorType();
    if (auto *P = llvm::dyn_cast<PointerType>(T)) {
      if (P->isUntyped()) {
        Diags.error(E->getBase()->getBeginLoc(),
                    "cannot dereference an untyped pointer",
                    {E->getBase()->getRange()});
        return errorType();
      }
      return P->getPointee() ? P->getPointee() : errorType();
    }
    if (auto *F = llvm::dyn_cast<FileType>(T); F && !F->isUntyped())
      return F->getElementType(); // the buffer variable f^
    if (T->isFile()) {
      Diags.error(E->getBase()->getBeginLoc(),
                  "an untyped file has no buffer variable",
                  {E->getBase()->getRange()});
      return errorType();
    }
    Diags.error(E->getBase()->getBeginLoc(),
                "'^' applied to a value of type " + quoted(T) +
                    ", which is not a pointer",
                {E->getBase()->getRange()});
    return errorType();
  }

  Type *visitSetCtorExpr(SetCtorExpr *E) {
    Type *Base = nullptr;
    bool OK = true;
    for (const SetElement &El : E->getElements()) {
      for (Expr *X : {El.Lo, El.Hi}) {
        if (!X)
          continue;
        Type *T = check(X);
        if (T->isError()) {
          OK = false;
          continue;
        }
        if (!T->isOrdinal()) {
          Diags.error(X->getBeginLoc(),
                      "set elements must be of an ordinal type, not " +
                          quoted(T),
                      {X->getRange()});
          OK = false;
          continue;
        }
        if (!Base) {
          Base = T->getBase();
        } else if (!haveSameOrdinalHost(T, Base)) {
          Diags.error(X->getBeginLoc(),
                      "set element of type " + quoted(T) +
                          " does not match earlier elements of type " +
                          quoted(Base),
                      {X->getRange()});
          OK = false;
          continue;
        }
        if (auto C = fold(X); C && (C->Value.Int < 0 || C->Value.Int > 255)) {
          Diags.error(X->getBeginLoc(),
                      "set element " + formatValue(T, C->Value.Int) +
                          " is outside 0..255",
                      {X->getRange()});
          OK = false;
        }
      }
    }
    return OK ? Ctx.getSetConstructorType(Base) : errorType();
  }

  /// TFoo in TFoo.Create (objfpc): a class named before '.'.
  /// The class Base names (TFoo.Create), or for an advanced record name
  /// (TR.Create) its member table (Base's type: the record).
  ClassType *classNamedBy(Expr *Base) {
    if (!LangOpts.ObjKeywords)
      return nullptr;
    TypeDecl *TD = typeNamedBy(Base);
    auto *C = TD ? llvm::dyn_cast_or_null<ClassType>(TD->getType()) : nullptr;
    if (C && C->isHelper()) // (not a type: its methods are used on values)
      return nullptr;
    if (!C && TD)
      C = memberTableOf(TD->getType());
    if (C) {
      if (auto *N = llvm::dyn_cast<NameRefExpr>(Base))
        N->setDecl(TD);
      Base->setType(C->getRecord() ? static_cast<Type *>(C->getRecord()) : C);
    }
    return C;
  }

  /// The type that Base names (TFoo, or a nested TOuter.TInner), or null.
  TypeDecl *typeNamedBy(Expr *Base) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Base))
      return llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(N->getName().Name));
    auto *F = llvm::dyn_cast<FieldExpr>(Base);
    if (!F || !LangOpts.ObjKeywords)
      return nullptr;
    ClassType *Outer = classNamedBy(F->getBase());
    if (!Outer)
      return nullptr;
    const ClassType *Owner = nullptr;
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
        Outer->findMember(F->getField().Name, &Owner));
    if (TD && F->getMember() != TD) {
      checkMemberAccess(TD, Owner, F->getField());
      F->setMember(TD);
      F->setType(TD->getType() ? TD->getType() : errorType());
    }
    return TD;
  }

  /// TColor.Red: the value of the enumeration type E's base names (any
  /// enumeration's; the only way to a scoped one's, {$scopedenums on}).
  /// Null if the base does not name an enumeration type (or names one with
  /// a helper, and not one of its values: TColor.Helped), or after a
  /// diagnostic (Reported).
  Decl *enumValueNamedBy(FieldExpr *E, bool &Reported) {
    TypeDecl *TD = typeNamedBy(E->getBase());
    auto *ET = TD ? llvm::dyn_cast_or_null<EnumType>(TD->getType()) : nullptr;
    if (!ET)
      return nullptr;
    const Identifier &F = E->getField();
    for (ConstDecl *V : ET->getValues())
      if (V->getName().Name == F.Name) {
        if (auto *N = llvm::dyn_cast<NameRefExpr>(E->getBase())) {
          N->setDecl(TD);
          warnHints(TD, N->getName());
        }
        E->getBase()->setType(ET);
        return V;
      }
    if (helperFor(ET))
      return nullptr; // a member of its helper
    Diags.error(F.Loc,
                "no value named '" + F.Spelling + "' in enumeration " +
                    quoted(ET),
                {F.getRange()});
    Reported = true;
    return nullptr;
  }

  /// AnsiString.Join: a static method or constant of the helper H of the
  /// type E's base names.
  Decl *resolveHelperStatic(FieldExpr *E, ClassType *H) {
    const Identifier &F = E->getField();
    const ClassType *Owner = nullptr;
    Decl *D = H->findMember(F.Name, &Owner);
    if (!D) {
      Diags.error(F.Loc,
                  "no member named '" + F.Spelling + "' in " +
                      H->helperKindName() + " " + quoted(H),
                  {F.getRange()});
      return nullptr;
    }
    checkMemberAccess(D, Owner, F);
    auto *R = llvm::dyn_cast<RoutineDecl>(D);
    if (!llvm::isa<ConstDecl>(D) && !(R && R->isStatic()) &&
        !llvm::isa<VarDecl>(D)) {
      Diags.error(F.Loc,
                  "only static class methods, class variables and constants "
                  "of a " +
                      H->helperKindName() +
                      " can be used through a type name; '" +
                      F.Spelling + "' is a " + D->getKindName(),
                  {F.getRange()});
      return nullptr;
    }
    E->setMember(D);
    return D;
  }

  /// Resolves r.f / obj.member / TFoo.Create; null after a diagnostic.
  Decl *resolveMember(FieldExpr *E) {
    const Identifier &F = E->getField();
    ClassType *ViaClass = classNamedBy(E->getBase());
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E->getBase()); ViaClass && N)
      warnHints(N->getDecl(), N->getName()); // TFoo.Create: a use of TFoo
    if (!ViaClass) {
      bool Reported = false;
      if (Decl *V = enumValueNamedBy(E, Reported)) // TColor.Red
        return V;
      if (Reported)
        return nullptr;
      if (ClassType *H = helperNamedBy(E->getBase())) // AnsiString.Join
        return resolveHelperStatic(E, H);
      if (TypeDecl *TD = typeNamedBy(E->getBase())) // TH.Join
        if (auto *H = llvm::dyn_cast_or_null<ClassType>(TD->getType());
            H && H->isHelper()) {
          Diags.error(E->getBase()->getBeginLoc(),
                      "the members of " + H->helperKindName() + " " +
                          quoted(H) +
                          " are used through " + quoted(H->getHelperFor()),
                      {E->getBase()->getRange()});
          return nullptr;
        }
    }
    Type *BT = ViaClass ? ViaClass : check(E->getBase());
    if (BT->isError())
      return nullptr;
    if (!ViaClass && !BT->isClass() && !BT->isClassRef() && !BT->isInterface())
      if (ClassType *H = helperFor(BT)) { // a type helper's member
        const ClassType *Owner = nullptr;
        Decl *D = E->getTypeArgs().empty() ? H->findMember(F.Name, &Owner) : nullptr;
        if (!D) // a generic method (see the classes' below)
          D = genericMethodOf(E, H, Owner);
        if (D) {
          checkMemberAccess(D, Owner, F);
          E->setMember(D);
          // Self is by reference: a temporary for a value
          E->setHelperValue(!isHelperVariable(E->getBase()) ||
                            constParamRoot(E->getBase()));
          return D;
        }
      }
    if (ClassType *MT = memberTableOf(BT)) // an advanced record's member
      BT = MT;
    if (auto *RT = llvm::dyn_cast<RecordType>(BT)) {
      FieldDecl *FD = RT->findField(F.Name);
      if (!FD) {
        Diags.error(F.Loc,
                    "no field named '" + F.Spelling + "' in " + quoted(RT),
                    {F.getRange()});
        return nullptr;
      }
      E->setMember(FD);
      return FD;
    }
    if (auto *I = llvm::dyn_cast<InterfaceType>(BT)) { // s.M, s: an interface
      RoutineDecl *R = findInterfaceMethod(I, F.Name);
      if (!R) {
        Diags.error(F.Loc,
                    "no method named '" + F.Spelling + "' in interface " +
                        quoted(I),
                    {F.getRange()});
        return nullptr;
      }
      E->setMember(R);
      return R;
    }
    auto *C = llvm::dyn_cast<ClassType>(BT);
    auto *ViaRef = llvm::dyn_cast<ClassRefType>(BT); // c.M, c: class of T
    if (ViaRef)
      C = ViaRef->getClass();
    if (!C) {
      Diags.error(F.Loc,
                  "'.' applied to a value of type " + quoted(BT) +
                      ", which is not a record",
                  {E->getBase()->getRange()});
      return nullptr;
    }
    const ClassType *Owner = nullptr;
    Decl *D = E->getTypeArgs().empty() ? C->findMember(F.Name, &Owner) : nullptr;
    const char *What = C->getRecord() ? "record" : "class";
    if (!D) { // a generic method: specialized here, or by the call (inferred)
      if (!E->getTypeArgs().empty()) {
        D = C->findMember(genericKey(F.Name, E->getTypeArgs().size()), &Owner);
        if (!D) {
          Diags.error(F.Loc,
                      "no generic method '" + F.Spelling + "' with " +
                          llvm::Twine(E->getTypeArgs().size()) +
                          " type parameter" +
                          (E->getTypeArgs().size() == 1 ? "" : "s") + " in " +
                          What + " " + quoted(C),
                      {F.getRange()});
          return nullptr;
        }
      } else if (auto Gs = genericMethodsNamed(C, F.Name); !Gs.empty()) {
        D = Gs[0];
        C->findMember(genericKey(F.Name, Gs[0]->getTypeParams().size()), &Owner);
      }
    }
    if (!D) {
      Diags.error(F.Loc,
                  "no member named '" + F.Spelling + "' in " + What + " " +
                      quoted(C) + didYouMeanMember(C, F.Name),
                  {F.getRange()});
      return nullptr;
    }
    checkMemberAccess(D, Owner, F);
    if (ViaClass || ViaRef) {
      // Through a class: constructors, class methods, class variables and
      // class properties.
      bool OK = llvm::isa<VarDecl>(D) || llvm::isa<ConstDecl>(D) ||
                llvm::isa<TypeDecl>(D);
      if (auto *R = llvm::dyn_cast<RoutineDecl>(D))
        OK = (R->isConstructor() || R->isClassMethod()) && !R->isOperator();
      if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
        OK = P->isClassProperty();
      if (!OK && !(llvm::isa<RoutineDecl>(D) &&
                   llvm::cast<RoutineDecl>(D)->isGenericMethod() &&
                   llvm::cast<RoutineDecl>(D)->isClassMethod())) {
        Diags.error(F.Loc,
                    "only constructors, class methods, class variables and "
                    "class properties can be used through " +
                        (ViaClass ? std::string("the ") + What + " name '" +
                                        C->getName() + "'"
                                  : std::string("a class reference")) +
                        "; '" + F.Spelling + "' is a " + D->getKindName(),
                    {F.getRange()});
        return nullptr;
      }
    }
    // obj.specialize M<T>: the specialization
    if (auto *G = llvm::dyn_cast<RoutineDecl>(D);
        G && G->isGenericMethod() && !E->getTypeArgs().empty()) {
      llvm::SmallVector<Type *, 2> Types;
      for (TypeSpec *TS : E->getTypeArgs()) {
        Type *T = resolveType(TS);
        if (T->isError())
          return nullptr;
        Types.push_back(T);
      }
      D = instantiateMethod(F, G, Types);
      if (!D)
        return nullptr;
    }
    E->setMember(D);
    return D;
  }

  /// E (obj.M, obj.specialize M<T>): a generic method of C or its
  /// ancestors, specialized for E's type arguments if it gives them; null
  /// if there is none (or after a diagnostic, Reported).
  Decl *genericMethodOf(FieldExpr *E, ClassType *C, const ClassType *&Owner) {
    const Identifier &F = E->getField();
    if (!E->getTypeArgs().empty()) {
      auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
          C->findMember(genericKey(F.Name, E->getTypeArgs().size()), &Owner));
      if (!G)
        return nullptr;
      llvm::SmallVector<Type *, 2> Types;
      for (TypeSpec *TS : E->getTypeArgs()) {
        Type *T = resolveType(TS);
        if (T->isError())
          return nullptr;
        Types.push_back(T);
      }
      return instantiateMethod(F, G, Types);
    }
    auto Gs = genericMethodsNamed(C, F.Name);
    if (Gs.empty())
      return nullptr;
    C->findMember(genericKey(F.Name, Gs[0]->getTypeParams().size()), &Owner);
    return Gs[0];
  }

  /// "; did you mean 'X'?" for a member name of C: the closest member of
  /// C or its ancestors (ties: the alphabetically first).
  std::string didYouMeanMember(ClassType *C, llvm::StringRef Name) {
    unsigned Limit = Name.size() <= 3 ? 1 : std::max<unsigned>(2, Name.size() / 3);
    unsigned Best = Limit + 1;
    llvm::StringRef BestName, BestSpelling;
    for (const ClassType *X = C; X; X = X->getParent())
      for (Decl *D : X->getMembers()) {
        llvm::StringRef Cand = D->getName().Name;
        if (Cand == Name)
          continue;
        unsigned Dist = Name.edit_distance(Cand, true, Limit);
        if (Dist < Best || (Dist == Best && Dist <= Limit && Cand < BestName)) {
          Best = Dist;
          BestName = Cand;
          BestSpelling = D->getName().Spelling;
        }
      }
    return BestName.empty() ? "" : "; did you mean '" + BestSpelling.str() + "'?";
  }

  /// A property used as a value: it needs a read accessor.
  Type *checkPropertyRead(PropertyDecl *P, Expr *E, const Identifier &Id) {
    if (P->isArrayProperty() && PendingIndexes.count(E) &&
        !llvm::isa<IndexExpr>(E))
      return P->getType(); // checked with its indexes
    if (E == CurAssignTarget || P->getRead() || P->getType()->isError() ||
        P->getReadName().isValid())
      return P->getType();
    Diags.error(Id.Loc, "property '" + Id.Spelling + "' is write-only",
                {Id.getRange()});
    noteDeclaredHere(P);
    return errorType();
  }

  /// p.Free or p.Destroy of an owned variable p (already resolved): an
  /// error, since its object is freed again when the routine ends.
  bool diagOwnedFree(FieldExpr *E) {
    const Identifier &F = E->getField();
    VarDecl *OV = ownedVarOf(E->getBase());
    if (!OV || (F.Name != "free" && F.Name != "destroy"))
      return false;
    Diags.error(F.Loc,
                "'" + F.Spelling + "' cannot be called on owned variable '" +
                    OV->getName().Spelling +
                    "', which is freed when its routine ends; assign nil to "
                    "free its object now",
                {E->getRange()});
    return true;
  }

  Type *visitFieldExpr(FieldExpr *E) {
    Decl *D = resolveMember(E);
    if (!D)
      return errorType();
    const Identifier &F = E->getField();
    if (diagOwnedFree(E))
      return errorType();
    if (auto *CD = llvm::dyn_cast<ConstDecl>(D)) { // TFoo.C, obj.C
      warnHints(CD, F);
      Identifier Id = CD->getName();
      Id.Loc = F.Loc;
      auto *Ref = Ctx.create<NameRefExpr>(Id);
      Ref->setDecl(CD);
      Ref->setType(CD->getType() ? CD->getType() : errorType());
      Used.insert(CD);
      E->setConstRef(Ref);
      return Ref->getType();
    }
    if (llvm::isa<TypeDecl>(D)) {
      Diags.error(F.Loc, "'" + F.Spelling + "' is a type, not a value",
                  {F.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }
    if (!llvm::isa<RoutineDecl>(D)) // (a method: once its overload is known)
      warnHints(D, F);
    if (auto *FD = llvm::dyn_cast<FieldDecl>(D))
      return FD->getType() ? FD->getType() : errorType();
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D)) {
      if (!checkArrayPropertyUse(P, E, F))
        return errorType();
      return checkPropertyRead(P, E, F);
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(D)) // a class variable
      return V->getType() ? V->getType() : errorType();
    // A method used as a value: a call without arguments.
    if (auto *G = llvm::cast<RoutineDecl>(D); G->isGenericMethod()) {
      Diags.error(F.Loc,
                  "cannot infer the type parameters of generic method '" +
                      F.Spelling + "' without arguments; give them: "
                      "specialize " + F.Spelling + "<...>",
                  {F.getRange()});
      noteDeclaredHere(G);
      return errorType();
    }
    auto *R = resolveOverload(llvm::cast<RoutineDecl>(D), {}, F, F.getRange());
    if (!R)
      return errorType();
    E->setMember(R);
    warnHints(R, F);
    if (!acceptsArgCount(R->getParams(), 0)) {
      Diags.error(F.Loc, "missing arguments in call to '" + F.Spelling + "'",
                  {F.getRange()});
      noteDeclaredHere(R);
      return errorType();
    }
    if (Type *T = methodResultType(R, E))
      return T;
    Diags.error(F.Loc,
                llvm::Twine(R->getKindName()) + " '" + F.Spelling +
                    "' does not return a value",
                {F.getRange()});
    return errorType();
  }

  /// The value of calling method R through E (obj.M or TFoo.Create): a
  /// function's result, or the new object of a constructor called through
  /// its class; null if the call has no value.
  static Type *methodResultType(RoutineDecl *R, FieldExpr *E) {
    if (R->isAsync())
      return R->getAsyncType(); // its task
    if (R->isFunction())
      return R->getResultType();
    if (R->isConstructor()) {
      if (namesType(E->getBase())) // TFoo.Create, TOuter.TInner.Create
        return E->getBase()->getType();
      // c.Create, c: class of T: an object of class T (or a descendant)
      if (auto *CR = llvm::dyn_cast_or_null<ClassRefType>(E->getBase()->getType()))
        return CR->getClass();
    }
    return nullptr;
  }

  /// Does E name a type (TFoo, TOuter.TInner), as resolved?
  static bool namesType(Expr *E) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
      return llvm::isa_and_nonnull<TypeDecl>(N->getDecl());
    auto *F = llvm::dyn_cast<FieldExpr>(E);
    return F && llvm::isa_and_nonnull<TypeDecl>(F->getMember());
  }

  /// Is Callee obj.M / TFoo.Create / inherited M -- a method call rather
  /// than a call through a procedural value?
  bool isMethodCallee(Expr *Callee) {
    if (llvm::isa<InheritedExpr>(Callee))
      return true;
    auto *F = llvm::dyn_cast<FieldExpr>(Callee);
    if (!F || !LangOpts.ObjKeywords)
      return false;
    if (classNamedBy(F->getBase()))
      return true;
    if (ClassType *H = helperNamedBy(F->getBase()))
      return llvm::isa_and_nonnull<RoutineDecl>(H->findMember(F->getField().Name)) ||
             !genericMethodsNamed(H, F->getField().Name).empty();
    Type *BT = check(F->getBase());
    if (!BT->isClass() && !BT->isClassRef() && !BT->isInterface())
      if (ClassType *H = helperFor(BT);
          H && (llvm::isa_and_nonnull<RoutineDecl>(H->findMember(F->getField().Name)) ||
                !genericMethodsNamed(H, F->getField().Name).empty()))
        return true;
    if (auto *I = llvm::dyn_cast<InterfaceType>(BT))
      return findInterfaceMethod(I, F->getField().Name) != nullptr;
    if (ClassType *MT = memberTableOf(BT)) // an advanced record's method
      BT = MT;
    auto *C = llvm::dyn_cast<ClassType>(BT);
    if (auto *CR = llvm::dyn_cast<ClassRefType>(BT))
      C = CR->getClass();
    return C && (llvm::isa_and_nonnull<RoutineDecl>(C->findMember(F->getField().Name)) ||
                 !genericMethodsNamed(C, F->getField().Name).empty());
  }

  /// obj.M(args), TFoo.Create(args), inherited M(args): the result type,
  /// null for a call without a value, or the error type.
  Type *checkMethodCall(IndirectCallExpr *E) {
    if (auto *I = llvm::dyn_cast<InheritedExpr>(E->getCallee()))
      return checkInheritedCall(I, E, E->getRange());
    auto *F = llvm::cast<FieldExpr>(E->getCallee());
    Decl *D = resolveMember(F);
    if (D && diagOwnedFree(F))
      return errorType();
    auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
    if (R && R->isGenericMethod()) { // obj.M(args): the types from the args
      llvm::SmallVector<RoutineDecl *, 2> Gs =
          genericMethodsNamed(R->getClass(), F->getField().Name);
      R = inferFor(F->getField(), Gs, E->getArgs(), nullptr);
      if (!R) {
        for (Expr *A : E->getArgs())
          check(A);
        return errorType();
      }
      F->setMember(R);
    } else if (R) { // objfpc: overloads
      R = resolveOverload(R, E->getArgs(), F->getField(), E->getRange());
      if (!R)
        return errorType();
      F->setMember(R);
      warnHints(R, F->getField());
    }
    if (!R) {
      const Identifier &Id = F->getField();
      if (llvm::isa_and_nonnull<TypeDecl>(D))
        Diags.error(Id.Loc,
                    "typecasts through a qualified type name are not supported "
                    "yet; declare a type alias for '" + Id.Spelling + "'",
                    {Id.getRange()});
      else if (D)
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' is a " + D->getKindName() +
                        ", not a method",
                    {Id.getRange()});
      for (Expr *A : E->getArgs())
        check(A);
      return errorType();
    }
    checkCallArgs(R, F->getField(),
                  orderedArgs(E, R->getParams(), F->getField()), E->getRange());
    Type *T = methodResultType(R, F);
    if (T)
      F->setType(T);
    return T;
  }

  /// inherited [Name]: the inherited method, or null (after a diagnostic,
  /// or with Nothing set for a bare `inherited` without an inherited method,
  /// which does nothing).
  RoutineDecl *resolveInherited(InheritedExpr *I, bool &Nothing) {
    Nothing = false;
    RoutineDecl *Encl = enclosingMethod();
    if (!Encl) {
      Diags.error(I->getBeginLoc(), "'inherited' is only allowed in a method",
                  {I->getRange()});
      return nullptr;
    }
    I->setEnclosing(Encl);
    if (Encl != CurRoutine) {
      // From a routine nested in the method: the call takes the method's
      // Self, and a bare `inherited` its parameters.
      if (VarDecl *Self = Encl->getSelfVar())
        Self->setCaptured();
      for (VarDecl *P : Encl->getParams())
        P->setCaptured();
    }
    ClassType *Parent = Encl->getClass()->getParent();
    const Identifier &Name = I->isBare() ? Encl->getName() : I->getName();
    const ClassType *Owner = nullptr;
    Decl *D = Parent ? Parent->findMember(Name.Name, &Owner) : nullptr;
    auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
    if (!R) {
      if (I->isBare() && !D) {
        Nothing = true;
        return nullptr;
      }
      SourceLocation L = I->isBare() ? I->getBeginLoc() : Name.Loc;
      std::string In = Parent ? Parent->getName() : std::string("TObject");
      if (D) {
        Diags.error(L,
                    "'inherited' needs a method, but '" + Name.Spelling +
                        "' in '" + Owner->getName() + "' is a " +
                        D->getKindName(),
                    {I->getRange()});
        noteDeclaredHere(D);
      } else {
        Diags.error(L,
                    "no inherited method '" + Name.Spelling + "' in '" + In + "'",
                    {I->getRange()});
      }
      return nullptr;
    }
    checkMemberAccess(R, Owner, Name);
    if (R->isAbstract()) {
      Diags.error(I->getBeginLoc(),
                  "cannot call abstract method '" + Name.Spelling + "'",
                  {I->getRange()});
      noteDeclaredHere(R);
      return nullptr;
    }
    I->setMethod(R);
    return R;
  }

  /// inherited [Name] [(Args)] called: the result type or null.
  Type *checkInheritedCall(InheritedExpr *I, IndirectCallExpr *Call,
                           SourceRange Range) {
    llvm::ArrayRef<Expr *> Args = Call->getArgs();
    bool Nothing;
    RoutineDecl *R = resolveInherited(I, Nothing);
    if (R) { // objfpc: overloads; a bare inherited means the same heading
      llvm::SmallVector<RoutineDecl *, 4> Cands;
      collectOverloads(R, Cands);
      if (Cands.size() > 1 && I->isBare() && Args.empty()) {
        RoutineDecl *Same = nullptr;
        for (RoutineDecl *X : Cands)
          if (!Same && sameSignature(X, I->getEnclosing()))
            Same = X;
        if (Same)
          R = Same;
      } else if (Cands.size() > 1) {
        R = resolveOverload(R, Args, I->isBare() ? R->getName() : I->getName(),
                            Range);
        if (!R)
          return errorType();
      }
      I->setMethod(R);
    }
    if (!R) {
      for (Expr *A : Args)
        check(argValue(A));
      return Nothing ? nullptr : errorType();
    }
    Identifier Name = I->isBare() ? R->getName() : I->getName();
    if (I->isBare() && Args.empty()) {
      // The enclosing method's parameters, passed on unchanged.
      llvm::SmallVector<Expr *, 4> Implicit;
      for (VarDecl *P : I->getEnclosing()->getParams()) {
        Identifier PN = P->getName();
        PN.Loc = I->getBeginLoc();
        auto *N = Ctx.create<NameRefExpr>(PN);
        N->setDecl(P);
        N->setType(P->getType() ? P->getType() : errorType());
        Used.insert(P);
        Implicit.push_back(N);
      }
      I->setImplicitArgs(Ctx.copyArray<Expr *>(Implicit));
      Name.Loc = I->getBeginLoc();
      checkCallArgs(R, Name, I->getImplicitArgs(), Range);
    } else {
      checkCallArgs(R, Name, orderedArgs(Call, R->getParams(), Name), Range);
    }
    Type *T = R->isFunction() ? R->getResultType() : nullptr;
    if (T)
      I->setType(T);
    return T;
  }

  /// inherited Name used as a value: a function without arguments.
  Type *visitInheritedExpr(InheritedExpr *E) {
    if (E->isBare()) {
      Diags.error(E->getBeginLoc(),
                  "expected a method name after 'inherited' in an expression",
                  {E->getRange()});
      return errorType();
    }
    bool Nothing;
    RoutineDecl *R = resolveInherited(E, Nothing);
    if (!R)
      return errorType();
    const Identifier &Id = E->getName();
    if (!acceptsArgCount(R->getParams(), 0)) {
      Diags.error(Id.Loc, "missing arguments in call to '" + Id.Spelling + "'",
                  {Id.getRange()});
      noteDeclaredHere(R);
      return errorType();
    }
    if (!R->isFunction()) {
      Diags.error(Id.Loc,
                  llvm::Twine(R->getKindName()) + " '" + Id.Spelling +
                      "' does not return a value",
                  {Id.getRange()});
      return errorType();
    }
    return R->getResultType();
  }
};

} // namespace

bool Sema::check(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                 bool SeparateUnits) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker(Ctx, Diags, LangOpts, Reparse, ReparseType)
      .checkProgram(P, Units, SeparateUnits);
  return Diags.getNumErrors() == ErrorsBefore && !Diags.shouldStop();
}

std::vector<std::string> Sema::standardNames() {
  return Checker(Ctx, Diags, LangOpts).standardNames();
}

bool Sema::checkUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker C(Ctx, Diags, LangOpts, Reparse, ReparseType);
  C.checkSeparateUnit(U, Deps);
  return Diags.getNumErrors() == ErrorsBefore && !Diags.shouldStop();
}
