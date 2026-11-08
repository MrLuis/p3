unit pcast;
{ The abstract syntax tree of the self-hosted compiler; the Pascal
  counterpart of include/pcc/AST/AST.h.

  Expressions and statements are one record type, TNode, with a variant
  part per kind; declarations are TDecl, written types TTypeSpec. All nodes
  are allocated in the arena (pcutil) and never freed. Lists of nodes are
  TPtrList.

  Fields that Sema fills in (types, resolved declarations, constant
  values) are marked (Sema). Semantic types are TType (Type.h): integer
  types, real, boolean, char, enumerations, subranges, string literals,
  arrays, records, pointers, nil, sets, files, procedural types and short
  strings, and (--mode=objfpc) classes. Types are compared by identity
  (name equivalence). }

interface

uses pcsys, pcutil, pcdiag;

{$I builtins.inc}

type
  { a name as written: name is the lower-case interned name (nil when
    missing), spelling the text as written }
  TIdent = record
    name: PName;
    spelling: PName;
    loc: TLoc;
  end;
  PIdent = ^TIdent;

  TConstKind = (ckNone, ckOrdinal, ckReal, ckString);
  TConstValue = record
    kind: TConstKind;
    intVal: int64;
    realVal: double;
    strVal: PName;
  end;

  TUnaryOp = (uoPlus, uoMinus, uoNot);
  TBinaryOp = (boAdd, boSub, boOr, boXor, boMul, boRealDiv, boIntDiv, boMod,
               boAnd, boShl, boShr, boEq, boNe, boLt, boLe, boGt, boGe, boIn,
               boIs, boAs);

  TNodeKind = (
    { expressions }
    nkIntegerLit, nkRealLit, nkStringLit, nkNameRef, nkParen, nkUnary,
    nkBinary, nkCall, nkFormatted, nkIndex, nkField, nkNil, nkDeref,
    nkSetCtor, nkAddrOf, nkAggregate, nkIndirectCall, nkInherited,
    { statements }
    nkEmpty, nkCompound, nkProcCall, nkAssign, nkIf, nkWhile, nkRepeat,
    nkFor, nkCase, nkWith, nkLabeled, nkGoto, nkIndirectCallStmt);

  PNode = ^TNode;
  PDecl = ^TDecl;
  PTypeSpec = ^TTypeSpec;
  PBlock = ^TBlock;
  PType = ^TType;
  PWithContext = ^TWithContext;
  PFieldList = ^TFieldList;

  TTypeKind = (tyError, tyInteger, tyReal, tyBoolean, tyChar, tyEnum,
               tySubrange, tyStringLit, tyArray, tyRecord, tyPointer, tyNil,
               tySet, tyFile, tyRoutine, tyShortString, tyClass);

  TType = record
    kind: TTypeKind;
    id: longint;     { unique: maps keyed by type use it }
    declName: PName; { first name given in a type declaration, or nil }
    case TTypeKind of
      tyInteger: (bits: longint; isSigned: boolean; intName: PName);
      tyEnum: (enumValues: TPtrList); { of PDecl (dkConst) }
      tySubrange: (host: PType; lo, hi: int64);
      tyStringLit: (strLength: int64);
      tyArray: (indexType, elementType: PType; isPackedArray: boolean);
      tyRecord: (fieldList: PFieldList; allFields: TPtrList; { of PDecl }
                 isPackedRecord: boolean);
      tyPointer: (pointee: PType; untyped: boolean);
      tySet: (setElement: PType); { nil: the empty set [] }
      tyFile: (fileElement: PType; isText: boolean);
      tyRoutine: (routineParams: TPtrList; routineResult: PType);
      tyShortString: (maxLength: longint);
      { a class (objfpc): its parent (nil only for TObject), own members in
        source order, all fields (the ancestors' first), the virtual methods
        by slot, the program or unit declaring it; a forward declaration
        is not defined yet }
      tyClass: (classParent: PType; classMembers, classAllFields,
                classVTable: TPtrList; classModule: pointer;
                classDefined: boolean);
  end;

  TNode = record
    kind: TNodeKind;
    b, e: TLoc;      { source range [b, e) }
    typ: PType;      { expressions: the type (Sema) }
    case TNodeKind of
      nkIntegerLit: (intValue: int64);
      nkRealLit: (realValue: double);
      nkStringLit: (strValue: PName);
      nkNameRef: (ref: TIdent; refDecl: PDecl; refWith: PWithContext);
      { paren: the inner expression; deref: the pointer; addrof: the operand }
      nkParen, nkDeref, nkAddrOf: (inner: PNode);
      nkUnary: (uop: TUnaryOp; operand: PNode);
      nkBinary: (bop: TBinaryOp; opLoc: TLoc; lhs, rhs: PNode;
                 operandType: PType);
      { call / proccall: callee named; indirectcall: calleeExpr; the
        indirect call statement holds an nkIndirectCall in calleeExpr }
      { callWith: a method found through a class scope is called on the
        object of this with context (Self, or a with statement's) }
      nkCall, nkProcCall, nkIndirectCall, nkIndirectCallStmt:
        (callee: TIdent; args: TPtrList; hasParens: boolean;
         calleeExpr: PNode; callDecl: PDecl; callWith: PWithContext);
      nkFormatted: (fmtValue, fmtWidth, fmtPrecision: PNode);
      { index: base[index]; field: base.fieldName; fieldDecl is a field,
        or (objfpc) a method or property of a class }
      nkIndex, nkField: (base, index: PNode; fieldName: TIdent;
                         fieldDecl: PDecl);
      { inherited [inhName] (objfpc): the method called and the method
        calling it (Sema); a bare inherited passes the calling method's
        parameters (inhArgs, Sema) }
      nkInherited: (inhName: TIdent; inhMethod, inhEnclosing: PDecl;
                    inhArgs: TPtrList);
      nkSetCtor: (setElems: TPtrList);      { of PSetElement }
      nkAggregate: (aggElems: TPtrList);    { of PAggElement }
      { compound: stmts; repeat: stmts until untilCond }
      nkCompound, nkRepeat: (stmts: TPtrList; untilCond: PNode);
      nkAssign: (target, value: PNode; assignLoc: TLoc);
      { if: cond then thenStmt else elseStmt; while: cond do thenStmt }
      nkIf, nkWhile: (cond, thenStmt, elseStmt: PNode);
      nkFor: (forVar, forInit, forFinal, forBody: PNode; isDownto: boolean);
      nkCase: (selector: PNode; arms: TPtrList; elseLoc: TLoc;
               elseBody: TPtrList);
      nkWith: (withRecords: TPtrList; withBody: PNode); { of PWithContext }
      { labeled: lbl: lblBody; goto lbl }
      nkLabeled, nkGoto: (lbl: TIdent; lblBody: PNode; lblDecl: PDecl);
  end;

  PSetElement = ^TSetElement;
  TSetElement = record
    lo, hi: PNode; { hi: nil unless a range }
  end;

  PAggElement = ^TAggElement;
  TAggElement = record
    field: TIdent;       { name = nil for positional elements }
    value: PNode;        { an expression or a nested aggregate }
    folded: TConstValue; { Sema }
  end;

  PCaseLabel = ^TCaseLabel;
  TCaseLabel = record
    lo, hi: PNode; { hi: nil unless a range }
    loValue, hiValue: int64; { Sema }
  end;

  PCaseArm = ^TCaseArm;
  TCaseArm = record
    labels: TPtrList; { of PCaseLabel }
    body: PNode;
  end;

  { A record of a with statement, or (objfpc) an object: of a with
    statement, or Self in a method body (isSelf). }
  TWithContext = record
    rec: PNode;
    isSelf: boolean;
    cgAddr: pointer; { code generation: the record's address (a boxed value) }
  end;

  TTypeSpecKind = (tsNamed, tsEnum, tsSubrange, tsArray, tsRecord,
                   tsPointer, tsSet, tsFile, tsRoutine, tsString, tsClass);

  PVariantPart = ^TVariantPart;

  TTypeSpec = record
    kind: TTypeSpecKind;
    b, e: TLoc;
    resolved: PType; { Sema }
    case TTypeSpecKind of
      { named: the type name; pointer: the pointee's name }
      tsNamed, tsPointer: (tname: TIdent);
      tsEnum: (enumNames: TPtrList); { of PIdent }
      tsSubrange: (lo, hi: PNode);
      tsArray: (indexes: TPtrList; element: PTypeSpec; arrPacked: boolean);
      tsRecord: (fields: PFieldList; recPacked: boolean);
      tsSet, tsFile: (elemSpec: PTypeSpec);
      tsRoutine: (rtIsFunction: boolean; rtParams: TPtrList; rtResult: PTypeSpec);
      tsString: (maxLen: PNode); { nil for plain string }
      { class (objfpc): the parent's name (nil name: TObject), the members
        (fields, method headings, properties) in source order }
      tsClass: (parentName: TIdent; memberList: TPtrList; isForwardClass: boolean);
  end;

  TFieldList = record
    fixed: TPtrList; { of PDecl (dkField) }
    variant: PVariantPart;
  end;

  PVariant = ^TVariant;
  TVariant = record
    labels: TPtrList; { of PCaseLabel }
    fields: PFieldList;
  end;

  TVariantPart = record
    tag: PDecl;         { the tag field, if named }
    tagSpec: PTypeSpec;
    tagType: PType;     { Sema }
    variants: TPtrList; { of PVariant }
    caseLoc: TLoc;
  end;

  TBlock = record
    decls: TPtrList; { of PDecl, in source order }
    body: PNode;     { nkCompound }
  end;

  TDeclKind = (dkProgram, dkVar, dkConst, dkType, dkField, dkRoutine,
               dkBuiltin, dkLabel, dkUnit, dkProperty);
  TVarKind = (vkVariable, vkValueParam, vkVarParam, vkConstParam, vkResult);
  { objfpc: the kind of a method, the methods of TObject (no Pascal body),
    the visibility of a class member }
  TMethodKind = (mtNone, mtMethod, mtConstructor, mtDestructor);
  TObjMethod = (obNone, obCreate, obDestroy, obFree, obClassName);
  TVisibility = (vsPublic, vsPublished, vsProtected, vsPrivate);

  TDecl = record
    kind: TDeclKind;
    ident: TIdent;
    loc: TLoc;          { noLoc for predeclared entities }
    { Sema: referenced anywhere; a function whose result is assigned; the
      number of labels in this routine targeted by non-local gotos }
    isUsed, resultAssigned: boolean;
    nonLocalLabels: longint;
    { code generation (pcgen): a static variable's address (a boxed value),
      a routine's function, frame type and jmp_buf slot, the frame slot of
      a local, a field's byte offset, a label's basic block }
    cgValue, cgFunc, cgFrame, cgBlock, cgDbg: pointer;
    { separate units: the external symbol of an interface variable or
      routine (nil: none), and whether its unit was compiled separately }
    cgExport: PName;
    cgImported: boolean;
    cgSlot, cgJmpSlot: longint;
    cgOffset: int64;
    vis: TVisibility; { class members }
    case TDeclKind of
      { owner: the routine whose frame holds it; nil for program-level
        variables and typed constants (Sema) }
      dkVar: (varSpec: PTypeSpec; varType: PType; varKind: TVarKind;
              owner: PDecl; init: PNode; initValue: TConstValue);
      dkConst: (constInit: PNode; constType: PType; constValue: TConstValue);
      dkType: (typeSpec: PTypeSpec; declType: PType);
      dkField: (fieldSpec: PTypeSpec; fieldType: PType);
      { methods (objfpc): methodClassName: TFoo of an implementation
        TFoo.Bar; ownerClass, selfVar (the hidden parameter), vmtIndex (a
        virtual method's slot, else -1) and implDecl (the implementation of
        a method heading): Sema }
      dkRoutine: (isFunction, hasParamList, isForward, isExternal: boolean;
                  params: TPtrList; resultSpec: PTypeSpec; resultType: PType;
                  body: PBlock; externalName: PName;
                  canonical, parent, resultVar: PDecl; level: longint;
                  methodKind: TMethodKind; methodClassName: TIdent;
                  isVirtual, isOverride, isAbstract: boolean;
                  ownerClass: PType; selfVar, implDecl: PDecl;
                  vmtIndex: longint; objMethod: TObjMethod);
      dkBuiltin: (builtin: TBuiltinKind; isProcedure: boolean);
      { labels (Sema): the statement labeled, the statement sequence that
        directly contains it, whether that is the block's outermost one,
        and whether a goto from a nested routine targets it }
      dkLabel: (labelOwner: PDecl; labelTarget: PNode; labelSequence: pointer;
                labelTopLevel, labelNonLocal: boolean; labelIndex: longint);
      dkProgram: (progParams: TPtrList; progUses: TPtrList; progBlock: PBlock);
      dkUnit: (ifaceUses, implUses: TPtrList; iface, impl: PBlock);
      { property (objfpc): the accessors' names and declarations (Sema) }
      dkProperty: (propSpec: PTypeSpec; propType: PType;
                   readName, writeName: TIdent; readDecl, writeDecl: PDecl);
  end;

function newNode(kind: TNodeKind; b, e: TLoc): PNode;
function newDecl(kind: TDeclKind; const id: TIdent; loc: TLoc): PDecl;
function newTypeSpec(kind: TTypeSpecKind; b, e: TLoc): PTypeSpec;
function newIdent(const id: TIdent): PIdent;
function noIdent: TIdent;
function identValid(const id: TIdent): boolean;
function identRange(const id: TIdent): TRange;
function nodeRange(n: PNode): TRange;

function unaryOpSpelling(op: TUnaryOp): string;
function binaryOpSpelling(op: TBinaryOp): string;
function isRelational(op: TBinaryOp): boolean;

implementation

function newNode(kind: TNodeKind; b, e: TLoc): PNode;
var
  n: PNode;
begin
  n := PNode(arenaAlloc(sizeof(TNode)));
  n^.kind := kind;
  n^.b := b;
  n^.e := e;
  newNode := n;
end;

function newDecl(kind: TDeclKind; const id: TIdent; loc: TLoc): PDecl;
var
  d: PDecl;
begin
  d := PDecl(arenaAlloc(sizeof(TDecl)));
  d^.kind := kind;
  d^.ident := id;
  d^.loc := loc;
  newDecl := d;
end;

function newTypeSpec(kind: TTypeSpecKind; b, e: TLoc): PTypeSpec;
var
  t: PTypeSpec;
begin
  t := PTypeSpec(arenaAlloc(sizeof(TTypeSpec)));
  t^.kind := kind;
  t^.b := b;
  t^.e := e;
  newTypeSpec := t;
end;

function newIdent(const id: TIdent): PIdent;
var
  p: PIdent;
begin
  p := PIdent(arenaAlloc(sizeof(TIdent)));
  p^ := id;
  newIdent := p;
end;

function noIdent: TIdent;
var
  id: TIdent;
begin
  id.name := nil;
  id.spelling := nil;
  id.loc := noLoc;
  noIdent := id;
end;

function identValid(const id: TIdent): boolean;
begin
  identValid := id.name <> nil;
end;

function identRange(const id: TIdent): TRange;
var
  len: longint;
begin
  len := 0;
  if id.spelling <> nil then
    len := id.spelling^.len;
  identRange := makeRange(id.loc, id.loc + len);
end;

function nodeRange(n: PNode): TRange;
begin
  nodeRange := makeRange(n^.b, n^.e);
end;

function unaryOpSpelling(op: TUnaryOp): string;
begin
  case op of
    uoPlus: unaryOpSpelling := '+';
    uoMinus: unaryOpSpelling := '-';
    uoNot: unaryOpSpelling := 'not';
  end;
end;

function binaryOpSpelling(op: TBinaryOp): string;
begin
  case op of
    boAdd: binaryOpSpelling := '+';
    boSub: binaryOpSpelling := '-';
    boOr: binaryOpSpelling := 'or';
    boXor: binaryOpSpelling := 'xor';
    boShl: binaryOpSpelling := 'shl';
    boShr: binaryOpSpelling := 'shr';
    boMul: binaryOpSpelling := '*';
    boRealDiv: binaryOpSpelling := '/';
    boIntDiv: binaryOpSpelling := 'div';
    boMod: binaryOpSpelling := 'mod';
    boAnd: binaryOpSpelling := 'and';
    boEq: binaryOpSpelling := '=';
    boNe: binaryOpSpelling := '<>';
    boLt: binaryOpSpelling := '<';
    boLe: binaryOpSpelling := '<=';
    boGt: binaryOpSpelling := '>';
    boGe: binaryOpSpelling := '>=';
    boIn: binaryOpSpelling := 'in';
    boIs: binaryOpSpelling := 'is';
    boAs: binaryOpSpelling := 'as';
  end;
end;

function isRelational(op: TBinaryOp): boolean;
begin
  isRelational := op in [boEq, boNe, boLt, boLe, boGt, boGe, boIn, boIs];
end;

end.
