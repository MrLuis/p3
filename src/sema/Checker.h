// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- Checker.h - Internal Sema Checker declaration ------------*- C++ -*-===//
//
// Shared declaration of p3::detail::Checker across the Sema translation units.
//
//===----------------------------------------------------------------------===//
#ifndef P3_SEMA_CHECKER_H
#define P3_SEMA_CHECKER_H

#include "p3/Sema/Sema.h"
#include "p3/AST/ASTContext.h"
#include "p3/AST/ASTVisitor.h"
#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/Directives.h"
#include "p3/Basic/LangOptions.h"
#include "p3/Sema/Scope.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <bitset>
#include <cmath>
#include <map>
#include <memory>
#include <optional>

namespace p3::detail {

/// Predeclared names from PLAN.md 4.5 that later milestones implement. Using
/// one gives "not supported yet" instead of "undeclared identifier".
inline bool isFutureBuiltin(llvm::StringRef Name) {
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
  const CheckDirectives *Directives;

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
    /// The instance whose body asked for it (an index into
    /// PendingInstances), -1 if none: an error in it notes that one's use
    /// too, so it leads back to the code outside the generics.
    int Parent = CheckingInstance;
  };
  /// The pending instance whose body is being checked, or -1.
  static inline int CheckingInstance = -1;
  std::vector<PendingInstance> PendingInstances;
  std::vector<std::unique_ptr<Scope>> InstanceScopes;
  llvm::SmallVector<Decl *, 4> ModuleInstances;
  /// `class of C` for each class C: the type of C used as a value, and of
  /// Self in class methods (`TFooClass = class of TFoo` is a type of its own).
  llvm::DenseMap<ClassType *, ClassRefType *> ClassRefs;

  ClassRefType *classRefOf(ClassType *C);
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
    /// The owned variables, and (null safety) the references followed:
    /// for those, Valid means "may be non-nil" and Moved "may be nil".
    Decl *Vars[MaxOwned];
    bool Null[MaxOwned] = {};
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
  static VarDecl *ownedVarOf(Expr *E);

  int ownedIndex(VarDecl *V);

  OwnedState ownedGet() { return Flow ? Flow->S : OwnedState(); }
  void ownedSet(const OwnedState &S);
  void ownedJoin(const OwnedState &S);
  /// What follows is not reached (after exit, break, continue, raise).
  void ownedDead();

  /// A use of the owned variable V: an error if it may have been moved.
  void ownedUse(VarDecl *V, SourceRange R);

  void ownedChanged(int I);

  /// V := object.
  void ownedAssign(VarDecl *V);

  /// move V.
  void ownedMove(VarDecl *V, SourceLocation Loc);

  /// V, an owned variable of the block being checked, is declared.
  void ownedDeclare(VarDecl *V);

  void ownedLoopBegin(OwnedLoop &L);

  /// The loop L ends: Before is the state in which it runs no (more)
  /// iterations. A variable that an iteration may leave moved, and that an
  /// iteration uses as it finds it, is used after a move.
  void ownedLoopEnd(OwnedLoop &L, const OwnedState &Before);

  void ownedBreak(bool IsContinue);

  void ownedTryBegin(OwnedTry &T);
  void ownedTryEnd(OwnedTry &T);

  //===--------------------------------------------------------------------===//
  // Null safety (objfpc, {$nullsafe on})
  //===--------------------------------------------------------------------===//
  //
  // In a block whose statements start where {$nullsafe on}, a reference of a
  // class, interface or class-reference type written there without `?` is
  // never nil: what is assigned or passed to it, and the object whose
  // member is used, must not be nil. A value may be nil when it is nil, a
  // variable, field or function result declared T?, a `?.`, or a followed
  // variable that the flow of the statements does not show to be non-nil:
  // the block's T? variables and parameters (narrowed by `x <> nil`,
  // Assigned(x), `x is T` and assignments), its non-null variables until
  // they are first assigned, a function's result and a constructor's own
  // non-null fields (which must be assigned by its end). Declarations
  // written outside null-safe code are trusted: their values are not nil,
  // and nil may be given to them. They are followed in the owned
  // variables' flow (OwnedFlow), with Valid "may be non-nil" and Moved "may
  // be nil".
  bool NullSafeHere = false;

  static bool isNullRef(Type *T);

  /// The type written for D (a variable, field, property or function's
  /// result), if any.
  static TypeSpec *nullSpec(Decl *D);
  /// Is D declared T? (its value may be nil)?
  static bool declNullable(Decl *D);
  /// Is D a reference that must not be given nil (written without `?` in
  /// null-safe code; not a lambda's result, whose type is not written)?
  static bool declNonNull(Decl *D);

  int nullIndex(Decl *D);

  /// D is followed from here (if there is room: the owned variables keep
  /// some), with these states.
  void nullTrack(Decl *D, bool MayNil, bool MayNonNil);

  void nullSet(unsigned I, bool MayNil, bool MayNonNil);

  /// D is not nil here (a condition said so).
  void nullNarrow(Decl *D);

  /// A use of followed D that needs it not nil: may it be nil? (Once
  /// reported, a non-null variable not assigned yet counts as assigned.)
  bool nullUse(unsigned I, SourceLocation Loc, bool Once);

  /// The declaration a reference to a followed or declared value is to:
  /// a variable or routine (x), a field of Self (F, Self.F), another
  /// member (o.F); null otherwise.
  static Decl *nullDeclOf(Expr *E);
  /// E is F or Self.F (in a method).
  static bool isSelfMember(Expr *E);

  /// The followed declaration E refers to, or -1.
  int nullIndexOf(Expr *E);

  enum class Nilness { NotNil, MaybeNil, IsNil };

  /// May E (checked) be nil here? (A followed reference: as the flow has
  /// it, without reporting.)
  Nilness nilness(Expr *E);

  /// How E is named in a diagnostic.
  static std::string nullDesc(Expr *E);

  /// Where E must not be nil (Why: what needs it, "but ..."; Alt: another
  /// way out, ", or ..."): an error if it may be.
  void requireNotNil(Expr *E, const std::string &Why,
                     const std::string &Alt = "");

  /// Target := Value (or the initialization of a variable): Value must not
  /// be nil if Target is a non-null reference; a followed Target gets its
  /// nil-ness.
  void nullAssign(Expr *Target, Decl *TD, Expr *Value);

  static Type *nullTypeOf(Decl *D);

  /// The followed declarations a condition shows to be non-nil when it is
  /// true (WhenTrue) or false.
  void nullFacts(Expr *C, bool WhenTrue, llvm::SmallVectorImpl<Decl *> &Out);

  void nullNarrowBy(Expr *C, bool WhenTrue);

  /// Where the routine being checked ends (its end, or exit): its non-null
  /// result, and a constructor's non-null fields, must be set.
  void nullCheckEnd(SourceLocation Loc, bool AtExit);

  /// The block's references that are followed from its start: its
  /// variables, its routine's T? parameters, non-null result and (a
  /// constructor) own non-null fields.
  void nullTrackBlock(Block &B);

  /// Functions whose result has been assigned somewhere in their body.
  llvm::DenseSet<RoutineDecl *> ResultAssigned;

  /// Variables referenced anywhere (read or written).
  llvm::DenseSet<Decl *> Used;
  llvm::DenseSet<Decl *> AttributesChecked; // (checkAttributes ran on them)

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

  Identifier predeclaredName(llvm::StringRef N);

  void declareStandardScope();

  /// Identifier for a predeclared member: lookup name lowercase, spelled
  /// as in Free Pascal.
  Identifier memberName(llvm::StringRef Spelling);

  /// class TObject: constructor Create; destructor Destroy; virtual;
  /// procedure Free; function ClassName: string; (bodies in codegen and the
  /// runtime). Destroy is virtual method 0.
  void declareTObject();

  /// TVarRec, the elements of an array of const, and the vt* constants:
  ///   record VType: int64; case of ... VInteger, VBoolean, VChar, ... end
  void declareVarRec();

  /// TGUID = packed record case integer of
  ///   1: (Data1: longword; Data2, Data3: word; Data4: array[0..7] of byte);
  ///   2: (D1: longword; D2, D3: word; D4: array[0..7] of byte);
  ///   3: (time_low: longword; time_mid, time_hi_and_version: word;
  ///       clock_seq_hi_and_reserved, clock_seq_low: byte;
  ///       node: array[0..5] of byte);
  /// end; and PGUID = ^TGUID, as in Free Pascal's system unit (tp, objfpc).
  void declareTGUID();

  Type *ArrayOfConstTy = nullptr;
  Type *TMethodTy = nullptr;
  RecordType *TGuidTy = nullptr; ///< the predeclared TGUID
  bool AddrOfWantsCode = false; ///< @TFoo.M for a pointer: its code address

  /// TMethod = record Code, Data: pointer; end (objfpc).
  void declareTMethod();
  Type *PCharTy = nullptr;

  /// class Exception (Free Pascal's SysUtils): private FMessage: string;
  /// constructor Create(const msg: string); property Message: string read
  /// FMessage write FMessage. Its VMT is in the runtime, which prints the
  /// message of an unhandled exception.
  void declareException();

  /// IUnknown = interface end (COM; also named IInterface: Free Pascal's
  /// QueryInterface, _AddRef and _Release are not declared, the compiled
  /// code counts references itself) and
  ///   TInterfacedObject = class(TObject, IUnknown)
  ///   protected FRefCount: longint;
  ///   public property RefCount: longint read FRefCount; end;
  /// Their iid and VMT are in the runtime.
  void declareInterfacedObject();

  /// TSharedObject = class(TObject) end: a class derived from it says it is
  /// thread-safe (its methods synchronize what they share), so `shared`
  /// variables can hold its objects. Its VMT is in the runtime.
  void declareSharedObject();

  /// A predeclared `Name = class(Parent);` (its VMT is in the runtime).
  ClassType *declareExceptionClass(llvm::StringRef Name, ClassType *Parent);

  /// Turbo/FPC builtins, predeclared only with --mode=tp.
  static bool isTPOnly(BuiltinKind K);

  /// Free Pascal's objpas built-ins, predeclared only with --mode=objfpc.
  static bool isObjFPCOnly(BuiltinKind K);

  //===--------------------------------------------------------------------===//
  // Diagnostics helpers
  //===--------------------------------------------------------------------===//

  /// The visible name closest to Name (edit distance), for "did you mean".
  std::string suggest(llvm::StringRef Name) const;

  std::string didYouMean(llvm::StringRef Name) const;

  void diagUndeclared(const Identifier &Id);

  /// A use (named Id) of D: warnings for its hint directives (deprecated,
  /// experimental, platform, library, unimplemented), as in Free Pascal.
  void warnHints(Decl *D, const Identifier &Id);

  void noteDeclaredHere(Decl *D);

  void declare(Decl *D);

  static std::string quoted(Type *T) { return "'" + T->getName() + "'"; }

  /// A routine's calling convention in a message (see CallConv).
  static const char *callConvDesc(bool CDecl);

  //===--------------------------------------------------------------------===//
  // Integer types (Turbo/FPC rules; ISO has only `integer`)
  //===--------------------------------------------------------------------===//

  /// Integer operands narrower than 32 bits widen to integer.
  Type *promoteInteger(Type *T);

  /// Is E a constant that a cardinal can hold?
  bool isCardinalConstant(Expr *E);

  /// Is E a constant of zero or more (that a qword can hold)?
  bool isNonNegativeConstant(Expr *E);

  /// The type integer operands LE and RE are converted to for an operator:
  /// qword if one is qword and the other an int64 constant of zero or more;
  /// int64 if either is int64; qword if either is qword (the other, of 32
  /// bits or less, is converted to qword, as in FPC); cardinal if both are
  /// unsigned (or the other is a constant that fits); int64 when cardinal
  /// meets a signed value (so no value is lost, as in FPC); integer
  /// otherwise.
  Type *integerOperandType(Expr *LE, Type *L, Expr *RE, Type *R);

  /// Type-checks E once (later calls return the cached type).
  Type *check(Expr *E);

  /// Untyped parameters (Turbo/FPC  var x;  const x) can only be typecast,
  /// have their address taken, or be passed to untyped parameters (and to
  /// move and fillchar): those operands are checked with this.
  bool AllowUntyped = false;
  Type *checkAllowingUntyped(Expr *E);

  /// The current routine has a try statement (see hasTry).
  void markHasTry();

  /// The current routine computes a managed value (see usesManaged).
  void markUsesManaged();

  //===--------------------------------------------------------------------===//
  // Type relations (ISO 7185 6.4.5 / 6.4.6)
  //===--------------------------------------------------------------------===//

  /// Compatible types: identical, ordinal types with the same host, or
  /// string types of the same length.
  static bool areCompatible(Type *A, Type *B);

  static bool isPChar(Type *T);

  /// Are A and B tuple types with the same element types, in turn (the
  /// names may differ)? Such tuples have one layout.
  static bool sameTupleShape(Type *A, Type *B);

  /// Can a value of type Value be assigned to a variable of type Target?
  static bool isAssignable(Type *Target, Type *Value);

  /// Can E be used where a variable is required (assignment target, `var`
  /// argument, with record)?
  static bool isVariableReference(Expr *E);

  static bool isBitField(Expr *E) { return isBitPackedComponent(E); }

  /// E (a bit field) where an address is needed.
  void diagBitFieldAddress(Expr *E);

  /// Does T have a record laid out with a field alignment (packed, or
  /// {$packrecords N}): itself, an element, a field?
  static bool containsAlignedRecord(Type *T);

  /// A type in T whose values a constant's byte image cannot hold (one that
  /// is not an ordinal, real or short string, or an array or record of
  /// those), or null. (The constants of records laid out with a field
  /// alignment are their bytes.)
  static Type *notByteImage(Type *T);

  /// Does T have a bitpacked part (itself, an element, a field)?
  static bool containsBitPacked(Type *T);

  /// Does a value of the non-ordinal type T need no alignment (it can sit
  /// at any byte of a bitpacked record)?
  static bool isByteAligned(Type *T);

  /// Is E a variable a type helper's method can change (not a method's
  /// result: r.M, r.M.f)?
  static bool isHelperVariable(Expr *E);

  /// Is E (already checked) a value of a class type?
  static bool isObject(Expr *E);

  /// The `const` parameter a designator is part of (x, x[i], x.f; not x^ or
  /// obj.f, whose target is a different variable), if any.
  static VarDecl *constParamRoot(Expr *E);

  /// Reports a change to a `const` parameter. Returns false if reported.
  bool checkModifiable(Expr *E);

  //===--------------------------------------------------------------------===//
  // Constant folding
  //===--------------------------------------------------------------------===//

  /// ISO 7185 6.3 constant: [sign] (number | constant-identifier) | string.
  static bool isISOConstantForm(Expr *E);

  static double asReal(const Constant &C);

  /// Is a value of type T kept as the 64 bits of a qword (read unsigned)?
  static bool isQWordValue(Type *T) { return T && isQWord(T->getBase()); }

  /// Does constant V of type VTy lie within T's values? (qword values are
  /// unsigned.)
  static bool constantInRange(int64_t V, Type *VTy, Type *T);

  /// Folds an already type-checked expression. Returns nullopt if it is not
  /// constant (without diagnosing).
  std::optional<Constant> fold(Expr *E);

  /// Checks and folds a constant expression in a declaration (const value,
  /// subrange bound, case/variant label). Reports non-constants and, in ISO
  /// mode, constant expressions beyond the ISO `constant` syntax.
  std::optional<Constant> evaluateConstant(Expr *E, llvm::StringRef What);

  /// The value of a typecast T(V) of a value of type From: integer types
  /// wrap to their width (byte(300) = 44), and widechar to 16 bits; a
  /// widechar above 255 is '?' as a char (as Free Pascal converts it);
  /// other ordinal types keep the value.
  static int64_t castOrdinalValue(int64_t V, Type *From, Type *T);

  /// A widechar's code as a char: '?' above 255 (Free Pascal's
  /// conversion).
  static int64_t wideToChar(int64_t V);

  /// A widechar constant above 255 that becomes a char or a string: a
  /// warning, as Free Pascal gives ('?' in its place).
  void checkWideToChar(Type *Target, Expr *E);

  static std::string formatValue(Type *T, int64_t V);

  //===--------------------------------------------------------------------===//
  // Types
  //===--------------------------------------------------------------------===//

  Type *resolveType(TypeSpec *TS, llvm::StringRef DeclName = {});

  Type *buildType(TypeSpec *TS);

  /// The name of a tuple's N-th element that has none: ItemN.
  Identifier tupleItemName(size_t N, SourceLocation Loc);

  /// The tuple type with these element types and names (Names empty:
  /// Item1, Item2, ...). There is one for each such list, so that tuple
  /// types written alike are the same type.
  RecordType *getTupleType(llvm::ArrayRef<Type *> Types,
                           llvm::ArrayRef<Identifier> Names);

  /// A hidden variable of type T of the block being checked (a tuple is
  /// built in one); null outside the statements of a block.
  VarDecl *makeHiddenVar(Type *T, SourceLocation Loc);

  bool addField(FieldDecl *F, llvm::SmallVectorImpl<FieldDecl *> &All,
                llvm::StringMap<FieldDecl *> &Seen);

  bool resolveFieldList(FieldList *FL, llvm::SmallVectorImpl<FieldDecl *> &All,
                        llvm::StringMap<FieldDecl *> &Seen);

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  void declareConst(ConstDecl *C);

  /// The arguments of D's attributes must be constants. (An attribute names
  /// a source generator; nothing here gives one a meaning.)
  void checkAttributes(Decl *D);

  /// The attributes of a type declaration and of its fields and members.
  void checkTypeAttributes(TypeDecl *TD);

  void declareType(TypeDecl *TD);

  /// The name of the type declared as Id: TOuter.TInner for a nested one.
  llvm::StringRef typeDeclName(const Identifier &Id);

  //===--------------------------------------------------------------------===//
  // Type helpers (objfpc, {$modeswitch typehelpers})
  //===--------------------------------------------------------------------===//

  /// The scope key of the helper active for T.
  llvm::StringRef helperKey(Type *T);

  /// The type helper active for values of type T (the last declared in
  /// scope; a string constant: the ansistring or shortstring one), or null.
  ClassType *helperFor(Type *T);

  /// The scope key of the last generic type helper declared (see
  /// TypeDecl::getPrevGenericHelper).
  llvm::StringRef genericHelpersKey();

  /// The generic type helpers in scope, the last declared first, whose
  /// extended type T matches (generic TH<T> = type helper for array of T:
  /// any dynamic array): the first one's specialization for the type
  /// arguments the match gives, or null.
  ClassType *genericHelperFor(Type *T);

  /// Does type T match TS, a type written with the type parameters of
  /// generic G (a parameter, `array of P`, `specialize TGen<P...>`, or a
  /// type without them)? The types the parameters stand for go to Bound.
  bool matchTypePattern(TypeDecl *G, TypeSpec *TS, Type *T,
                        llvm::SmallVectorImpl<Type *> &Bound);

  /// The helper of the type Base names (AnsiString.Join), or null.
  ClassType *helperNamedBy(Expr *Base);

  /// The type helper D is a member of (a property: by its getter), or null.
  static ClassType *helperOfMember(Decl *D);
  static bool isHelperMember(Decl *D) { return helperOfMember(D); }

  /// TH = type helper [(Parent)] for T ... end: a member table (a ClassType
  /// that is not a type of values) whose methods' Self is a T, by
  /// reference; from here on the helper active for T. A record helper is
  /// the same, for a record type T.
  void declareHelper(TypeDecl *TD, HelperTypeSpec *HS);

  //===--------------------------------------------------------------------===//
  // Advanced records (objfpc, {$modeswitch advancedrecords})
  //===--------------------------------------------------------------------===//

  /// TR = record fields; methods; operators; properties end; -- the record
  /// type (its fields), then its member table (see RecordType).
  void declareAdvancedRecord(TypeDecl *TD, RecordTypeSpec *RS);

  static bool isForwardClassDecl(Decl *D);

  /// A type declared in a class or advanced record (in Cur, its member
  /// scope).
  void declareNestedType(TypeDecl *TD);

  /// Members of Outer declared with TD: a nested class or record knows its
  /// outer one; an enumeration's values are members too.
  void addNestedMembers(ClassType *Outer, TypeDecl *TD,
                        llvm::SmallVectorImpl<Decl *> &Members,
                        llvm::StringMap<Decl *> &Own);

  /// The member Name of the class or advanced record that Quals name
  /// (TOuter, TOuter.TInner, ...); null after a diagnostic.
  Decl *lookupMemberPath(llvm::ArrayRef<Identifier> Quals,
                         const Identifier &Name);

  void defineRecordMembers(RecordType *RT, RecordTypeSpec *RS);

  /// A class variable of class, advanced record or helper C (class var):
  /// static storage, one for C and its descendants.
  void declareClassVar(ClassType *C, VarDecl *V);

  /// A method, class method (static) or class operator of record RT.
  void defineRecordMethod(ClassType *C, RecordType *RT, RoutineDecl *R);

  /// A class operator's parameters: two (binary), one (unary - + not, and
  /// the conversions := and Explicit); no var parameters; the record among
  /// the parameter or result types.
  void checkOperatorHeading(RoutineDecl *R, RecordType *RT);

  static ClassType *memberTableOf(Type *T);

  /// A user-defined conversion of a value of type From to To: a `:=`
  /// operator (Explicit: an `Explicit` one first) of either record whose
  /// parameter takes From (exactly, else by assignment) and whose result is
  /// To. Null if there is none.
  RoutineDecl *findConversion(Type *From, Type *To, bool Explicit);

  /// Converts Value to To by a user-defined `:=` operator if there is one.
  bool convertImplicitly(Expr *Value, Type *To);

  /// The class operator Name of the records among Args' types that fits
  /// Args best (exact types, then assignable ones, then ones converted by a
  /// `:=` operator), with the conversions it needs annotated; null if none
  /// fits. Found: whether either record has an operator Name of that arity.
  RoutineDecl *resolveOperator(llvm::StringRef Name, llvm::ArrayRef<Expr *> Args,
                               bool &Found);

  static const char *operatorSymbol(BinaryOp Op);

  /// L op R where an operand is an advanced record: its class operator
  /// (`<>` may use `=`, negated).
  Type *checkRecordOperator(BinaryExpr *E);

  /// op X where X is an advanced record: its class operator.
  Type *checkRecordOperator(UnaryExpr *E);

  //===--------------------------------------------------------------------===//
  // Classes (objfpc)
  //===--------------------------------------------------------------------===//

  static std::string qualified(ClassType *C, const Identifier &Member);

  /// TFoo = class ... end;  /  TFoo = class;
  void declareClass(TypeDecl *TD, ClassTypeSpec *CS);

  /// A forward class must be defined in the same type section.
  void checkPendingClasses();

  //===--------------------------------------------------------------------===//
  // Interfaces (objfpc, CORBA style)
  //===--------------------------------------------------------------------===//

  /// IFoo = interface ... end;  /  IFoo = interface;
  void declareInterface(TypeDecl *TD, InterfaceTypeSpec *IS);

  /// The interface named Id, or null after a diagnostic.
  /// specialize IGen<...> in a class's or interface's heading: the
  /// interface it is, or null after a diagnostic.
  InterfaceType *specializedInterface(const Identifier &Id, TypeSpec *Spec);

  InterfaceType *lookupInterface(const Identifier &Id);

  /// The method Name of I or its nearest ancestor declaring it (the first
  /// of its overloads).
  static RoutineDecl *findInterfaceMethod(InterfaceType *I, llvm::StringRef Name);

  void defineInterface(InterfaceType *I, InterfaceTypeSpec *IS);

  /// property P: T read GetP [write SetP] in interface I (objfpc): its
  /// accessors are methods of I (or of its ancestors), a function without
  /// parameters returning T and a procedure taking one T. Array properties,
  /// index and default specifiers are not supported. False after an error.
  bool defineInterfaceProperty(InterfaceType *I, PropertyDecl *Prop,
                               llvm::ArrayRef<PropertyDecl *> Earlier);

  static PropertyDecl *findInterfaceProperty(InterfaceType *I,
                                             llvm::StringRef Name);

  /// The methods of class C implementing the methods of interface I (by
  /// slot): methods (not class methods) of C or its ancestors with the same
  /// name and heading. Where: the interface's name in C's declaration, to
  /// report a missing one (null: inherited, already checked).
  llvm::ArrayRef<RoutineDecl *> implementInterface(ClassType *C, InterfaceType *I,
                                                   const Identifier *Where);

  /// The class named Id (a parent class or a method's class), or null after
  /// a diagnostic. What: "parent class" / "class".
  ClassType *lookupClass(const Identifier &Id);

  /// The hidden parameter Self of method R of class C.
  void makeSelf(RoutineDecl *R, ClassType *C);

  static bool sameHeading(RoutineDecl *A, RoutineDecl *B);

  void defineClass(ClassType *C, ClassTypeSpec *CS);

  /// generic function M<T>(...) in class C: a member found only by its key
  /// (M<N>), not checked; its specializations are made from its
  /// implementation (instantiateMethod). Not virtual: each specialization
  /// is a method of its own.
  void defineGenericMethod(ClassType *C, RoutineDecl *R);

  /// The class constructor or destructor R of class or advanced record C:
  /// one of each, without parameters or directives, in a type declared at
  /// program or unit level (as in Free Pascal); static, so without Self.
  void defineClassInit(ClassType *C, RoutineDecl *R);

  void defineMethod(ClassType *C, ClassType *Parent, RoutineDecl *R,
                    RoutineDecl *Inherited,
                    const ClassType *Owner,
                    llvm::SmallVectorImpl<RoutineDecl *> &VTable);

  /// property P[params]: T [index N] read R write W -- R: a field of type T
  /// (a plain property) or a function returning T; W: a field of type T or
  /// a procedure taking the value (a value or const parameter of type T).
  /// The methods first take the index parameters of an array property
  /// (same types and kinds), then N (an integer) for an indexed property.
  /// Both must be declared before the property.
  void definePropertyAccessors(ClassType *C, PropertyDecl *P,
                               const llvm::StringMap<Decl *> &Own,
                               ClassType *Parent);

  /// The default property of C or its nearest ancestor declaring one.
  static PropertyDecl *defaultProperty(const ClassType *C);

  /// TS in the heading of a method of C: the nested types of C and of the
  /// classes around it are visible.
  Type *resolveTypeIn(ClassType *C, TypeSpec *TS);

  /// procedure TFoo.Bar ...; begin ... end;  -- the body of a method.
  void declareMethodBody(RoutineDecl *R);

  /// C's class constructor and destructor have bodies.
  void checkClassInitImplemented(ClassType *C);

  /// Every method of the classes declared in B has a body.
  void checkMethodsImplemented(Block &B);

  /// ... of the class or advanced record TD, and of those it declares.
  void checkMethodsImplemented(TypeDecl *TD);

  /// The method whose body (or a routine nested in it) is being checked.
  RoutineDecl *enclosingMethod() const;

  /// The class whose members a with context exposes (null for records):
  /// an object's, or in a class method Self's (a class reference).
  static ClassType *classOf(WithContext *W);

  /// Does with context W expose methods (an object's, a class's in a class
  /// method, or an advanced record's)?
  static bool hasMembers(WithContext *W);

  /// Is D an instance member (a field, a property, a method that is not a
  /// class method or constructor)? Class methods cannot use them.
  static bool isInstanceMember(Decl *D);

  /// In a class method, Self is a class: a constructor called through it
  /// creates an object of that class (its static type: Self's class).
  static ClassType *constructedThroughSelf(RoutineDecl *R, WithContext *W);

  /// Reports use of a private or protected member D of class Owner where it
  /// is not visible. Private members are visible in the program or unit
  /// declaring the class, protected ones also in methods of descendants;
  /// strict private members only in methods of the class, strict protected
  /// ones also in methods of descendants.
  void checkMemberAccess(Decl *D, const ClassType *Owner, const Identifier &Id);

  /// Visibility of a member found by name through a class scope; in a
  /// class method (Self is a class), only class members are usable.
  void checkScopeAccess(Decl *D, WithContext *W, const Identifier &Id);

  /// The variable that E, the target of an `absolute`, is in: E is a
  /// variable, a field of a record in one or an element at a constant
  /// index of an array in one. Null for anything else.
  VarDecl *absoluteRootOf(Expr *E);

  /// `V: T absolute Target`, as in Free Pascal. Checked before V is
  /// declared, so that it cannot name itself.
  void checkAbsolute(VarDecl *V, Expr *Target);

  /// x: T; [cvar;] external ['lib'] [name 'sym'];  -- a C variable, at
  /// program or unit level; the library and symbol: string constants.
  void checkExternalVar(VarDecl *V);

  void declareVar(VarDecl *V);

  /// `x: T owned`: a variable of a routine (or one declared among the
  /// statements), of a class type.
  void checkOwnedDecl(VarDecl *V);

  /// Can values of type T be shared between tasks? A task, a token, or an
  /// object of a class derived from TSharedObject (which synchronizes what
  /// it shares).
  bool isThreadSafe(Type *T) const;

  /// Does a value of type T give access to data that others may change (an
  /// object that is not thread-safe, a pointer, a dynamic array, an
  /// interface)?
  bool sharesMutableData(Type *T) const;

  /// `shared T`: T thread-safe; `isolated T`: T a reference (a class, a
  /// dynamic array or a pointer), which `move` hands over.
  void checkRaceDecl(RaceQualifier Q, Type *T, const Identifier &Id,
                     const char *What);

  void declareVarInit(VarDecl *V);

  /// Is T an array of char (any index type) that a string constant of
  /// length N initializes (Turbo/FPC)?
  static bool isCharArrayOfLength(Type *T, uint64_t N);

  /// Checks the initial value of a typed constant of type T: a constant
  /// expression (folded into Folded) or, for arrays and records, an
  /// aggregate whose elements are checked recursively.
  bool checkConstInit(Expr *Init, Type *T, ConstValue &Folded);

  /// A typed constant (or initialized variable) of procedural type RT: nil,
  /// a routine (@F) declared at program or unit level or a static method,
  /// or a lambda that uses nothing of the routines around it and has no
  /// object.
  bool checkRoutineConstInit(Expr *Init, RoutineType *RT);

  /// A TGUID typed constant (or initialized variable) from a string
  /// constant, as in Free Pascal: '{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}'
  /// (hexadecimal digits) or '' (all zeros). Folded receives its 16 bytes
  /// (Data1, Data2 and Data3 little-endian, then Data4).
  bool checkGuidInit(Expr *Init, ConstValue &Folded);

  /// Is E, a checked expression, a constant (nil, a set of constants, a
  /// concatenation of string constants)?
  bool isConstantDefault(Expr *E);

  /// Checks the default value of parameter P, of a resolved type: a
  /// constant that can be passed to it.
  void checkParamDefault(VarDecl *P);

  /// The elements of the constant set S.
  std::bitset<256> constantSetBits(SetCtorExpr *S);

  /// Are the constant default values A and B of a parameter equal?
  bool sameDefaultValue(Expr *A, Expr *B);

  /// Do the default values of Impl, the implementation of Heading (with
  /// the same signature), fit? It may leave them out, or repeat them.
  bool sameDefaults(RoutineDecl *Heading, RoutineDecl *Impl);

  /// Resolves a routine's parameter and result types.
  void resolveSignature(RoutineDecl *R);

  /// An async routine's restrictions: its task may outlive its caller, so
  /// its arguments are copied (no var, out or constref parameters, no open
  /// or untyped ones), and it is not nested (but a spawn block, which its
  /// scope waits for) nor a virtual method, constructor or destructor.
  /// (The same for an iterator routine, whose iterator may outlive its
  /// caller too.)
  void checkAsyncRoutine(RoutineDecl *R);

  /// CompareOut false: `out` and `var` parameters count as the same, and
  /// `constref` and `const` ones, as
  /// when looking for routines that overloads cannot tell apart.
  static bool sameSignature(RoutineDecl *A, RoutineDecl *B, bool CompareOut = true);

  /// The word for a by-reference parameter, for diagnostics.
  static const char *paramKeyword(VarDecl *P);

  /// The scope key of a generic routine: its name and type parameter count
  /// (F<1>), so it does not clash with routines named F.
  llvm::StringRef genericKey(llvm::StringRef Name, size_t N);

  /// generic function F<T>...: declared (under its key), not checked.
  void declareGeneric(RoutineDecl *R);

  /// generic function TFoo.M<T>(...) begin ... end: the definition of the
  /// generic method M<N> of TFoo; its heading must match the declaration's
  /// in its shape (type parameters, parameters' kinds and number, function
  /// or procedure), the types being checked in each specialization.
  void declareGenericMethodBody(RoutineDecl *R);

  /// The specialization of generic method G (of its class) for type
  /// arguments Types: its implementation parsed again, named M<types>, a
  /// method of the class (not a member) with G's directives; its heading
  /// resolved now, its body checked later. Null after a diagnostic.
  RoutineDecl *instantiateMethod(const Identifier &Name, RoutineDecl *G,
                                 llvm::ArrayRef<Type *> Types);

  /// The generic methods named Name of class C or its ancestors (the
  /// nearest of each number of type parameters).
  llvm::SmallVector<RoutineDecl *, 2> genericMethodsNamed(const ClassType *C,
                                                          llvm::StringRef Name);

  /// Is TS, the type generic helper TD extends, a pattern that gives all
  /// its type parameters (and not a bare parameter: a helper for every
  /// type)?
  bool genericHelperPatternOK(TypeDecl *TD, TypeSpec *TS);

  /// Is Name the name of a generic (type or routine), with any number of
  /// type parameters?
  bool isGenericName(llvm::StringRef Name);

  /// generic TList<T> = ...: declared (under its key), not checked.
  void declareGenericType(TypeDecl *TD);

  /// specialize Name<Args>, a type: the declaration of the type made of
  /// the generic type Name for those type arguments, parsed again from the
  /// generic's tokens on first use and declared like any type (named
  /// Name<integer>), in the generic's scope plus the type parameters as
  /// type aliases; its methods' implementations are parsed again too, and
  /// their bodies checked later. Null after a diagnostic.
  TypeDecl *specializeType(const Identifier &Name,
                           llvm::ArrayRef<TypeSpec *> Args);

  /// The specialization of generic type G for Types (asked for at Name),
  /// made on first use (see specializeType).
  TypeDecl *specializeTypeFor(const Identifier &Name, TypeDecl *G,
                              llvm::ArrayRef<Type *> Types);

  /// A forward or interface heading without its definition (yet).
  static bool isUndefinedHeading(RoutineDecl *R);

  /// specialize Name<Args>: the routine for those type arguments, made
  /// from the generic's declaration on first use (its heading resolved now,
  /// its body checked later); null after a diagnostic.
  /// (W: the with context the generic was found through, a generic
  /// method's object: Self in a method.)
  RoutineDecl *specialize(const Identifier &Name, llvm::ArrayRef<TypeSpec *> Args,
                          WithContext **W = nullptr);

  /// The specialization of the generic routine G for type arguments Types
  /// (see specialize).
  RoutineDecl *instantiate(const Identifier &Name, RoutineDecl *G,
                           llvm::ArrayRef<Type *> Types);

  /// The index of the type parameter of Def that TS names, or -1.
  static int typeParamNamed(RoutineDecl *Def, TypeSpec *TS);

  /// Does TS (written in generic Def) name one of Def's type parameters,
  /// itself or in its type arguments?
  static bool mentionsTypeParam(RoutineDecl *Def, TypeSpec *TS);

  /// SS, a parameter's type written in generic Def (specialize TGen<...>),
  /// and AT, the type of its argument A: if AT is a specialization of TGen,
  /// the type arguments of Def that SS's arguments name are AT's (through
  /// Give; nested specializations and dynamic arrays alike).
  template <typename GiveFn>
  void inferFromSpecialization(RoutineDecl *Def, SpecializeTypeSpec *SS,
                               Type *AT, Expr *A, GiveFn &Give) {
    auto *GS = static_cast<Scope *>(Def->getGenericScope());
    auto *G = GS ? llvm::dyn_cast_or_null<TypeDecl>(GS->lookup(
                       genericKey(SS->getName().Name, SS->getArgs().size())))
                 : nullptr;
    if (!G)
      return;
    for (auto &[Key, I] : TypeInstances) {
      if (Key.first != G || I->getType() != AT)
        continue;
      for (size_t K = 0; K < SS->getArgs().size(); ++K) {
        TypeSpec *TA = SS->getArgs()[K];
        Type *Of = Key.second[K];
        if (int P = typeParamNamed(Def, TA); P >= 0)
          Give(P, Of, A);
        else if (auto *Nested = llvm::dyn_cast<SpecializeTypeSpec>(TA))
          inferFromSpecialization(Def, Nested, Of, A, Give);
        else if (auto *DS = llvm::dyn_cast<DynArrayTypeSpec>(TA))
          if (int Q = typeParamNamed(Def, DS->getElement()); Q >= 0)
            if (auto *DT = llvm::dyn_cast<DynArrayType>(Of))
              Give(Q, DT->getElementType(), A);
      }
      return;
    }
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
                                   bool &Found, WithContext **W = nullptr);

  /// Of Generics (routines or methods named Name), the one whose
  /// parameters take Args, specialized for the type arguments they (and
  /// Want, the type wanted of the result) give; null after a diagnostic.
  RoutineDecl *inferFor(const Identifier &Name,
                        llvm::SmallVectorImpl<RoutineDecl *> &Generics,
                        llvm::ArrayRef<Expr *> Args, Type *Want);

  /// Checks the bodies of the specializations made so far (and those they
  /// make), then adds the module's to its block.
  void checkInstances(Block &ModuleBlock);

  void declareRoutine(RoutineDecl *R);

  /// Declares R; in objfpc, another routine of its name in the same scope
  /// makes it an overload (with other parameters). False if R was rejected
  /// as such (its body is not checked then).
  bool declareRoutineName(RoutineDecl *R);

  /// R (resolved) joins the overloads First...: its parameters must differ.
  bool addOverload(RoutineDecl *First, RoutineDecl *R);

  /// A record's `:=` or `Explicit` operator: its overloads may differ in the
  /// result type alone.
  static bool isConversionOperator(RoutineDecl *R);

  //===--------------------------------------------------------------------===//
  // Overload resolution (objfpc)
  //===--------------------------------------------------------------------===//

  /// The routines a call of First's name may mean: First's overloads; for a
  /// method with the `overload` directive also those of its ancestors that
  /// it does not override.
  void collectOverloads(RoutineDecl *First,
                        llvm::SmallVectorImpl<RoutineDecl *> &Out);

  /// How far an argument of type A is from a parameter of type P (lower
  /// is better): an exact type, a subrange of it, a wider integer type
  /// (integer before int64), a same-size or narrower one, real, an
  /// ancestor class (the nearest first), anything else assignable.
  static unsigned conversionCost(Type *P, Type *A);

  /// Breaks ties between costs: widening an integer to 64 bits of the
  /// other signedness (byte to int64 rather than qword, longint to qword
  /// rather than int64), as Free Pascal prefers the same signedness.
  static unsigned signPenalty(Type *P, Type *A);

  /// Is Arg the name of a routine (passed for a procedural parameter)?
  bool isRoutineName(Expr *Arg);

  /// Can parameters Params take N arguments (the rest from default values)?
  static bool acceptsArgCount(llvm::ArrayRef<VarDecl *> Params, size_t N);

  /// The value of an argument: of Name := value, the value.
  static Expr *argValue(Expr *Arg);

  static bool hasNamedArgs(llvm::ArrayRef<Expr *> Args);

  /// The parameter of Params that argument I of Args is for (by its
  /// position, or by its name), or null.
  static VarDecl *paramOfArg(llvm::ArrayRef<VarDecl *> Params,
                             llvm::ArrayRef<Expr *> Args, size_t I);

  /// Where the arguments of a call with named ones go: Map[I] is the index
  /// in Params of the parameter of argument I. Arguments by position come
  /// first and are for the first parameters; each parameter is given once;
  /// one that is not given has a default value. False if Args do not fit,
  /// reported if Callee is given.
  bool mapArgs(llvm::ArrayRef<VarDecl *> Params, llvm::ArrayRef<Expr *> Args,
               llvm::SmallVectorImpl<unsigned> &Map,
               const Identifier *Callee);

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
               llvm::ArrayRef<Type *> ArgTys, unsigned &Penalty);

  /// The routine of First's name that Args fit best: First itself unless
  /// it is overloaded. Null after a diagnostic (no fit, or a tie).
  static bool hasConstArrayParam(llvm::ArrayRef<RoutineDecl *> Cands,
                                 llvm::ArrayRef<Expr *> Args, size_t I);

  static bool hasProceduralParam(llvm::ArrayRef<RoutineDecl *> Cands,
                                 llvm::ArrayRef<Expr *> Args, size_t I);

  /// The parameters of R that a call with N arguments leaves to their
  /// default values.
  static unsigned defaultsUsed(RoutineDecl *R, size_t N);

  /// Does an expression of type A fit an open array of Elem without being
  /// an array constructor: the same open array, an array of exactly Elem,
  /// or a string constant for chars?
  bool fitsOpenArray(Expr *Arg, Type *A, OpenArrayType *OA);

  /// The cost of passing [...] for a parameter of type PT (-1: it cannot
  /// be): as an array of const first, then a set, then an open array, as in
  /// Free Pascal.
  int setCtorCost(SetCtorExpr *E, VarDecl *P);

  void checkOpenArrayArg(Expr *Arg, VarDecl *P, OpenArrayType *OA);

  /// [a, b, ...] passed for an array of const: any values it can hold.
  void checkConstArray(SetCtorExpr *E, Type *PT);

  /// E is a lambda not completed yet: its routine.
  static RoutineDecl *pendingLambda(Expr *E);

  /// The types written in lambda L (of its parameters and result), resolved
  /// once while overloads are chosen (kept as their types until
  /// completeLambda sets them). False after a diagnostic.
  bool resolveLambdaSpecs(RoutineDecl *L);

  /// The cost of passing lambda L for a parameter of type PT, by its
  /// shape (-1: it does not fit): a procedural type of the same kind
  /// (function or procedure), with as many parameters, of the types and
  /// kinds L writes, and the result type it writes. A parameter written
  /// with a type but without a kind takes the kind wanted, but fits a value
  /// parameter best (cost 1 for each other); one without either fits any
  /// parameter alike (so overloads that differ there are ambiguous).
  int lambdaCost(RoutineDecl *L, Type *PT);

  RoutineDecl *resolveOverload(RoutineDecl *First, llvm::ArrayRef<Expr *> Args,
                               const Identifier &Callee, SourceRange Range);

  /// A lambda's shape for diagnostics: "function(integer; ?): ?" (? for
  /// types left out).
  std::string lambdaShape(RoutineDecl *L);

  /// "f(integer; var string): real" for diagnostics.
  static std::string routineHeading(RoutineDecl *R);

  /// The string constant E, the library or symbol name (What) of an
  /// external routine; nullopt after a diagnostic.
  std::optional<llvm::StringRef> externalString(Expr *E, const char *What,
                                                const char *Of = "routine");

  /// A type with a C equivalent, for external routines.
  static bool isCType(Type *T);

  /// A procedural type C code can call: cdecl, with parameters and result
  /// of C types.
  static bool isCCallback(RoutineType *RT);

  /// External (C) routines: declared at the outermost level, with
  /// parameters and result of types that have a C equivalent; the library
  /// and symbol names: string constants.
  void checkExternal(RoutineDecl *R);

  /// A string constant passed where a pchar is expected (Turbo/FPC): the
  /// compiler supplies a NUL-terminated copy.
  bool isPCharLiteral(Type *Target, Expr *Value);

  void checkRoutineBody(RoutineDecl *R);

  // --- compiletime functions ------------------------------------------------
  // A compiletime function (and the routines nested in it) runs while
  // compiling: no I/O, pointers, objects, procedural values or files, no
  // global variables (typed constants can be read), and it calls only
  // compiletime functions, its nested routines and the builtins without
  // side effects.

  /// The compiletime function the current routine is (or is nested in).
  RoutineDecl *compiletimeRoutine() const;

  /// Is R the current routine, or one around it?
  bool enclosesCurrent(const RoutineDecl *R) const;

  /// F, if E is a call of compiletime function F (F or F(args)).
  RoutineDecl *compiletimeCallee(Expr *E);

  static bool compiletimeBuiltinAllowed(BuiltinKind K);

  void compiletimeError(RoutineDecl *CT, SourceLocation L, SourceRange R,
                        const llvm::Twine &What);

  /// A use of D (a name, a call) in compiletime function CT.
  void checkCompiletimeUse(RoutineDecl *CT, Decl *D, SourceLocation L,
                           SourceRange Range, bool Write = false);

  /// E (of type T) in compiletime function CT.
  void checkCompiletimeExpr(RoutineDecl *CT, Expr *E, Type *T);

  void visitStmt(Stmt *S);

  /// Resolves the pointer types since Mark whose targets are visible now
  /// (declared in a class); the others stay pending.
  void resolvePendingPointersFrom(size_t Mark);

  void resolvePendingPointers();

  void checkBlock(Block &B);

  void declareAll(Block &B);

  void finishBlock(Block &B, bool WarnUnused);

  void checkGotos();

  void checkProgramParams(ProgramDecl &P);

  /// ISO 7185 6.10: program parameters other than input/output must be
  /// declared as file variables in the program block. They are bound to the
  /// command-line arguments in order.
  void checkProgramParamDecls(ProgramDecl &P);

  //===--------------------------------------------------------------------===//
  // Statement helpers
  //===--------------------------------------------------------------------===//

  void checkCondition(Expr *Cond, llvm::StringRef Keyword);

  /// Reports an assignment (or read / var argument) to the control variable
  /// of an enclosing for statement. Returns true if one was reported.
  bool checkNotForControlVar(Expr *Target);

  /// The order of label value V of a selector of type SelTy: qword's
  /// values compare unsigned.
  static int64_t labelKey(int64_t V, Type *SelTy);

  /// Checks one case or variant label against the selector/tag type.
  bool checkLabel(CaseLabel *L, Type *SelTy, llvm::StringRef What);

  /// ISO 7185 6.6.3.6: a procedure/function passed as an argument must have
  /// congruent parameter lists (same kinds, identical types) and result.
  static bool congruent(llvm::ArrayRef<VarDecl *> A, Type *AResult,
                        llvm::ArrayRef<VarDecl *> B, Type *BResult);

  /// An argument for a procedural/functional parameter: the name of a
  /// routine (or of another procedural parameter) with a congruent heading.
  void checkRoutineArg(Expr *Arg, VarDecl *P, RoutineType *Formal);

  /// An argument (number I, from 0) beyond the parameters of a 'varargs'
  /// external routine: a value C can take (ordinals are passed as int at
  /// least, reals as double, string constants as C strings).
  void checkVarArg(Expr *Arg, size_t I, const Identifier &Callee);

  /// Why routine R cannot be a procedural value (empty: it can). Nested
  /// routines can be passed to ISO procedural parameters (AllowNested).
  static std::string notProceduralValue(RoutineDecl *R, bool AllowNested);

  /// Checks the arguments of a call to a user routine.
  void checkCallArgs(RoutineDecl *R, const Identifier &Callee,
                     llvm::ArrayRef<Expr *> Args, SourceRange CallRange);

  void checkCallArgs(llvm::ArrayRef<VarDecl *> Params, Decl *CalleeDecl,
                     const Identifier &Callee, llvm::ArrayRef<Expr *> Args,
                     SourceRange CallRange);

  /// x passed for var or out parameter P: the callee may rely on it not
  /// being nil (var P: T), and leaves it as P's type says.
  void nullVarArg(Expr *Arg, VarDecl *P);

  /// Can E's address be passed for a `constref` parameter: a variable (not
  /// a bit field or a property)?
  static bool isConstRefAddressable(Expr *E);

  //===--------------------------------------------------------------------===//
  // Built-in procedure calls
  //===--------------------------------------------------------------------===//

  static bool isWritable(Type *T);

  /// If the first argument of read/write/... is a file, checks and returns
  /// it; otherwise returns null.
  FileType *checkFileArg(ProcCallStmt *S);

  /// write/read of an untyped file: an error (they use blockwrite and
  /// blockread). True if reported.
  bool untypedFileIO(ProcCallStmt *S, FileType *File, bool Write);

  void checkWrite(ProcCallStmt *S, bool IsWriteln);

  /// A value written to a text file, or put in a template string, with its
  /// field width and precision if it has them (a FormattedExpr).
  void checkWritten(Expr *Arg, bool Template);

  void checkRead(ProcCallStmt *S, bool IsReadln);

  /// reset/rewrite/get/put(f) and page[(f)].
  /// reset, rewrite (an untyped file's: with an optional record size),
  /// get, put, page, close, closefile, flush.
  void checkFileProc(ProcCallStmt *S, BuiltinKind K);

  /// An integer argument of built-in Callee (What: its role).
  void checkIntegerArg(Expr *E, const Identifier &Callee, llvm::StringRef What);

  /// A file variable argument of Callee: the file type, or null after a
  /// diagnostic. NotText: text files are not allowed.
  FileType *checkFileVarArg(Expr *E, const Identifier &Callee,
                            bool NotText = false, bool Untyped = false);

  /// assign(f, name) / assignfile: f any file variable, name a string, a
  /// char or a pchar.
  void checkAssignFile(ProcCallStmt *S);

  /// blockread(f, buf, count [, done]) / blockwrite: f an untyped file, buf
  /// any variable, count an integer, done an integer variable.
  void checkBlockIO(ProcCallStmt *S);

  /// seek(f, n): f a typed or untyped file, n an integer.
  void checkSeek(ProcCallStmt *S);

  /// pack(a, i, z): z := a[i..]   unpack(z, a, i): a[i..] := z
  void checkPackUnpack(ProcCallStmt *S, bool IsPack);

  /// new(p [, tag constants]) / dispose(p [, tag constants]). The tags
  /// select record variants (ISO 6.6.5.3); storage is always allocated for
  /// the largest variant, so they are only checked.
  /// delete(var s, index, count) / insert(source, var s, index)
  void checkDeleteInsert(ProcCallStmt *S, bool IsInsert);

  /// setlength(a, n, ...): a a dynamic array variable, one length for each
  /// level of arrays of arrays that is set.
  void checkDynSetLength(ProcCallStmt *S);

  /// setlength(s, n): s a string variable, n an integer.
  void checkSetLength(ProcCallStmt *S);

  /// inc(x [, n]) / dec(x [, n]): x an ordinal variable (Turbo/FPC: or a
  /// pointer, moved by n of what it points to), n an integer.
  void checkIncDec(ProcCallStmt *S);

  void checkNewDispose(ProcCallStmt *S, bool IsNew);

  //===--------------------------------------------------------------------===//
  // Built-in function calls
  //===--------------------------------------------------------------------===//

  Type *checkStringFunction(CallExpr *E, BuiltinDecl *BD,
                            llvm::ArrayRef<Type *> ArgTypes);

  /// slice(a, n), for an open array parameter only.
  bool AllowSlice = false;

  bool isSliceCall(Expr *E);

  Type *checkSlice(CallExpr *E, llvm::ArrayRef<Type *> ArgTypes);

  Type *checkBuiltinFunction(CallExpr *E, BuiltinDecl *BD,
                             llvm::ArrayRef<Type *> ArgTypes);

  /// T(x) (Turbo/FPC): between ordinal types (integer types wrap, nothing
  /// is range-checked) and between pointer types.
  Type *checkTypecast(CallExpr *E, TypeDecl *TD);

  /// A type name or an expression as the argument of sizeof/low/high:
  /// returns its type (also stored on the argument).
  Type *checkTypeOrValue(Expr *Arg);

  /// Built-in functions whose arguments are not ordinary values (type
  /// names, procedural variables) or that take no arguments. Returns
  /// nullopt for the others.
  std::optional<Type *> checkSpecialBuiltin(CallExpr *E, BuiltinDecl *BD);

  /// getmem(p, n) freemem(p) reallocmem(p, n) fillchar(x, n, v)
  /// move(src, dst, n) halt[(code)]  (Turbo/FPC)
  void checkMemoryProc(ProcCallStmt *S, BuiltinKind K);

  /// VectorStore(address, vector)
  void checkVectorStore(ProcCallStmt *S);

public:
  Checker(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LO,
          Sema::ReparseFn Reparse = nullptr,
          Sema::ReparseTypeFn ReparseType = nullptr,
          const CheckDirectives *Directives = nullptr);

  std::vector<std::string> standardNames() const;

  /// Makes the interfaces of the units in Uses visible in S.
  void importUnits(Scope &S, llvm::ArrayRef<Identifier> Uses);

  /// Checks unit U. InterfaceOnly: U was compiled separately (S11), so
  /// only its interface is declared, for the units and program using it.
  void checkUnit(UnitDecl &U, bool InterfaceOnly = false);

  void checkProgram(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                    bool SeparateUnits);

  /// A unit compiled on its own: the units it uses were compiled separately.
  void checkSeparateUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps);

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *);

  void visitSequence(const void *Seq, llvm::ArrayRef<Stmt *> Body);

  /// The type of a variable declared without one, from its value's type T.
  /// Is E a quoted character ('a')? Where a type is inferred it is a
  /// string, like any quoted text.
  static bool isCharLiteral(Expr *E);

  /// The type that values of types A and B have in common where one type
  /// is inferred for both (the elements of [a, b], the arguments for one
  /// type parameter), or null: the same type; the wider integer type; real
  /// for a real and an integer (nothing is lost); Str for strings and
  /// characters; else the one the other can be assigned to.
  Type *commonType(Type *A, Type *B, Type *Str);

  /// Can a variable (an element, a type argument) have the type T of a
  /// value, when it is given no type of its own?
  static bool isInferable(Type *T);

  /// var x := [a, b, c]: the dynamic array type of the value, an array
  /// written out. Null if Init is not one: [], a list with a range, or one
  /// whose elements are all enumeration values, characters or booleans (a
  /// set, as ever). The error type after a diagnostic.
  Type *inferArrayLiteral(Expr *Init, bool LongStr, const Identifier &Name);

  Type *inferVarType(VarDeclStmt *S, VarDecl *V, Type *T);

  /// var x: T;  var x: T := e;  var x := e;  -- among the statements of a
  /// sequence, to whose end the variables are visible.
  void visitVarDeclStmt(VarDeclStmt *S);

  /// (a, b) := e;  var (a, b) := e;  -- e, a tuple, is kept in a hidden
  /// variable (a tuple written out is built in one), then each target is
  /// assigned its element, with the checks of an assignment.
  void visitDestructureStmt(DestructureStmt *S);

  void visitCompoundStmt(CompoundStmt *S) { visitSequence(S, S->getBody()); }

  void visitLabeledStmt(LabeledStmt *S);

  void visitGotoStmt(GotoStmt *S);

  /// The procedural type of the field or property D, or null.
  static RoutineType *proceduralMember(Decl *D);

  void visitProcCallStmt(ProcCallStmt *S);

  /// Resolves an assignment target that is a plain name: a variable, a field
  /// (inside with), or the result of an enclosing function.
  Type *checkNameTarget(NameRefExpr *Target);

  void visitIndirectCallStmt(IndirectCallStmt *S);

  /// Type of a property assigned to; it needs a write accessor.
  Type *checkPropertyWrite(PropertyDecl *P, const Identifier &Id);

  /// Is the target of an assignment a property (written by its accessor)?
  static PropertyDecl *assignedProperty(Expr *Target);

  /// The name an array property is used by (obj.P / P), or for a default
  /// property, its name at the indexed object.
  static Identifier propertyName(IndexExpr *E);

  /// An array property used without its indexes (outside IndexBase).
  bool checkArrayPropertyUse(PropertyDecl *P, Expr *E, const Identifier &Id);

  /// Value, the first value a lambda's body gives its result while its
  /// result type is not known: that type (a quoted constant: a string).
  void inferLambdaResult(Expr *Value);

  void visitAssignStmt(AssignStmt *S);

  /// The rest of the checks of Target := Value, both already checked.
  void finishAssign(AssignStmt *S, Type *TargetTy, Type *ValueTy);

  void visitTryStmt(TryStmt *S);

  /// The object of the owned variable OV would be kept (or returned)
  /// through E without `move`.
  void diagOwnedEscape(VarDecl *OV, Expr *E, bool Returned);

  /// on [E:] TFoo do Body: E is a variable of type TFoo visible in Body.
  void checkHandler(ExceptHandler *H);

  void visitRaiseStmt(RaiseStmt *S);

  void visitIfStmt(IfStmt *S);

  void visitWhileStmt(WhileStmt *S);

  void visitRepeatStmt(RepeatStmt *S);

  /// `for var x [: T] ...`: declares x in Loop, the scope of the loop. T is
  /// its type if it has none written (null: not known yet).
  void declareLoopVar(VarDecl *V, Type *T, Scope &Loop);

  void visitForStmt(ForStmt *S);

  /// A hidden variable of a for-in loop (code generation binds it), named
  /// in Sc only: its name is no Pascal identifier.
  VarDecl *forInVar(llvm::StringRef Name, Type *T, SourceLocation Loc,
                    Scope &Sc);

  NameRefExpr *forInRef(VarDecl *V, SourceLocation Loc);

  /// Base.Name (Name as written in Free Pascal), at Loc.
  FieldExpr *forInMember(Expr *Base, llvm::StringRef Spelling,
                         SourceLocation Loc);

  /// The member Name of a class, advanced record or interface, or null.
  Decl *findEnumMember(Type *T, llvm::StringRef Name);

  /// for Var in Collection do Body (Turbo/FPC), as in Free Pascal: Var
  /// takes the elements of an array or string, the members of a set in
  /// order, the values of an ordinal type, or the values of an enumerator.
  /// Var := element is an ordinary assignment (Step).
  void visitForInStmt(ForInStmt *S);

  /// A for-in loop over CT through Collection.GetEnumerator (its own, or its
  /// helper's): a function without arguments whose result has a function
  /// MoveNext: boolean and a property Current. False after an error.
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

  void visitCaseStmt(CaseStmt *S);

  void visitWithStmt(WithStmt *S);

  //===--------------------------------------------------------------------===//
  // Expressions (each returns the expression's type)
  //===--------------------------------------------------------------------===//

  Type *visitIntegerLiteralExpr(IntegerLiteralExpr *E);

  Type *visitRealLiteralExpr(RealLiteralExpr *) { return Ctx.getRealType(); }

  Type *visitStringLiteralExpr(StringLiteralExpr *E);

  Type *visitNameRefExpr(NameRefExpr *E);

  Type *visitParenExpr(ParenExpr *E) { return check(E->getInner()); }

  /// move p: the object of the owned variable p of this routine, which
  /// gives it up.
  Type *visitMoveExpr(MoveExpr *E);

  /// vector[N] of boolean: what lane-by-lane comparisons give (a mask).
  llvm::DenseMap<unsigned, VectorType *> BooleanVectors;
  VectorType *booleanVector(unsigned N);

  Type *visitUnaryExpr(UnaryExpr *E);

  /// obj is TFoo / obj as TFoo (objfpc); the left operand may also be a
  /// COM interface (its object).
  Type *checkIsAs(BinaryExpr *E);

  static bool isMethodPointer(Type *T);

  /// An operand of = or <>: a method pointer variable is compared, not called.
  Type *checkComparand(Expr *E);

  /// The text of a string or char constant (S), if E is one.
  bool constantText(Expr *E, std::string &S);

  /// A + B of string or char constants: the constant it is (a hidden
  /// constant declaration E refers to); its type, or null if it is not one.
  Type *foldConcatenation(BinaryExpr *E);

  Type *visitBinaryExpr(BinaryExpr *E);

  Type *visitCallExpr(CallExpr *E);

  /// `a ${x} b`: an ansistring; each value is one `write` can write, with
  /// the field width and precision it takes there.
  Type *visitTemplateExpr(TemplateExpr *E);

  /// Name := value where the parameters have no names to give (a standard
  /// routine, a typecast).
  Type *visitNamedArgExpr(NamedArgExpr *E);

  Type *visitFormattedExpr(FormattedExpr *E);

  Type *visitIndexExpr(IndexExpr *E);

  /// One more index for the array property of Pend: while indexes are
  /// missing, E itself is pending (if more follow); with all of them, they
  /// are checked like arguments and E is the property access.
  Type *checkPropertyIndex(IndexExpr *E, PendingIndex Pend);

  Type *visitNilExpr(NilExpr *) { return Ctx.getNilType(); }

  /// Checks a value assigned to a target of type TargetTy. A procedural
  /// variable named as the value of a procedural target is copied, not
  /// called.
  /// [a, b, c] given to a dynamic array of type T.
  Type *checkDynArrayCtor(SetCtorExpr *S, DynArrayType *T);

  Type *checkValueFor(Type *TargetTy, Expr *Value);

  /// The procedural type of a call through a designator, or null after a
  /// diagnostic.
  RoutineType *checkIndirectCallee(IndirectCallExpr *E);

  Type *visitIndirectCallExpr(IndirectCallExpr *E);

  Type *visitTupleExpr(TupleExpr *E) { return checkTuple(E, nullptr); }

  Type *visitCondExpr(CondExpr *E) { return checkCond(E, nullptr); }

  /// Can a value of type T be nil, and be tested for it (?., ??)?
  static bool isNilable(Type *T);

  /// A reference to hidden variable V (of type T) at Loc.
  NameRefExpr *hiddenRef(VarDecl *V, SourceLocation Loc);

  Type *visitSafeNavExpr(SafeNavExpr *E) { return checkSafeNav(E); }

  /// x?.m: m's type (null for a statement's call).
  Type *checkSafeNav(SafeNavExpr *E);

  /// a ?? b: a, or b where a is nil. The type: a's, b's value being
  /// assignable to it (or b's, if a's is).
  Type *visitCoalesceExpr(CoalesceExpr *E);

  /// if c then a else b, a value. Want: the type it is a value for, which
  /// both branches then take; where they cannot, and without Want, its
  /// type is the common type of the branches. It becomes an if statement
  /// that assigns the branch chosen to a hidden variable.
  Type *checkCond(CondExpr *E, Type *Want);

  /// (a, b): a tuple value. Want: the tuple type it is a value for (with
  /// as many elements), whose element types its elements then take; where
  /// they cannot, and without Want, each element has the type a variable
  /// declared with its value would have.
  Type *checkTuple(TupleExpr *E, RecordType *Want);

  Type *visitAggregateExpr(AggregateExpr *E);

  /// @obj.M, @TFoo.M, or @M in a method: a method pointer. Null if the
  /// operand is not a method.
  Type *checkMethodAddr(AddrOfExpr *E);

  /// The lambda A (function(x) => e), used where a value of type Want is
  /// wanted (null: none): its hidden routine is declared here, nested in
  /// the current routine and in the current scope. Parameter types left
  /// out are Want's; the result type left out is Want's, or else the type
  /// of the body (set by visitAssignStmt). Then A is @routine.
  void completeLambda(AddrOfExpr *A, RoutineType *Want);

  /// Lambda R (or a copy factory), its types known: declared here, nested
  /// in the current routine, and its body checked now, in the middle of a
  /// statement.
  void declareAndCheckLambda(RoutineDecl *R);

  /// The copy factory F of a lambda that copies what it uses, called by E
  /// where Want is wanted: a closure must be. Its parameters, the copies,
  /// take the types of the variables they copy (read-only); its result is
  /// Want, its body makes the lambda (completed for Want). False after a
  /// diagnostic.
  bool completeCopyFactory(RoutineDecl *F, CallExpr *E, Type *Want);

  /// Lambda R (at A) wanted as a method pointer: it gets a Self of its own,
  /// of the class of the method it is written in (the nearest routine
  /// around it with a Self, which must be an object), and that method's
  /// Self is its object. Reported (and R bad) if there is none.
  bool makeMethodLambda(RoutineDecl *R, AddrOfExpr *A);

  /// A use, in a lambda, of D (named Id, found through W): if D belongs to
  /// a routine around the lambda (a variable, a member through Self or a
  /// with statement, a nested routine), the lambdas up to that routine
  /// must not outlive it (RoutineDecl::getCapture).
  ///
  /// A closure (a lambda made a `reference to` value) may outlive them:
  /// their frames it uses are then kept on the heap (setHeapFrame), and
  /// it must not use what does not live in a frame (closureCannotUse).
  void noteLambdaUse(Decl *D, WithContext *W, const Identifier &Id);

  /// The innermost spawn block around the code being checked, if R (a
  /// routine; null: the program) is outside it: the task that would share
  /// R's variables. Null otherwise.
  RoutineDecl *spawnOutside(RoutineDecl *R) const;

  /// D (used through W) in a spawn block: an isolated variable of another
  /// task is an error; under {$modeswitch racechecks}, so is anything
  /// mutable that is not shared: a variable of the routines around it or
  /// of the program, Self's members (unless its class is thread-safe), a
  /// routine nested in another (which may use its variables).
  void noteTaskUse(Decl *D, WithContext *W, const Identifier &Id);

  /// E an isolated variable (by name): it cannot be copied, only moved.
  void checkIsolatedCopy(Expr *E, const char *Where);

  /// Why a closure cannot use V, a variable of a routine around it (empty:
  /// it can): it is not kept in that routine's frame, or is not counted
  /// there.
  static std::string closureCannotUse(VarDecl *V);

  //===--------------------------------------------------------------------===//
  // Tasks (objfpc): async routines, await, spawn, scope, detached
  //===--------------------------------------------------------------------===//

  /// Nesting of scope blocks around the code being checked (spawn needs
  /// one).
  unsigned ScopeDepth = 0;

  /// Is the code being checked in an async routine (or a lambda in one)?
  bool inAsyncRoutine() const;

  /// Is E (checked) the call of an async routine?
  static bool isAsyncCall(Expr *E);

  void warnUnusedIterator(const Identifier &Callee, SourceRange Range);

  void warnDanglingTask(const Identifier &Callee, SourceRange Range);

  /// await e: e a task; its result (NeedValue: one is needed).
  Type *checkAwait(AwaitExpr *E, bool NeedValue);

  Type *visitAwaitExpr(AwaitExpr *E) { return checkAwait(E, true); }

  /// The hidden async procedure of spawn begin ... end: declared and
  /// checked where it is spawned (it uses the variables declared so far).
  void completeSpawnBlock(RoutineDecl *R);

  Type *visitSpawnExpr(SpawnExpr *E);

  void visitExprStmt(ExprStmt *S);

  void visitScopeStmt(ScopeStmt *S);

  /// yield e / yield break / yield from it, in an iterator routine (not in
  /// a lambda in one, nor in a finally part).
  void visitYieldStmt(YieldStmt *S);

  void visitDetachedStmt(DetachedStmt *S);

  /// Wait(t) (also as a statement: NeedValue false), WhenAll(tasks),
  /// WhenAny(tasks).
  Type *checkWait(llvm::ArrayRef<Expr *> Args, const Identifier &Callee,
                  SourceRange Range, bool NeedValue);

  /// The tasks of WhenAll / WhenAny: an array of tasks (static, dynamic or
  /// open); their task type, or null after a diagnostic.
  TaskType *checkTaskArray(Expr *A, const Identifier &Callee);

  /// A lambda that uses what belongs to the routines around it must not
  /// outlive them: reported if Value is one and is stored (Local: in a
  /// local variable of the current routine, which is allowed).
  void checkLambdaEscape(Expr *Value, bool Local);

  Type *visitAddrOfExpr(AddrOfExpr *E);

  Type *visitDerefExpr(DerefExpr *E);

  Type *visitSetCtorExpr(SetCtorExpr *E);

  /// TFoo in TFoo.Create (objfpc): a class named before '.'.
  /// The class Base names (TFoo.Create), or for an advanced record name
  /// (TR.Create) its member table (Base's type: the record).
  ClassType *classNamedBy(Expr *Base);

  /// The type that Base names (TFoo, or a nested TOuter.TInner), or null.
  TypeDecl *typeNamedBy(Expr *Base);

  /// TColor.Red: the value of the enumeration type E's base names (any
  /// enumeration's; the only way to a scoped one's, {$scopedenums on}).
  /// Null if the base does not name an enumeration type (or names one with
  /// a helper, and not one of its values: TColor.Helped), or after a
  /// diagnostic (Reported).
  Decl *enumValueNamedBy(FieldExpr *E, bool &Reported);

  /// AnsiString.Join: a static method or constant of the helper H of the
  /// type E's base names.
  Decl *resolveHelperStatic(FieldExpr *E, ClassType *H);

  /// Resolves r.f / obj.member / TFoo.Create; null after a diagnostic.
  /// (and, in null-safe code, its object must not be nil)
  Decl *resolveMember(FieldExpr *E);

  Decl *resolveMemberImpl(FieldExpr *E);

  /// E (obj.M, obj.specialize M<T>): a generic method of C or its
  /// ancestors, specialized for E's type arguments if it gives them; null
  /// if there is none (or after a diagnostic, Reported).
  Decl *genericMethodOf(FieldExpr *E, ClassType *C, const ClassType *&Owner);

  /// "; did you mean 'X'?" for a member name of C: the closest member of
  /// C or its ancestors (ties: the alphabetically first).
  std::string didYouMeanMember(ClassType *C, llvm::StringRef Name);

  /// A property used as a value: it needs a read accessor.
  Type *checkPropertyRead(PropertyDecl *P, Expr *E, const Identifier &Id);

  /// p.Free or p.Destroy of an owned variable p (already resolved): an
  /// error, since its object is freed again when the routine ends.
  bool diagOwnedFree(FieldExpr *E);

  Type *visitFieldExpr(FieldExpr *E);

  /// The value of calling method R through E (obj.M or TFoo.Create): a
  /// function's result, or the new object of a constructor called through
  /// its class; null if the call has no value.
  static Type *methodResultType(RoutineDecl *R, FieldExpr *E);

  /// Does E name a type (TFoo, TOuter.TInner), as resolved?
  static bool namesType(Expr *E);

  /// Is Callee obj.M / TFoo.Create / inherited M -- a method call rather
  /// than a call through a procedural value?
  bool isMethodCallee(Expr *Callee);

  /// obj.M(args), TFoo.Create(args), inherited M(args): the result type,
  /// null for a call without a value, or the error type.
  Type *checkMethodCall(IndirectCallExpr *E);

  /// inherited [Name]: the inherited method, or null (after a diagnostic,
  /// or with Nothing set for a bare `inherited` without an inherited method,
  /// which does nothing).
  RoutineDecl *resolveInherited(InheritedExpr *I, bool &Nothing);

  /// inherited [Name] [(Args)] called: the result type or null.
  Type *checkInheritedCall(InheritedExpr *I, IndirectCallExpr *Call,
                           SourceRange Range);

  /// inherited Name used as a value: a function without arguments.
  Type *visitInheritedExpr(InheritedExpr *E);
};

} // namespace p3::detail

#endif // P3_SEMA_CHECKER_H
