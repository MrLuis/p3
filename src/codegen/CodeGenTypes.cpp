// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGenTypes.cpp - AST -> LLVM IR ---------------------------------------===//

#include "CodeGenInternal.h"

using namespace p3;
using namespace p3::detail;


//===--------------------------------------------------------------------===//
// Types
//===--------------------------------------------------------------------===//

/// In-memory representation of a Pascal type.
llvm::Type *CodeGenModule::memType(Type *T){
  if (llvm::Type *L = LoweredTypes.lookup(T))
    return L;
  llvm::Type *L = lowerType(T);
  LoweredTypes[T] = L;
  return L;
}


llvm::Type *CodeGenModule::lowerType(Type *T){
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
  case Type::TK_Vector: { // the lanes as one register-shaped value
    auto *V = llvm::cast<VectorType>(T);
    return llvm::VectorType::get(memType(V->getElementType()),
                                 static_cast<unsigned>(V->getCount()),
                                 /*Scalable=*/false);
  }
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
unsigned CodeGenModule::setBits(Type *T){
  return llvm::cast<SetType>(T)->getStorageBits();
}

/// The set value V of type From as a value of the set type To (the
/// members that do not fit are dropped).
llvm::Value *CodeGenModule::convertSet(llvm::Value *V, Type *From, Type *To){
  (void)From;
  return B.CreateZExtOrTrunc(V, setType(To));
}


/// Register representation: like memType, but booleans are i1.
llvm::Type *CodeGenModule::regType(Type *T){
  return T->getBase()->isBoolean() ? B.getInt1Ty() : memType(T);
}


/// Builds the struct for a record field list and records each field's
/// byte offset from the start of the record. Variants are laid out as
/// their own structs, all overlaying one storage buffer.
llvm::StructType *CodeGenModule::layoutFieldList(FieldList *FL, const std::string &Name,
                                  uint64_t BaseOffset){
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


/// Lays out the fields of FL from byte End as Free Pascal does under
/// {$packrecords N}: each field at a multiple of the smaller of its
/// alignment and N; a variant part (each variant from the same offset) at
/// a multiple of the smaller of N and 16. End becomes the end of the
/// last field; Align the largest alignment used.
void CodeGenModule::layoutAligned(FieldList *FL, uint64_t N, uint64_t &End,
                   uint64_t &Align){
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
uint64_t CodeGenModule::layoutBitPacked(FieldList *FL, uint64_t Start){
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


void CodeGenModule::shiftFieldOffsets(FieldList *FL, uint64_t Delta){
  for (FieldDecl *F : FL->Fixed)
    FieldOffsets[F] += Delta;
  if (VariantPart *VP = FL->Variant) {
    if (VP->Tag)
      FieldOffsets[VP->Tag] += Delta;
    for (Variant *V : VP->Variants)
      shiftFieldOffsets(V->Fields, Delta);
  }
}


/// What pointer arithmetic on a PT counts in: its pointee (bytes, for an
/// untyped pointer).
llvm::Type *CodeGenModule::pointeeMemType(PointerType *PT){
  return PT->isUntyped() || !PT->getPointee() ? B.getInt8Ty()
                                              : memType(PT->getPointee());
}


/// The layout of an object of class C: VMT pointer, then all fields.
/// Records each field's offset; a prefix of fields has the same offsets
/// in every descendant.
llvm::StructType *CodeGenModule::classLayout(ClassType *C){
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


uint64_t CodeGenModule::instanceSize(ClassType *C){
  return DL.getTypeAllocSize(classLayout(C));
}


/// The alignment of the value of type T at address P.
uint64_t CodeGenModule::addrAlign(llvm::Value *P, Type *T){
  auto It = AddrAligns.find(P);
  return It != AddrAligns.end() ? It->second : alignOf(T);
}


/// Notes that the component of type T at Addr, Offset bytes into a value
/// aligned to BaseAlign, may be less aligned than T.
void CodeGenModule::noteComponentAlign(llvm::Value *Addr, uint64_t BaseAlign,
                        uint64_t Offset, Type *T){
  uint64_t A = Offset ? std::min(BaseAlign, Offset & (~Offset + 1))
                      : BaseAlign;
  if (A < alignOf(T))
    AddrAligns[Addr] = A;
}


llvm::MDNode *CodeGenModule::tbaaCharScalarNode(){
  if (!TBAACharNode) {
    llvm::MDBuilder MDB(M.getContext());
    if (!TBAARoot)
      TBAARoot = MDB.createTBAARoot("Simple Pascal TBAA");
    TBAACharNode = MDB.createTBAAScalarTypeNode("omnipotent char", TBAARoot);
  }
  return TBAACharNode;
}


llvm::MDNode *CodeGenModule::tbaaTagFor(Type *T){
  if (!T)
    return nullptr;
  Type *Base = T->getBase();
  auto makeTag = [&](llvm::MDNode *&Slot, const char *Name) -> llvm::MDNode * {
    if (!Slot) {
      llvm::MDNode *Char = tbaaCharScalarNode();
      llvm::MDBuilder MDB(M.getContext());
      llvm::MDNode *Node =
          Name ? MDB.createTBAAScalarTypeNode(Name, Char) : Char;
      Slot = MDB.createTBAAStructTagNode(Node, Node, 0);
    }
    return Slot;
  };
  switch (Base->getKind()) {
  case Type::TK_Char:
    return makeTag(TBAA_Char, nullptr);
  case Type::TK_Boolean:
    return makeTag(TBAA_Bool, "bool");
  case Type::TK_WideChar:
    return makeTag(TBAA_I16, "i16");
  case Type::TK_Integer: {
    unsigned Bits = llvm::cast<IntegerType>(Base)->getBits();
    if (Bits <= 8)
      return makeTag(TBAA_Char, nullptr);
    if (Bits <= 16)
      return makeTag(TBAA_I16, "i16");
    if (Bits <= 32)
      return makeTag(TBAA_I32, "i32");
    return makeTag(TBAA_I64, "i64");
  }
  case Type::TK_Enum:
    return makeTag(TBAA_I32, "i32");
  case Type::TK_Real:
    return makeTag(TBAA_F64, "f64");
  case Type::TK_Set:
    return makeTag(TBAA_Set, "set");
  case Type::TK_Vector:
    return tbaaTagFor(llvm::cast<VectorType>(Base)->getElementType());
  case Type::TK_Pointer:
  case Type::TK_Nil:
  case Type::TK_File:
  case Type::TK_DynArray:
  case Type::TK_OpenArray:
  case Type::TK_ArrayOfConst:
  case Type::TK_AnsiString:
  case Type::TK_ClassRef:
  case Type::TK_Class:
  case Type::TK_Task:
  case Type::TK_Token:
  case Type::TK_Iterator:
    return makeTag(TBAA_Ptr, "any pointer");
  default:
    return nullptr;
  }
}


bool CodeGenModule::isAliasedExpr(const Expr *E, bool DirectDeref){
  if (!E)
    return false;
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
    if (DirectDeref)
      return true;
    const Expr *Inner = D->getBase();
    while (auto *P = llvm::dyn_cast<ParenExpr>(Inner))
      Inner = P->getInner();
    if (auto *C = llvm::dyn_cast<CallExpr>(Inner))
      if (C->isVarCast() || llvm::isa_and_nonnull<TypeDecl>(C->getDecl()))
        return true;
    return isAliasedExpr(Inner, /*DirectDeref=*/false);
  }
  if (auto *C = llvm::dyn_cast<CallExpr>(E))
    return C->isVarCast() || llvm::isa_and_nonnull<TypeDecl>(C->getDecl());
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl()))
      return V->isAliased() || V->getAbsolute() != nullptr ||
             (V->getAbsoluteRoot() && V->getAbsoluteRoot()->isAliased());
    if (N->getWith())
      return isAliasedExpr(N->getWith()->Record, /*DirectDeref=*/false);
    return false;
  }
  if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
    if (auto *RT = llvm::dyn_cast<RecordType>(F->getBase()->getType()))
      if (RT->getFieldList() && RT->getFieldList()->Variant)
        return true;
    return isAliasedExpr(F->getBase(), /*DirectDeref=*/false);
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E))
    return isAliasedExpr(I->getBase(), /*DirectDeref=*/false);
  return false;
}


llvm::Value *CodeGenModule::loadScalar(llvm::Value *Ptr, Type *T,
                                       const llvm::Twine &Name, bool AllowTBAA){
  llvm::LoadInst *Raw = B.CreateLoad(memType(T), Ptr, Name);
  if (auto It = AddrAligns.find(Ptr); It != AddrAligns.end())
    Raw->setAlignment(llvm::Align(It->second));
  if (AllowTBAA)
    if (llvm::MDNode *Tag = tbaaTagFor(T))
      Raw->setMetadata(llvm::LLVMContext::MD_tbaa, Tag);
  return T->getBase()->isBoolean() ? B.CreateTrunc(Raw, B.getInt1Ty()) : Raw;
}


void CodeGenModule::storeScalar(llvm::Value *Ptr, llvm::Value *Val, Type *T,
                                bool AllowTBAA){
  if (T->getBase()->isBoolean())
    Val = B.CreateZExt(Val, B.getInt8Ty());
  llvm::StoreInst *S = B.CreateStore(Val, Ptr);
  if (auto It = AddrAligns.find(Ptr); It != AddrAligns.end())
    S->setAlignment(llvm::Align(It->second));
  if (AllowTBAA)
    if (llvm::MDNode *Tag = tbaaTagFor(T))
      S->setMetadata(llvm::LLVMContext::MD_tbaa, Tag);
}


/// Copies the array, record or string T: both are aligned as T is (or as
/// AddrAligns says).
void CodeGenModule::copyAggregate(llvm::Value *Dst, llvm::Value *Src, Type *T){
  B.CreateMemCpy(Dst, llvm::Align(addrAlign(Dst, T)), Src,
                 llvm::Align(addrAlign(Src, T)), sizeOf(T));
}


/// Address of a private constant holding S (without a terminating NUL).
llvm::GlobalVariable *CodeGenModule::getStringConstant(llvm::StringRef S){
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

std::string CodeGenModule::where(SourceLocation L){
  PresumedLoc P = SM.getPresumedLoc(L);
  if (!P.isValid())
    return "";
  return (" at " + P.Filename + ":" + llvm::Twine(P.Line) + ":" +
          llvm::Twine(P.Column))
      .str();
}


llvm::BasicBlock *CodeGenModule::newBlock(const llvm::Twine &Name){
  return llvm::BasicBlock::Create(M.getContext(), Name, CurFn);
}


/// The .rterr global of a message (shared by every check that uses it).
llvm::Constant *CodeGenModule::errorMessage(llvm::StringRef Msg){
  llvm::Constant *&Text = ErrorMessages[Msg];
  if (!Text)
    Text = B.CreateGlobalString(Msg, ".rterr");
  return Text;
}


/// The .file global of a source file name (one per file, item 13).
llvm::Constant *CodeGenModule::fileString(llvm::StringRef File){
  llvm::Constant *&Text = FileStrings[File];
  if (!Text)
    Text = B.CreateGlobalString(File, ".file");
  return Text;
}


/// The pooled { file, line, column } record of L, or null (item 13).
llvm::Constant *CodeGenModule::locationRecord(SourceLocation L){
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
void CodeGenModule::emitRuntimeError(llvm::StringRef Msg, SourceLocation L){
  B.CreateCall(RT.RuntimeErrorAt, {errorMessage(Msg), locationRecord(L)});
  B.CreateUnreachable();
}


/// if (Cond) __pas_runtime_error("<Msg> at file:line:col");
void CodeGenModule::emitRuntimeCheck(llvm::Value *Cond, llvm::StringRef Msg,
                      SourceLocation L){
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


/// Converts an ordinal register value of type From to the register type of
/// To (sign- or zero-extending by From, or truncating). No range check.
llvm::Value *CodeGenModule::convertOrdinal(llvm::Value *V, Type *From, Type *To){
  llvm::Type *Dst = regType(To);
  if (V->getType() == Dst)
    return V;
  if (From->getBase()->isWideChar() && To->getBase()->isChar())
    return wideToChar(V);
  if (Dst->isIntegerTy(1))
    return B.CreateICmpNE(V, llvm::ConstantInt::get(V->getType(), 0));
  llvm::Value *R = isSignedOrdinal(From) ? B.CreateSExtOrTrunc(V, Dst)
                                         : B.CreateZExtOrTrunc(V, Dst);
  // A known range (or the value range of a non-qword ordinal) survives a
  // conversion that keeps the value in a signed target type.
  ValueRange VR = rangeOfValue(V), DstRange;
  if (!VR.Known && From->isOrdinal() && !isQWord(From->getBase())) {
    OrdinalBounds FB = getOrdinalBounds(From);
    if (FB.Lo <= FB.Hi)
      VR = {true, FB.Lo, FB.Hi};
  }
  if (VR.Known && (isSignedOrdinal(From) || !isQWord(From->getBase())) &&
      isSignedOrdinal(To) &&
      signedRange(Dst->getIntegerBitWidth(), DstRange) &&
      VR.Lo >= DstRange.Lo && VR.Hi <= DstRange.Hi)
    noteRange(R, VR);
  return R;
}


/// A widechar's code as a char, as Free Pascal converts it: '?' above
/// 255.
llvm::Value *CodeGenModule::wideToChar(llvm::Value *V){
  llvm::Value *Fits = B.CreateICmpULE(V, B.getInt16(255));
  return B.CreateSelect(Fits, B.CreateTrunc(V, B.getInt8Ty()), B.getInt8('?'),
                        "tochar");
}


/// A char or widechar expression's value as a char.
llvm::Value *CodeGenModule::emitCharValue(Expr *E){
  llvm::Value *V = visitExpr(E);
  return E->getType()->getBase()->isWideChar() ? wideToChar(V) : V;
}


/// A character as the (pointer, count) piece of a join: an own entry
/// byte holding it, instead of a fresh one-character string.
llvm::Value *CodeGenModule::emitCharPiece(Expr *E){
  llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                          CurFn->getEntryBlock().begin());
  llvm::Value *M =
      Entry.CreateAlloca(B.getInt8Ty(), nullptr, "char.tmp");
  Entry.CreateStore(llvm::Constant::getNullValue(B.getInt8Ty()), M);
  B.CreateStore(emitCharValue(E), M);
  return M;
}


llvm::Value *CodeGenModule::toDouble(llvm::Value *V, Type *T){
  if (T->isReal())
    return V;
  bool Signed = isSignedOrdinal(T);
  llvm::Value *R = Signed ? B.CreateSIToFP(V, B.getDoubleTy(), "toreal")
                          : B.CreateUIToFP(V, B.getDoubleTy(), "toreal");
  ValueRange VR = rangeOfValue(V);
  FloatBound FB;
  if (!Signed) {
    FB.NonNegOrNaN = true;
    if (VR.Known && VR.Lo > 0) {
      FB.NonZero = true;
      FB.MinPos = static_cast<double>(VR.Lo);
    }
  } else if (VR.Known) {
    FB.NonNegOrNaN = VR.Lo >= 0;
    FB.NonZero = VR.Lo > 0 || VR.Hi < 0;
    if (VR.Lo > 0)
      FB.MinPos = static_cast<double>(VR.Lo);
  }
  noteFloatBound(R, FB);
  return R;
}


//===--------------------------------------------------------------------===//
// Variables and frames
//===--------------------------------------------------------------------===//

/// Parameters passed as a pointer to the caller's variable: var
/// parameters, const parameters of string and structured types, and
/// constref parameters (to the caller's variable, or a temporary).
bool CodeGenModule::isByRef(VarDecl *V){
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
bool CodeGenModule::isConstRefPtr(VarDecl *P){
  Type *T = P->getType();
  return P->isConstRefParam() && !T->isStructured() && !T->isShortString() &&
         !T->isOpenArray() && !T->isArrayOfConst() && !T->isUntyped() &&
         !llvm::isa<RoutineType>(T);
}


/// Is a function result of type T written through a pointer the caller
/// passes (to a temporary of its own), instead of being returned? Short
/// strings are: as a value LLVM would move all their 256 bytes, one by
/// one. The function's result variable is then that temporary.
bool CodeGenModule::returnsByPointer(Type *T){
  if (!T)
    return false;
  if (T->isShortString())
    return true;
  // optimization3 item 1: a record of more than two registers (and
  // without counted fields, which the callee releases on an exception)
  // is written through the caller's temporary too. As a first-class
  // aggregate LLVM copies it element by element at every call.
  return T->isRecord() && !isManaged(T) && sizeOf(T) > 16;
}


/// pas.<outer>.<name> for routines, pas.<class>.<name> for methods.
std::string CodeGenModule::mangle(RoutineDecl *R){
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
std::string CodeGenModule::overloadSuffix(RoutineDecl *R){
  return R->isOverloaded() ? "$" + std::to_string(R->getOverloadIndex())
                           : std::string();
}


/// Pointer to the frame of routine Target, reached from the current
/// routine by following static links.
llvm::Value *CodeGenModule::framePtrFor(RoutineDecl *Target){
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
llvm::GlobalVariable *CodeGenModule::standardFile(llvm::StringRef Name){
  std::string Sym = ("__pas_" + Name).str();
  if (auto *G = M.getNamedGlobal(Sym))
    return G;
  return new llvm::GlobalVariable(M, B.getPtrTy(), false,
                                  llvm::GlobalValue::ExternalLinkage, nullptr,
                                  Sym);
}


llvm::Value *CodeGenModule::loadFile(llvm::Value *VarAddr){
  return B.CreateLoad(B.getPtrTy(), VarAddr, "file");
}


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
void CodeGenModule::pinInMemory(llvm::Value *Addr){
  if (!CurPin)
    CurPin = createTemp(B.getPtrTy(), "pin");
  B.CreateStore(Addr, CurPin, /*isVolatile=*/true);
}


/// Allocates a temporary in the entry block of the current function.
llvm::Value *CodeGenModule::createTemp(llvm::Type *Ty, const llvm::Twine &Name){
  llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                          CurFn->getEntryBlock().begin());
  return Entry.CreateAlloca(Ty, nullptr, Name);
}


/// Calls Fn(elementAddress) for every element of array A at Addr.
void CodeGenModule::forEachElement(llvm::Value *Addr, ArrayType *A,
                    llvm::function_ref<void(llvm::Value *)> Fn,
                    llvm::StringRef What){
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
void CodeGenModule::emitFileLifetime(llvm::Value *Addr, Type *T, bool Init,
                      llvm::Value *Name){
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
void CodeGenModule::initFileVars(llvm::ArrayRef<Decl *> Decls,
                  const llvm::StringMap<unsigned> *ParamIndex){
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
bool CodeGenModule::isErrno(VarDecl *V){ return V->getExternalName() == "errno"; }


llvm::Value *CodeGenModule::errnoAddress(){
  llvm::Triple T(M.getTargetTriple());
  const char *Name = T.isOSDarwin()    ? "__error"
                     : T.isOSWindows() ? "_errno"
                                       : "__errno_location";
  return B.CreateCall(classRuntime(Name, B.getPtrTy(), {}), {}, "errno");
}


llvm::Value *CodeGenModule::varAddress(VarDecl *V){
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
llvm::Value *CodeGenModule::varSlot(VarDecl *V){
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

llvm::Value *CodeGenModule::dynLength(llvm::Value *A){
  return astrCall("__pas_dyn_length", B.getInt64Ty(), {A});
}


/// The constant PasDynInfo of arrays of type T (runtime/include/pascalrt.h);
/// for managed elements, with functions that release and count one.
llvm::Constant *CodeGenModule::dynInfoOf(DynArrayType *T){
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
llvm::Constant *CodeGenModule::typeInfoOf(Type *T){
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


void CodeGenModule::emitDynOp(Type *Elem, llvm::Function *F, bool IsFini){
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
void CodeGenModule::emitDynOps(){
  for (const DynOp &Op : DynOpsToEmit)
    emitDynOp(Op.Elem, Op.F, Op.IsFini);
  DynOpsToEmit.clear();
}


/// setlength of the array variable at Addr, of type T: Ns[0] elements,
/// each of them (arrays) set to the next lengths.
void CodeGenModule::emitDynSetLength(llvm::Value *Addr, DynArrayType *T,
                      llvm::ArrayRef<llvm::Value *> Ns){
  llvm::Constant *Info = dynInfoOf(T);
  llvm::Value *Call =
      astrCall("__pas_dyn_setlength", B.getVoidTy(), {Addr, Info, Ns[0]});
  if (!dynMayRaise(T->getElementType()))
    if (auto *CB = llvm::dyn_cast<llvm::CallBase>(Call))
      NoRaiseCalls.insert(CB);
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
llvm::Value *CodeGenModule::emitDynArrayCtor(SetCtorExpr *S, DynArrayType *T){
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
bool CodeGenModule::inFrame(VarDecl *V, bool PinAll, const VarSet &PinVars){
  return PinAll || PinVars.count(V) || V->isCaptured() ||
         (!isByRef(V) && !V->isConstParam() && isManaged(V->getType()));
}


/// The start value of a local variable or function result at Addr: zero.
/// ISO leaves them undefined; this makes programs deterministic. Without
/// zero initialization (-fno-zero-init, the default from -O1) arrays,
/// records and strings are only cleared when they hold managed values or
/// files; a string starts empty.
void CodeGenModule::emitZeroInit(llvm::Value *Addr, Type *T){
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
llvm::Value *CodeGenModule::fieldAddress(llvm::Value *RecordAddr, Type *RT, FieldDecl *F){
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


CodeGenModule::BitRef CodeGenModule::bitRef(Expr *E) {
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


CodeGenModule::BitWindow CodeGenModule::bitWindow(const BitRef &R) {
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


llvm::Value *CodeGenModule::bitLoad(const BitRef &R){
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
void CodeGenModule::bitStore(Expr *Target, llvm::Value *V){
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
llvm::Value *CodeGenModule::emitAddress(Expr *E){
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
    if (NameRefExpr *Ref = llvm::cast<BinaryExpr>(E)->getConstRef())
      return emitAddress(Ref); // 'a' + 'b': a string constant
    return visitExpr(E); // a record's operator: a temporary holding it
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
      Type *Elem = OA->getElementType();
      const auto *H = checkedScopeOf(I->getBase());
      llvm::Value *Data = H ? H->second : emitAddress(I->getBase());
      llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
      if (rangeChecks(I->getIndex()->getBeginLoc()) && !H)
        emitRuntimeCheck(B.CreateICmpUGE(X, openCount(openDescriptor(I->getBase()))),
                         "array index out of bounds",
                         I->getIndex()->getBeginLoc());
      llvm::Value *Addr = B.CreateGEP(memType(Elem), Data, X, "",
                                      indexFlags(I->getIndex(), 0));
      if (alignOf(Elem) > 16)
        AddrAligns[Addr] = 16;
      return Addr;
    }
    if (auto *AC = llvm::dyn_cast<ArrayOfConstType>(I->getBase()->getType())) {
      llvm::Value *A = visitExpr(I->getBase());
      llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
      if (rangeChecks(I->getIndex()->getBeginLoc()) &&
          !checkedScopeOf(I->getBase()))
        emitRuntimeCheck(B.CreateICmpNE(astrCall("__pas_aoc_bad_index",
                                                 B.getInt32Ty(), {A, X}),
                                        B.getInt32(0)),
                         "array index out of bounds",
                         I->getIndex()->getBeginLoc());
      return B.CreateInBoundsGEP(memType(AC->getElementType()), A,
                                 B.CreateAdd(X, B.getInt64(1)));
    }
    if (auto *PT = llvm::dyn_cast<PointerType>(I->getBase()->getType())) {
      // p[i]: (p + i)^, unchecked as in Free Pascal unless {$pointermath checked}.
      llvm::Value *P = visitExpr(I->getBase());
      llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
      if (pointerMathChecked(I->getIndex()->getBeginLoc()))
        emitPointerIndexCheck(PT, P, X, I->getIndex()->getBeginLoc());
      return B.CreateGEP(pointeeMemType(PT), P, X);
    }
    if (I->getBase()->getType()->isDynArray()) {
      Type *Elem = llvm::cast<DynArrayType>(I->getBase()->getType())->getElementType();
      const auto *H = checkedScopeOf(I->getBase());
      bool SafeBounds = H || canElideDynBoundsCheck(I->getBase(), I->getIndex());
      llvm::Value *Data = H ? H->second : visitExpr(I->getBase());
      llvm::Value *X = toI64(visitExpr(I->getIndex()), I->getIndex()->getType());
      if (rangeChecks(I->getIndex()->getBeginLoc()) && !SafeBounds)
        emitRuntimeCheck(B.CreateICmpNE(astrCall("__pas_dyn_bad_index",
                                                 B.getInt32Ty(), {Data, X}),
                                        B.getInt32(0)),
                         "array index out of bounds",
                         I->getIndex()->getBeginLoc());
      llvm::Value *Addr = B.CreateGEP(memType(Elem), Data, X, "",
                                      indexFlags(I->getIndex(), 0));
      if (alignOf(Elem) > 16)
        AddrAligns[Addr] = 16;
      return Addr;
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
      return B.CreateGEP(B.getInt8Ty(), Data,
                         B.CreateSub(X, B.getInt64(1)), "",
                         indexFlags(I->getIndex(), 1));
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
      return B.CreateGEP(B.getInt8Ty(), Base,
                         B.CreateSub(X, B.getInt64(1)), "",
                         indexFlags(I->getIndex(), 1));
    }
    // vector[N] of T (llvm-23.md LB-6): elements 1..N, laid out like an
    // array of T (a GEP into the LLVM vector).
    if (auto *VT = llvm::dyn_cast<VectorType>(I->getBase()->getType())) {
      llvm::Value *Base = emitAddress(I->getBase());
      llvm::Value *Idx = visitExpr(I->getIndex());
      llvm::Value *X = toI64(Idx, I->getIndex()->getType());
      if (rangeChecks(I->getIndex()->getBeginLoc()))
        emitRuntimeCheck(outsideRange(X, 1, VT->getCount()),
                         "vector index out of bounds",
                         I->getIndex()->getBeginLoc());
      llvm::Value *Elem = B.CreateGEP(
          memType(VT), Base, {B.getInt64(0), B.CreateSub(X, B.getInt64(1))},
          "", indexFlags(I->getIndex(), 1));
      return Elem;
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
    llvm::Value *Elem = B.CreateGEP(memType(AT), Base,
                                    {B.getInt64(0), Offset}, "",
                                    indexFlags(I->getIndex(), Bd.Lo));
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


/// The constant for a typed-constant initializer Init of type T (memory
/// type Ty); Folded is Sema's value for non-aggregates.
llvm::Constant *CodeGenModule::constantFor(Expr *Init, const ConstValue &Folded, Type *T,
                            llvm::Type *Ty){
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
bool CodeGenModule::containsAlignedRecord(Type *T){
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
void CodeGenModule::writeConstantBytes(Expr *Init, const ConstValue &Folded, Type *T,
                        std::string &Bytes, uint64_t Offset){
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


llvm::Constant *CodeGenModule::scalarConstant(const ConstValue &C, Type *T, llvm::Type *Ty){
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
llvm::Constant *CodeGenModule::bytesConstant(llvm::StringRef Bytes, uint64_t Offset,
                              llvm::Type *Ty){
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


llvm::Constant *CodeGenModule::initializerForString(std::string S, llvm::Type *Ty, Type *T){
  uint64_t Size = llvm::cast<llvm::ArrayType>(Ty)->getNumElements();
  if (auto *SS = llvm::dyn_cast<ShortStringType>(T)) {
    S = S.substr(0, SS->getMaxLength());
    S.insert(S.begin(), static_cast<char>(S.size()));
  }
  S.resize(Size, '\0');
  return llvm::ConstantDataArray::getString(M.getContext(), S, false);
}

