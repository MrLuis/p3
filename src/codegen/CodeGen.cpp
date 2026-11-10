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

#include "pcc/CodeGen/CodeGen.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/AST/Type.h"
#include "pcc/Basic/Directives.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceManager.h"

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

using namespace pcc;

namespace {

/// Declarations of the libpascalrt entry points (runtime/include/pascalrt.h)
/// and the C library functions we call.
struct Runtime {
  llvm::FunctionCallee Init, Fini, WriteStr, WriteChar, WriteInt, WriteBool,
      WriteReal, Writeln, ReadInt, ReadInt64, ReadChar, ReadReal, Readln,
      RuntimeError,
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
      llvm::cast<llvm::Function>(F.getCallee())
          ->addFnAttr(llvm::Attribute::NoUnwind);
      return F;
    };
    Init = Decl("__pas_init", Void, {I32, Ptr});
    Fini = Decl("__pas_fini", Void, {});
    // Text I/O: the first argument is the PasFile*.
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
  /// Classes (objfpc): instance layouts (used only for offsets and sizes)
  /// and VMTs, in the order the classes were declared.
  llvm::DenseMap<ClassType *, llvm::StructType *> ClassLayouts;
  llvm::DenseMap<ClassType *, llvm::GlobalVariable *> VMTs;
  std::vector<ClassType *> VMTsToDefine;
  llvm::DenseMap<WithContext *, llvm::Value *> WithAddrs;
  llvm::StringMap<llvm::GlobalVariable *> StringConstants;
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
  llvm::SmallVector<llvm::Value *, 4> RoutineTemps;
  llvm::DenseMap<llvm::Value *, Type *> RecordTemps; ///< record temporaries' types
  /// Classes whose cleanup functions (releasing their own fields' COM
  /// interfaces) are to be emitted.
  std::vector<std::pair<ClassType *, llvm::Function *>> CleanupsToEmit;

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
    for (Decl *D : U.getInterface()->getDecls()) {
      if (ClassType *C = definedClass(D)) {
        // A class: its VMT and its methods.
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
          if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && !R->isAbstract()) {
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
        for (Decl *M : MT->getMembers())
          if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
            ExportNames[R] = Prefix + "." + R->getName().Name.str() +
                             overloadSuffix(R);
            if (Imported)
              ImportedDecls.insert(R);
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
      if (!llvm::isa<VarDecl>(D) && !(R && !R->isExternal()))
        continue;
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
  /// The member table of the advanced record D declares, or null.
  static ClassType *advancedRecord(Decl *D) {
    auto *TD = llvm::dyn_cast<TypeDecl>(D);
    auto *RT = TD ? llvm::dyn_cast_or_null<RecordType>(TD->getType()) : nullptr;
    return RT ? RT->getMemberTable() : nullptr;
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
    case Type::TK_Pointer:
    case Type::TK_Nil:
    case Type::TK_File: // a PasFile* owned by the runtime
      return B.getPtrTy();
    case Type::TK_Set:
      return setType();
    case Type::TK_ShortString: // length byte + chars
      return llvm::ArrayType::get(
          B.getInt8Ty(), llvm::cast<ShortStringType>(T)->getMaxLength() + 1);
    case Type::TK_Routine: // {function, static link}
      return llvm::StructType::get(B.getPtrTy(), B.getPtrTy());
    case Type::TK_Interface: // {object, method table}
      return llvm::StructType::get(B.getPtrTy(), B.getPtrTy());
    case Type::TK_OpenArray: // its {data, count} pair
    case Type::TK_ArrayOfConst: // its header slot
    case Type::TK_AnsiString: // its characters, or null when empty
    case Type::TK_ClassRef: // a reference to the class's VMT
    case Type::TK_Class: // a reference to the object
      return B.getPtrTy();
    case Type::TK_Error:
    case Type::TK_Untyped: // (untyped parameters are passed by reference)
      break;
    }
    llvm_unreachable("type has no storage representation");
  }

  /// Sets are 256-bit masks: bit i is set iff ord(i) is a member.
  llvm::IntegerType *setType() { return B.getIntNTy(256); }

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

  /// Are range / overflow checks on at L? (command line, then {$R±}/{$Q±})
  bool rangeChecks(SourceLocation L) const {
    return !CGOpts.Directives
               ? CGOpts.RangeChecks
               : CGOpts.Directives->isEnabled(CheckDirectives::Range, L,
                                              CGOpts.RangeChecks);
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
    if (Dst->isIntegerTy(1))
      return B.CreateICmpNE(V, llvm::ConstantInt::get(V->getType(), 0));
    return isSignedOrdinal(From) ? B.CreateSExtOrTrunc(V, Dst)
                                 : B.CreateZExtOrTrunc(V, Dst);
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

  /// X < Lo or X > Hi, for an i64 X.
  ///
  /// Throughout this file, instructions are created one statement at a
  /// time, never as two arguments of one call: C++ leaves the order of
  /// argument evaluation unspecified, and the IR must not depend on the
  /// compiler that built pcc (the self-hosted compiler writes the same IR,
  /// SELFHOST.md S10).
  llvm::Value *outsideRange(llvm::Value *X, int64_t Lo, int64_t Hi) {
    llvm::Value *Below = B.CreateICmpSLT(X, B.getInt64(Lo));
    llvm::Value *Above = B.CreateICmpSGT(X, B.getInt64(Hi));
    return B.CreateOr(Below, Above);
  }

  /// Checks that ordinal value V (of type From) lies within Target's bounds.
  void emitRangeCheck(llvm::Value *V, Type *From, Type *Target,
                      SourceLocation L,
                      llvm::StringRef Msg = "value out of range") {
    if (!rangeChecks(L) || !Target->isOrdinal())
      return;
    OrdinalBounds TB = getOrdinalBounds(Target);
    OrdinalBounds FB = getOrdinalBounds(From);
    if (FB.Lo >= TB.Lo && FB.Hi <= TB.Hi)
      return; // every value of From fits
    llvm::Value *X = toI64(V, From);
    emitRuntimeCheck(outsideRange(X, TB.Lo, TB.Hi), Msg, L);
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
        return V;
      llvm::Value *Outside = B.CreateAnd(V, B.CreateNot(constRangeMask(TB.Lo, TB.Hi)));
      emitRuntimeCheck(B.CreateICmpNE(Outside, llvm::ConstantInt::get(setType(), 0)),
                       "set member out of range", L);
    }
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
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
      if (auto *C = llvm::dyn_cast<ConstDecl>(N->getDecl());
          C && C->getValue().K == ConstValue::String)
        return getAnsiConstant(C->getValue().Str);
    llvm::Type *Ptr = B.getPtrTy();
    if (T->getBase()->isChar())
      return holdResult(astrCall("__pas_astr_from_char", Ptr, {visitExpr(E)}));
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
    return createTemp(llvm::ArrayType::get(B.getInt8Ty(), MaxLen + 1), "str");
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
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
      if (auto *C = llvm::dyn_cast<ConstDecl>(N->getDecl());
          C && C->getValue().K == ConstValue::String)
        return getPStrConstant(C->getValue().Str);
    if (T->getBase()->isChar()) {
      llvm::Value *Tmp = newStringTemp(1);
      B.CreateStore(B.getInt8(1), Tmp);
      llvm::Value *C = visitExpr(E);
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
      if (T->isArrayOfConst())
        return B.CreateTrunc(B.CreateLoad(B.getInt64Ty(), visitExpr(Args[0])),
                             B.getInt32Ty());
      if (T->isAnsiString())
        return astrCall("__pas_astr_length", B.getInt32Ty(), {visitExpr(Args[0])});
      if (T->isShortString())
        return B.CreateZExt(B.CreateLoad(B.getInt8Ty(), emitPStr(Args[0])),
                            B.getInt32Ty(), "length");
      if (T->getBase()->isChar()) {
        visitExpr(Args[0]);
        return B.getInt32(1);
      }
      return B.getInt32(static_cast<uint32_t>(T->getStringLength()));
    }
    case BuiltinKind::UpCase: {
      llvm::Value *C = visitExpr(Args[0]);
      llvm::Value *GeA = B.CreateICmpUGE(C, B.getInt8('a'));
      llvm::Value *LeZ = B.CreateICmpULE(C, B.getInt8('z'));
      llvm::Value *IsLower = B.CreateAnd(GeA, LeZ);
      return B.CreateSelect(IsLower, B.CreateSub(C, B.getInt8(32)), C, "upcase");
    }
    case BuiltinKind::Pos:
      if (Args[0]->getType()->isAnsiString() || Args[1]->getType()->isAnsiString()) {
        llvm::Value *Sub = emitAnsi(Args[0]);
        llvm::Value *S = emitAnsi(Args[1]);
        return astrCall("__pas_astr_pos", B.getInt32Ty(), {Sub, S});
      }
      return B.CreateCall(RT.StrPos, {emitPStr(Args[0]), emitPStr(Args[1])});
    case BuiltinKind::Copy: {
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

  llvm::Constant *constRangeMask(int64_t Lo, int64_t Hi) {
    llvm::APInt M = llvm::APInt::getBitsSet(256, static_cast<unsigned>(Lo),
                                            static_cast<unsigned>(Hi) + 1);
    return llvm::ConstantInt::get(setType(), M);
  }

  /// Ordinal value -> bit index (i256), checking 0..255 under range checks.
  llvm::Value *setIndex(llvm::Value *V, Type *T, SourceLocation L) {
    llvm::Value *X = toI64(V, T);
    OrdinalBounds Bd = getOrdinalBounds(T);
    if (rangeChecks(L) && (Bd.Lo < 0 || Bd.Hi > 255))
      emitRuntimeCheck(B.CreateICmpUGT(X, B.getInt64(255)),
                       "set element out of range 0..255", L);
    return B.CreateZExt(B.CreateAnd(X, B.getInt64(255)), setType());
  }

  llvm::Value *emitSetCtor(SetCtorExpr *E) {
    llvm::Value *Result = llvm::ConstantInt::get(setType(), 0);
    llvm::Value *One = llvm::ConstantInt::get(setType(), 1);
    llvm::Value *AllOnes = llvm::ConstantInt::getAllOnesValue(setType());
    for (const SetElement &El : E->getElements()) {
      Type *LT = El.Lo->getType();
      llvm::Value *Lo = visitExpr(El.Lo);
      if (!El.Hi) {
        Result = B.CreateOr(Result, B.CreateShl(One, setIndex(Lo, LT, El.Lo->getBeginLoc())));
        continue;
      }
      llvm::Value *Hi = visitExpr(El.Hi);
      Type *HT = El.Hi->getType();
      // Bits Lo..Hi, or nothing if Lo > Hi.
      llvm::Value *Empty = isSignedOrdinal(LT) ? B.CreateICmpSGT(Lo, Hi)
                                               : B.CreateICmpUGT(Lo, Hi);
      llvm::Value *LoI = setIndex(Lo, LT, El.Lo->getBeginLoc());
      llvm::Value *HiI = setIndex(Hi, HT, El.Hi->getBeginLoc());
      llvm::Value *IsTop =
          B.CreateICmpEQ(HiI, llvm::ConstantInt::get(setType(), 255));
      llvm::Value *Below = B.CreateSub(B.CreateShl(One, B.CreateAdd(HiI, One)), One);
      llvm::Value *HiMask = B.CreateSelect(IsTop, AllOnes, Below);
      llvm::Value *LoMask = B.CreateNot(B.CreateSub(B.CreateShl(One, LoI), One));
      llvm::Value *Range = B.CreateSelect(
          Empty, llvm::ConstantInt::get(setType(), 0), B.CreateAnd(HiMask, LoMask));
      Result = B.CreateOr(Result, Range);
    }
    return Result;
  }

  /// x in s  ==  0 <= x <= 255 and bit x of s is set
  llvm::Value *emitIn(BinaryExpr *E) {
    Type *XT = E->getLHS()->getType();
    llvm::Value *X = toI64(visitExpr(E->getLHS()), XT);
    llvm::Value *S = visitExpr(E->getRHS());
    llvm::Value *InRange = B.CreateICmpULE(X, B.getInt64(255));
    llvm::Value *Idx = B.CreateZExt(B.CreateAnd(X, B.getInt64(255)), setType());
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
    if (!overflowChecks(Loc)) {
      switch (ID) {
      case llvm::Intrinsic::sadd_with_overflow:
      case llvm::Intrinsic::uadd_with_overflow: return B.CreateAdd(L, R, Name);
      case llvm::Intrinsic::ssub_with_overflow:
      case llvm::Intrinsic::usub_with_overflow: return B.CreateSub(L, R, Name);
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

  /// Parameters passed as a pointer to the caller's variable: var
  /// parameters, and const parameters of string and structured types.
  static bool isByRef(VarDecl *V) {
    return (V->isVarParam() && !V->getType()->isArrayOfConst() &&
            !V->getType()->isOpenArray()) ||
           V->getType()->isUntyped() ||
           (V->isConstParam() && V->getType()->isStructured());
  }

  /// pas.<outer>.<name> for routines, pas.<class>.<name> for methods.
  static std::string mangle(RoutineDecl *R) {
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
      llvm::Value *F = B.CreateCall(
          RT.FileNew, {B.getInt64(FT->isText() ? 1 : sizeOf(FT->getElementType())),
                       B.getInt32(FT->isText()), Name});
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
      if (!V || !containsFile(V->getType()))
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

  llvm::Value *varAddress(VarDecl *V) {
    if (llvm::Value *H = HandlerVars.lookup(V))
      return H;
    if (!V->getOwner()) {
      if (V->isPredeclared())
        return standardFile(V->getName().Name);
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

  /// Is V kept in its routine's frame record? Variables that nested
  /// routines use are, and those that must be in memory when an exception
  /// arrives through _longjmp: all of them in a routine where execution
  /// continues afterwards (try statements, non-local labels), else the
  /// managed ones, which are released on the way out. The other variables
  /// get an alloca each, which the optimizer turns into registers.
  bool inFrame(VarDecl *V, bool PinAll) {
    return PinAll || V->isCaptured() ||
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
    return B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), RecordAddr,
                                        FieldOffsets[F], F->getName().Name);
  }

  /// Address of a designator (variable, a[i], r.f, with-field) or of an
  /// aggregate constant (string literal / string constant).
  llvm::Value *emitAddress(Expr *E) {
    if (E->getConversion()) // converted to a record: a temporary
      return emitConversion(E);
    if (E->getType() && E->getType()->isOpenArray()) // its elements
      return B.CreateLoad(B.getPtrTy(), openDescriptor(E), "data");
    switch (E->getKind()) {
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
      return B.CreateInBoundsGEP(memType(AT), Base, {B.getInt64(0), Offset});
    }
    case Expr::EK_Field: {
      auto *F = llvm::cast<FieldExpr>(E);
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
                                "pcc", CGOpts.OptLevel > 0, "", 0);
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
    case Type::TK_Subrange:
      return debugType(llvm::cast<SubrangeType>(T)->getHost());
    case Type::TK_OpenArray:
    case Type::TK_ArrayOfConst:
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
      for (FieldDecl *F : R->getAllFields())
        Members.push_back(DIB->createMemberType(
            Fwd, F->getName().Spelling, DIUnitFile, lineOf(F->getLocation()),
            sizeOf(F->getType()) * 8, 0, FieldOffsets[F] * 8,
            llvm::DINode::FlagZero, debugType(F->getType())));
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
    case Type::TK_Set: {
      llvm::Metadata *Sub = DIB->getOrCreateSubrange(0, 32);
      return DIB->createArrayType(
          256, 0, DIB->createBasicType("byte", 8, DW_ATE_unsigned),
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
    unsigned ArgNo = 1;
    if (R->hasSelf())
      Declare(R->getSelfVar(), ArgNo++);
    for (VarDecl *P : R->getParams())
      Declare(P, ArgNo++);
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D); V && !V->isTypedConst())
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

  /// The constant for a typed-constant initializer Init of type T (memory
  /// type Ty); Folded is Sema's value for non-aggregates.
  llvm::Constant *constantFor(Expr *Init, const ConstValue &Folded, Type *T,
                              llvm::Type *Ty) {
    auto *Agg = llvm::dyn_cast<AggregateExpr>(Init);
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
      return initializerForString(C.Str.str(), Ty, T);
    case ConstValue::None:
      break;
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
        createGlobal(V, Ty, Imported ? nullptr : initializerFor(V, Ty));
    if (DIB && !Imported)
      declareGlobalDebugInfo(V, Globals[V]);
  }

  void emitGlobals(Block &Blk) {
    for (Decl *D : Blk.getDecls()) {
      if (ClassType *C = definedClass(D)) { // class variables: no debug info
        for (Decl *M : C->getMembers())
          if (auto *V = llvm::dyn_cast<VarDecl>(M)) {
            llvm::Type *Ty = memType(V->getType());
            Globals[V] = createGlobal(V, Ty, llvm::Constant::getNullValue(Ty));
          }
        continue;
      }
      auto *V = llvm::dyn_cast<VarDecl>(D);
      if (!V)
        continue;
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
    if (HasSelf) // methods: Self after the static link
      ParamTys.push_back(B.getPtrTy());
    for (VarDecl *P : Params)
      ParamTys.push_back(P->isVarParam() || P->getType()->isStructured() ||
                                 P->getType()->isUntyped()
                             ? B.getPtrTy()
                             : regType(P->getType()));
    llvm::Type *RetTy = Result ? regType(Result) : B.getVoidTy();
    return llvm::FunctionType::get(RetTy, ParamTys, false);
  }

  llvm::FunctionType *routineFnType(RoutineDecl *R) {
    return routineFnType(R->getParams(),
                         R->isFunction() ? R->getResultType() : nullptr,
                         R->hasSelf());
  }

  /// An external (C) routine: no static link, C types (a callback: a
  /// function pointer), maybe variadic.
  void declareExternal(RoutineDecl *R) {
    llvm::SmallVector<llvm::Type *, 8> ParamTys;
    for (VarDecl *P : R->getParams())
      ParamTys.push_back(P->isVarParam() || P->getType()->isUntyped() ||
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
        Target->getFunctionType()->param_begin() + 1,
        Target->getFunctionType()->param_end());
    auto *T = llvm::Function::Create(llvm::FunctionType::get(Result, ParamTys, false),
                                     llvm::Function::InternalLinkage,
                                     "cthunk." + Target->getName(), M);
    llvm::IRBuilder<> TB(llvm::BasicBlock::Create(M.getContext(), "entry", T));
    llvm::SmallVector<llvm::Value *, 8> Args{
        llvm::ConstantPointerNull::get(B.getPtrTy())};
    for (unsigned I = 0; I < T->arg_size(); ++I) {
      T->getArg(I)->setName(R->getParams()[I]->getName().Name);
      Args.push_back(T->getArg(I));
    }
    llvm::Value *V = TB.CreateCall(Target, Args);
    if (Result->isVoidTy())
      TB.CreateRetVoid();
    else
      TB.CreateRet(V);
    CThunks[R] = T;
  }

  /// The function a procedural value of routine R points to.
  llvm::Value *routineFunction(RoutineDecl *R) {
    if (R->isExternal())
      return ExternalFns[R].getCallee();
    if (llvm::Function *T = CThunks.lookup(R))
      return T;
    return Functions[R];
  }

  void declareRoutines(Block &Blk) {
    for (Decl *D : Blk.getDecls()) {
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
          if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && ImportedDecls.count(R))
            Functions[R] = llvm::Function::Create(routineFnType(R),
                                                  llvm::Function::ExternalLinkage,
                                                  ExportNames[R], this->M);
        continue;
      }
      auto *R = llvm::dyn_cast<RoutineDecl>(D);
      if (R && R->isRedeclaration() && R->getCanonical()->isMethod()) {
        declareRoutine(R->getCanonical()); // a method's implementation
        continue;
      }
      if (!R || R->isRedeclaration())
        continue; // the forward declaration is the canonical node
      if (R->isExternal()) {
        declareExternal(R);
        continue;
      }
      if (ImportedDecls.count(R)) {
        // A routine of a separately compiled unit: declared only.
        Functions[R] = llvm::Function::Create(
            routineFnType(R), llvm::Function::ExternalLinkage, ExportNames[R], M);
        declareCThunk(R);
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
    auto Export = ExportNames.find(D);
    bool Imported = ImportedDecls.count(D);
    auto *Ty = llvm::ArrayType::get(B.getPtrTy(), 4 + C->getVTable().size());
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
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && !R->isAbstract())
        Functions[R] = llvm::Function::Create(routineFnType(R),
                                              llvm::Function::ExternalLinkage,
                                              ExportNames[R], this->M);
  }

  /// A runtime function used only by programs with classes.
  llvm::FunctionCallee classRuntime(llvm::StringRef Name, llvm::Type *Ret,
                                    llvm::ArrayRef<llvm::Type *> Params) {
    llvm::FunctionCallee F = M.getOrInsertFunction(
        Name, llvm::FunctionType::get(Ret, Params, /*isVarArg=*/false));
    llvm::cast<llvm::Function>(F.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
    return F;
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
          M, llvm::ArrayType::get(B.getPtrTy(), 5), /*isConstant=*/true,
          llvm::GlobalValue::ExternalLinkage, nullptr, Name);
    VMTs[C] = G;
    return G;
  }

  /// Fills in the VMTs: parent VMT, class name, virtual methods.
  void defineVMTs() {
    for (ClassType *C : VMTsToDefine) {
      llvm::SmallVector<llvm::Constant *, 8> Slots;
      Slots.push_back(vmtOf(C->getParent()));
      Slots.push_back(getPStrConstant(C->getDeclName()));
      Slots.push_back(llvm::ConstantExpr::getIntToPtr(
          B.getInt64(instanceSize(C)), B.getPtrTy()));
      Slots.push_back(interfaceTable(C));
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
    bool PinAll = R->hasTry() || hasNonLocalLabels(*R->getBody());
    auto AddSlot = [&](VarDecl *V) {
      if (!inFrame(V, PinAll))
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
        else
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
    F->getArg(ArgNo++)->setName("link");
    if (R->hasSelf())
      F->getArg(ArgNo++)->setName("self");
    for (VarDecl *P : R->getParams())
      F->getArg(ArgNo++)->setName(P->getName().Name);
    if (R->isInline()) // the `inline` directive
      F->addFnAttr(llvm::Attribute::InlineHint);
    Functions[R] = F;
    declareCThunk(R);
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

  void emitRoutineBody(RoutineDecl *R) {
    llvm::Function *F = Functions[R];
    CurFn = F;
    CurRoutine = R;
    CurSP = DIB ? Subprograms[R] : nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
    setDebugLoc(R->getBodyLocation());

    // The frame record, if any, then an alloca for each other variable.
    llvm::StructType *FrameTy = FrameTypes.lookup(R);
    CurFrame = FrameTy ? B.CreateAlloca(FrameTy, nullptr, "frame") : nullptr;
    LocalAddrs.clear();
    VarDecl *ResultVar = R->isFunction() ? R->getResultVar() : nullptr;
    llvm::SmallVector<VarDecl *, 8> Locals; // start from zero
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D); V && !V->isTypedConst())
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
    for (VarDecl *V : Locals)
      emitZeroInit(varSlot(V), V->getType());

    unsigned ArgNo = 1;
    if (LinkInFrame.count(R))
      B.CreateStore(F->getArg(0), B.CreateStructGEP(FrameTy, CurFrame, 0));
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
          V && !V->isTypedConst() && isManaged(V->getType()))
        Managed.push_back({varAddress(V), V->getType()});
    bool ManagedResult = ResultVar && isManaged(ResultVar->getType());

    initFileVars(R->getBody()->getDecls(), nullptr);
    if (JmpBufSlots.count(R))
      emitSetJmpDispatch(*R->getBody(),
                         B.CreateStructGEP(FrameTy, CurFrame, JmpBufSlots[R]));
    // An exception leaving the routine releases them too (and the result).
    llvm::Value *ExcFrame = nullptr;
    llvm::BasicBlock *Raised = nullptr;
    if (!Managed.empty() || ManagedResult || R->usesManaged()) {
      Raised = newBlock("managed.raise");
      ExcFrame = emitProtect(Raised, "managed.body");
    }
    RoutineTemps.clear();
    ExitBlock = newBlock("exit");
    visitStmt(R->getBody()->getBody());
    B.CreateBr(ExitBlock);
    B.SetInsertPoint(ExitBlock);
    auto Finalize = [&] {
      for (auto &[Addr, T] : Managed)
        finalizeValue(Addr, T);
      for (llvm::Value *Slot : RoutineTemps)
        clearTemp(Slot);
    };
    if (ExcFrame)
      B.CreateCall(excRuntime("__pas_exc_pop"), {ExcFrame});
    Finalize();
    for (Decl *D : R->getBody()->getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D))
        emitFileLifetime(varAddress(V), V->getType(), /*Init=*/false);

    if (Raised) {
      llvm::BasicBlock *Ret = B.GetInsertBlock();
      B.SetInsertPoint(Raised);
      Finalize();
      if (ManagedResult)
        finalizeValue(varAddress(ResultVar), ResultVar->getType());
      B.CreateCall(excRuntime("__pas_reraise"), {ExcFrame});
      B.CreateUnreachable();
      B.SetInsertPoint(Ret);
    }
    RoutineTemps.clear();

    if (R->isFunction()) {
      B.CreateRet(loadScalar(varSlot(ResultVar), ResultVar->getType(), "result"));
    } else {
      B.CreateRetVoid();
    }
  }

  //===--------------------------------------------------------------------===//
  // Calls
  //===--------------------------------------------------------------------===//

  llvm::Value *staticLinkFor(RoutineDecl *R) {
    return R->hasStaticLink() ? framePtrFor(R->getParent())
                              : llvm::ConstantPointerNull::get(B.getPtrTy());
  }

  /// {function, static link} for a routine passed as a procedural argument.
  llvm::Value *emitRoutineValue(Expr *Arg) {
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

  llvm::Value *emitCallTo(llvm::FunctionCallee Callee, llvm::Value *Link,
                          llvm::ArrayRef<VarDecl *> Params,
                          llvm::ArrayRef<Expr *> Args,
                          llvm::Value *Self = nullptr,
                          Type *ResultTy = nullptr, bool ToC = false) {
    llvm::SmallVector<llvm::Value *, 8> Values;
    if (Link) // external (C) routines have none
      Values.push_back(Link);
    if (Self) // methods
      Values.push_back(Self);
    emitArgValues(Params, Args, Values, ToC);
    llvm::Value *Result = B.CreateCall(Callee, Values);
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
    if (ResultTy && isManaged(ResultTy))
      return holdResult(Result);
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
    for (size_t I = 0; I < Args.size(); ++I) {
      Expr *Arg = Args[I];
      if (I >= Params.size()) {
        Values.push_back(emitVarArg(Arg));
        continue;
      }
      VarDecl *P = Params[I];
      if (P->getType()->isUntyped()) // Turbo/FPC: a variable, by reference
        Values.push_back(emitAddress(Arg));
      else if (auto *OA = llvm::dyn_cast<OpenArrayType>(P->getType()))
        Values.push_back(emitOpenArrayArg(Arg, OA));
      else if (P->getType()->isArrayOfConst())
        Values.push_back(emitConstArray(Arg));
      else if (llvm::Constant *CStr = cStringFor(P->getType(), Arg))
        Values.push_back(CStr);
      else if (llvm::isa<RoutineType>(P->getType()) && ToC) {
        llvm::Value *Pair = emitRoutineValue(Arg);
        Values.push_back(B.CreateExtractValue(Pair, 0));
      } else if (auto *RT = llvm::dyn_cast<RoutineType>(P->getType());
                 RT && RT->isOfObject())
        Values.push_back(P->isVarParam() ? emitAddress(Arg) : emitRoutinePair(Arg, RT));
      else if (llvm::isa<RoutineType>(P->getType()))
        Values.push_back(emitRoutineValue(Arg));
      else if (P->isConstParam() && P->getType()->isShortString())
        Values.push_back(emitPStr(Arg)); // read-only: no copy
      else if (auto *SS = llvm::dyn_cast<ShortStringType>(P->getType());
               SS && !P->isVarParam()) {
        // The callee copies MaxLen+1 bytes: pass a full-size temporary.
        llvm::Value *Tmp = newStringTemp(SS->getMaxLength());
        emitStringAssign(Tmp, SS, Arg);
        Values.push_back(Tmp);
      } else if (P->isVarParam() || P->getType()->isStructured())
        Values.push_back(emitAddress(Arg));
      else if (P->getType()->isAnsiString())
        Values.push_back(emitAnsi(Arg));
      else
        Values.push_back(convertForAssign(visitExpr(Arg), Arg->getType(),
                                          P->getType(), Arg->getBeginLoc()));
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
      auto *C = llvm::cast<ConstDecl>(llvm::cast<NameRefExpr>(Arg)->getDecl());
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
    auto *N = llvm::dyn_cast<NameRefExpr>(Arg);
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
    if (R->isExternal())
      return emitCallTo(ExternalFns[R], nullptr, R->getParams(), Args, nullptr,
                        nullptr, /*ToC=*/true);
    return emitCallTo(Functions[R], staticLinkFor(R), R->getParams(), Args,
                      nullptr, resultOf(R));
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
      } else if (N && llvm::isa<TypeDecl>(N->getDecl())) {
        Obj = vmtOf(llvm::cast<ClassType>(N->getType()));
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
        Code = loadVMTSlot(Obj, 4 + R->getVMTIndex(), "method");
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
      auto *R = llvm::cast<RoutineDecl>(
          llvm::cast<NameRefExpr>(A->getOperand())->getDecl());
      llvm::Value *Pair = llvm::UndefValue::get(memType(T));
      Pair = B.CreateInsertValue(Pair, routineFunction(R), 0);
      return B.CreateInsertValue(Pair, staticLinkFor(R), 1);
    }
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
    if (T->isInterface()) {
      B.CreateCall(intfRuntime("__pas_intf_clear"), {Addr});
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
    if (T->isInterface()) {
      llvm::Value *Obj = B.CreateLoad(B.getPtrTy(), Addr, "obj");
      B.CreateCall(intfRuntime("__pas_intf_addref"), {Obj});
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
      astrCall("__pas_astr_assign", B.getVoidTy(), {Addr, V});
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
  /// holds): kept in a temporary until the statement ends.
  llvm::Value *holdResult(llvm::Value *V) {
    llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                            CurFn->getEntryBlock().begin());
    llvm::Value *Slot = Entry.CreateAlloca(
        V->getType(), nullptr, V->getType()->isPointerTy() ? "astr.tmp" : "intf.tmp");
    Entry.CreateStore(llvm::Constant::getNullValue(V->getType()), Slot);
    clearTemp(Slot); // a loop's previous
    B.CreateStore(V, Slot);
    LiveTemps.push_back(Slot);
    if (CurRoutine)
      RoutineTemps.push_back(Slot);
    return V;
  }

  /// A function's record result V, holding ansistrings the caller now owns:
  /// kept in a temporary until the statement ends. Returns its address.
  llvm::Value *holdRecordResult(llvm::Value *V, Type *T) {
    llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                            CurFn->getEntryBlock().begin());
    llvm::Value *Slot = Entry.CreateAlloca(V->getType(), nullptr, "rec.tmp");
    Entry.CreateStore(llvm::Constant::getNullValue(V->getType()), Slot);
    finalizeValue(Slot, T); // a loop's previous
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
    CurRoutine = nullptr;
    CurFrame = nullptr;
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
    if (W->IsSelf) // load Self; a record's Self is its address
      return W->Record->getType()->isRecord() ? emitAddress(W->Record)
                                              : visitExpr(W->Record);
    return WithAddrs.lookup(W);
  }

  //===--------------------------------------------------------------------===//
  // Advanced records (objfpc)
  //===--------------------------------------------------------------------===//

  /// The value of E, a user-defined conversion applied (its operator called
  /// with E's unconverted value); an address for a record.
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
    return loadVMTSlot(B.CreateLoad(B.getPtrTy(), Obj, "vmt"), 4 + Index,
                       "method");
  }

  /// Entry Slot of a VMT: 0 the parent's VMT, 1 the class name, 2 the
  /// instance size, 3 the interface table, 4... the virtual methods.
  llvm::Value *loadVMTSlot(llvm::Value *VMT, unsigned Slot,
                           const llvm::Twine &Name) {
    llvm::Value *P = B.CreateConstInBoundsGEP1_64(B.getPtrTy(), VMT, Slot);
    return B.CreateLoad(B.getPtrTy(), P, Name);
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
    if (R->getClass() && R->getClass()->getRecord())
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
      if (!Inherited && R->isDispatched()) {
        emitNilClassCheck(Self, L);
        Fn = {routineFnType(R), loadVMTSlot(Self, 4 + R->getVMTIndex(), "method")};
      }
      Result = emitCallTo(Fn, llvm::ConstantPointerNull::get(B.getPtrTy()),
                          R->getParams(), Args, Self, resultOf(R));
      return R->isFunction() ? Result : nullptr;
    }
    if (!Inherited && R->isDispatched()) {
      emitNilObjectCheck(Obj, L);
      llvm::Value *Fn = loadVirtual(Obj, static_cast<unsigned>(R->getVMTIndex()));
      Result = emitCallTo({routineFnType(R), Fn},
                          llvm::ConstantPointerNull::get(B.getPtrTy()),
                          R->getParams(), Args, Obj, resultOf(R));
    } else {
      Result = emitCallTo(Functions[R], llvm::ConstantPointerNull::get(B.getPtrTy()),
                          R->getParams(), Args, Obj, resultOf(R));
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
    auto *Failed = newBlock("ctor.raise");
    llvm::Value *F = emitProtect(Failed, "ctor.call");
    B.CreateCall(Functions[Ctor], Values);
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
                   loadVMTSlot(VMT, 4 + Ctor->getVMTIndex(), "ctor"), Values);
    else
      B.CreateCall(Functions[Ctor], Values);
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
  llvm::Value *emitMemberCall(FieldExpr *F, llvm::ArrayRef<Expr *> Args) {
    auto *R = llvm::cast<RoutineDecl>(F->getMember());
    if (auto *RT = llvm::dyn_cast<RecordType>(F->getBase()->getType())) {
      auto *N = llvm::dyn_cast<NameRefExpr>(F->getBase());
      if (N && llvm::isa<TypeDecl>(N->getDecl())) { // TR.Create, TR.M
        if (R->isConstructor())
          return emitRecordConstruction(RT, R, Args);
        return emitCall(R, Args);
      }
      if (!R->hasSelf())
        return emitCall(R, Args);
      return emitRecordMethodCall(R, emitAddress(F->getBase()), Args);
    }
    if (F->getBase()->getType()->isInterface()) { // through the method table
      llvm::Value *V = visitExpr(F->getBase());
      llvm::Value *Obj = B.CreateExtractValue(V, 0, "obj");
      llvm::Value *Table = B.CreateExtractValue(V, 1, "itable");
      if (rangeChecks(F->getField().Loc))
        emitRuntimeCheck(B.CreateIsNull(Obj), "use of a nil interface reference",
                         F->getField().Loc);
      llvm::Value *Slot =
          B.CreateConstInBoundsGEP1_64(B.getPtrTy(), Table, R->getVMTIndex());
      llvm::Value *Fn = B.CreateLoad(B.getPtrTy(), Slot, "method");
      llvm::Value *Result =
          emitCallTo({routineFnType(R), Fn},
                     llvm::ConstantPointerNull::get(B.getPtrTy()),
                     R->getParams(), Args, Obj, resultOf(R));
      return R->isFunction() ? Result : nullptr;
    }
    if (auto *N = llvm::dyn_cast<NameRefExpr>(F->getBase());
        N && llvm::isa<TypeDecl>(N->getDecl())) {
      auto *C = llvm::cast<ClassType>(N->getType());
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
      emitRuntimeCheck(Overflow, "integer overflow in 'div'", Loc);
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
      llvm::Value *L = visitExpr(E->getLHS()), *R = visitExpr(E->getRHS());
      llvm::Value *Zero = llvm::ConstantInt::get(setType(), 0);
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
        LT->isInterface()) { // interfaces: their objects
      llvm::Value *L = interfaceObject(visitExpr(E->getLHS()));
      llvm::Value *R = interfaceObject(visitExpr(E->getRHS()));
      return Op == BinaryOp::Eq ? B.CreateICmpEQ(L, R, "cmp")
                                : B.CreateICmpNE(L, R, "cmp");
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
    for (Expr *Arg : Args)
      emitWriteArg(File, Arg);
    if (IsWriteln)
      B.CreateCall(RT.Writeln, {File});
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
    else if (T->getBase()->isBoolean())
      B.CreateCall(RT.WriteBool, {File, B.CreateZExt(V, B.getInt32Ty()), Width});
    else
      B.CreateCall(RT.WriteInt, {File, toI64(V, T), Width});
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

  llvm::Value *emitBuiltinCall(CallExpr *E, BuiltinDecl *BD) {
    switch (BD->getBuiltinKind()) {
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
        V = IsHigh ? Bd.Hi : Bd.Lo;
      }
      return llvm::ConstantInt::get(regType(E->getType()),
                                    static_cast<uint64_t>(V), true);
    }
    case BuiltinKind::Assigned: {
      Expr *Arg = E->getArgs()[0];
      llvm::Value *P;
      if (isMethodPointerType(Arg->getType()))
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
        emitRuntimeCheck(B.CreateICmpEQ(Arg, Min), "integer overflow in 'abs'", Loc);
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
        LangOpts(LangOpts), CGOpts(CGOpts) {}

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
    defineVMTs();
    for (RoutineDecl *R : RoutinesToEmit)
      emitRoutineBody(R);
    emitCleanups();

    CurFn = Main;
    CurRoutine = nullptr;
    CurFrame = nullptr;
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
      for (UnitDecl *U : Units)
        visitStmt(U->getImplementation()->getBody());
    }
    initFileVars(Blk.getDecls(), &ParamIndex);
    if (MainJmpBuf)
      emitSetJmpDispatch(Blk, MainJmpBuf);
    ExitBlock = newBlock("exit");
    visitStmt(Blk.getBody());
    B.CreateBr(ExitBlock);
    B.SetInsertPoint(ExitBlock);
    B.CreateCall(RT.Fini);
    B.CreateRet(B.getInt32(0));
    if (DIB)
      DIB->finalize();
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
    defineVMTs();
    for (RoutineDecl *R : RoutinesToEmit)
      emitRoutineBody(R);
    emitCleanups();

    CurFn = Init;
    CurRoutine = nullptr;
    CurFrame = nullptr;
    CurSP = DIB ? createSubprogram(Init, U.getName().Spelling, DIUnitFile,
                                   lineOf(U.getLocation()), nullptr, {})
                : nullptr;
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Init));
    setDebugLoc(U.getLocation());
    initFileVars(U.getInterface()->getDecls(), nullptr);
    initFileVars(U.getImplementation()->getDecls(), nullptr);
    ExitBlock = newBlock("exit");
    visitStmt(U.getImplementation()->getBody());
    B.CreateBr(ExitBlock);
    B.SetInsertPoint(ExitBlock);
    B.CreateRetVoid();
    if (DIB)
      DIB->finalize();
  }

  /// Statements carry their source position into the debug info.
  void visitStmt(Stmt *S) {
    setDebugLoc(S->getBeginLoc());
    size_t Mark = LiveTemps.size();
    StmtVisitor<CodeGenModule>::visitStmt(S);
    releaseTemps(Mark); // the results held by its expressions
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
    if (PropertyDecl *P = assignedProperty(Target)) {
      emitPropertyWrite(P, Target, Value);
      return;
    }
    if (auto *SS = llvm::dyn_cast<ShortStringType>(TT)) {
      emitStringAssign(emitAddress(Target), SS, Value);
      return;
    }
    if (TT->isAnsiString()) {
      llvm::Value *V = emitAnsi(Value);
      storeValue(emitAddress(Target), V, TT);
      return;
    }
    if (TT->isStructured()) {
      llvm::Value *Dst = emitAddress(Target);
      copyValue(Dst, emitAddress(Value), TT);
      return;
    }
    if (llvm::isa<RoutineType>(TT)) { // procedural variable (Turbo/FPC)
      llvm::Value *Pair = emitRoutinePair(Value, TT);
      B.CreateStore(Pair, emitAddress(Target));
      return;
    }
    llvm::Value *V = convertForAssign(visitExpr(Value), Value->getType(), TT,
                                      Value->getBeginLoc());
    storeValue(emitAddress(Target), V, TT);
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
    llvm::Value *Obj;
    Type *C;
    if (auto *I = llvm::dyn_cast<IndexExpr>(Target)) {
      Obj = propertyObject(I, C);
    } else if (auto *N = llvm::dyn_cast<NameRefExpr>(Target)) {
      Obj = withBase(N->getWith());
      C = N->getWith()->Record->getType();
    } else {
      auto *F = llvm::cast<FieldExpr>(Target);
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
    Type *T = P->getType();
    llvm::Value *Addr = fieldAddress(Obj, C, llvm::cast<FieldDecl>(P->getWrite()));
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
      B.CreateStore(emitRoutinePair(Value, T), Addr);
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
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Write:
    case BuiltinKind::Writeln:
      emitWrite(S, BD->getBuiltinKind() == BuiltinKind::Writeln);
      return;
    case BuiltinKind::Read:
    case BuiltinKind::Readln:
      emitRead(S, BD->getBuiltinKind() == BuiltinKind::Readln);
      return;
    case BuiltinKind::Reset: case BuiltinKind::Rewrite: case BuiltinKind::Get:
    case BuiltinKind::Put: case BuiltinKind::Page: {
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
        else if (isMethodPointerType(RT))
          B.CreateStore(emitRoutinePair(Value, RT), Addr);
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
      if (T->isIntegerLike() && intBits(T) == 64) { // no wider type to check in
        llvm::Value *New = emitCheckedArith(IsInc ? BinaryOp::Add : BinaryOp::Sub,
                                            /*Signed=*/true, Old, Step, L,
                                            S->getCallee().Name);
        emitRangeCheck(New, T->getBase(), T, L);
        storeScalar(Addr, New, T);
        return;
      }
      // Compute in i64 so the range check sees the true result.
      llvm::Value *Old64 = toI64(Old, T);
      llvm::Value *Delta = IsInc ? Step : B.CreateNeg(Step);
      llvm::Value *Wide = B.CreateAdd(Old64, Delta);
      if (T->isIntegerLike() ? overflowChecks(L) || rangeChecks(L) : rangeChecks(L)) {
        OrdinalBounds Bd = getOrdinalBounds(T);
        emitRuntimeCheck(outsideRange(Wide, Bd.Lo, Bd.Hi),
                         T->isIntegerLike() ? "integer overflow in '" +
                                                  S->getCallee().Name.str() + "'"
                                            : "value out of range",
                         L);
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
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
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
    llvm::Value *Enter = Down ? B.CreateICmpSGE(Start64, Final64)
                              : B.CreateICmpSLE(Start64, Final64);
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
    Loops.push_back({LatchBB, EndBB, Cleanups.size()});
    visitStmt(S->getBody());
    Loops.pop_back();
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

  /// PasExcFrame (runtime/include/pascalrt.h): prev, object, where, jmp_buf.
  llvm::StructType *excFrameType() {
    return llvm::StructType::get(B.getPtrTy(), B.getPtrTy(), B.getPtrTy(),
                                 jmpBufType());
  }

  /// The runtime's exception entry points, declared when first used.
  llvm::FunctionCallee excRuntime(llvm::StringRef Name) {
    llvm::Type *Ptr = B.getPtrTy();
    llvm::Type *Void = B.getVoidTy();
    bool NoReturn = false;
    llvm::FunctionCallee F;
    if (Name == "__pas_exc_push")
      F = classRuntime(Name, Void, {Ptr, Ptr});
    else if (Name == "__pas_raise_divzero") {
      F = classRuntime(Name, Void, {Ptr});
      llvm::cast<llvm::Function>(F.getCallee())->addFnAttr(llvm::Attribute::Cold);
      NoReturn = true;
    } else if (Name == "__pas_raise" || Name == "__pas_reraise_free") {
      F = classRuntime(Name, Void, {Ptr, Ptr});
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
  /// named BodyName (the insertion point). Returns the frame.
  llvm::Value *emitProtect(llvm::BasicBlock *Landing, const llvm::Twine &BodyName) {
    llvm::Value *F = createTemp(excFrameType(), "excframe");
    // The routine's frame goes along so that the optimizer keeps its
    // variables in memory, where _longjmp does not undo their changes.
    llvm::Value *Frame =
        CurFrame ? CurFrame : llvm::ConstantPointerNull::get(B.getPtrTy());
    B.CreateCall(excRuntime("__pas_exc_push"), {F, Frame});
    llvm::Value *R = B.CreateCall(
        RT.SetJmp, {B.CreateStructGEP(excFrameType(), F, 3)}, "jmp");
    auto *Body = newBlock(BodyName);
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

  void visitTryStmt(TryStmt *S) {
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
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
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
      llvm::Value *Is = B.CreateCall(
          classRuntime("__pas_class_is", B.getInt32Ty(),
                       {B.getPtrTy(), B.getPtrTy()}),
          {Obj, vmtOf(Handler->getClass())});
      auto *Do = newBlock("handler.do");
      auto *Next = newBlock("handler.next");
      B.CreateCondBr(B.CreateICmpNE(Is, B.getInt32(0)), Do, Next);
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
      for (Stmt *Child : S->getExceptBody())
        visitStmt(Child);
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
    auto *Raised = newBlock("finally.raise");
    auto *Finally = newBlock("finally");
    auto *End = newBlock("try.end");
    llvm::Value *State = createTemp(B.getInt32Ty(), "finally.state");
    llvm::Value *F = emitProtect(Raised, "try.body");
    Cleanups.emplace_back(Cleanup::Finally, F, nullptr, State, Finally);
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
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
    for (Stmt *Child : S->getFinallyBody())
      visitStmt(Child);
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

  void visitRaiseStmt(RaiseStmt *S) {
    if (S->isReraise()) {
      B.CreateCall(excRuntime("__pas_reraise"), {HandledFrames.back()});
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
    return llvm::ConstantInt::get(regType(E->getType()), E->getValue());
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
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D)) {
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
    if (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D))
      return loadScalar(emitAddress(E), E->getType(), E->getName().Name);
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D))
      return emitCall(R, {}); // parameterless function
    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D)) {
      if (BD->getBuiltinKind() == BuiltinKind::ParamCount)
        return B.CreateCall(RT.ParamCount, {}, "paramcount");
      return emitFileQuery(BD->getBuiltinKind(), nullptr); // eof / eoln
    }
    auto *C = llvm::cast<ConstDecl>(D);
    const ConstValue &V = C->getValue();
    if (V.K == ConstValue::Real)
      return llvm::ConstantFP::get(B.getDoubleTy(), V.RealVal);
    return llvm::ConstantInt::get(regType(C->getType()),
                                  static_cast<uint64_t>(V.Int), true);
  }

  llvm::Value *visitParenExpr(ParenExpr *E) { return visitExpr(E->getInner()); }

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
    llvm::Constant *VMT = vmtOf(llvm::cast<ClassType>(E->getRHS()->getType()));
    if (E->getOp() == BinaryOp::Is) {
      llvm::Value *R = B.CreateCall(
          classRuntime("__pas_class_is", B.getInt32Ty(), {B.getPtrTy(), B.getPtrTy()}),
          {Obj, VMT});
      return B.CreateICmpNE(R, B.getInt32(0), "is");
    }
    llvm::Constant *Msg = B.CreateGlobalString(
        ("invalid class typecast" + where(E->getOpLoc())), ".rterr");
    return B.CreateCall(classRuntime("__pas_class_as", B.getPtrTy(),
                                     {B.getPtrTy(), B.getPtrTy(), B.getPtrTy()}),
                        {Obj, VMT, Msg}, "as");
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

    if (E->getType()->isSet()) {
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

  llvm::Value *visitFormattedExpr(FormattedExpr *) {
    llvm_unreachable("formatted expressions are handled by emitWriteArg");
  }

  llvm::Value *visitIndirectCallExpr(IndirectCallExpr *E) {
    Expr *Callee = E->getCallee();
    if (auto *I = llvm::dyn_cast<InheritedExpr>(Callee))
      return emitInheritedCall(I, E->getArgs());
    if (auto *F = llvm::dyn_cast<FieldExpr>(Callee);
        F && llvm::isa_and_nonnull<RoutineDecl>(F->getMember()))
      return emitMemberCall(F, E->getArgs());
    auto *RT = llvm::cast<RoutineType>(Callee->getType());
    llvm::Value *Pair = RT->isOfObject()
                            ? visitExpr(Callee)
                            : B.CreateLoad(memType(RT), emitAddress(Callee));
    return emitCallThrough(Pair, RT, E->getArgs(), Callee->getBeginLoc());
  }

  llvm::Value *visitInheritedExpr(InheritedExpr *E) {
    return emitInheritedCall(E, {});
  }

  llvm::Value *visitAggregateExpr(AggregateExpr *) {
    llvm_unreachable("aggregates only initialize typed constants");
  }

  llvm::Value *visitAddrOfExpr(AddrOfExpr *E) {
    if (E->getMethod())
      return emitMethodPointer(E);
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

  llvm::Value *visitSetCtorExpr(SetCtorExpr *E) { return emitSetCtor(E); }

  llvm::Value *visitIndexExpr(IndexExpr *E) {
    if (E->getProperty())
      return emitArrayPropertyRead(E);
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
pcc::generateModule(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
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
pcc::generateUnitModule(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps,
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
