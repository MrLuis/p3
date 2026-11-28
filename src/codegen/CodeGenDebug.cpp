// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGenDebug.cpp - AST -> LLVM IR ---------------------------------------===//

#include "CodeGenInternal.h"

using namespace p3;
using namespace p3::detail;


llvm::DIFile *CodeGenModule::diFileFor(SourceLocation L){
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


//===--------------------------------------------------------------------===//
// Debug information
//===--------------------------------------------------------------------===//

unsigned CodeGenModule::lineOf(SourceLocation L) const{ return SM.getPresumedLoc(L).Line; }


void CodeGenModule::initDebugInfo(llvm::StringRef SourceFileName){
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


llvm::DIType *CodeGenModule::debugType(Type *T){
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


llvm::DIType *CodeGenModule::buildDebugType(Type *T){
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


llvm::DISubprogram *CodeGenModule::createSubprogram(llvm::Function *F, llvm::StringRef Name,
                                     llvm::DIScope *Scope, unsigned Line,
                                     Type *Result,
                                     llvm::ArrayRef<VarDecl *> Params,
                                     llvm::DIFile *File,
                                     VarDecl *Self){
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


void CodeGenModule::setDebugLoc(SourceLocation L){
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
void CodeGenModule::declareFrameVariables(RoutineDecl *R){
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
        V && !V->isTypedConst() && !V->getAbsolute() && !SliceLocals.count(V))
      Declare(V, 0);
  if (R->isFunction())
    Declare(R->getResultVar(), 0);
}


void CodeGenModule::declareGlobalDebugInfo(VarDecl *V, llvm::GlobalVariable *G){
  G->addDebugInfo(DIB->createGlobalVariableExpression(
      // No linkage name: debuggers would show "pas.x" instead of "x".
      CU, V->getName().Spelling, /*LinkageName=*/"", DIUnitFile,
      lineOf(V->getLocation()), debugType(V->getType()),
      /*IsLocalToUnit=*/true));
}

