// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGenInternal.h - Internal CodeGen declarations --------*- C++ -*-===//
//
// Shared declarations of p3::detail::{Runtime, PinAnalysis, CodeGenModule}
// across the CodeGen translation units.
//
//===----------------------------------------------------------------------===//
#ifndef P3_CODEGEN_CODEGENINTERNAL_H
#define P3_CODEGEN_CODEGENINTERNAL_H

#include "p3/CodeGen/CodeGen.h"
#include "p3/AST/ASTVisitor.h"
#include "p3/AST/Type.h"
#include "p3/Basic/Directives.h"
#include "p3/Basic/LangOptions.h"
#include "p3/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

namespace p3::detail {

/// Adds to the runtime function F the attributes that are true of it: its
/// memory effects and return, plus those of its parameters (item 9).
void addRuntimeAttrs(llvm::Function *F);

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
      StrDelete, StrInsert, WritePStr, ReadStr, FreeMem, Realloc, Halt,
      ParamCount, ParamStr;

  explicit Runtime(llvm::Module &M);
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
  /// Leave out the targets of plain `x := e` (otherWrites).
  bool SkipPlainAssigns = false;
  /// Leave out indexed writes `a[i] := e` when checking whether array
  /// lengths/bases themselves change (hoistableBases).
  bool SkipIndexWrites = false;

public:
  /// The variable of a plain `x := e`: a bare variable name, not reached
  /// through `with` and not an `absolute` overlay.
  static VarDecl *plainAssignTarget(Expr *E);

private:

  /// The variable whose storage an assignment to E writes, if local: x,
  /// r.f and a[i] write x, r and a (not the index).
  void addTarget(Expr *E, VarSet &Out);

  /// Calls that can write through a var/out parameter. A routine's parameter
  /// list says which arguments; builtins are handled by their known effects;
  /// an unknown call gets the conservative answer (every argument).
  void addCallWrites(Decl *D, llvm::ArrayRef<Expr *> Args, VarSet &Out);

  void addWrittenExpr(Expr *E, VarSet &Out);

  void addWrittenList(llvm::ArrayRef<Stmt *> Body, VarSet &Out);

  void addWrittenStmt(Stmt *S, VarSet &Out);

  /// The variables an assignment target reads to compute its address: a[i]
  /// reads a and i, p^ reads p, r.f reads r, but x := e does not read x.
  static void addTargetReads(Expr *E, VarSet &Out);

  static void addUsed(Expr *E, VarSet &Out);

  static void addUsedList(llvm::ArrayRef<Stmt *> Body, VarSet &Out);

  static void addUsedStmt(Stmt *S, VarSet &Out);

  void pinList(llvm::ArrayRef<Stmt *> Body, VarSet After);

  void pinStmt(Stmt *S, const VarSet &After);

public:
  explicit PinAnalysis(RoutineDecl *R) : R(R) {}

  /// Adds the variables that E reads or names (optimization3 item 1).
  static void usedBy(Expr *E, VarSet &Out) { addUsed(E, Out); }

  /// Adds the variables that S may write (optimization3 item 4).
  void writtenBy(Stmt *S, VarSet &Out) { addWrittenStmt(S, Out); }

  /// Adds variables that S may write directly (excluding indexed element
  /// writes like `a[i] := x`, which cannot change `a`'s base or length).
  void directWrites(Stmt *S, VarSet &Out) {
    SkipIndexWrites = true;
    addWrittenStmt(S, Out);
    SkipIndexWrites = false;
  }

  /// The variables that S writes other than by a plain `x := e`: through
  /// a var argument, @x, a for loop, an overlay... (optimization3 item 4).
  VarSet otherWrites(Stmt *S);

  VarSet run();
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
  /// TBAA metadata nodes & access tags ("Simple Pascal TBAA").
  llvm::MDNode *TBAARoot = nullptr;
  llvm::MDNode *TBAACharNode = nullptr;
  llvm::MDNode *TBAA_Char = nullptr;
  llvm::MDNode *TBAA_Bool = nullptr;
  llvm::MDNode *TBAA_I16 = nullptr;
  llvm::MDNode *TBAA_I32 = nullptr;
  llvm::MDNode *TBAA_I64 = nullptr;
  llvm::MDNode *TBAA_F64 = nullptr;
  llvm::MDNode *TBAA_Set = nullptr;
  llvm::MDNode *TBAA_Ptr = nullptr;
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
  /// The current routine's local variables whose range is followed from
  /// store to store (optimization3 item 4): signed ordinals written only
  /// by plain `x := e`, never captured, overlaid or address-taken. Empty in
  /// a routine with labels.
  VarSet RangeLocals;
  /// Inside a for loop: the range such a variable keeps throughout it (an
  /// accumulator, `x := x + c`), which a forgotten range falls back to.
  llvm::DenseMap<VarDecl *, ValueRange> LoopRanges;
  struct LengthBound {
    VarDecl *Idx = nullptr;
    VarDecl *Base = nullptr;
    int64_t Slack = 0;
  };
  struct ConstBound {
    VarDecl *V = nullptr;
    bool HasLo = false;
    int64_t Lo = 0;
    bool HasHi = false;
    int64_t Hi = 0;
  };
  struct RangeState {
    llvm::DenseMap<VarDecl *, ValueRange> VarRanges;
    llvm::DenseMap<VarDecl *, VarDecl *> VarLengthOf;
    llvm::SmallVector<LengthBound, 4> ActiveLengthBounds;
  };
  VarSet ImmutableAnsiVars;
  llvm::DenseMap<VarDecl *, VarDecl *> VarLengthOf;
  llvm::SmallVector<LengthBound, 4> ActiveLengthBounds;
  /// The exact class of an object pointer when it is a direct constructor
  /// result (optimization2 item 10).
  llvm::DenseMap<const llvm::Value *, ClassType *> KnownClass;
  /// A direct call whose ansistring result is moved into the assignment
  /// target: no temporary holds it (NoHoldAnsiResult), and the call that
  /// returned it took that path (TookAnsiResult).
  bool NoHoldAnsiResult = false;
  bool TookAnsiResult = false;
  /// True while emitting the tail statement of a function body with no
  /// cleanups or non-local labels (allows moving local ansistrings to Result).
  bool AtRoutineTail = false;
  /// Unmutated ansistring value parameters of the current routine whose
  /// caller references remain live (no prologue addref / epilogue clear).
  VarSet BorrowedAnsiParams;
  struct AnsiSliceVal {
    llvm::Value *Ptr = nullptr;
    llvm::Value *Len = nullptr;
  };
  struct SliceLocalSlots {
    llvm::Value *Ptr = nullptr;
    llvm::Value *Len = nullptr;
  };
  struct SliceSpec {
    RoutineDecl *R = nullptr;
    uint32_t Mask = 0;
    llvm::Function *F = nullptr;
  };
  llvm::DenseMap<RoutineDecl *, llvm::SmallVector<VarDecl *, 4>>
      RoutineSliceLocals;
  llvm::DenseMap<VarDecl *, SliceLocalSlots> SliceLocals;
  llvm::DenseMap<VarDecl *, AnsiSliceVal> SliceParams;
  llvm::DenseMap<std::pair<RoutineDecl *, uint32_t>, llvm::Function *>
      SliceSpecFns;
  std::vector<SliceSpec> SliceSpecsToEmit;
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

  llvm::DIFile *diFileFor(SourceLocation L);
  /// Separate units (S11): the symbols of units' interface variables and
  /// routines, and those of units compiled separately (declared only).
  llvm::DenseMap<Decl *, std::string> ExportNames;
  llvm::DenseSet<Decl *> ImportedDecls;

  void collectExports(UnitDecl &U, bool Imported);

  static std::string initName(UnitDecl &U);

  /// A unit-level or program variable's global (or a class variable's,
  /// pas.<class>.<name>).
  llvm::GlobalVariable *createGlobal(VarDecl *V, llvm::Type *Ty,
                                     llvm::Constant *Init);

  llvm::DenseMap<RoutineDecl *, unsigned> JmpBufSlots; ///< frame slot
  llvm::GlobalVariable *MainJmpBuf = nullptr;

  /// Generous for every supported libc (glibc: 200-312 bytes, macOS: 192).
  llvm::Type *jmpBufType() { return llvm::ArrayType::get(B.getInt64Ty(), 64); }

  /// The interface D defines (not a forward declaration), or null.
  static InterfaceType *definedInterface(Decl *D);

  /// The class D defines (a type declaration `TFoo = class ... end`, not a
  /// forward one), or null.
  /// The member table of the advanced record D declares (or the type
  /// helper D declares), or null.
  static ClassType *advancedRecord(Decl *D);

  /// Does E name a type (TFoo, TOuter.TInner)?
  static bool namesType(Expr *E);

  /// Decls, each followed by the types declared in it when it is a class
  /// or an advanced record (recursively).
  static llvm::SmallVector<Decl *, 16> withNested(llvm::ArrayRef<Decl *> Decls);

  static ClassType *definedClass(Decl *D);

  static bool hasNonLocalLabels(Block &Blk);
  static bool stmtHasTry(Stmt *S);

  //===--------------------------------------------------------------------===//
  // Types
  //===--------------------------------------------------------------------===//

  /// In-memory representation of a Pascal type.
  llvm::Type *memType(Type *T);

  llvm::Type *lowerType(Type *T);

  /// Sets are bit masks: bit i is set iff ord(i) is a member. 32, 64 or
  /// 256 bits, by the element type's highest value (SetType::
  /// getStorageBits; optimization.md, item 6).
  static unsigned setBits(Type *T);
  llvm::IntegerType *setType(Type *T) { return B.getIntNTy(setBits(T)); }
  /// The set value V of type From as a value of the set type To (the
  /// members that do not fit are dropped).
  llvm::Value *convertSet(llvm::Value *V, Type *From, Type *To);

  /// Register representation: like memType, but booleans are i1.
  llvm::Type *regType(Type *T);

  /// Builds the struct for a record field list and records each field's
  /// byte offset from the start of the record. Variants are laid out as
  /// their own structs, all overlaying one storage buffer.
  llvm::StructType *layoutFieldList(FieldList *FL, const std::string &Name,
                                    uint64_t BaseOffset);

  /// The alignment of a value of type T (a field's natural alignment).
  uint64_t alignOf(Type *T) { return DL.getABITypeAlign(memType(T)).value(); }

  /// Lays out the fields of FL from byte End as Free Pascal does under
  /// {$packrecords N}: each field at a multiple of the smaller of its
  /// alignment and N; a variant part (each variant from the same offset) at
  /// a multiple of the smaller of N and 16. End becomes the end of the
  /// last field; Align the largest alignment used.
  void layoutAligned(FieldList *FL, uint64_t N, uint64_t &End,
                     uint64_t &Align);

  /// Lays out the fields of a bitpacked record from bit Start, as Free
  /// Pascal does: ordinals take their bitPackedWidth bits, one after
  /// another; other fields and the variant part start at a byte. Returns
  /// the bit after the last field.
  uint64_t layoutBitPacked(FieldList *FL, uint64_t Start);

  void shiftFieldOffsets(FieldList *FL, uint64_t Delta);

  uint64_t sizeOf(Type *T) { return DL.getTypeAllocSize(memType(T)); }

  /// What pointer arithmetic on a PT counts in: its pointee (bytes, for an
  /// untyped pointer).
  llvm::Type *pointeeMemType(PointerType *PT);

  /// The layout of an object of class C: VMT pointer, then all fields.
  /// Records each field's offset; a prefix of fields has the same offsets
  /// in every descendant.
  llvm::StructType *classLayout(ClassType *C);

  uint64_t instanceSize(ClassType *C);

  //===--------------------------------------------------------------------===//
  // Scalars in memory
  //===--------------------------------------------------------------------===//

  /// The addresses (in the current function) of fields and elements that
  /// a field alignment ({$packrecords}, packed) leaves less aligned than
  /// their types: the alignment they have. Loads, stores and copies
  /// through them say so.
  llvm::DenseMap<llvm::Value *, uint64_t> AddrAligns;

  /// The alignment of the value of type T at address P.
  uint64_t addrAlign(llvm::Value *P, Type *T);

  /// Notes that the component of type T at Addr, Offset bytes into a value
  /// aligned to BaseAlign, may be less aligned than T.
  void noteComponentAlign(llvm::Value *Addr, uint64_t BaseAlign,
                          uint64_t Offset, Type *T);

  llvm::MDNode *tbaaCharScalarNode();
  llvm::MDNode *tbaaTagFor(Type *T);
  static bool isAliasedExpr(const Expr *E, bool DirectDeref = true);

  llvm::Value *loadScalar(llvm::Value *Ptr, Type *T,
                          const llvm::Twine &Name = "", bool AllowTBAA = true);

  void storeScalar(llvm::Value *Ptr, llvm::Value *Val, Type *T,
                   bool AllowTBAA = true);

  /// Copies the array, record or string T: both are aligned as T is (or as
  /// AddrAligns says).
  void copyAggregate(llvm::Value *Dst, llvm::Value *Src, Type *T);

  /// Address of a private constant holding S (without a terminating NUL).
  llvm::GlobalVariable *getStringConstant(llvm::StringRef S);

  //===--------------------------------------------------------------------===//
  // Runtime checks
  //===--------------------------------------------------------------------===//

  std::string where(SourceLocation L);

  llvm::BasicBlock *newBlock(const llvm::Twine &Name);

  /// The .rterr global of a message (shared by every check that uses it).
  llvm::Constant *errorMessage(llvm::StringRef Msg);

  /// The .file global of a source file name (one per file, item 13).
  llvm::Constant *fileString(llvm::StringRef File);

  /// The pooled { file, line, column } record of L, or null (item 13).
  llvm::Constant *locationRecord(SourceLocation L);

  /// __pas_runtime_error_at("<Msg>", &loc); unreachable. Callers must set
  /// a new insertion point afterwards.
  void emitRuntimeError(llvm::StringRef Msg, SourceLocation L);

  /// if (Cond) __pas_runtime_error("<Msg> at file:line:col");
  void emitRuntimeCheck(llvm::Value *Cond, llvm::StringRef Msg,
                        SourceLocation L);

  /// Are range / overflow checks on at L? (command line, then {$R±}/{$Q±})
  bool rangeChecks(SourceLocation L) const;
  /// In an unchecked block at L: integer values converted to a narrower
  /// integer type wrap instead of being range-checked.
  bool wraps(SourceLocation L) const;
  /// In a checked block or expression at L (Wrap explicitly off): integer
  /// values converted to a narrower integer type are checked, whatever
  /// {$R} says.
  bool checksConversions(SourceLocation L) const;
  bool overflowChecks(SourceLocation L) const;
  bool pointerMathChecked(SourceLocation L) const;
  void emitPointerAddCheck(PointerType *PT, llvm::Value *Base,
                           llvm::Value *Delta, SourceLocation L);
  void emitPointerIndexCheck(PointerType *PT, llvm::Value *Base,
                             llvm::Value *Index, SourceLocation L);
  void emitPointerDiffCheck(llvm::Value *LHS, llvm::Value *RHS,
                            SourceLocation L);

  /// Ordinal values compare as signed if their host is a signed integer
  /// type, unsigned otherwise (byte, word, cardinal, char, boolean, enum).
  static bool isSignedOrdinal(Type *T);

  /// Converts an ordinal register value of type From to the register type of
  /// To (sign- or zero-extending by From, or truncating). No range check.
  llvm::Value *convertOrdinal(llvm::Value *V, Type *From, Type *To);

  /// A widechar's code as a char, as Free Pascal converts it: '?' above
  /// 255.
  llvm::Value *wideToChar(llvm::Value *V);

  /// A char or widechar expression's value as a char.
  llvm::Value *emitCharValue(Expr *E);

  /// A character as the (pointer, count) piece of a join: an own entry
  /// byte holding it, instead of a fresh one-character string.
  llvm::Value *emitCharPiece(Expr *E);

  /// The loop's hoist scope that covers the index's base: the array whose
  /// length check is hoisted, innermost loop last.
  /// The loop's hoist scope that covers the index's base: the array whose
  /// length this for checks (null: none). Innermost loop last.
  const std::pair<VarDecl *, llvm::Value *> *checkedScopeOf(const Expr *E) const;

  /// An integer expression as i32 (field widths, string positions).
  llvm::Value *emitI32(Expr *E);

  /// The no-wrap flags for an element index: `nuw` when the index is a
  /// plain variable whose range the analysis knows at or above LoNeeded
  /// and small enough that the scaled offset stays inside 64 bits
  /// (llvm-23.md LB-1). Inbounds is the GEP's own.
  llvm::GEPNoWrapFlags indexFlags(Expr *Idx, int64_t LoNeeded);

  static unsigned intBits(Type *T);

  /// Widens an ordinal register value to i64 for range comparisons.
  llvm::Value *toI64(llvm::Value *V, Type *T);

  /// The range of an already emitted integer value: a literal, a load of an
  /// active loop variable, or a result noted by the arithmetic below.
  ValueRange rangeOfValue(const llvm::Value *V) const;

  /// (Not for a constant: those are shared, and rangeOfValue knows their
  /// exact range.)
  void noteRange(const llvm::Value *V, ValueRange R);

  /// Sets (or, R unknown, forgets) the range of a variable of RangeLocals.
  /// Inside a loop that bounds it, a forgotten range is that bound.
  void setVarRange(VarDecl *V, ValueRange R);

  /// The variables of RangeLocals that S may write.
  VarSet rangeLocalsWrittenBy(Stmt *S);

  void forgetRanges(const VarSet &Vars);

  //===--- LB-5 (localdocs/llvm-23.md): one length check per loop for the
  // counted types. A for loop whose body indexes a dynamic array, an
  // open array or an array of const with the bare control variable only,
  // and whose body provably cannot change the array, checks the length
  // once at the loop's entry; the accesses inside run without their
  // per-element checks, which unblocks LLVM's vectorizer. The walk bails
  // (and the accesses keep their own checks) for anything it cannot
  // follow: indirect calls and lambdas (unseen writes), spawn blocks and
  // async calls (the task may outlive and reach the array), await,
  // detached, yields, move (the variable nils), `@x` and `x^` (an
  // escape), memory-management calls, goto targets (labels) and
  // break/continue/exit (whose flow would make an out-of-range first
  // access skip the per-element check the hoist replaces).

  /// The counted-type bases whose checks the for loop hoists: the array
  /// variable with its loaded length (null for open arrays, whose count
  /// rides the argument descriptor), innermost loop last.
  llvm::SmallVector<std::pair<VarDecl *, llvm::Value *>, 2> CheckedBases;

  /// A bare variable reference, unwrapping parens and const wrappers.
  static NameRefExpr *plainVarRef(const Expr *E);

  /// The base's variable when E reads or writes a counted type (dynamic
  /// array, open array, array of const) through a plain variable of the
  /// routine itself (never captured: a nested routine or a spawn block
  /// could write it unseen).
  VarDecl *countedBase(const Expr *E) const;

  /// The hoist's state for one walked loop: bail = anything unseen was
  /// met; the bases' element accesses, with the bare control variable's
  /// bareness and the unconditional-ness of the access's statement.
  struct HoistEntry {
    VarDecl *V = nullptr;
    Expr *Base = nullptr;
    bool Bare = true;
    bool Conditional = false;
    bool BadChecks = false;
  };

  struct HoistWalk {
    bool Bail = false; // a global unseen: no hoisting at all
    bool HasCall = false;
    bool WantChecks = false;
    // Deterministic AST-order list of counted bases seen in the loop.
    llvm::SmallVector<HoistEntry, 4> Entries;
    void touch(VarDecl *V, Expr *Base, bool Bare, bool Unconditional,
               bool ChecksMatch) {
      for (HoistEntry &E : Entries) {
        if (E.V == V) {
          E.Bare = E.Bare && Bare;
          E.Conditional = E.Conditional || !Unconditional;
          E.BadChecks = E.BadChecks || !ChecksMatch;
          return;
        }
      }
      Entries.push_back({V, Base, Bare, !Unconditional, !ChecksMatch});
    }
  };

  void hoistWalk(const Stmt *S, HoistWalk &W, bool Unconditional) const;

  void hoistExpr(const Expr *E, HoistWalk &W, bool Unconditional) const;

  VarDecl *CurForVar = nullptr;

  /// The bases the loop hoists: every access's index is the bare control
  /// variable, unconditional, with nothing that bails. Null on none.
  llvm::SmallVector<std::pair<VarDecl *, Expr *>, 2>
  hoistableBases(ForStmt *S);

  /// Chooses RangeLocals for R, each starting at 0 (the value every scalar
  /// local starts with, see emitZeroInit).
  void initRangeLocals(RoutineDecl *R);
  void endRangeLocals();
  void invalidateRangeLocalLength(VarDecl *V);
  static VarDecl *plainAnsiVar(Expr *E);
  VarDecl *matchLengthExpr(Expr *E) const;
  bool matchVarPlusConst(Expr *E, VarDecl *&V, int64_t &Off) const;
  void collectLengthBounds(Expr *E, llvm::SmallVectorImpl<LengthBound> &Out) const;
  void applyLengthBounds(llvm::ArrayRef<LengthBound> Bounds);
  void collectConstBounds(Expr *E, llvm::SmallVectorImpl<ConstBound> &Out) const;
  void applyConstBounds(llvm::ArrayRef<ConstBound> Bounds);
  RangeState saveRangeState() const;
  void restoreRangeState(const RangeState &S);
  bool canElideAnsiBoundsCheck(Expr *Base, Expr *Idx) const;
  static bool exprWritesVar(Expr *E, VarDecl *V);
  bool monotonicNonNegSteps(Stmt *S, VarDecl *V) const;
  bool evalExprRange(Expr *E, VarDecl *V, ValueRange VR,
                     const VarSet *Written, ValueRange &Out) const;
  bool collectModUpperBound(Stmt *S, VarDecl *V, int64_t &OutM,
                            bool &HasWrite) const;
  bool nonNegModStep(Stmt *S, VarDecl *V, int64_t M,
                     const VarSet *Written, bool &HasWrite) const;
  bool nonNegModLoop(Stmt *S, VarDecl *V, int64_t InitHi,
                     const VarSet *Written, int64_t &OutM) const;

  /// An integer constant written as a literal, a named constant or one of
  /// those negated: its value.
  static bool constInt(Expr *E, int64_t &Out);

  /// V := V + c, V := c + V or V := V - c, for a constant c: its step.
  static bool accumulatorStep(AssignStmt *A, VarDecl *V, int64_t &Step);

  /// Does every write of V in S (a for loop's body) add a constant to it,
  /// each at most once per iteration (not inside an inner loop)? Adds the
  /// possible change per iteration to [DLo, DHi]; false if that overflows
  /// int64 (as everywhere in this rule: the self-hosted compiler has no
  /// wider integers, and must come to the same answer).
  bool accumulatorSteps(Stmt *S, VarDecl *V, int64_t &DLo, int64_t &DHi);

  /// The bounds of a signed integer type of Bits bits.
  static bool signedRange(unsigned Bits, ValueRange &R);

  static bool inInt64(__int128 V);

  /// The range of A op B (add, sub or mul), when it fits int64.
  static bool combineRange(BinaryOp Op, ValueRange A, ValueRange B,
                           ValueRange &R);

  /// X < Lo or X > Hi, for an i64 X.
  ///
  /// Throughout this file, instructions are created one statement at a
  /// time, never as two arguments of one call: C++ leaves the order of
  /// argument evaluation unspecified, and the IR must not depend on the
  /// compiler that built p3 (the self-hosted compiler writes the same IR,
  /// SELFHOST.md S10).
  llvm::Value *outsideRange(llvm::Value *X, int64_t Lo, int64_t Hi);

  /// Checks that ordinal value V (of type From) lies within Target's bounds.
  void emitRangeCheck(llvm::Value *V, Type *From, Type *Target,
                      SourceLocation L,
                      llvm::StringRef Msg = "value out of range");

  /// Converts a value for assignment / parameter passing: integer -> real,
  /// and range-checks ordinal values (and set members) against the target.
  llvm::Value *convertForAssign(llvm::Value *V, Type *From, Type *To,
                                SourceLocation L);

  //===--------------------------------------------------------------------===//
  // Short strings (Turbo/FPC): values are pointers to length-prefixed bytes
  //===--------------------------------------------------------------------===//

  llvm::GlobalVariable *getPStrConstant(llvm::StringRef S);

  /// A string constant as an ansistring: static characters after a header
  /// {count -1, length}; null when empty.
  llvm::Constant *getAnsiConstant(llvm::StringRef S);

  /// A call of an ansistring routine of the runtime on Args.
  llvm::Value *astrCall(llvm::StringRef Name, llvm::Type *Ret,
                        llvm::ArrayRef<llvm::Value *> Args);

  /// Any string-like expression, or a pchar, as an ansistring (borrowed: a
  /// new one is held until the statement ends).
  llvm::Value *emitAnsi(Expr *E);

  static bool isMethodPointerType(Type *T);

  /// slice(a, n): the first n elements of a, as an open array.
  llvm::Value *emitSlice(CallExpr *C);

  /// The descriptor {data, count} of the open array parameter E.
  llvm::Value *openDescriptor(Expr *E);

  llvm::Value *openCount(llvm::Value *Desc);

  llvm::Value *newOpenDescriptor(llvm::Value *Data, llvm::Value *Count);

  /// The open array passed for Arg: another one, a slice built in a
  /// temporary, a call's own result, an array, or a string constant.
  llvm::Value *emitOpenArrayArg(Expr *Arg, OpenArrayType *OA);

  llvm::Value *emitOpenArrayCtor(SetCtorExpr *S, OpenArrayType *OA);

  /// A value parameter's own copy of the open array in frame slot Slot.
  void copyOpenArray(llvm::Value *Slot, OpenArrayType *OA);

  /// Runs Fn on the address of each element of the open array in slot Slot.
  void forEachOpenElement(llvm::Value *Slot, OpenArrayType *OA,
                          llvm::function_ref<void(llvm::Value *)> Fn,
                          llvm::StringRef What);

  /// The array of const passed for Arg: another array of const, or [...]
  /// built in a temporary: a header slot {count}, then {VType, value}.
  llvm::Value *emitConstArray(Expr *Arg);

  /// An element's value as stored in its TVarRec (Kind: its VType).
  llvm::Value *emitConstArrayValue(Expr *E, int64_t &Kind);

  llvm::Value *newStringTemp(unsigned MaxLen = 255);

  /// Ends the lifetime of the short-string temporaries made since Mark.
  void releaseStringTemps(size_t Mark);

  /// Any string-like expression as a pointer to a length-prefixed string.
  llvm::Value *emitPStr(Expr *E);

  void emitStringAssign(llvm::Value *Dst, ShortStringType *T, Expr *Value);

  llvm::Value *emitStringFunction(CallExpr *E, BuiltinKind K);

  //===--------------------------------------------------------------------===//
  // Sets
  //===--------------------------------------------------------------------===//

  /// The members Lo..Hi (within 0..255) as a set value of Bits bits.
  llvm::Constant *constRangeMask(int64_t Lo, int64_t Hi, unsigned Bits);

  /// Ordinal value -> bit index of a set of Bits bits, checking 0..255
  /// under range checks. (A set constructor's representation holds every
  /// value of its elements' type that is in 0..255.)
  llvm::Value *setIndex(llvm::Value *V, Type *T, SourceLocation L,
                        unsigned Bits);

  llvm::Value *emitSetCtor(SetCtorExpr *E);

  /// x in [a, b..c, ...] without building the set (optimization.md, item
  /// 6). The members given by constants are tested together: one value or
  /// one range by comparisons; values at most 63 apart by a bit of a
  /// 64-bit mask; up to four ranges by comparisons; else the bit of the
  /// 256-bit set. Each other element is a comparison (a range: two). All
  /// elements are evaluated, in order, after x.
  llvm::Value *emitInCtor(llvm::Value *X, SetCtorExpr *S);

  /// x in s  ==  x is within s's representation and bit x of s is set
  llvm::Value *emitIn(BinaryExpr *E);

  llvm::Value *toDouble(llvm::Value *V, Type *T);

  /// L op R (Add, Sub or Mul) in a signed or unsigned integer type, with
  /// an overflow check if enabled.
  llvm::Value *emitCheckedArith(BinaryOp Op, bool Signed, llvm::Value *L,
                                llvm::Value *R, SourceLocation Loc,
                                const llvm::Twine &Name);

  /// L op R with an overflow check (s/u add/sub/mul.with.overflow).
  llvm::Value *emitCheckedArith(llvm::Intrinsic::ID ID, llvm::Value *L,
                                llvm::Value *R, SourceLocation Loc,
                                const llvm::Twine &Name);

  /// if (Cond) integer overflow (What): a runtime error, or (objfpc) an
  /// EIntOverflow exception, as in Free Pascal with SysUtils.
  void emitOverflowCheck(llvm::Value *Cond, const std::string &What,
                         SourceLocation L);

  //===--------------------------------------------------------------------===//
  // Variables and frames
  //===--------------------------------------------------------------------===//

  /// Parameters passed as a pointer to the caller's variable: var
  /// parameters, const parameters of string and structured types, and
  /// constref parameters (to the caller's variable, or a temporary).
  bool isByRef(VarDecl *V);

  /// A constref parameter passed as a pointer where a const one is not:
  /// of a type other than structured, short string, open array, array of
  /// const or procedural (those are passed as for const).
  static bool isConstRefPtr(VarDecl *P);

  /// Is a function result of type T written through a pointer the caller
  /// passes (to a temporary of its own), instead of being returned? Short
  /// strings are: as a value LLVM would move all their 256 bytes, one by
  /// one. The function's result variable is then that temporary.
  bool returnsByPointer(Type *T);

  /// pas.<outer>.<name> for routines, pas.<class>.<name> for methods.
  static std::string mangle(RoutineDecl *R);

  /// "$N" for the N-th routine of an overloaded name (objfpc).
  static std::string overloadSuffix(RoutineDecl *R);

  /// Pointer to the frame of routine Target, reached from the current
  /// routine by following static links.
  llvm::Value *framePtrFor(RoutineDecl *Target);

  /// Address of the variable holding the PasFile* of input / output.
  llvm::GlobalVariable *standardFile(llvm::StringRef Name);

  llvm::Value *loadFile(llvm::Value *VarAddr);
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
  void pinInMemory(llvm::Value *Addr);

  /// Allocates a temporary in the entry block of the current function.
  llvm::Value *createTemp(llvm::Type *Ty, const llvm::Twine &Name = "tmp");

  /// Calls Fn(elementAddress) for every element of array A at Addr.
  void forEachElement(llvm::Value *Addr, ArrayType *A,
                      llvm::function_ref<void(llvm::Value *)> Fn,
                      llvm::StringRef What = "files");

  /// Creates (Init) or releases (!Init) the PasFile objects of all files in
  /// an object of type T at Addr: plain files, arrays of files, and file
  /// fields of records (fixed parts).
  void emitFileLifetime(llvm::Value *Addr, Type *T, bool Init,
                        llvm::Value *Name = nullptr);

  /// Creates the PasFile objects for the file variables among Decls.
  /// ParamIndex maps program-parameter names to their argv position.
  void initFileVars(llvm::ArrayRef<Decl *> Decls,
                    const llvm::StringMap<unsigned> *ParamIndex);

  /// The C library's errno is per thread, not a variable: reached through
  /// the library's accessor (__errno_location, __error, _errno).
  static bool isErrno(VarDecl *V);

  llvm::Value *errnoAddress();

  llvm::Value *varAddress(VarDecl *V);

  /// Where a routine's variable V is kept (a by-reference parameter: the
  /// pointer to it): a slot of its routine's frame record, or an alloca of
  /// the current routine.
  llvm::Value *varSlot(VarDecl *V);

  //===--------------------------------------------------------------------===//
  // Dynamic arrays
  //===--------------------------------------------------------------------===//

  llvm::Value *dynLength(llvm::Value *A);

  /// The constant PasDynInfo of arrays of type T (runtime/include/pascalrt.h);
  /// for managed elements, with functions that release and count one.
  llvm::Constant *dynInfoOf(DynArrayType *T);

  /// The constant PasDynInfo of values of type T (size, fini and addref,
  /// as for a dynamic array's elements): a task's result.
  llvm::Constant *typeInfoOf(Type *T);
  llvm::DenseMap<Type *, llvm::GlobalVariable *> TypeInfos;

  void emitDynOp(Type *Elem, llvm::Function *F, bool IsFini);

  /// The element functions of the array types used: last of all, as any
  /// code, the main program's too, can be the first to use a type.
  void emitDynOps();

  /// setlength of the array variable at Addr, of type T: Ns[0] elements,
  /// each of them (arrays) set to the next lengths.
  void emitDynSetLength(llvm::Value *Addr, DynArrayType *T,
                        llvm::ArrayRef<llvm::Value *> Ns);

  /// [a, b, c] as a new dynamic array of type T.
  llvm::Value *emitDynArrayCtor(SetCtorExpr *S, DynArrayType *T);

  /// Is V kept in its routine's frame record? Variables that nested
  /// routines use are, and those that must be in memory when an exception
  /// arrives through _longjmp: all of them in a routine where execution
  /// continues afterwards (try statements, non-local labels), else the
  /// managed ones, which are released on the way out. The other variables
  /// get an alloca each, which the optimizer turns into registers.
  bool inFrame(VarDecl *V, bool PinAll, const VarSet &PinVars);

  /// The start value of a local variable or function result at Addr: zero.
  /// ISO leaves them undefined; this makes programs deterministic. Without
  /// zero initialization (-fno-zero-init, the default from -O1) arrays,
  /// records and strings are only cleared when they hold managed values or
  /// files; a string starts empty.
  void emitZeroInit(llvm::Value *Addr, Type *T);

  /// Address of field F of record type RT at RecordAddr. Field offsets are
  /// recorded when the record is laid out, which may not have happened yet
  /// (a record reached only through pointers), so lay it out first.
  llvm::Value *fieldAddress(llvm::Value *RecordAddr, Type *RT, FieldDecl *F);

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

  BitRef bitRef(Expr *E);

  /// The bytes holding R's bits, loaded and stored as one integer (Ty, at
  /// Ptr), and where the bits start in it (Shift, of type Ty). An array's
  /// window never reaches past the array.
  struct BitWindow {
    llvm::Value *Ptr;
    llvm::IntegerType *Ty;
    llvm::Value *Shift;
  };

  BitWindow bitWindow(const BitRef &R);

  /// The value (register type) of bitpacked component E.
  llvm::Value *bitLoad(Expr *E) { return bitLoad(bitRef(E)); }

  llvm::Value *bitLoad(const BitRef &R);

  /// Target := V for bitpacked component Target: its bits replaced.
  void bitStore(Expr *Target, llvm::Value *V);

  /// Address of a designator (variable, a[i], r.f, with-field) or of an
  /// aggregate constant (string literal / string constant).
  llvm::Value *emitAddress(Expr *E);

  //===--------------------------------------------------------------------===//
  // Debug information
  //===--------------------------------------------------------------------===//

  unsigned lineOf(SourceLocation L) const;

  void initDebugInfo(llvm::StringRef SourceFileName);

  llvm::DIType *debugType(Type *T);

  llvm::DIType *buildDebugType(Type *T);

  llvm::DISubprogram *createSubprogram(llvm::Function *F, llvm::StringRef Name,
                                       llvm::DIScope *Scope, unsigned Line,
                                       Type *Result,
                                       llvm::ArrayRef<VarDecl *> Params,
                                       llvm::DIFile *File = nullptr,
                                       VarDecl *Self = nullptr);

  void setDebugLoc(SourceLocation L);

  /// Describes the variables of R (params, locals, result) to the
  /// debugger: offsets into the frame alloca, or their own allocas.
  void declareFrameVariables(RoutineDecl *R);

  void declareGlobalDebugInfo(VarDecl *V, llvm::GlobalVariable *G);

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  /// Initial value of a typed constant (or zero).
  llvm::Constant *initializerFor(VarDecl *V, llvm::Type *Ty);

  /// The initial value of static variable V (memory type Ty) when it is
  /// created: one holding procedural values gets its value once the
  /// routines are declared (finishStaticInits).
  llvm::Constant *staticInitializer(VarDecl *V, llvm::Type *Ty);
  std::vector<VarDecl *> DeferredInits;

  void finishStaticInits();

  /// A procedural constant: nil, a routine, or a lambda (with no frame and
  /// no object), as {function, nil}.
  llvm::Constant *routineConstant(Expr *Init, llvm::Type *Ty);

  /// The constant for a typed-constant initializer Init of type T (memory
  /// type Ty); Folded is Sema's value for non-aggregates.
  llvm::Constant *constantFor(Expr *Init, const ConstValue &Folded, Type *T,
                              llvm::Type *Ty);

  /// Does T have a record laid out with a field alignment?
  static bool containsAlignedRecord(Type *T);

  /// Writes the bytes of a typed constant's value (Init, or Folded for a
  /// scalar) of type T into Bytes from Offset (little-endian): ordinals,
  /// reals, short strings and char arrays, and arrays and records of them.
  void writeConstantBytes(Expr *Init, const ConstValue &Folded, Type *T,
                          std::string &Bytes, uint64_t Offset);

  llvm::Constant *scalarConstant(const ConstValue &C, Type *T, llvm::Type *Ty);

  /// The constant of memory type Ty whose bytes are Bytes from Offset
  /// (integers little-endian; bytes past the end are 0).
  llvm::Constant *bytesConstant(llvm::StringRef Bytes, uint64_t Offset,
                                llvm::Type *Ty);

  llvm::Constant *initializerForString(std::string S, llvm::Type *Ty, Type *T);

  void emitStaticVar(VarDecl *V);

  void emitGlobals(Block &Blk);

  /// Creates LLVM functions and frame types for every routine in Blk
  /// (recursively), and queues their bodies for emission.
  /// Every routine takes its static link first (null at program level), so
  /// procedural parameters can call any routine the same way.
  llvm::FunctionType *routineFnType(llvm::ArrayRef<VarDecl *> Params,
                                    Type *Result, bool HasSelf = false,
                                    bool HasLink = true,
                                    uint32_t SliceMask = 0);

  /// Does R take the hidden static-link first parameter? Only a nested
  /// routine needs it; methods keep it for method pointers, as do async and
  /// iterator routines (their argument record has a link slot) (item 20).
  static bool needsLink(RoutineDecl *R);

  llvm::FunctionType *routineFnType(RoutineDecl *R);

  /// An external (C) routine: no static link, C types (a callback: a
  /// function pointer), maybe variadic.
  void declareExternal(RoutineDecl *R);

  /// The C-callable entry of a cdecl routine R at program or unit level
  /// (its procedural values point to it): cthunk.<symbol>, which calls R
  /// with a null static link. Created (with its body) right after R.
  llvm::DenseMap<RoutineDecl *, llvm::Function *> CThunks;

  void declareCThunk(RoutineDecl *R);

  /// Thunks that give a top-level routine the {link, params} signature its
  /// procedural values have (item 20).
  llvm::DenseMap<RoutineDecl *, llvm::Function *> LinkThunks;

  /// The function a procedural value of routine R points to.
  llvm::Value *routineFunction(RoutineDecl *R);

  void declareRoutines(Block &Blk);

  /// An interface's iid: a constant holding its name (length-prefixed),
  /// pas.<unit>$<name>$iid for an interface of a unit's interface.
  llvm::DenseMap<InterfaceType *, llvm::GlobalVariable *> IIDs;

  void declareInterface(Decl *D, InterfaceType *I);

  /// The interface table of class C (null without interfaces): its
  /// interfaces' iids and method tables.
  llvm::Constant *interfaceTable(ClassType *C);

  /// A class: its VMT (defined once all methods are declared), and for a
  /// class of a separately compiled unit its methods' declarations.
  void declareClass(Decl *D, ClassType *C);

  /// A runtime function used only by programs with classes.
  llvm::FunctionCallee classRuntime(llvm::StringRef Name, llvm::Type *Ret,
                                    llvm::ArrayRef<llvm::Type *> Params);

  /// Can a call of the runtime function Name raise an exception? Those
  /// that raise one, and those that may run a destructor (which is Pascal
  /// code): releasing an object or an interface reference.
  static bool runtimeMayRaise(llvm::StringRef Name);

  /// Does the code emitted for the current function so far have a call
  /// that may raise an exception? Any call does, except of an LLVM
  /// intrinsic, of a runtime function that cannot (runtimeMayRaise), of
  /// the function itself and of a routine emitted before that was found
  /// not to (NoRaiseFns). So calls through procedural values and virtual
  /// methods, of external routines and of routines emitted later count:
  /// this errs on the side of protecting.
  bool mayRaise() const;

  /// The VMT of C; the predeclared classes' (TObject, Exception, EDivByZero
  /// and its ancestors) are in the runtime, as __pas_<class>_vmt.
  llvm::Constant *vmtOf(ClassType *C);

  /// The depth of C in its hierarchy (TObject: 0).
  static unsigned classDepth(ClassType *C);

  /// The VMTs from C's root to C itself, for a VMT's ancestor display.
  llvm::SmallVector<llvm::Constant *, 8> classAncestors(ClassType *C);

  /// Fills in the VMTs: parent VMT, class name, instance size, interface
  /// table, hierarchy depth, ancestor display, then the virtual methods.
  void defineVMTs();

  /// A by-reference parameter is an address: never nil, and it points to a
  /// value of its type (item 9). LLVM cannot infer this across a call.
  void addByRefParamAttrs(llvm::Argument *A, Type *T);

  /// The hidden result pointer is a fresh caller temporary (item 4): it
  /// cannot alias an argument, and LLVM's ABI can return through it.
  void addResultParamAttrs(llvm::Argument *A, Type *T);

  /// The same facts at a call site, where they change the ABI on targets
  /// with an indirect-result convention.
  void addResultCallAttrs(llvm::CallBase *CB, unsigned ArgNo, Type *T);

  /// Marks the hidden result pointer of a function declaration, if it has
  /// one.
  void addResultAttrs(llvm::Function *F, RoutineDecl *R);

  /// Creates the LLVM function and frame type of routine R (a method: of
  /// its canonical declaration), queues its body and declares the
  /// routines nested in it.
  void declareRoutine(RoutineDecl *R);

  /// The variables of R's frame that hold managed values R releases when
  /// it returns: its value parameters (one more reference each) and local
  /// variables.
  llvm::SmallVector<VarDecl *, 8> managedFrameVars(RoutineDecl *R);

  /// R's frame is on the heap (a closure made in it uses it): the function
  /// that finalizes it when its last reference goes, frame.fini.<R>. It
  /// releases the managed variables (as R does on its way out when its
  /// frame is not on the heap) and the frame of R's parent, if that is on
  /// the heap too (R's frame holds a reference to it, its static link).
  llvm::Function *emitFrameFini(RoutineDecl *R);

  /// Does R's frame hold its static link, to a frame on the heap?
  bool parentFrameOnHeap(RoutineDecl *R);

  /// __pas_frame_new, __pas_frame_addref / release (runtime/io.c).
  llvm::FunctionCallee frameRuntime(llvm::StringRef Name);

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
  void declareAsync(RoutineDecl *R, const std::string &Name, bool External);

  /// The parameters of R from its task's arguments: their slot index.
  unsigned firstParamSlot(RoutineDecl *R) { return R->hasSelf() ? 2 : 1; }

  void emitAsyncParts(RoutineDecl *R);

  /// How the next async call starts its task (set by spawn and Pool.Run,
  /// taken by the call).
  enum class StartMode { Here, Spawn, Pool };
  StartMode NextStart = StartMode::Here;

  /// A call of async routine R (link, Self: Self's value or null): its
  /// task, made with copies of the arguments and started (here: it runs
  /// until it waits or ends).
  llvm::Value *emitAsyncCall(RoutineDecl *R, llvm::Value *Link, llvm::Value *Self,
                             llvm::ArrayRef<Expr *> Args);

  /// A call of iterator routine R (link, Self: Self's value or null): its
  /// iterator, made with copies of the arguments; its body runs when its
  /// values are asked for.
  llvm::Value *emitIteratorCall(RoutineDecl *R, llvm::Value *Link, llvm::Value *Self,
                                llvm::ArrayRef<Expr *> Args);

  /// IteratorNext(it): runs it to its next value; an i1.
  llvm::Value *emitIteratorNext(llvm::Value *It);

  /// The address of iterator It's current value.
  llvm::Value *iteratorValueAddr(llvm::Value *It);

  /// await / Wait: waits for task T (of type TT); its result (an address
  /// for a structured type), or null without one.
  llvm::Value *emitAwait(llvm::Value *T, TaskType *TT);

public:
  llvm::Value *visitAwaitExpr(AwaitExpr *E);

  llvm::Value *visitSpawnExpr(SpawnExpr *E);

  void visitExprStmt(ExprStmt *S) { visitExpr(S->getExpr()); }

  void visitScopeStmt(ScopeStmt *S);

  /// yield e / yield break / yield from it.
  void visitYieldStmt(YieldStmt *S);

  void visitDetachedStmt(DetachedStmt *S);

  /// WhenAll(tasks) / WhenAny(tasks): the tasks' data and count.
  llvm::Value *emitWhen(CallExpr *E, bool All);

  void emitRoutineBody(RoutineDecl *R, uint32_t SliceMask = 0,
                       llvm::Function *SpecFn = nullptr);
  llvm::Function *getOrCreateSliceSpec(RoutineDecl *R, uint32_t SliceMask);
  void emitSliceSpecs();

  //===--------------------------------------------------------------------===//
  // Calls
  //===--------------------------------------------------------------------===//

  llvm::Value *staticLinkFor(RoutineDecl *R);

  /// The link argument of a direct call: null when the routine has no link
  /// parameter (emitCallTo must not pass an extra null).
  llvm::Value *callLink(RoutineDecl *R);

  /// {function, static link} for a routine passed as a procedural argument.
  llvm::Value *emitRoutineValue(Expr *Arg);

  llvm::Value *emitCallTo(llvm::FunctionCallee Callee, llvm::Value *Link,
                          llvm::ArrayRef<VarDecl *> Params,
                          llvm::ArrayRef<Expr *> Args,
                          llvm::Value *Self = nullptr,
                          Type *ResultTy = nullptr, bool ToC = false,
                          llvm::Value *ResultDst = nullptr,
                          uint32_t SliceMask = 0);

  static Type *resultOf(RoutineDecl *R);

  /// Appends the values passed for Args (parameters Params) to Values.
  /// ToC: to an external (C) routine, which takes callbacks as function
  /// pointers, and maybe variadic arguments after Params.
  void emitArgValues(llvm::ArrayRef<VarDecl *> Params,
                     llvm::ArrayRef<Expr *> Args,
                     llvm::SmallVectorImpl<llvm::Value *> &Values,
                     bool ToC = false, uint32_t SliceMask = 0);

  /// A variadic argument of an external routine, with C's promotions:
  /// ordinals to at least int, reals to double, string constants as C
  /// strings.
  llvm::Value *emitVarArg(Expr *Arg);

  /// A string constant passed for a pchar parameter: a NUL-terminated copy.
  llvm::Constant *cStringFor(Type *ParamTy, Expr *Arg);

  llvm::Value *emitCall(RoutineDecl *R, llvm::ArrayRef<Expr *> Args,
                        llvm::Value *ResultDst = nullptr);

  /// Call through a procedural / functional parameter or variable.
  llvm::Value *emitIndirectCall(VarDecl *V, llvm::ArrayRef<Expr *> Args);

  /// Calls the {function, static link} pair Pair of type RT. NilCheckLoc:
  /// where to report a nil procedural value (invalid: no check).
  llvm::Value *emitCallThrough(llvm::Value *Pair, RoutineType *RT,
                               llvm::ArrayRef<Expr *> Args,
                               SourceLocation NilCheckLoc);

  /// @obj.M, @TFoo.M, @M: {method's code, object or class}.
  llvm::Value *emitMethodPointer(AddrOfExpr *A);

  /// A procedural value of type T (Turbo/FPC): @routine, nil, or a
  /// procedural variable / field / element.
  llvm::Value *emitRoutinePair(Expr *E, Type *T);

  //===--------------------------------------------------------------------===//
  // Objects and methods (objfpc)
  //===--------------------------------------------------------------------===//

  /// The object a with context stands for (Self for a method body), or the
  /// address of a with statement's record.
  //===--------------------------------------------------------------------===//
  // Reference counting (COM interfaces, objfpc)
  //===--------------------------------------------------------------------===//

  /// The runtime's reference counting entry points (runtime/io.c).
  llvm::FunctionCallee intfRuntime(llvm::StringRef Name);

  /// Releases the COM interfaces in the object of managed type T at Addr
  /// and sets them to nil.
  void finalizeValue(llvm::Value *Addr, Type *T);

  /// One more reference to each COM interface in the object of managed type
  /// T at Addr (a copy was made).
  void addRefValue(llvm::Value *Addr, Type *T);

  /// *Dst := *Src for a record or array T with COM interfaces.
  void copyManaged(llvm::Value *Dst, llvm::Value *Src, Type *T);

  /// Addr^ := V for a COM interface.
  void storeInterface(llvm::Value *Addr, llvm::Value *V);

  /// Stores V (scalar, of type T) at Addr: counted for COM interfaces.
  void storeValue(llvm::Value *Addr, llvm::Value *V, Type *T,
                  bool AllowTBAA = true);

  /// Copies the record or array T at Src to Dst: counted if managed.
  void copyValue(llvm::Value *Dst, llvm::Value *Src, Type *T);

  /// A function's result V, a COM interface (a reference the caller now
  /// holds): kept in a temporary until the statement ends. T: its type, if
  /// that may be a dynamic array.
  llvm::Value *holdResult(llvm::Value *V, Type *T = nullptr);

  /// Returns V directly when NoHold is true (marking TookAnsiResult), else holds it.
  llvm::Value *takeOrHoldAnsi(llvm::Value *V, bool NoHold);

  bool canMoveAnsiLocalToResult(VarDecl *TV, Expr *Value, bool TailMove);

  static bool canBorrowAnsiParams(RoutineDecl *R);
  static bool routineWritesOnlyLocals(RoutineDecl *R);
  bool isSliceSafeParam(RoutineDecl *R, VarDecl *P);
  llvm::SmallVector<VarDecl *, 4> findSliceLocals(RoutineDecl *R);
  bool isSliceExpr(Expr *E) const;
  AnsiSliceVal emitSliceExpr(Expr *E);
  AnsiSliceVal emitSliceCopy(Expr *SrcExpr, Expr *IdxExpr, Expr *CntExpr);
  llvm::Value *emitSliceIndex(IndexExpr *E);
  llvm::Value *emitAnsiEqSlices(BinaryOp Op, AnsiSliceVal L, AnsiSliceVal R);
  llvm::Value *emitSliceCompare(BinaryOp Op, Expr *LHS, Expr *RHS);

  void emitAnsiAssign(llvm::Value *Dst, Expr *Target, VarDecl *TV, Expr *Value,
                      bool TailMove);

  /// holdResult's temporary: its address.
  llvm::Value *heldTemp(llvm::Value *V, Type *T = nullptr);

  /// A function's record result V, holding ansistrings the caller now owns:
  /// kept in a temporary until the statement ends. Returns its address.
  llvm::Value *holdRecordResult(llvm::Value *V, Type *T);

  /// Releases the temporaries held since LiveTemps had Mark entries.
  void releaseTemps(size_t Mark);

  /// Releases the ansistring or COM interface held in the temporary Slot.
  void clearTemp(llvm::Value *Slot);

  /// Is E a const parameter (whose value its caller owns)?
  static bool isConstParamRef(Expr *E);

  void emitAnsiIndexCheck(llvm::Value *S, llvm::Value *X, SourceLocation L);

  /// Is C TInterfacedObject (the predeclared one) or a descendant?
  static bool isInterfacedClass(const ClassType *C);

  /// Does class C have fields of its own holding COM interfaces?
  static bool hasManagedFields(const ClassType *C);

  /// The iid of interface I (the predeclared IUnknown's is in the runtime).
  llvm::Constant *iidOf(InterfaceType *I);

  /// The cleanup function of class C: releases its own fields' COM
  /// interfaces (the runtime calls it for each class of a freed object).
  void emitCleanup(ClassType *C, llvm::Function *F);

  void emitCleanups();

  llvm::Value *withBase(WithContext *W);

  //===--------------------------------------------------------------------===//
  // Advanced records (objfpc)
  //===--------------------------------------------------------------------===//

  /// The value of E, a user-defined conversion applied (its operator called
  /// with E's unconverted value); an address for a record.
  /// A constant used as a member (TFoo.C, obj.C): its reference, which is
  /// emitted instead; otherwise E.
  static Expr *unwrapConst(Expr *E);

  llvm::Value *visitExpr(Expr *E);

  llvm::Value *emitConversion(Expr *E);

  static bool isVarParamRef(Expr *E);

  /// The method of type helper member D (a property: its getter), or null.
  static RoutineDecl *helperRoutine(Decl *D);

  /// The Self of a type helper's member used through F: the address of
  /// its base, or of a temporary holding the base's value.
  llvm::Value *helperSelf(FieldExpr *F);

  /// R(Args) on record Obj (its address): a method, or a class method or
  /// operator (static: no Self).
  llvm::Value *emitRecordMethodCall(RoutineDecl *R, llvm::Value *Obj,
                                    llvm::ArrayRef<Expr *> Args);

  /// TR.Create(args): a new record (zeroed, in a temporary) initialized by
  /// the constructor Ctor.
  llvm::Value *emitRecordConstruction(RecordType *RT, RoutineDecl *Ctor,
                                      llvm::ArrayRef<Expr *> Args);

  /// The interface I value of object Obj: {Obj, its class's table for I}.
  llvm::Value *toInterface(llvm::Value *Obj, InterfaceType *I);

  llvm::Value *makeInterface(llvm::Value *Obj, llvm::Value *Table);

  /// The object of an interface value (nil: itself).
  llvm::Value *interfaceObject(llvm::Value *V);

  /// Is W's object a class (Self in a class method)?
  static bool withIsClass(WithContext *W);

  /// if (Obj == nil) runtime error, under range checks.
  void emitNilObjectCheck(llvm::Value *Obj, SourceLocation L);

  /// The object an expression of a class type refers to, checked for nil.
  llvm::Value *emitObject(Expr *E);

  /// Address of virtual method Index in the VMT of object Obj.
  llvm::Value *loadVirtual(llvm::Value *Obj, unsigned Index);

  /// Can no separately compiled unit see C (and so subclass it)?
  bool classIsClosed(ClassType *C) const;

  /// Does C have descendants in what this compilation can see? Unknown
  /// (separately compiled) descendants count when C is not closed.
  bool classHasDescendants(ClassType *C);

  /// Can a virtual call of R be a direct call? Only if no class of the
  /// whole-program hierarchy that descends from R's overrides it; with
  /// -fseparate-units only for a class that a separately compiled unit
  /// cannot see (item 11).
  bool canDevirtualize(RoutineDecl *R);

  /// Entry Slot of a VMT: 0 the parent's VMT, 1 the class name, 2 the
  /// instance size, 3 the interface table, 4... the virtual methods.
  llvm::Value *loadVMTSlot(llvm::Value *VMT, unsigned Slot,
                           const llvm::Twine &Name);

  /// The class (VMT) of a method call's receiver: Obj itself if SelfIsClass,
  /// else the object's (checked for nil).
  llvm::Value *receiverClass(llvm::Value *Obj, bool SelfIsClass,
                             SourceLocation L);

  /// if (VMT == nil) runtime error, under range checks.
  void emitNilClassCheck(llvm::Value *VMT, SourceLocation L);

  /// obj.Free: if obj <> nil then obj.Destroy (virtual) and release it.
  void emitFree(llvm::Value *Obj);

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
                              SourceLocation L, bool SelfIsClass = false);

  /// TFoo.Create(args): a new object of class C, initialized by Ctor.
  /// The arguments are evaluated first. An exception leaving a constructor
  /// written in Pascal destroys and releases the new object (as in Free
  /// Pascal), so the call runs protected.
  llvm::Value *emitConstruction(ClassType *C, RoutineDecl *Ctor,
                                llvm::ArrayRef<Expr *> Args, SourceLocation L);

  /// c.Create(args) with c a class (a VMT; also Self in a class method): a
  /// new object of the class c refers to, of its size, initialized by Ctor
  /// (virtual: c's). Protected like emitConstruction.
  llvm::Value *emitConstructionThrough(llvm::Value *VMT, RoutineDecl *Ctor,
                                       llvm::ArrayRef<Expr *> Args,
                                       SourceLocation L);

  /// obj.M(args) or TFoo.Create(args) through FieldExpr F.
  /// Base.R(Args), Base an interface value: R through its method table.
  llvm::Value *emitInterfaceCall(RoutineDecl *R, Expr *Base, SourceLocation L,
                                 llvm::ArrayRef<Expr *> Args);

  llvm::Value *emitMemberCall(FieldExpr *F, llvm::ArrayRef<Expr *> Args);

  /// inherited [M] [(args)]: on the enclosing method's Self.
  llvm::Value *emitInheritedCall(InheritedExpr *I, llvm::ArrayRef<Expr *> Args);

  /// The value of property P of object Obj (static class C; for an advanced
  /// record, Obj is its address and C the record type): an address for
  /// strings and records. Indexes: an array property's.
  llvm::Value *emitPropertyRead(PropertyDecl *P, llvm::Value *Obj, Type *C,
                                llvm::ArrayRef<Expr *> Indexes = {});

  /// The arguments of a property's accessor method: the indexes of an
  /// array property, the `index` constant, the value written (if any).
  llvm::SmallVector<Expr *, 4> accessorArgs(PropertyDecl *P,
                                            llvm::ArrayRef<Expr *> Indexes,
                                            Expr *Value);

  /// The object of an array property access E (obj.P[i], P[i], obj[i]),
  /// and its static class C (an advanced record: its address and type).
  llvm::Value *propertyObject(IndexExpr *E, Type *&C);

  /// The object whose member Base.M is used: Base's object (checked for
  /// nil), or a record's address.
  llvm::Value *memberObject(Expr *Base);

  /// The value of array property access E (an address for strings and
  /// records).
  llvm::Value *emitArrayPropertyRead(IndexExpr *E);

  /// A member of a class used as a value through FieldExpr F: a field, a
  /// property, or a call of a method without arguments.
  llvm::Value *emitMemberValue(FieldExpr *F);

  //===--------------------------------------------------------------------===//
  // goto
  //===--------------------------------------------------------------------===//

  llvm::BasicBlock *labelBlock(LabelDecl *L);

  /// At entry of a block that is the target of non-local gotos:
  /// switch (_setjmp(buf)) { case k: goto label_k; }
  void emitSetJmpDispatch(Block &Blk, llvm::Value *Buf);

  //===--------------------------------------------------------------------===//
  // Operators
  //===--------------------------------------------------------------------===//

  llvm::Value *emitShortCircuit(BinaryExpr *E);

  // Checks are skipped when a constant divisor makes them provably pass.
  static bool isConstNonZero(llvm::Value *V);
  static bool isConstNotMinusOne(llvm::Value *V);

  llvm::Constant *intConst(llvm::Value *Like, int64_t V);

  llvm::Value *emitDiv(llvm::Value *L, llvm::Value *R, bool Signed,
                       SourceLocation Loc);

  llvm::Value *emitMod(llvm::Value *L, llvm::Value *R, bool Signed,
                       SourceLocation Loc);

  llvm::Value *compareInts(BinaryOp Op, llvm::Value *L, llvm::Value *R,
                           bool Signed);

  llvm::Value *emitCompare(BinaryExpr *E);

  //===--------------------------------------------------------------------===//
  // Built-in procedures and functions
  //===--------------------------------------------------------------------===//

  /// Splits off a leading file argument of read/write; returns the PasFile*.
  llvm::Value *fileTarget(llvm::ArrayRef<Expr *> &Args, bool Input,
                          FileType *&FT);

  /// Address of a value convertible to type T: the designator itself if it
  /// already has type T, else a temporary holding the converted value.
  llvm::Value *addressForValue(Expr *E, Type *T);

  /// An expression that cannot call Pascal code: its evaluation cannot raise
  /// an exception, so it is safe to format it into the write buffer. Runtime
  /// calls (bounds checks, string concatenation) are fine: the end of the
  /// statement flushes the buffer, or the next statement discards it.
  static bool simpleWriteExpr(Expr *E);

  static bool simpleWriteArg(Expr *E);

  /// Appends a compile-time constant write argument to a merged literal.
  /// Only ordinals: a real would need the runtime's exact formatting.
  static bool appendConstText(Expr *E, std::string &Text);

  void emitWrite(ProcCallStmt *S, bool IsWriteln);

  void emitRead(ProcCallStmt *S, bool IsReadln);

  void emitWriteArg(llvm::Value *File, Expr *Arg);

  /// The names of E's values as length-prefixed strings: a private
  /// constant [N x ptr], one per enumeration type.
  llvm::GlobalVariable *enumNameTable(EnumType *E);

  /// write(e) of an enumeration's value (Turbo/FPC): its name, as declared,
  /// padded on the right to the field width. A value outside the type is a
  /// runtime error (always, as in Free Pascal).
  void emitWriteEnum(llvm::Value *File, llvm::Value *V, Type *T, EnumType *E,
                     llvm::Value *Width, SourceLocation L);

  void emitReadArg(llvm::Value *File, Expr *Arg);

  /// pack(a, i, z) / unpack(z, a, i): packed and unpacked arrays share a
  /// representation, so both are a bounds-checked memcpy.
  void emitPackUnpack(ProcCallStmt *S, bool IsPack);

  llvm::Value *emitFileQuery(BuiltinKind K, Expr *FileArg);

  /// T(x): ordinal conversions keep the low bits (sign/zero-extending by
  /// the source); pointer casts change only the type.
  llvm::Value *emitTypecast(CallExpr *E);

  /// A runtime file routine (declared when first used).
  llvm::FunctionCallee fileRuntime(llvm::StringRef Name, llvm::Type *Ret,
                                   llvm::ArrayRef<llvm::Type *> Params);

  llvm::Value *emitBuiltinCall(CallExpr *E, BuiltinDecl *BD);

public:
  CodeGenModule(llvm::Module &M, const SourceManager &SM,
                const LangOptions &LangOpts, const CodeGenOptions &CGOpts);

  void emitProgram(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units);

  /// Nothing unwinds through p3's frames: exceptions are _setjmp/_longjmp
  /// based (item 10). LLVM cannot infer it for calls through procedural
  /// values, virtual methods or separately compiled units.
  void markDefinedFunctionsNounwind();

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
  void removeUnreachable();

  /// The start of a program or unit (Decls, then More: its declarations):
  /// the class constructors of the classes and advanced records it
  /// declares (nested ones too), in declaration order; then its class
  /// destructors are registered with the runtime, which runs them at exit
  /// (last registered first: the program's, then the units' in reverse
  /// order, each one's in declaration order, as in Free Pascal).
  void emitClassInits(llvm::ArrayRef<Decl *> Decls, llvm::ArrayRef<Decl *> More);

  /// A unit compiled on its own: its variables and routines, and its
  /// initialization part as pas.<unit>$$init (files, then the statements).
  void emitUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps);

  /// Statements carry their source position into the debug info.
  void visitStmt(Stmt *S);

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *);

  void visitCompoundStmt(CompoundStmt *S) { emitSequence(S->getBody()); }

  /// The statements of a sequence. After one that declares owned
  /// variables, the rest is the protected part of a try-finally that frees
  /// them: their scope ends with the sequence, however it is left.
  void emitSequence(llvm::ArrayRef<Stmt *> Body);

  /// var x: T [:= e]: x is set each time the statement runs, to e or to
  /// zero (what it held, in an earlier iteration, is released).
  void visitVarDeclStmt(VarDeclStmt *S);

  /// If Value is Target + x, for a short string variable Target named in
  /// both places: x.
  static Expr *appendedTo(Expr *Target, Expr *Value);

  static Expr *skipParens(Expr *E);

  static bool isTargetName(Expr *E, VarDecl *V);

  /// Is E, after parentheses and ansistring casts, Target itself?
  static bool isDirectTargetRef(Expr *E, VarDecl *V);

  /// The right operands of a left-nested ansistring addition whose leftmost
  /// operand is V: s + a + b gives {a, b}.
  static bool ansiAppends(Expr *Value, VarDecl *V,
                          llvm::SmallVectorImpl<Expr *> &Parts);

  /// A direct call of a function (or through a procedural value) whose
  /// result is an ansistring: the reference it returns is new, so an
  /// assignment can move it instead of counting it and releasing it later.
  static bool isFreshAnsiCall(Expr *Value);

  /// optimization3 item 1: `Result := F(...)` where F returns a record by
  /// pointer: this function's own result pointer is passed on to F, instead
  /// of a temporary that is then copied. The pointer is `noalias` (the
  /// caller's fresh temporary), so nothing else can see the record while F
  /// writes it. Not done when
  ///  - the routine has a try or a non-local label (an exception caught
  ///    here could see a half-written result),
  ///  - the result variable is captured (a nested routine could read it),
  ///  - an argument mentions the result (F zeroes and fills it before it
  ///    copies its value parameters).
  /// Returns whether the assignment was emitted.
  bool emitResultInPlace(Expr *Target, Expr *Value, Type *TT);

  void visitAssignStmt(AssignStmt *S);

  /// Is E an owned variable (`x: TFoo owned`)?
  static bool isOwnedVar(Expr *E);

  static PropertyDecl *assignedProperty(Expr *Target);

  /// Target := Value for a property Target: stores into its field or
  /// calls its write procedure.
  void emitPropertyWrite(PropertyDecl *P, Expr *Target, Expr *Value);

  /// Value stored at Addr, property P's field or class variable.
  void storeProperty(PropertyDecl *P, llvm::Value *Addr, Expr *Value);

  /// (a, b): its hidden variable, each element assigned its value.
  llvm::Value *emitTuple(TupleExpr *E);

  void visitDestructureStmt(DestructureStmt *S);

  /// Value, assignable to type T, stored at Addr (a variable of type T).
  void storeExpr(llvm::Value *Addr, Type *T, Expr *Value);

  void visitProcCallStmt(ProcCallStmt *S);

  void visitIndirectCallStmt(IndirectCallStmt *S);

  void visitIfStmt(IfStmt *S);

  void visitWhileStmt(WhileStmt *S);

  void visitRepeatStmt(RepeatStmt *S);

  //   start = Init; final = Final            (each evaluated once)
  //   if !(start <= final) goto end          (>= for downto)
  //   [range-check start and final against the control variable's type]
  // loop:
  //   i = phi [start, pre], [next, inc]; Var := i
  //   Body
  //   if i == final goto end                 (test before stepping: no
  // inc:                                      overflow at maxint)
  //   next = i + 1 (- 1); goto loop
  void visitForStmt(ForStmt *S);

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
  void visitForInStmt(ForInStmt *S);

  /// for I := Lo to Hi (i64, signed; nothing if Lo > Hi), with break and
  /// continue: Fn(I, latch) emits an iteration, which may branch to the
  /// latch to skip the rest of it.
  void emitForInLoop(
      llvm::Value *Lo, llvm::Value *Hi,
      llvm::function_ref<void(llvm::Value *, llvm::BasicBlock *)> Fn);

  void emitForInEnumerator(ForInStmt *S);

  //   while e.MoveNext do begin Var := e.Current; Body end
  void emitEnumeratorLoop(ForInStmt *S);

  void visitCaseStmt(CaseStmt *S);

  void visitLabeledStmt(LabeledStmt *S);

  void visitGotoStmt(GotoStmt *S);

  //===--------------------------------------------------------------------===//
  // Exceptions (objfpc)
  //===--------------------------------------------------------------------===//

  /// PasExcFrame (runtime/include/pascalrt.h): prev, object, where,
  /// address, jmp_buf.
  llvm::StructType *excFrameType();

  /// The runtime's exception entry points, declared when first used.
  llvm::FunctionCallee excRuntime(llvm::StringRef Name);

  /// Pushes a new PasExcFrame and calls _setjmp on it: an exception raised
  /// to the frame continues at Landing, the protected code in a new block
  /// named BodyName, or in Body if given (the insertion point). Returns
  /// the frame.
  llvm::Value *emitProtect(llvm::BasicBlock *Landing, const llvm::Twine &BodyName,
                           llvm::BasicBlock *Body = nullptr);

  /// Branches to Target (exit, break, continue), leaving the Cleanups
  /// above Depth on the way.
  void emitJump(llvm::BasicBlock *Target, size_t Depth);

  void visitTryStmt(TryStmt *S);

  //   push F; if (_setjmp(F)) goto raised;  body; pop F; state = 0
  // finally:
  //   finally part
  //   switch (state) { 0: end; 1: reraise F; 2 + k: exit k (exit, break or
  //     continue out of the body) }
  // raised: state = 1; goto finally
  void emitTryFinally(TryStmt *S);

  /// The finally part of a routine with owned variables: frees their
  /// objects, the last declared first. A destructor's exception does not
  /// stop the others; the first one is raised afterwards, unless an
  /// exception is already leaving the routine (State 1), which goes on.
  void emitFreeOwned(llvm::ArrayRef<VarDecl *> Owned, llvm::Value *State);

  /// FinallyPart is given the state's variable: 0 after the body's normal
  /// end, 1 after an exception, 2 + k after an exit, break or continue.
  void emitTryFinally(llvm::function_ref<void()> Body,
                      llvm::function_ref<void(llvm::Value *)> FinallyPart);

  /// "file:line:col" for the runtime's exception functions, or null.
  llvm::Value *whereText(SourceLocation L);

  /// if (Cond) integer division by zero: a runtime error, or (objfpc) an
  /// EDivByZero exception, as in Free Pascal with SysUtils.
  void emitDivZeroCheck(llvm::Value *Cond, SourceLocation L);

  /// ExceptAddr: the address of the exception the innermost enclosing
  /// handler handles (the frame it was raised to), nil outside handlers.
  llvm::Value *exceptAddr();

  void visitRaiseStmt(RaiseStmt *S);

  void visitWithStmt(WithStmt *S);

  //===--------------------------------------------------------------------===//
  // Expressions (scalar values in register representation)
  //===--------------------------------------------------------------------===//

  llvm::Value *visitIntegerLiteralExpr(IntegerLiteralExpr *E);

  llvm::Value *visitRealLiteralExpr(RealLiteralExpr *E);

  llvm::Value *visitStringLiteralExpr(StringLiteralExpr *E);

  llvm::Value *visitNameRefExpr(NameRefExpr *E);

  llvm::Value *visitParenExpr(ParenExpr *E) { return visitExpr(E->getInner()); }

  /// move p: p's object; p is nil afterwards.
  llvm::Value *visitMoveExpr(MoveExpr *E);

  llvm::Value *visitUnaryExpr(UnaryExpr *E);

  /// obj is TFoo / obj as TFoo: the runtime walks obj's VMT chain.
  /// Is a VMT the VMT of C or of a descendant? A class with no descendants
  /// only needs the equality test; otherwise the depth and ancestor display
  /// make it two loads and two compares (item 12).
  llvm::Value *classIsVMT(llvm::Value *VMT, ClassType *C);

  /// obj is C: false for nil, else classIsVMT.
  llvm::Value *emitClassIs(llvm::Value *Obj, ClassType *C);

  /// obj as C: obj, or a runtime error if it is not a C (item 12).
  llvm::Value *emitClassAs(llvm::Value *Obj, ClassType *C, SourceLocation L);

  llvm::Value *emitIsAs(BinaryExpr *E);

  /// vector[N] of T: a comparison lane by lane (a vector of booleans, lanes
  /// of 0 and 1), or and/or/xor of the lanes; a scalar operand is
  /// broadcast to every lane.
  llvm::Value *emitVectorLanes(BinaryExpr *E);

  llvm::Value *visitBinaryExpr(BinaryExpr *E);

  /// The range of the signed x and y: with a non-negative operand, 0 up to
  /// it (the other operand's bits can only clear its bits).
  static ValueRange andRange(ValueRange X, ValueRange Y);

  /// The range of the signed l div c, for a constant c > 0: l's range (or
  /// its type's) divided by c; division truncates, which is monotonic.
  ValueRange divRange(llvm::Value *L, llvm::Value *R) const;

  /// The range of the signed l mod c, for a constant c > 0: 0 .. c-1 for a
  /// non-negative l (and in ISO mode, where the result is never negative),
  /// else -(c-1) .. c-1 (Turbo/FPC: the sign of l). Within l's own range
  /// when that is smaller.
  ValueRange modRange(llvm::Value *L, llvm::Value *R) const;

  llvm::Value *visitCallExpr(CallExpr *E);

  /// `a ${x} b`: each value is written, as `write` writes it, to the
  /// runtime's text file that collects characters (__pas_tmpl_sink);
  /// __pas_tmpl_add then makes the string so far, what was collected and
  /// the text that follows into one string. (Nothing but the write itself
  /// runs between the two, so a template inside a value is done by then.)
  llvm::Value *visitTemplateExpr(TemplateExpr *E);

  llvm::Value *visitFormattedExpr(FormattedExpr *);

  llvm::Value *visitNamedArgExpr(NamedArgExpr *);

  llvm::Value *visitIndirectCallExpr(IndirectCallExpr *E);

  llvm::Value *visitInheritedExpr(InheritedExpr *E);

  llvm::Value *visitTupleExpr(TupleExpr *E) { return emitTuple(E); }

  /// Is the reference of type T at Addr nil? (an i1)
  llvm::Value *isNilAt(llvm::Value *Addr, Type *T);

  /// The default value of type T (0, false, '', nil), as visitExpr gives it.
  llvm::Value *zeroValue(Type *T);

  /// x?.m: x's value in a hidden variable; where it is not nil, m (in
  /// another, which is the value; else the default value) or the call.
  llvm::Value *visitSafeNavExpr(SafeNavExpr *E);

  /// a ?? b: a in a hidden variable, replaced by b where a is nil (or where
  /// a's chain of ?. stopped).
  llvm::Value *visitCoalesceExpr(CoalesceExpr *E);

  /// if c then a else b: the if statement Sema made of it sets a hidden
  /// variable, which is the value.
  llvm::Value *visitCondExpr(CondExpr *E);

  llvm::Value *visitAggregateExpr(AggregateExpr *);

  llvm::Value *visitAddrOfExpr(AddrOfExpr *E);

  llvm::Value *visitNilExpr(NilExpr *);

  llvm::Value *visitDerefExpr(DerefExpr *E);

  llvm::Value *visitSetCtorExpr(SetCtorExpr *E);

  llvm::Value *visitIndexExpr(IndexExpr *E);

  llvm::Value *visitFieldExpr(FieldExpr *E);
};

} // namespace p3::detail

#endif // P3_CODEGEN_CODEGENINTERNAL_H
