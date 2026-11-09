unit pcir;
{ A builder for textual LLVM IR that writes exactly what LLVM 19 prints for
  the module the C++ pcc builds with IRBuilder (SELFHOST.md S10: the two
  compilers' --emit-llvm output must be identical). It models what shows
  in the text:

  - the target's data layout (sizes, ABI and preferred alignments);
  - IRBuilder's constant folding (ConstantFolder): an operation on
    constants gives a constant, not an instruction;
  - names: a value or block gets its requested name, made unique per
    function with a suffix from one counter (x1, p2), globals and
    functions per module (.str.1); unnamed values are numbered %0, %1, ...
    in layout order when printed;
  - blocks in creation order, with '; preds = ...' comments; temporaries
    allocated at the start of the entry block;
  - functions and globals in creation order; named struct types in the
    order llvm::TypeFinder finds them (unused ones are not printed);
    attribute groups and branch-weight metadata.

  Texts are short strings where possible; markers stand for longer or not
  yet known parts and are expanded when the module is printed:
    #1 id #1   an unnamed instruction result (%N)
    #2 id #2   a long constant (an atom)
    #3 id #3   an unnamed global (@N)
    #4 id #4   a named struct type (%name)
    #5 id #5   a metadata node (!N)

  Metadata (debug information, branch weights) is numbered as LLVM's
  SlotTracker does: global variables' attachments, named metadata, then
  each function's attachment and, per instruction, its debug records and
  attachments, in a pre-order walk of each node's operands (in LLVM's
  internal operand order, given by the creator of the node). }

interface

uses pcsys, pcutil;

type
  TIRKind = (ikVoid, ikInt, ikDouble, ikPtr, ikArray, ikStruct);

  TInt64Array = array[0..$FFFFFF] of int64;
  PInt64Array = ^TInt64Array;

  PIRType = ^TIRType;
  TIRType = record
    kind: TIRKind;
    bits: longint;        { ikInt }
    count: int64;         { ikArray }
    elem: PIRType;        { ikArray }
    fields: TPtrList;     { ikStruct: of PIRType }
    offsets: PInt64Array; { ikStruct: byte offset of each field }
    text: string;         { 'i32', '[4 x i8]', a literal struct, or a marker }
    size, align, prefAlign: int64; { alloc size, ABI and preferred alignment }
    named: boolean;
    id: longint;
    name: string;         { a named struct's (unique) name }
  end;

  { integers of up to 256 bits: 8 limbs of 32 bits, least significant
    first, two's complement, sign-extended from the type's width }
  TLimbs = array[0..7] of int64;

  TValueKind = (vkInst, vkInt, vkFP, vkNull, vkZero, vkUndef, vkPoison,
                vkGlobal, vkConstExpr, vkAgg);

  TV = record
    t: PIRType;
    v: string;       { the operand text }
    kind: TValueKind;
    limbs: TLimbs;   { vkInt }
    fp: double;      { vkFP }
    agg: pointer;    { vkAgg: a PPtrList of PTVBox, the elements }
  end;
  PTVBox = ^TV;

  PIRBlock = ^TIRBlock;
  PIRFunc = ^TIRFunc;
  PIRInst = ^TIRInst;

  { a metadata node }
  PMeta = ^TMeta;
  TMeta = record
    text: TBuf;          { as printed, references as markers }
    ops: TPtrList;       { of PMeta: its node operands, in operand order }
    id: longint;
    num: longint;        { the printed number (-1 until numbered) }
  end;

  TIRBlock = record
    name: string;
    insts: TPtrList;     { of PIRInst }
    preds: TPtrList;     { of PIRBlock: the branches here, in use order }
    pending: TPtrList;   { of PDbgRecord: debug records for the next instruction }
  end;

  PDbgRecord = ^TDbgRecord;
  TDbgRecord = record
    text: string;        { "ptr %frame, <var>, !DIExpression(...), <loc>" }
    variable, loc: PMeta;
  end;

  TIRInst = record
    text: TBuf;
    defId: longint;      { an unnamed result: its marker id, else 0 }
    isSwitch, isPhi: boolean;
    phiCount: longint;
    block: PIRBlock;     { the block it is in }
    dbgLoc: PMeta;       { !dbg, or nil }
    unlikely: boolean;   { !prof branch weights }
    records: TPtrList;   { of PDbgRecord, printed before the instruction }
  end;

  TIRFunc = record
    name: string;        { as printed, with @ }
    ret: PIRType;
    params: TPtrList;    { of PIRType }
    argNames: TPtrList;  { of PName (nil: unnamed) }
    isDecl: boolean;
    linkage: string;
    attrs: string;
    paramText: string;
    blocks: TPtrList;    { of PIRBlock, in creation order }
    entryTemps: TPtrList; { of PIRInst: allocas at the entry's start, oldest first }
    names: TIntMap;      { name id -> non-nil: used in this function }
    lastUnique: longint;
    nextDef: longint;
    dbg: PMeta;          { the function's !dbg (a DISubprogram), or nil }
  end;

var
  irVoidT, irI1, irI8, irI16, irI32, irI64, irI256, irDoubleT, irPtrT: PIRType;

{ Starts a module for a target (data layout string as LLVM prints it). }
procedure irInit(const sourceName, triple, dataLayout: string);

{ --- Types --- }
function irInt(bits: longint): PIRType;
function irArray(n: int64; elem: PIRType): PIRType;
function irStructLit(const fields: TPtrList): PIRType;
{ A named struct; the name is made unique as LLVM does (name.0, ...). }
function irStructNamed(const name: string; const fields: TPtrList): PIRType;
function irFieldOffset(t: PIRType; i: longint): int64;
function irSameType(a, b: PIRType): boolean;

{ --- Values --- }
function irV(t: PIRType; const v: string): TV;
function irConstInt(t: PIRType; v: int64): TV;
function irConstLimbs(t: PIRType; const l: TLimbs): TV;
function irConstBool(b: boolean): TV;
function irConstDouble(x: double): TV;
function irNull: TV;
{ the constant expression inttoptr (i64 n to ptr) }
function irConstIntToPtr(n: int64): TV;
function irZero(t: PIRType): TV;
function irUndef(t: PIRType): TV;
function irIsConst(const v: TV): boolean;
{ An integer constant of at most 64 bits, and its (signed) value. }
function irIsConstInt(const v: TV; var value: int64): boolean;
function irIsNullValue(const v: TV): boolean;
{ Aggregate constants, with ConstantArray / ConstantStruct's rules (all
  zero: zeroinitializer; i8 arrays print as c"..."). elems: PTVBox list. }
function irConstArray(t: PIRType; const elems: TPtrList): TV;
function irConstStruct(t: PIRType; const elems: TPtrList): TV;
{ An i8 array of the bytes (ConstantDataArray::getString). }
function irConstBytes(p: PCharArray; start, count: int64): TV;
function irBox(const v: TV): PTVBox;
procedure limbsOfInt(v: int64; var l: TLimbs);

{ --- Module --- }
{ A global variable: name made unique ('' for an unnamed one); init: its
  initializer, unless isDecl. Returns the global as a value. }
function irGlobal(const name, linkage: string; t: PIRType; const init: TV;
                  isConstant, unnamedAddr, isDecl: boolean; align: longint): TV;
{ The global named name (M.getNamedGlobal). }
function irFindGlobal(const name: string; var g: TV): boolean;
{ Gives global g, created as a declaration without an alignment, its
  initializer (GlobalVariable::setInitializer). }
procedure irSetGlobalInit(const g: TV; const init: TV);

{ A declaration. params: of PIRType; attrs: the function attributes as
  LLVM prints them ('' for none); paramText: the parameter list if it has
  parameter attributes, else ''. }
function irDeclareFunction(const name: string; ret: PIRType; const params: TPtrList;
                           const attrs, paramText: string): PIRFunc;
function irFindFunction(const name: string): PIRFunc;
{ A definition; linkage '' or 'internal'. }
function irCreateFunction(const name, linkage: string; ret: PIRType;
                          const params: TPtrList): PIRFunc;
procedure irSetArgName(f: PIRFunc; i: longint; const name: string);
function irArg(f: PIRFunc; i: longint): TV;
function irFuncRef(f: PIRFunc): TV;

{ --- Function bodies --- }
procedure irBeginBody(f: PIRFunc);
function irNewBlock(const name: string): PIRBlock;
procedure irSetInsertPoint(b: PIRBlock);
function irInsertBlock: PIRBlock;

{ Instructions; name '' for an unnamed result. Constant operands fold as
  with IRBuilder's ConstantFolder. }
function irBin(const op: string; const a, b: TV; const name: string): TV;
function irICmp(const pred: string; const a, b: TV; const name: string): TV;
function irFCmp(const pred: string; const a, b: TV; const name: string): TV;
function irFNeg(const a: TV; const name: string): TV;
function irCast(const op: string; const v: TV; dst: PIRType; const name: string): TV;
function irSelect(const c, a, b: TV; const name: string): TV;
function irLoad(t: PIRType; const p: TV; const name: string): TV;
procedure irStore(const v, p: TV);
{ getelementptr [inbounds] src, p, idx (one or two i64/i32 indices) }
function irGEP1(src: PIRType; const p, i0: TV; inbounds: boolean; const name: string): TV;
function irGEP2(src: PIRType; const p, i0, i1: TV; const name: string): TV;
function irStructGEP(st: PIRType; const p: TV; field: longint; const name: string): TV;
function irExtract(const agg: TV; idx: longint; t: PIRType; const name: string): TV;
function irInsert(const agg, v: TV; idx: longint; const name: string): TV;
{ alloca at the insertion point, or at the start of the entry block }
function irAlloca(t: PIRType; const name: string): TV;
function irCreateTemp(t: PIRType; const name: string): TV;
{ A temporary at the start of the entry block followed by a store of init
  into it (an IRBuilder there creating both). }
function irCreateTempInit(t: PIRType; const name: string; const init: TV): TV;
{ Calls; args: "i32 %x, ptr align 1 %p" (irArgsAdd / irArgsAddAttr). }
procedure irArgsAdd(var args: TBuf; const v: TV);
procedure irArgsAddAttr(var args: TBuf; const v: TV; const attr: string);
function irCall(f: PIRFunc; const args: TBuf; const name: string): TV;
function irCallPtr(ret: PIRType; const callee: TV; const args: TBuf; const name: string): TV;
procedure irBr(b: PIRBlock);
{ unlikely: with !prof branch weights 1 : 1048575 }
procedure irCondBr(const c: TV; t, f: PIRBlock; unlikely: boolean);
function irSwitch(const v: TV; default: PIRBlock): PIRInst;
procedure irAddCase(sw: PIRInst; const c: TV; b: PIRBlock);
function irPhi(t: PIRType; const name: string; var phi: PIRInst): TV;
procedure irAddIncoming(phi: PIRInst; const v: TV; b: PIRBlock);
procedure irRet(const v: TV);
procedure irRetVoid;
function irVarArgParams(const params: TPtrList): string;
function irCallVarArg(f: PIRFunc; const callee: TV; const args: TBuf;
                      const name: string): TV;
procedure irUnreachable;

{ --- Metadata --- }
{ A node with the given text (references from irMetaRef) and node operands
  (PMeta, in LLVM's operand order). Nodes that are not distinct are
  uniqued by their text. }
function irMeta(distinct: boolean; const text: TBuf; const ops: TPtrList): PMeta;
{ A node to be filled later (a node that is referred to before it exists). }
function irMetaPlaceholder: PMeta;
procedure irMetaFill(m: PMeta; const text: TBuf; const ops: TPtrList);
function irMetaRef(m: PMeta): string;
{ named metadata: !name = a tuple of the nodes }
procedure irNamedMeta(const name: string; const ops: TPtrList);
procedure irSetGlobalDbg(const g: TV; m: PMeta);
procedure irSetFuncDbg(f: PIRFunc; m: PMeta);
{ the !dbg location of the instructions created from now on (nil: none) }
procedure irSetDebugLoc(m: PMeta);
{ #dbg_declare(value, variable, expression, location) before the next
  instruction of the current block }
procedure irInsertDeclare(const v: TV; variable: PMeta; const expr: string; loc: PMeta);

{ The finished module. }
procedure irWriteModule(var outText: TBuf);

implementation

type
  PGlobal = ^TGlobal;
  TGlobal = record
    name: string;        { '' for an unnamed global }
    text: TBuf;          { the line after '@name = ' }
    id: longint;
    dbg: PMeta;
  end;
  PBufBox = ^TBuf;
  PIntBox = ^longint;
  PPtrList = ^TPtrList;
  TSpecFields = array[0..3] of string;

  TBigNum = record
    n: longint;
    d: array[0..127] of int64;
  end;

  TDoubleOverlay = record
    case boolean of
      true: (d: double);
      false: (bits: int64);
  end;

const
  limbMask = 4294967295;
  limbBase = 4294967296;

var
  { data layout }
  dlIntBits: array[0..15] of longint;
  dlIntAbi, dlIntPref: array[0..15] of int64;
  dlNumInts: longint;
  dlPtrSize, dlPtrAbi, dlPtrPref, dlF64Abi, dlF64Pref, dlAggPref: int64;
  modSource, modTriple, modLayout: string;

  intTypes: TIntMap;     { bits -> PIRType }
  typeById: TIntMap;     { id -> PIRType }
  structNames: TIntMap;  { name id -> non-nil }
  structUniqueId: longint;
  typeCount: longint;

  globals: TPtrList;     { of PGlobal }
  globalByName: TIntMap; { name id -> PGlobal }
  funcs: TPtrList;       { of PIRFunc }
  funcByName: TIntMap;   { name id -> PIRFunc }
  moduleNames: TIntMap;  { name id -> non-nil }
  moduleLastUnique: longint;
  atoms: TPtrList;       { of PBufBox }
  metas: TPtrList;       { of PMeta, by id }
  metaByText: TIntMap;   { name id of a uniqued node's text -> PMeta }
  namedMetas: TPtrList;  { of PName (name), PPtrList (operands), in pairs }
  curLoc: PMeta;
  branchWeights: PMeta;

  cur: PIRFunc;
  curBlock: PIRBlock;

{ ===================================================================== }
{ Data layout                                                           }
{ ===================================================================== }

procedure setIntAlign(bits: longint; abi, pref: int64);
var
  i: longint;
begin
  for i := 0 to dlNumInts - 1 do
    if dlIntBits[i] = bits then
    begin
      dlIntAbi[i] := abi;
      dlIntPref[i] := pref;
      exit;
    end;
  { the table stays sorted by width }
  i := dlNumInts;
  while (i > 0) and (dlIntBits[i - 1] > bits) do
  begin
    dlIntBits[i] := dlIntBits[i - 1];
    dlIntAbi[i] := dlIntAbi[i - 1];
    dlIntPref[i] := dlIntPref[i - 1];
    i := i - 1;
  end;
  dlIntBits[i] := bits;
  dlIntAbi[i] := abi;
  dlIntPref[i] := pref;
  dlNumInts := dlNumInts + 1;
end;

function parseNum(const s: string): int64;
var
  i: longint;
  v: int64;
begin
  v := 0;
  for i := 1 to length(s) do
    if (s[i] >= '0') and (s[i] <= '9') then
      v := v * 10 + ord(s[i]) - ord('0');
  parseNum := v;
end;

{ A layout spec "i8:8:32": letter 'i', number '8', fields '8', '32'. }
procedure parseSpec(const spec: string; var num: string; var f: TSpecFields;
                    var nf: longint);
var
  i: longint;
  part: string;
  first: boolean;
begin
  num := '';
  nf := 0;
  part := '';
  first := true;
  for i := 2 to length(spec) + 1 do
    if (i > length(spec)) or (spec[i] = ':') then
    begin
      if first then
        num := part
      else if nf <= 3 then
      begin
        f[nf] := part;
        nf := nf + 1;
      end;
      first := false;
      part := '';
    end
    else
      part := part + spec[i];
end;

procedure parseDataLayout(const dl: string);
var
  i, start, nf: longint;
  spec, num: string;
  f: TSpecFields;
  abi, pref: int64;
begin
  { LLVM 19's defaults }
  dlNumInts := 0;
  setIntAlign(1, 1, 1);
  setIntAlign(8, 1, 1);
  setIntAlign(16, 2, 2);
  setIntAlign(32, 4, 4);
  setIntAlign(64, 4, 8);
  dlPtrSize := 8;
  dlPtrAbi := 8;
  dlPtrPref := 8;
  dlF64Abi := 8;
  dlF64Pref := 8;
  dlAggPref := 8;
  start := 1;
  for i := 1 to length(dl) + 1 do
    if (i > length(dl)) or (dl[i] = '-') then
    begin
      spec := copy(dl, start, i - start);
      start := i + 1;
      if spec = '' then
        continue;
      parseSpec(spec, num, f, nf);
      if nf = 0 then
        continue;
      abi := parseNum(f[0]) div 8;
      pref := abi;
      case spec[1] of
        'i':
          begin
            if nf > 1 then
              pref := parseNum(f[1]) div 8;
            setIntAlign(longint(parseNum(num)), abi, pref);
          end;
        'p':
          if (num = '') or (num = '0') then
          begin
            { p:size:abi[:pref] }
            dlPtrSize := parseNum(f[0]) div 8;
            if nf > 1 then
              dlPtrAbi := parseNum(f[1]) div 8;
            dlPtrPref := dlPtrAbi;
            if nf > 2 then
              dlPtrPref := parseNum(f[2]) div 8;
          end;
        'f':
          if num = '64' then
          begin
            if nf > 1 then
              pref := parseNum(f[1]) div 8;
            dlF64Abi := abi;
            dlF64Pref := pref;
          end;
        'a':
          begin
            if nf > 1 then
              pref := parseNum(f[1]) div 8;
            if pref > 0 then
              dlAggPref := pref;
          end;
      else
        ;
      end;
    end;
end;

function intAlign(bits: longint; pref: boolean): int64;
var
  i: longint;
begin
  { the first entry at least as wide, else the widest }
  for i := 0 to dlNumInts - 1 do
    if dlIntBits[i] >= bits then
    begin
      if pref then
        exit(dlIntPref[i]);
      exit(dlIntAbi[i]);
    end;
  if pref then
    intAlign := dlIntPref[dlNumInts - 1]
  else
    intAlign := dlIntAbi[dlNumInts - 1];
end;

function alignTo(n, a: int64): int64;
begin
  alignTo := (n + a - 1) div a * a;
end;

{ ===================================================================== }
{ Types                                                                 }
{ ===================================================================== }

function newIRType(kind: TIRKind): PIRType;
var
  t: PIRType;
begin
  t := PIRType(arenaAlloc(sizeof(TIRType)));
  t^.kind := kind;
  listInit(t^.fields);
  typeCount := typeCount + 1;
  t^.id := typeCount;
  mapPut(typeById, t^.id, t);
  newIRType := t;
end;

function irInt(bits: longint): PIRType;
var
  t: PIRType;
begin
  t := PIRType(mapGet(intTypes, bits));
  if t <> nil then
    exit(t);
  t := newIRType(ikInt);
  t^.bits := bits;
  t^.text := 'i' + intToStr(bits);
  t^.align := intAlign(bits, false);
  t^.prefAlign := intAlign(bits, true);
  t^.size := alignTo((bits + 7) div 8, t^.align);
  mapPut(intTypes, bits, t);
  irInt := t;
end;

function irArray(n: int64; elem: PIRType): PIRType;
var
  t: PIRType;
begin
  t := newIRType(ikArray);
  t^.count := n;
  t^.elem := elem;
  t^.text := '[' + intToStr(n) + ' x ' + elem^.text + ']';
  t^.align := elem^.align;
  t^.prefAlign := elem^.prefAlign;
  t^.size := n * elem^.size;
  irArray := t;
end;

procedure layoutStruct(t: PIRType);
var
  i: longint;
  f: PIRType;
  off, al: int64;
begin
  getmem(t^.offsets, int64(t^.fields.count + 1) * sizeof(int64));
  off := 0;
  al := 1;
  for i := 0 to t^.fields.count - 1 do
  begin
    f := PIRType(listGet(t^.fields, i));
    off := alignTo(off, f^.align);
    {$R-}
    t^.offsets^[i] := off;
    {$R+}
    off := off + f^.size;
    if f^.align > al then
      al := f^.align;
  end;
  t^.align := al;
  t^.size := alignTo(off, al);
  t^.prefAlign := al;
  if dlAggPref > al then
    t^.prefAlign := dlAggPref;
end;

function fieldsText(const fields: TPtrList): string;
var
  s: string;
  i: longint;
begin
  if fields.count = 0 then
    exit('{}');
  s := '{ ';
  for i := 0 to fields.count - 1 do
  begin
    if i > 0 then
      s := s + ', ';
    s := s + PIRType(listGet(fields, i))^.text;
  end;
  fieldsText := s + ' }';
end;

function irStructLit(const fields: TPtrList): PIRType;
var
  t: PIRType;
  i: longint;
begin
  t := newIRType(ikStruct);
  for i := 0 to fields.count - 1 do
    listAdd(t^.fields, listGet(fields, i));
  t^.text := fieldsText(t^.fields);
  layoutStruct(t);
  irStructLit := t;
end;

function irStructNamed(const name: string; const fields: TPtrList): PIRType;
var
  t: PIRType;
  i: longint;
  n: string;
begin
  t := newIRType(ikStruct);
  for i := 0 to fields.count - 1 do
    listAdd(t^.fields, listGet(fields, i));
  { StructType::setName: name, else name.<context-wide counter> }
  n := name;
  while mapHas(structNames, intern(n)^.id) do
  begin
    n := name + '.' + intToStr(structUniqueId);
    structUniqueId := structUniqueId + 1;
  end;
  mapPut(structNames, intern(n)^.id, t);
  t^.name := n;
  t^.named := true;
  t^.text := #4 + intToStr(t^.id) + #4;
  layoutStruct(t);
  irStructNamed := t;
end;

function irFieldOffset(t: PIRType; i: longint): int64;
begin
  {$R-}
  irFieldOffset := t^.offsets^[i];
  {$R+}
end;

function irSameType(a, b: PIRType): boolean;
begin
  irSameType := (a = b) or (a^.text = b^.text);
end;

{ ===================================================================== }
{ Integers of up to 256 bits                                            }
{ ===================================================================== }

procedure limbsOfInt(v: int64; var l: TLimbs);
var
  i: longint;
  fill: int64;
begin
  l[0] := v and limbMask;
  l[1] := (v shr 32) and limbMask;
  fill := 0;
  if v < 0 then
    fill := limbMask;
  for i := 2 to 7 do
    l[i] := fill;
end;

{ the low 64 bits as an int64 }
function limbsToInt(const l: TLimbs): int64;
begin
  limbsToInt := (l[1] shl 32) or l[0];
end;

function limbBit(const l: TLimbs; i: longint): boolean;
begin
  limbBit := ((l[i div 32] shr (i mod 32)) and 1) <> 0;
end;

{ sign-extends from bit bits-1 }
procedure normalize(var l: TLimbs; bits: longint);
var
  i, limb, off: longint;
  neg: boolean;
  mask: int64;
begin
  if bits >= 256 then
    exit;
  neg := limbBit(l, bits - 1);
  limb := bits div 32;
  off := bits mod 32;
  if off > 0 then
  begin
    mask := (int64(1) shl off) - 1;
    if neg then
      l[limb] := (l[limb] and mask) or (limbMask - mask)
    else
      l[limb] := l[limb] and mask;
    limb := limb + 1;
  end;
  for i := limb to 7 do
    if neg then
      l[i] := limbMask
    else
      l[i] := 0;
end;

procedure zeroExtend(var l: TLimbs; bits: longint);
var
  i, limb, off: longint;
begin
  if bits >= 256 then
    exit;
  limb := bits div 32;
  off := bits mod 32;
  if off > 0 then
  begin
    l[limb] := l[limb] and ((int64(1) shl off) - 1);
    limb := limb + 1;
  end;
  for i := limb to 7 do
    l[i] := 0;
end;

procedure limbsAdd(const a, b: TLimbs; var r: TLimbs);
var
  i: longint;
  s, carry: int64;
begin
  carry := 0;
  for i := 0 to 7 do
  begin
    s := a[i] + b[i] + carry;
    r[i] := s and limbMask;
    carry := s shr 32;
  end;
end;

procedure limbsNot(const a: TLimbs; var r: TLimbs);
var
  i: longint;
begin
  for i := 0 to 7 do
    r[i] := limbMask - a[i];
end;

procedure limbsNeg(const a: TLimbs; var r: TLimbs);
var
  one, t: TLimbs;
begin
  limbsNot(a, t);
  limbsOfInt(1, one);
  limbsAdd(t, one, r);
end;

procedure limbsSub(const a, b: TLimbs; var r: TLimbs);
var
  nb: TLimbs;
begin
  limbsNeg(b, nb);
  limbsAdd(a, nb, r);
end;

procedure limbsMul(const a, b: TLimbs; var r: TLimbs);
var
  acc, da, db: array[0..15] of int64; { 16-bit digits }
  i, j: longint;
  carry: int64;
begin
  for i := 0 to 7 do
  begin
    da[2 * i] := a[i] and 65535;
    da[2 * i + 1] := a[i] shr 16;
    db[2 * i] := b[i] and 65535;
    db[2 * i + 1] := b[i] shr 16;
  end;
  for i := 0 to 15 do
    acc[i] := 0;
  for i := 0 to 15 do
  begin
    carry := 0;
    for j := 0 to 15 - i do
    begin
      carry := carry + acc[i + j] + da[i] * db[j];
      acc[i + j] := carry and 65535;
      carry := carry shr 16;
    end;
  end;
  for i := 0 to 7 do
    r[i] := acc[2 * i] or (acc[2 * i + 1] shl 16);
end;

procedure limbsShl1(var t: TLimbs);
var
  j: longint;
begin
  for j := 7 downto 1 do
    t[j] := ((t[j] shl 1) and limbMask) or (t[j - 1] shr 31);
  t[0] := (t[0] shl 1) and limbMask;
end;

procedure limbsShl(const a: TLimbs; n: longint; var r: TLimbs);
var
  i: longint;
  t: TLimbs;
begin
  t := a;
  for i := 1 to n do
    limbsShl1(t);
  r := t;
end;

procedure limbsLshr(const a: TLimbs; n: longint; var r: TLimbs);
var
  i, j: longint;
  t: TLimbs;
begin
  t := a;
  for i := 1 to n do
  begin
    for j := 0 to 6 do
      t[j] := (t[j] shr 1) or ((t[j + 1] and 1) shl 31);
    t[7] := t[7] shr 1;
  end;
  r := t;
end;

function limbsIsZero(const a: TLimbs): boolean;
var
  i: longint;
begin
  for i := 0 to 7 do
    if a[i] <> 0 then
      exit(false);
  limbsIsZero := true;
end;

function limbsUCmp(const a, b: TLimbs): longint;
var
  i: longint;
begin
  for i := 7 downto 0 do
  begin
    if a[i] < b[i] then
      exit(-1);
    if a[i] > b[i] then
      exit(1);
  end;
  limbsUCmp := 0;
end;

function limbsNegative(const a: TLimbs): boolean;
begin
  limbsNegative := a[7] >= 2147483648;
end;

function limbsSCmp(const a, b: TLimbs): longint;
var
  na, nb: boolean;
begin
  na := limbsNegative(a);
  nb := limbsNegative(b);
  if na and not nb then
    exit(-1);
  if nb and not na then
    exit(1);
  limbsSCmp := limbsUCmp(a, b);
end;

{ unsigned division of 256-bit values; b <> 0 }
procedure limbsUDivRem(const a, b: TLimbs; var q, r: TLimbs);
var
  i: longint;
begin
  limbsOfInt(0, q);
  limbsOfInt(0, r);
  for i := 255 downto 0 do
  begin
    limbsShl1(r);
    if limbBit(a, i) then
      r[0] := r[0] or 1;
    if limbsUCmp(r, b) >= 0 then
    begin
      limbsSub(r, b, r);
      q[i div 32] := q[i div 32] or (int64(1) shl (i mod 32));
    end;
  end;
end;

function limbsToDecimal(const a: TLimbs): string;
var
  t: TLimbs;
  s: string;
  i: longint;
  rem, x: int64;
  neg: boolean;
begin
  t := a;
  neg := limbsNegative(t);
  if neg then
    limbsNeg(t, t);
  s := '';
  repeat
    rem := 0;
    for i := 7 downto 0 do
    begin
      x := rem * limbBase + t[i];
      t[i] := x div 10;
      rem := x mod 10;
    end;
    s := chr(ord('0') + rem) + s;
  until limbsIsZero(t);
  if neg then
    s := '-' + s;
  limbsToDecimal := s;
end;

{ ===================================================================== }
{ Doubles as LLVM prints them                                           }
{ ===================================================================== }

procedure bigSet(var b: TBigNum; v: int64);
begin
  b.d[0] := v and limbMask;
  b.d[1] := (v shr 32) and limbMask;
  b.n := 2;
  while (b.n > 0) and (b.d[b.n - 1] = 0) do
    b.n := b.n - 1;
end;

procedure bigMulSmall(var b: TBigNum; m: int64);
var
  i: longint;
  carry: int64;
begin
  carry := 0;
  for i := 0 to b.n - 1 do
  begin
    carry := carry + b.d[i] * m;
    b.d[i] := carry and limbMask;
    carry := carry shr 32;
  end;
  while carry > 0 do
  begin
    b.d[b.n] := carry and limbMask;
    b.n := b.n + 1;
    carry := carry shr 32;
  end;
end;

function bigDivSmall(var b: TBigNum; m: int64): int64;
var
  i: longint;
  rem, x: int64;
begin
  rem := 0;
  for i := b.n - 1 downto 0 do
  begin
    x := rem * limbBase + b.d[i];
    b.d[i] := x div m;
    rem := x mod m;
  end;
  while (b.n > 0) and (b.d[b.n - 1] = 0) do
    b.n := b.n - 1;
  bigDivSmall := rem;
end;

function bigActiveBits(const b: TBigNum): longint;
var
  top: int64;
  bits: longint;
begin
  if b.n = 0 then
    exit(0);
  top := b.d[b.n - 1];
  bits := 0;
  while top > 0 do
  begin
    bits := bits + 1;
    top := top shr 1;
  end;
  bigActiveBits := (b.n - 1) * 32 + bits;
end;

{ APFloat's AdjustToPrecision on the digits (least significant first) }
procedure adjustDigits(var buf: string; var exp: longint; prec: longint);
var
  n, first, i: longint;
begin
  n := length(buf);
  if n <= prec then
    exit;
  first := n - prec + 1; { the first significant digit (1-based) }
  if buf[first - 1] < '5' then
  begin
    while (first <= n) and (buf[first] = '0') do
      first := first + 1;
    exp := exp + first - 1;
    buf := copy(buf, first, 255);
    exit;
  end;
  i := first;
  while i <= n do
  begin
    if buf[i] = '9' then
      first := first + 1
    else
    begin
      buf[i] := chr(ord(buf[i]) + 1);
      break;
    end;
    i := i + 1;
  end;
  if first > n then
  begin
    exp := exp + first - 1;
    buf := '1';
    exit;
  end;
  exp := exp + first - 1;
  buf := copy(buf, first, 255);
end;

{ APFloat::toString(Str, 6, 0, false) of mant * 2^exp (mant > 0) }
function apfloatString(mant: int64; exp: longint): string;
var
  sig: TBigNum;
  i, bits, tens, nd, req: longint;
  buf, s, ebuf: string;
  d: int64;
  inTrail: boolean;
begin
  { drop trailing binary zeros }
  while (mant and 1) = 0 do
  begin
    mant := mant shr 1;
    exp := exp + 1;
  end;
  bigSet(sig, mant);
  if exp > 0 then
  begin
    for i := 1 to exp do
      bigMulSmall(sig, 2);
    exp := 0;
  end
  else if exp < 0 then
    { N * 2^-e = N * 5^e * 10^-e }
    for i := 1 to -exp do
      bigMulSmall(sig, 5);
  { AdjustToPrecision on the significand }
  bits := bigActiveBits(sig);
  req := (6 * 196 + 58) div 59;
  if bits > req then
  begin
    tens := (bits - req) * 59 div 196;
    if tens > 0 then
    begin
      exp := exp + tens;
      for i := 1 to tens do
        d := bigDivSmall(sig, 10);
    end;
  end;
  { the digits, least significant first, trailing zeros dropped }
  buf := '';
  inTrail := true;
  while sig.n > 0 do
  begin
    d := bigDivSmall(sig, 10);
    if inTrail and (d = 0) then
      exp := exp + 1
    else
    begin
      buf := buf + chr(ord('0') + d);
      inTrail := false;
    end;
  end;
  adjustDigits(buf, exp, 6);
  nd := length(buf);
  exp := exp + nd - 1;
  s := buf[nd] + '.';
  for i := nd - 1 downto 1 do
    s := s + buf[i];
  for i := 1 to 6 - nd + 1 do
    s := s + '0';
  s := s + 'e';
  if exp >= 0 then
    s := s + '+'
  else
  begin
    s := s + '-';
    exp := -exp;
  end;
  ebuf := intToStr(exp);
  if length(ebuf) < 2 then
    ebuf := '0' + ebuf;
  apfloatString := s + ebuf;
end;

function hexDigit(v: longint): char;
begin
  if v < 10 then
    hexDigit := chr(ord('0') + v)
  else
    hexDigit := chr(ord('A') + v - 10);
end;

function fpBits(x: double): int64;
var
  o: TDoubleOverlay;
begin
  o.d := x;
  fpBits := o.bits;
end;

function fpOfBits(b: int64): double;
var
  o: TDoubleOverlay;
begin
  o.bits := b;
  fpOfBits := o.d;
end;

function isNaNBits(b: int64): boolean;
begin
  isNaNBits := (((b shr 52) and 2047) = 2047) and ((b and 4503599627370495) <> 0);
end;

{ the text of a double constant: exponential if it reads back exactly,
  else hexadecimal }
function doubleText(x: double): string;
var
  b, mant: int64;
  e: longint;
  s: string;
  back: double;
  ok: boolean;
begin
  b := fpBits(x);
  e := longint((b shr 52) and 2047);
  mant := b and 4503599627370495;
  if e <> 2047 then
  begin
    if (e = 0) and (mant = 0) then
      s := '0.000000e+00'
    else if e = 0 then
      s := apfloatString(mant, -1074)
    else
      s := apfloatString(mant or 4503599627370496, e - 1075);
    if b < 0 then
      s := '-' + s;
    back := sysStrToReal(s, ok);
    if ok and ((fpBits(back) = b) or ((e = 0) and (mant = 0))) then
      exit(s);
  end;
  s := '';
  repeat
    s := hexDigit(longint(b and 15)) + s;
    b := b shr 4;
  until b = 0;
  doubleText := '0x' + s;
end;

{ ===================================================================== }
{ Constants                                                             }
{ ===================================================================== }

function irV(t: PIRType; const v: string): TV;
var
  r: TV;
begin
  r.t := t;
  r.v := v;
  r.kind := vkInst;
  r.agg := nil;
  r.fp := 0.0;
  limbsOfInt(0, r.limbs);
  irV := r;
end;

function intText(t: PIRType; const l: TLimbs): string;
begin
  if t^.bits = 1 then
  begin
    if (l[0] and 1) <> 0 then
      intText := 'true'
    else
      intText := 'false';
  end
  else if t^.bits <= 64 then
    intText := intToStr(limbsToInt(l))
  else
    intText := limbsToDecimal(l);
end;

function irConstLimbs(t: PIRType; const l: TLimbs): TV;
var
  r: TV;
begin
  r := irV(t, '');
  r.kind := vkInt;
  r.limbs := l;
  normalize(r.limbs, t^.bits);
  r.v := intText(t, r.limbs);
  irConstLimbs := r;
end;

function irConstInt(t: PIRType; v: int64): TV;
var
  l: TLimbs;
begin
  limbsOfInt(v, l);
  irConstInt := irConstLimbs(t, l);
end;

function irConstBool(b: boolean): TV;
begin
  if b then
    irConstBool := irConstInt(irI1, 1)
  else
    irConstBool := irConstInt(irI1, 0);
end;

function irConstDouble(x: double): TV;
var
  r: TV;
begin
  r := irV(irDoubleT, doubleText(x));
  r.kind := vkFP;
  r.fp := x;
  irConstDouble := r;
end;

function irNull: TV;
var
  r: TV;
begin
  r := irV(irPtrT, 'null');
  r.kind := vkNull;
  irNull := r;
end;

function irConstIntToPtr(n: int64): TV;
var
  r: TV;
begin
  if n = 0 then { the constant folder gives null }
    exit(irNull);
  r := irV(irPtrT, 'inttoptr (i64 ' + intToStr(n) + ' to ptr)');
  r.kind := vkConstExpr;
  irConstIntToPtr := r;
end;

function irZero(t: PIRType): TV;
var
  r: TV;
begin
  case t^.kind of
    ikInt: exit(irConstInt(t, 0));
    ikDouble: exit(irConstDouble(0.0));
    ikPtr: exit(irNull);
  else
    ;
  end;
  r := irV(t, 'zeroinitializer');
  r.kind := vkZero;
  irZero := r;
end;

function irUndef(t: PIRType): TV;
var
  r: TV;
begin
  r := irV(t, 'undef');
  r.kind := vkUndef;
  irUndef := r;
end;

function irPoison(t: PIRType): TV;
var
  r: TV;
begin
  r := irV(t, 'poison');
  r.kind := vkPoison;
  irPoison := r;
end;

function irIsConst(const v: TV): boolean;
begin
  irIsConst := v.kind <> vkInst;
end;

function irIsConstInt(const v: TV; var value: int64): boolean;
begin
  irIsConstInt := false;
  if (v.kind <> vkInt) or (v.t^.bits > 64) then
    exit;
  value := limbsToInt(v.limbs);
  irIsConstInt := true;
end;

function irIsNullValue(const v: TV): boolean;
begin
  case v.kind of
    vkInt: irIsNullValue := limbsIsZero(v.limbs);
    vkFP: irIsNullValue := fpBits(v.fp) = 0;
    vkNull, vkZero: irIsNullValue := true;
  else
    irIsNullValue := false;
  end;
end;

function irBox(const v: TV): PTVBox;
var
  b: PTVBox;
begin
  b := PTVBox(arenaAlloc(sizeof(TV)));
  b^ := v;
  irBox := b;
end;

{ A text of any length: itself if short, else an atom marker. }
function atomText(const b: TBuf): string;
var
  box: PBufBox;
  s: string;
  i: longint;
begin
  if b.len <= 200 then
  begin
    s := '';
    for i := 0 to longint(b.len) - 1 do
      s := s + bufCharAt(b, i);
    exit(s);
  end;
  box := PBufBox(arenaAlloc(sizeof(TBuf)));
  bufInit(box^);
  bufAddChars(box^, b.data, 0, b.len);
  listAdd(atoms, box);
  atomText := #2 + intToStr(atoms.count) + #2;
end;

procedure addCString(var b: TBuf; p: PCharArray; start, count: int64);
var
  i: int64;
  c: char;
begin
  bufAdd(b, 'c"');
  {$R-}
  for i := start to start + count - 1 do
  begin
    c := p^[i];
    if c = '\' then
      bufAdd(b, '\\')
    else if (c >= ' ') and (c <= '~') and (c <> '"') then
      bufAddChar(b, c)
    else
    begin
      bufAddChar(b, '\');
      bufAddChar(b, hexDigit(ord(c) shr 4));
      bufAddChar(b, hexDigit(ord(c) and 15));
    end;
  end;
  {$R+}
  bufAddChar(b, '"');
end;

function allKind(const elems: TPtrList; kind: TValueKind): boolean;
var
  i: longint;
begin
  for i := 0 to elems.count - 1 do
    if PTVBox(listGet(elems, i))^.kind <> kind then
      exit(false);
  allKind := true;
end;

function allNull(const elems: TPtrList): boolean;
var
  i: longint;
begin
  for i := 0 to elems.count - 1 do
    if not irIsNullValue(PTVBox(listGet(elems, i))^) then
      exit(false);
  allNull := true;
end;

function makeAgg(t: PIRType; const elems: TPtrList; const text: TBuf): TV;
var
  r: TV;
  l: PPtrList;
  i: longint;
begin
  r := irV(t, atomText(text));
  r.kind := vkAgg;
  l := PPtrList(arenaAlloc(sizeof(TPtrList)));
  listInit(l^);
  for i := 0 to elems.count - 1 do
    listAdd(l^, listGet(elems, i));
  r.agg := l;
  makeAgg := r;
end;

{ the canonical form of an aggregate with these elements, or false }
function simpleAgg(t: PIRType; const elems: TPtrList; var r: TV): boolean;
begin
  simpleAgg := true;
  if elems.count = 0 then
    r := irZero(t)
  else if allKind(elems, vkUndef) then
    r := irUndef(t)
  else if allKind(elems, vkPoison) then
    r := irPoison(t)
  else if allNull(elems) then
    r := irZero(t)
  else
    simpleAgg := false;
end;

function irConstArray(t: PIRType; const elems: TPtrList): TV;
var
  b, bytes: TBuf;
  i: longint;
  e: PTVBox;
  r: TV;
begin
  if simpleAgg(t, elems, r) then
    exit(r);
  bufInit(b);
  if (t^.elem^.kind = ikInt) and (t^.elem^.bits = 8) and allKind(elems, vkInt) then
  begin
    { a ConstantDataArray of i8 prints as a string }
    bufInit(bytes);
    for i := 0 to elems.count - 1 do
      bufAddChar(bytes, chr(PTVBox(listGet(elems, i))^.limbs[0] and 255));
    addCString(b, bytes.data, 0, bytes.len);
    bufFree(bytes);
  end
  else
  begin
    bufAddChar(b, '[');
    for i := 0 to elems.count - 1 do
    begin
      e := PTVBox(listGet(elems, i));
      if i > 0 then
        bufAdd(b, ', ');
      bufAdd(b, t^.elem^.text + ' ');
      bufAdd(b, e^.v);
    end;
    bufAddChar(b, ']');
  end;
  irConstArray := makeAgg(t, elems, b);
  bufFree(b);
end;

function irConstStruct(t: PIRType; const elems: TPtrList): TV;
var
  b: TBuf;
  i: longint;
  r: TV;
begin
  if simpleAgg(t, elems, r) then
    exit(r);
  bufInit(b);
  bufAdd(b, '{ ');
  for i := 0 to elems.count - 1 do
  begin
    if i > 0 then
      bufAdd(b, ', ');
    bufAdd(b, PIRType(listGet(t^.fields, i))^.text + ' ');
    bufAdd(b, PTVBox(listGet(elems, i))^.v);
  end;
  bufAdd(b, ' }');
  irConstStruct := makeAgg(t, elems, b);
  bufFree(b);
end;

function irConstBytes(p: PCharArray; start, count: int64): TV;
var
  elems: TPtrList;
  i: int64;
begin
  listInit(elems);
  {$R-}
  for i := start to start + count - 1 do
    listAdd(elems, irBox(irConstInt(irI8, ord(p^[i]))));
  {$R+}
  irConstBytes := irConstArray(irArray(count, irI8), elems);
  listFree(elems);
end;

{ The elements of a constant aggregate: a new list of PTVBox. }
procedure aggElements(const v: TV; var elems: TPtrList);
var
  i, n: longint;
  ft: PIRType;
begin
  listInit(elems);
  if v.t^.kind = ikStruct then
    n := v.t^.fields.count
  else
    n := longint(v.t^.count);
  for i := 0 to n - 1 do
  begin
    if v.t^.kind = ikStruct then
      ft := PIRType(listGet(v.t^.fields, i))
    else
      ft := v.t^.elem;
    case v.kind of
      vkAgg: listAdd(elems, listGet(PPtrList(v.agg)^, i));
      vkUndef: listAdd(elems, irBox(irUndef(ft)));
      vkPoison: listAdd(elems, irBox(irPoison(ft)));
    else
      listAdd(elems, irBox(irZero(ft)));
    end;
  end;
end;

function aggOf(t: PIRType; const elems: TPtrList): TV;
begin
  if t^.kind = ikStruct then
    aggOf := irConstStruct(t, elems)
  else
    aggOf := irConstArray(t, elems);
end;

{ ===================================================================== }
{ Names                                                                 }
{ ===================================================================== }

{ as LLVM prints a name: plain if it is a valid identifier, else quoted }
function quotedName(const name: string): string;
var
  i: longint;
  c: char;
  plain: boolean;
  s: string;
begin
  plain := (name <> '') and not ((name[1] >= '0') and (name[1] <= '9'));
  for i := 1 to length(name) do
  begin
    c := name[i];
    if not (((c >= 'a') and (c <= 'z')) or ((c >= 'A') and (c <= 'Z')) or
            ((c >= '0') and (c <= '9')) or (c = '.') or (c = '_') or
            (c = '-')) then
      plain := false;
  end;
  if plain then
    exit(name);
  s := '"';
  for i := 1 to length(name) do
  begin
    c := name[i];
    if c = '\' then
      s := s + '\\'
    else if (c >= ' ') and (c <= '~') and (c <> '"') then
      s := s + c
    else
      s := s + '\' + hexDigit(ord(c) shr 4) + hexDigit(ord(c) and 15);
  end;
  quotedName := s + '"';
end;

{ a module-level name made unique: name, name.1, name.2 ... }
function uniqueGlobalName(const name: string): string;
var
  n: string;
begin
  n := name;
  while mapHas(moduleNames, intern(n)^.id) do
  begin
    moduleLastUnique := moduleLastUnique + 1;
    n := name + '.' + intToStr(moduleLastUnique);
  end;
  mapPut(moduleNames, intern(n)^.id, intern(n));
  uniqueGlobalName := n;
end;

{ a function-level name made unique: name, name1, name2 ... ('' stays '') }
function uniqueLocal(f: PIRFunc; const name: string): string;
var
  n: string;
begin
  if name = '' then
    exit('');
  n := name;
  while mapHas(f^.names, intern(n)^.id) do
  begin
    f^.lastUnique := f^.lastUnique + 1;
    n := name + intToStr(f^.lastUnique);
  end;
  mapPut(f^.names, intern(n)^.id, intern(n));
  uniqueLocal := n;
end;

{ ===================================================================== }
{ Module                                                                }
{ ===================================================================== }

procedure irInit(const sourceName, triple, dataLayout: string);
var
  fields: TPtrList;
begin
  mapInit(intTypes);
  mapInit(typeById);
  mapInit(structNames);
  mapInit(globalByName);
  mapInit(funcByName);
  mapInit(moduleNames);
  listInit(globals);
  listInit(funcs);
  listInit(atoms);
  structUniqueId := 0;
  typeCount := 0;
  moduleLastUnique := 0;
  listInit(metas);
  mapInit(metaByText);
  listInit(namedMetas);
  curLoc := nil;
  branchWeights := nil;
  modSource := sourceName;
  modTriple := triple;
  modLayout := dataLayout;
  parseDataLayout(dataLayout);
  irVoidT := newIRType(ikVoid);
  irVoidT^.text := 'void';
  irI1 := irInt(1);
  irI8 := irInt(8);
  irI16 := irInt(16);
  irI32 := irInt(32);
  irI64 := irInt(64);
  irI256 := irInt(256);
  irDoubleT := newIRType(ikDouble);
  irDoubleT^.text := 'double';
  irDoubleT^.size := 8;
  irDoubleT^.align := dlF64Abi;
  irDoubleT^.prefAlign := dlF64Pref;
  irPtrT := newIRType(ikPtr);
  irPtrT^.text := 'ptr';
  irPtrT^.size := dlPtrSize;
  irPtrT^.align := dlPtrAbi;
  irPtrT^.prefAlign := dlPtrPref;
  listInit(fields);
  cur := nil;
  curBlock := nil;
end;

function globalValue(g: PGlobal): TV;
var
  r: TV;
begin
  if g^.name = '' then
    r := irV(irPtrT, '@' + #3 + intToStr(g^.id) + #3)
  else
    r := irV(irPtrT, '@' + quotedName(g^.name));
  r.kind := vkGlobal;
  globalValue := r;
end;

function irGlobal(const name, linkage: string; t: PIRType; const init: TV;
                  isConstant, unnamedAddr, isDecl: boolean; align: longint): TV;
var
  g: PGlobal;
begin
  g := PGlobal(arenaAlloc(sizeof(TGlobal)));
  if name = '' then
    g^.name := ''
  else
    g^.name := uniqueGlobalName(name);
  g^.id := globals.count + 1;
  g^.dbg := nil;
  bufInit(g^.text);
  if linkage <> '' then
    bufAdd(g^.text, linkage + ' ');
  if unnamedAddr then
    bufAdd(g^.text, 'unnamed_addr ');
  if isConstant then
    bufAdd(g^.text, 'constant ')
  else
    bufAdd(g^.text, 'global ');
  bufAdd(g^.text, t^.text);
  if not isDecl then
  begin
    bufAddChar(g^.text, ' ');
    bufAdd(g^.text, init.v);
  end;
  if align > 0 then
    bufAdd(g^.text, ', align ' + intToStr(align));
  listAdd(globals, g);
  if g^.name <> '' then
    mapPut(globalByName, intern(g^.name)^.id, g);
  irGlobal := globalValue(g);
end;

function irFindGlobal(const name: string; var g: TV): boolean;
var
  p: PGlobal;
begin
  p := PGlobal(mapGet(globalByName, intern(name)^.id));
  if p = nil then
    exit(false);
  g := globalValue(p);
  irFindGlobal := true;
end;

procedure irSetGlobalInit(const g: TV; const init: TV);
var
  i: longint;
  p: PGlobal;
begin
  for i := 0 to globals.count - 1 do
  begin
    p := PGlobal(listGet(globals, i));
    if globalValue(p).v = g.v then
    begin
      bufAddChar(p^.text, ' ');
      bufAdd(p^.text, init.v);
      exit;
    end;
  end;
end;

function newFunc(const name: string; ret: PIRType; const params: TPtrList): PIRFunc;
var
  f: PIRFunc;
  i: longint;
  n: string;
begin
  f := PIRFunc(arenaAlloc(sizeof(TIRFunc)));
  n := uniqueGlobalName(name);
  f^.name := '@' + quotedName(n);
  f^.ret := ret;
  listInit(f^.params);
  listInit(f^.argNames);
  for i := 0 to params.count - 1 do
  begin
    listAdd(f^.params, listGet(params, i));
    listAdd(f^.argNames, nil);
  end;
  listInit(f^.blocks);
  listInit(f^.entryTemps);
  mapInit(f^.names);
  f^.lastUnique := 0;
  f^.nextDef := 0;
  f^.dbg := nil;
  f^.attrs := '';
  f^.paramText := '';
  f^.linkage := '';
  listAdd(funcs, f);
  mapPut(funcByName, intern(n)^.id, f);
  newFunc := f;
end;

function irDeclareFunction(const name: string; ret: PIRType; const params: TPtrList;
                           const attrs, paramText: string): PIRFunc;
var
  f: PIRFunc;
begin
  f := newFunc(name, ret, params);
  f^.isDecl := true;
  f^.attrs := attrs;
  f^.paramText := paramText;
  irDeclareFunction := f;
end;

function irFindFunction(const name: string): PIRFunc;
begin
  irFindFunction := PIRFunc(mapGet(funcByName, intern(name)^.id));
end;

function irCreateFunction(const name, linkage: string; ret: PIRType;
                          const params: TPtrList): PIRFunc;
var
  f: PIRFunc;
begin
  f := newFunc(name, ret, params);
  f^.isDecl := false;
  f^.linkage := linkage;
  irCreateFunction := f;
end;

procedure irSetArgName(f: PIRFunc; i: longint; const name: string);
begin
  listSet(f^.argNames, i, intern(uniqueLocal(f, name)));
end;

function irArg(f: PIRFunc; i: longint): TV;
begin
  irArg := irV(PIRType(listGet(f^.params, i)),
               '%' + quotedName(nameStr(PName(listGet(f^.argNames, i)))));
end;

function irFuncRef(f: PIRFunc): TV;
var
  r: TV;
begin
  r := irV(irPtrT, f^.name);
  r.kind := vkGlobal;
  irFuncRef := r;
end;

{ ===================================================================== }
{ Function bodies                                                       }
{ ===================================================================== }

procedure irBeginBody(f: PIRFunc);
begin
  cur := f;
  curBlock := nil;
end;

function irNewBlock(const name: string): PIRBlock;
var
  b: PIRBlock;
begin
  b := PIRBlock(arenaAlloc(sizeof(TIRBlock)));
  b^.name := uniqueLocal(cur, name);
  listInit(b^.insts);
  listInit(b^.preds);
  listInit(b^.pending);
  listAdd(cur^.blocks, b);
  irNewBlock := b;
end;

procedure irSetInsertPoint(b: PIRBlock);
begin
  curBlock := b;
end;

function irInsertBlock: PIRBlock;
begin
  irInsertBlock := curBlock;
end;

function newInst(var res: string; const name: string; hasResult: boolean): PIRInst;
var
  i: PIRInst;
  n: string;
begin
  i := PIRInst(arenaAlloc(sizeof(TIRInst)));
  bufInit(i^.text);
  i^.defId := 0;
  i^.dbgLoc := curLoc;
  i^.unlikely := false;
  listInit(i^.records);
  res := '';
  if hasResult then
  begin
    n := uniqueLocal(cur, name);
    if n = '' then
    begin
      cur^.nextDef := cur^.nextDef + 1;
      i^.defId := cur^.nextDef;
      res := '%' + #1 + intToStr(i^.defId) + #1;
    end
    else
      res := '%' + quotedName(n);
    bufAdd(i^.text, res + ' = ');
  end;
  newInst := i;
end;

procedure append(i: PIRInst);
var
  k: longint;
begin
  i^.block := curBlock;
  { debug records inserted at the end of the block go before it }
  for k := 0 to curBlock^.pending.count - 1 do
    listAdd(i^.records, listGet(curBlock^.pending, k));
  listClear(curBlock^.pending);
  listAdd(curBlock^.insts, i);
end;

{ --- Folding (ConstantFolder) --- }

function isIntConst(const v: TV): boolean;
begin
  isIntConst := v.kind = vkInt;
end;

function foldIntBin(const op: string; const a, b: TV; var r: TV): boolean;
var
  x, y, z, q, rem, minv: TLimbs;
  bits, n: longint;
  negA, negB: boolean;
begin
  foldIntBin := true;
  bits := a.t^.bits;
  x := a.limbs;
  y := b.limbs;
  if op = 'add' then
    limbsAdd(x, y, z)
  else if op = 'sub' then
    limbsSub(x, y, z)
  else if op = 'mul' then
    limbsMul(x, y, z)
  else if op = 'and' then
  begin
    for n := 0 to 7 do
      z[n] := x[n] and y[n];
  end
  else if op = 'or' then
  begin
    for n := 0 to 7 do
      z[n] := x[n] or y[n];
  end
  else if op = 'xor' then
  begin
    for n := 0 to 7 do
      z[n] := x[n] xor y[n];
  end
  else if (op = 'shl') or (op = 'lshr') then
  begin
    zeroExtend(y, bits);
    if (y[1] <> 0) or (y[2] <> 0) or (y[3] <> 0) or (y[4] <> 0) or
       (y[5] <> 0) or (y[6] <> 0) or (y[7] <> 0) or (y[0] >= bits) then
    begin
      r := irPoison(a.t);
      exit;
    end;
    if op = 'shl' then
      limbsShl(x, longint(y[0]), z)
    else
    begin
      zeroExtend(x, bits);
      limbsLshr(x, longint(y[0]), z);
    end;
  end
  else if (op = 'udiv') or (op = 'urem') or (op = 'sdiv') or (op = 'srem') then
  begin
    if limbsIsZero(y) then
    begin
      r := irPoison(a.t);
      exit;
    end;
    if (op = 'udiv') or (op = 'urem') then
    begin
      zeroExtend(x, bits);
      zeroExtend(y, bits);
      limbsUDivRem(x, y, q, rem);
    end
    else
    begin
      { the minimum divided by -1 overflows }
      limbsOfInt(0, minv);
      minv[(bits - 1) div 32] := int64(1) shl ((bits - 1) mod 32);
      normalize(minv, bits);
      if (limbsUCmp(x, minv) = 0) and (limbsToInt(y) = -1) and limbsNegative(y) then
      begin
        r := irPoison(a.t);
        exit;
      end;
      negA := limbsNegative(x);
      negB := limbsNegative(y);
      if negA then
        limbsNeg(x, x);
      if negB then
        limbsNeg(y, y);
      limbsUDivRem(x, y, q, rem);
      if negA <> negB then
        limbsNeg(q, q);
      if negA then
        limbsNeg(rem, rem);
    end;
    if (op = 'udiv') or (op = 'sdiv') then
      z := q
    else
      z := rem;
  end
  else
  begin
    foldIntBin := false;
    exit;
  end;
  r := irConstLimbs(a.t, z);
end;

{ IEEE arithmetic as APFloat does it; NaN results are the default NaN }
function foldFPBin(const op: string; x, y: double): double;
var
  bx, by: int64;
  negZero: boolean;
  r: double;
begin
  bx := fpBits(x);
  by := fpBits(y);
  if isNaNBits(bx) then
    exit(fpOfBits(bx or 2251799813685248)); { quieted }
  if isNaNBits(by) then
    exit(fpOfBits(by or 2251799813685248));
  if op = 'fadd' then
    r := realAdd(x, y)
  else if op = 'fsub' then
    r := realSub(x, y)
  else if op = 'fmul' then
    r := realMul(x, y)
  else
  begin
    { fdiv }
    if (by and 9223372036854775807) = 0 then
    begin
      if (bx and 9223372036854775807) = 0 then
        exit(fpOfBits(9221120237041090560)); { 0/0: NaN }
      negZero := (bx < 0) <> (by < 0);
      exit(realInfinity(negZero));
    end;
    r := realDiv(x, y);
  end;
  if isNaNBits(fpBits(r)) then
    r := fpOfBits(9221120237041090560);
  foldFPBin := r;
end;

function irBin(const op: string; const a, b: TV; const name: string): TV;
var
  i: PIRInst;
  res: string;
  r: TV;
begin
  if (a.kind = vkPoison) or (b.kind = vkPoison) then
    if irIsConst(a) and irIsConst(b) then
      exit(irPoison(a.t));
  if isIntConst(a) and isIntConst(b) and foldIntBin(op, a, b, r) then
    exit(r);
  if (a.kind = vkFP) and (b.kind = vkFP) then
    exit(irConstDouble(foldFPBin(op, a.fp, b.fp)));
  i := newInst(res, name, true);
  bufAdd(i^.text, op + ' ' + a.t^.text + ' ' + a.v + ', ' + b.v);
  append(i);
  irBin := irV(a.t, res);
end;

function icmpHolds(const pred: string; const a, b: TV): boolean;
var
  x, y: TLimbs;
  bits: longint;
  s, u: longint;
begin
  bits := a.t^.bits;
  x := a.limbs;
  y := b.limbs;
  s := limbsSCmp(x, y);
  zeroExtend(x, bits);
  zeroExtend(y, bits);
  u := limbsUCmp(x, y);
  if pred = 'eq' then icmpHolds := u = 0
  else if pred = 'ne' then icmpHolds := u <> 0
  else if pred = 'slt' then icmpHolds := s < 0
  else if pred = 'sle' then icmpHolds := s <= 0
  else if pred = 'sgt' then icmpHolds := s > 0
  else if pred = 'sge' then icmpHolds := s >= 0
  else if pred = 'ult' then icmpHolds := u < 0
  else if pred = 'ule' then icmpHolds := u <= 0
  else if pred = 'ugt' then icmpHolds := u > 0
  else icmpHolds := u >= 0;
end;

function irICmp(const pred: string; const a, b: TV; const name: string): TV;
var
  i: PIRInst;
  res: string;
  known, eq: boolean;
begin
  if irIsConst(a) and irIsConst(b) and ((a.kind = vkPoison) or (b.kind = vkPoison)) then
    exit(irPoison(irI1));
  if isIntConst(a) and isIntConst(b) then
    exit(irConstBool(icmpHolds(pred, a, b)));
  { pointers: null against null or a global }
  if (a.t^.kind = ikPtr) and irIsConst(a) and irIsConst(b) and
     ((pred = 'eq') or (pred = 'ne')) then
  begin
    known := false;
    eq := false;
    if (a.kind = vkNull) and (b.kind = vkNull) then
    begin
      known := true;
      eq := true;
    end
    else if ((a.kind = vkNull) and (b.kind = vkGlobal)) or
            ((a.kind = vkGlobal) and (b.kind = vkNull)) then
      known := true
    else if (a.kind = vkGlobal) and (b.kind = vkGlobal) and (a.v = b.v) then
    begin
      known := true;
      eq := true;
    end;
    if known then
      exit(irConstBool(eq = (pred = 'eq')));
  end;
  i := newInst(res, name, true);
  bufAdd(i^.text, 'icmp ' + pred + ' ' + a.t^.text + ' ' + a.v + ', ' + b.v);
  append(i);
  irICmp := irV(irI1, res);
end;

function fcmpHolds(const pred: string; x, y: double): boolean;
var
  unordered, lt, eq, gt: boolean;
begin
  unordered := isNaNBits(fpBits(x)) or isNaNBits(fpBits(y));
  lt := false;
  eq := false;
  gt := false;
  if not unordered then
  begin
    lt := x < y;
    eq := x = y;
    gt := x > y;
  end;
  if pred = 'oeq' then fcmpHolds := eq
  else if pred = 'one' then fcmpHolds := lt or gt
  else if pred = 'olt' then fcmpHolds := lt
  else if pred = 'ole' then fcmpHolds := lt or eq
  else if pred = 'ogt' then fcmpHolds := gt
  else if pred = 'oge' then fcmpHolds := gt or eq
  else if pred = 'ueq' then fcmpHolds := unordered or eq
  else if pred = 'une' then fcmpHolds := unordered or lt or gt
  else if pred = 'ult' then fcmpHolds := unordered or lt
  else if pred = 'ule' then fcmpHolds := unordered or lt or eq
  else if pred = 'ugt' then fcmpHolds := unordered or gt
  else fcmpHolds := unordered or gt or eq; { uge }
end;

function irFCmp(const pred: string; const a, b: TV; const name: string): TV;
var
  i: PIRInst;
  res: string;
begin
  if (a.kind = vkFP) and (b.kind = vkFP) then
    exit(irConstBool(fcmpHolds(pred, a.fp, b.fp)));
  i := newInst(res, name, true);
  bufAdd(i^.text, 'fcmp ' + pred + ' double ' + a.v + ', ' + b.v);
  append(i);
  irFCmp := irV(irI1, res);
end;

function irFNeg(const a: TV; const name: string): TV;
var
  i: PIRInst;
  res: string;
begin
  if a.kind = vkFP then
    exit(irConstDouble(fpOfBits(fpBits(a.fp) xor (-9223372036854775807 - 1))));
  i := newInst(res, name, true);
  bufAdd(i^.text, 'fneg double ' + a.v);
  append(i);
  irFNeg := irV(irDoubleT, res);
end;

function foldCast(const op: string; const v: TV; dst: PIRType; var r: TV): boolean;
var
  l: TLimbs;
  x, lo, hi: double;
  bits: longint;
begin
  foldCast := true;
  if v.kind = vkPoison then
  begin
    r := irPoison(dst);
    exit;
  end;
  if v.kind = vkInt then
  begin
    l := v.limbs;
    if op = 'zext' then
    begin
      zeroExtend(l, v.t^.bits);
      r := irConstLimbs(dst, l);
    end
    else if (op = 'sext') or (op = 'trunc') then
      r := irConstLimbs(dst, l)
    else if op = 'sitofp' then
      r := irConstDouble(limbsToInt(l))
    else if op = 'uitofp' then
    begin
      zeroExtend(l, v.t^.bits);
      if v.t^.bits < 64 then
        r := irConstDouble(limbsToInt(l))
      else
        foldCast := false;
    end
    else
      foldCast := false;
    exit;
  end;
  if (v.kind = vkFP) and (op = 'fptosi') then
  begin
    x := v.fp;
    bits := dst^.bits;
    if isNaNBits(fpBits(x)) then
    begin
      r := irPoison(dst);
      exit;
    end;
    if bits >= 64 then
    begin
      lo := -9223372036854775808.0;
      hi := 9223372036854775808.0;
    end
    else
    begin
      lo := -1.0 * (int64(1) shl (bits - 1));
      hi := 1.0 * (int64(1) shl (bits - 1));
    end;
    { truncation toward zero must give a value in [lo, hi) }
    if (x <= lo - 1.0) or (x >= hi) then
    begin
      r := irPoison(dst);
      exit;
    end;
    r := irConstInt(dst, trunc(x));
    exit;
  end;
  foldCast := false;
end;

function irCast(const op: string; const v: TV; dst: PIRType; const name: string): TV;
var
  i: PIRInst;
  res: string;
  r: TV;
begin
  if irSameType(v.t, dst) then
    exit(v);
  if irIsConst(v) and foldCast(op, v, dst, r) then
    exit(r);
  i := newInst(res, name, true);
  bufAdd(i^.text, op + ' ' + v.t^.text + ' ' + v.v + ' to ' + dst^.text);
  append(i);
  irCast := irV(dst, res);
end;

function irSelect(const c, a, b: TV; const name: string): TV;
var
  i: PIRInst;
  res: string;
begin
  if irIsConst(c) and irIsConst(a) and irIsConst(b) then
  begin
    if c.kind = vkInt then
    begin
      if (c.limbs[0] and 1) <> 0 then
        exit(a);
      exit(b);
    end;
    if c.kind = vkPoison then
      exit(irPoison(a.t));
  end;
  i := newInst(res, name, true);
  bufAdd(i^.text, 'select i1 ' + c.v + ', ' + a.t^.text + ' ' + a.v + ', ' +
         b.t^.text + ' ' + b.v);
  append(i);
  irSelect := irV(a.t, res);
end;

function irLoad(t: PIRType; const p: TV; const name: string): TV;
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, name, true);
  bufAdd(i^.text, 'load ' + t^.text + ', ptr ' + p.v + ', align ' + intToStr(t^.align));
  append(i);
  irLoad := irV(t, res);
end;

procedure irStore(const v, p: TV);
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, '', false);
  bufAdd(i^.text, 'store ' + v.t^.text + ' ' + v.v + ', ptr ' + p.v + ', align ' +
         intToStr(v.t^.align));
  append(i);
end;

{ a GEP with indices given as text ("i64 0, i64 3"); folds when the
  pointer and all indices are constants }
function gep(src: PIRType; const p: TV; const idxText: string; allConst, allZero,
             inbounds: boolean; const name: string): TV;
var
  i: PIRInst;
  res, kw: string;
  b: TBuf;
  r: TV;
begin
  kw := 'getelementptr ';
  if inbounds then
    kw := kw + 'inbounds ';
  if allConst and (p.kind in [vkGlobal, vkConstExpr, vkNull]) then
  begin
    if allZero then
      exit(p);
    bufInit(b);
    bufAdd(b, kw + '(' + src^.text + ', ptr ');
    bufAdd(b, p.v);
    bufAdd(b, ', ' + idxText + ')');
    r := irV(irPtrT, atomText(b));
    r.kind := vkConstExpr;
    bufFree(b);
    exit(r);
  end;
  i := newInst(res, name, true);
  bufAdd(i^.text, kw + src^.text + ', ptr ');
  bufAdd(i^.text, p.v);
  bufAdd(i^.text, ', ' + idxText);
  append(i);
  gep := irV(irPtrT, res);
end;

function irGEP1(src: PIRType; const p, i0: TV; inbounds: boolean; const name: string): TV;
begin
  irGEP1 := gep(src, p, i0.t^.text + ' ' + i0.v, irIsConst(i0),
                irIsConst(i0) and irIsNullValue(i0), inbounds, name);
end;

function irGEP2(src: PIRType; const p, i0, i1: TV; const name: string): TV;
begin
  irGEP2 := gep(src, p, i0.t^.text + ' ' + i0.v + ', ' + i1.t^.text + ' ' + i1.v,
                irIsConst(i0) and irIsConst(i1),
                irIsConst(i0) and irIsConst(i1) and irIsNullValue(i0) and
                irIsNullValue(i1), true, name);
end;

function irStructGEP(st: PIRType; const p: TV; field: longint; const name: string): TV;
begin
  irStructGEP := gep(st, p, 'i32 0, i32 ' + intToStr(field), true, field = 0, true, name);
end;

function irExtract(const agg: TV; idx: longint; t: PIRType; const name: string): TV;
var
  i: PIRInst;
  res: string;
  elems: TPtrList;
  r: TV;
begin
  if irIsConst(agg) then
  begin
    aggElements(agg, elems);
    r := PTVBox(listGet(elems, idx))^;
    listFree(elems);
    exit(r);
  end;
  i := newInst(res, name, true);
  bufAdd(i^.text, 'extractvalue ' + agg.t^.text + ' ' + agg.v + ', ' + intToStr(idx));
  append(i);
  irExtract := irV(t, res);
end;

function irInsert(const agg, v: TV; idx: longint; const name: string): TV;
var
  i: PIRInst;
  res: string;
  elems: TPtrList;
  r: TV;
begin
  if irIsConst(agg) and irIsConst(v) then
  begin
    aggElements(agg, elems);
    listSet(elems, idx, irBox(v));
    r := aggOf(agg.t, elems);
    listFree(elems);
    exit(r);
  end;
  i := newInst(res, name, true);
  bufAdd(i^.text, 'insertvalue ' + agg.t^.text + ' ' + agg.v + ', ' + v.t^.text + ' ' +
         v.v + ', ' + intToStr(idx));
  append(i);
  irInsert := irV(agg.t, res);
end;

function irAlloca(t: PIRType; const name: string): TV;
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, name, true);
  bufAdd(i^.text, 'alloca ' + t^.text + ', align ' + intToStr(t^.prefAlign));
  append(i);
  irAlloca := irV(irPtrT, res);
end;

function irCreateTemp(t: PIRType; const name: string): TV;
var
  i: PIRInst;
  entryB: PIRBlock;
  res: string;
begin
  i := newInst(res, name, true);
  bufAdd(i^.text, 'alloca ' + t^.text + ', align ' + intToStr(t^.prefAlign));
  { an IRBuilder positioned at the start of the entry block takes the
    location of the instruction there }
  entryB := PIRBlock(listGet(cur^.blocks, 0));
  if cur^.entryTemps.count > 0 then
    i^.dbgLoc := PIRInst(listGet(cur^.entryTemps, cur^.entryTemps.count - 1))^.dbgLoc
  else if entryB^.insts.count > 0 then
    i^.dbgLoc := PIRInst(listGet(entryB^.insts, 0))^.dbgLoc
  else
    i^.dbgLoc := nil;
  listAdd(cur^.entryTemps, i);
  irCreateTemp := irV(irPtrT, res);
end;

function irCreateTempInit(t: PIRType; const name: string; const init: TV): TV;
var
  slot: TV;
  i, alloca: PIRInst;
  res: string;
begin
  slot := irCreateTemp(t, name);
  { entryTemps print newest first: the store goes in before its alloca }
  alloca := PIRInst(listPop(cur^.entryTemps));
  i := newInst(res, '', false);
  bufAdd(i^.text, 'store ' + init.t^.text + ' ' + init.v + ', ptr ' + slot.v +
         ', align ' + intToStr(init.t^.align));
  i^.dbgLoc := alloca^.dbgLoc;
  listAdd(cur^.entryTemps, i);
  listAdd(cur^.entryTemps, alloca);
  irCreateTempInit := slot;
end;

procedure irArgsAdd(var args: TBuf; const v: TV);
begin
  if args.len > 0 then
    bufAdd(args, ', ');
  bufAdd(args, v.t^.text + ' ' + v.v);
end;

procedure irArgsAddAttr(var args: TBuf; const v: TV; const attr: string);
begin
  if args.len > 0 then
    bufAdd(args, ', ');
  bufAdd(args, v.t^.text + ' ' + attr + ' ' + v.v);
end;

{ The parameter list of a variadic function's type: 'ptr, i32, ...'. }
function irVarArgParams(const params: TPtrList): string;
var
  s: string;
  i: longint;
begin
  s := '';
  for i := 0 to params.count - 1 do
    s := s + PIRType(listGet(params, i))^.text + ', ';
  irVarArgParams := s + '...';
end;

{ A call of variadic function f (declared with irVarArgParams), which LLVM
  prints with the function's type: call i32 (ptr, ...) @printf(...). }
function irCallVarArg(f: PIRFunc; const callee: TV; const args: TBuf;
                      const name: string): TV;
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, name, f^.ret^.kind <> ikVoid);
  bufAdd(i^.text, 'call ' + f^.ret^.text + ' (' + f^.paramText + ') ' + callee.v + '(');
  bufAddChars(i^.text, args.data, 0, args.len);
  bufAddChar(i^.text, ')');
  append(i);
  irCallVarArg := irV(f^.ret, res);
end;

function irCallPtr(ret: PIRType; const callee: TV; const args: TBuf; const name: string): TV;
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, name, ret^.kind <> ikVoid);
  bufAdd(i^.text, 'call ' + ret^.text + ' ' + callee.v + '(');
  bufAddChars(i^.text, args.data, 0, args.len);
  bufAddChar(i^.text, ')');
  append(i);
  irCallPtr := irV(ret, res);
end;

function irCall(f: PIRFunc; const args: TBuf; const name: string): TV;
begin
  irCall := irCallPtr(f^.ret, irFuncRef(f), args, name);
end;

function irMetaText(const s: string): PMeta; forward;

procedure addPred(target: PIRBlock);
begin
  listAdd(target^.preds, curBlock);
end;

procedure irBr(b: PIRBlock);
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, '', false);
  bufAdd(i^.text, 'br label %' + quotedName(b^.name));
  append(i);
  addPred(b);
end;

procedure irCondBr(const c: TV; t, f: PIRBlock; unlikely: boolean);
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, '', false);
  bufAdd(i^.text, 'br i1 ' + c.v + ', label %' + quotedName(t^.name) + ', label %' +
         quotedName(f^.name));
  if unlikely then
  begin
    i^.unlikely := true;
    if branchWeights = nil then
      branchWeights := irMetaText('!{!"branch_weights", i32 1, i32 1048575}');
  end;
  append(i);
  { BranchInst's operands: the false destination is used first }
  addPred(f);
  addPred(t);
end;

function irSwitch(const v: TV; default: PIRBlock): PIRInst;
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, '', false);
  i^.isSwitch := true;
  bufAdd(i^.text, 'switch ' + v.t^.text + ' ' + v.v + ', label %' +
         quotedName(default^.name) + ' [');
  append(i);
  addPred(default);
  irSwitch := i;
end;

procedure irAddCase(sw: PIRInst; const c: TV; b: PIRBlock);
begin
  bufAdd(sw^.text, #10 + '    ' + c.t^.text + ' ' + c.v + ', label %' + quotedName(b^.name));
  { the switch's block is the predecessor }
  listAdd(b^.preds, sw^.block);
end;

function irPhi(t: PIRType; const name: string; var phi: PIRInst): TV;
var
  res: string;
begin
  phi := newInst(res, name, true);
  phi^.isPhi := true;
  phi^.phiCount := 0;
  bufAdd(phi^.text, 'phi ' + t^.text + ' ');
  append(phi);
  irPhi := irV(t, res);
end;

procedure irAddIncoming(phi: PIRInst; const v: TV; b: PIRBlock);
begin
  if phi^.phiCount > 0 then
    bufAdd(phi^.text, ', ');
  bufAdd(phi^.text, '[ ' + v.v + ', %' + quotedName(b^.name) + ' ]');
  phi^.phiCount := phi^.phiCount + 1;
end;

procedure irRet(const v: TV);
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, '', false);
  bufAdd(i^.text, 'ret ' + v.t^.text + ' ' + v.v);
  append(i);
end;

procedure irRetVoid;
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, '', false);
  bufAdd(i^.text, 'ret void');
  append(i);
end;

procedure irUnreachable;
var
  i: PIRInst;
  res: string;
begin
  i := newInst(res, '', false);
  bufAdd(i^.text, 'unreachable');
  append(i);
end;

{ ===================================================================== }
{ Metadata                                                              }
{ ===================================================================== }

function newMeta: PMeta;
var
  m: PMeta;
begin
  m := PMeta(arenaAlloc(sizeof(TMeta)));
  bufInit(m^.text);
  listInit(m^.ops);
  listAdd(metas, m);
  m^.id := metas.count;
  m^.num := -1;
  newMeta := m;
end;

function irMetaPlaceholder: PMeta;
begin
  irMetaPlaceholder := newMeta;
end;

procedure irMetaFill(m: PMeta; const text: TBuf; const ops: TPtrList);
var
  i: longint;
begin
  bufClear(m^.text);
  bufAddChars(m^.text, text.data, 0, text.len);
  listClear(m^.ops);
  for i := 0 to ops.count - 1 do
    listAdd(m^.ops, listGet(ops, i));
end;

function irMeta(distinct: boolean; const text: TBuf; const ops: TPtrList): PMeta;
var
  key: PName;
  m: PMeta;
begin
  if not distinct then
  begin
    key := internChars(text.data, 0, text.len);
    m := PMeta(mapGet(metaByText, key^.id));
    if m <> nil then
      exit(m);
  end;
  m := newMeta;
  irMetaFill(m, text, ops);
  if distinct then
  begin
    bufClear(m^.text);
    bufAdd(m^.text, 'distinct ');
    bufAddChars(m^.text, text.data, 0, text.len);
  end
  else
    mapPut(metaByText, key^.id, m);
  irMeta := m;
end;

{ a uniqued node without node operands }
function irMetaText(const s: string): PMeta;
var
  b: TBuf;
  ops: TPtrList;
begin
  bufInit(b);
  bufAdd(b, s);
  listInit(ops);
  irMetaText := irMeta(false, b, ops);
  bufFree(b);
end;

function irMetaRef(m: PMeta): string;
begin
  irMetaRef := #5 + intToStr(m^.id) + #5;
end;

procedure irNamedMeta(const name: string; const ops: TPtrList);
var
  l: PPtrList;
  i: longint;
begin
  l := PPtrList(arenaAlloc(sizeof(TPtrList)));
  listInit(l^);
  for i := 0 to ops.count - 1 do
    listAdd(l^, listGet(ops, i));
  listAdd(namedMetas, intern(name));
  listAdd(namedMetas, l);
end;

procedure irSetGlobalDbg(const g: TV; m: PMeta);
var
  name: string;
  p: PGlobal;
begin
  name := copy(g.v, 2, 255);
  { a quoted name (@"pas.u$x") }
  if (length(name) >= 2) and (name[1] = '"') then
    name := copy(name, 2, length(name) - 2);
  p := PGlobal(mapGet(globalByName, intern(name)^.id));
  if p <> nil then
    p^.dbg := m;
end;

procedure irSetFuncDbg(f: PIRFunc; m: PMeta);
begin
  f^.dbg := m;
end;

procedure irSetDebugLoc(m: PMeta);
begin
  curLoc := m;
end;

procedure irInsertDeclare(const v: TV; variable: PMeta; const expr: string; loc: PMeta);
var
  r: PDbgRecord;
begin
  r := PDbgRecord(arenaAlloc(sizeof(TDbgRecord)));
  r^.text := v.t^.text + ' ' + v.v + ', ' + irMetaRef(variable) + ', ' + expr + ', ' +
             irMetaRef(loc);
  r^.variable := variable;
  r^.loc := loc;
  listAdd(curBlock^.pending, r);
end;

{ ===================================================================== }
{ Printing                                                              }
{ ===================================================================== }

var
  typeOrder: TPtrList;   { of PIRType: named structs as TypeFinder finds them }
  typeSeen: TIntMap;     { type id -> non-nil }
  defNumbers: TIntMap;   { def id -> number + 1 (in the function being printed) }
  globalNumbers: TIntMap; { global id -> number + 1 }
  scanning: boolean;     { pass 1: find types; pass 2: print }

function boxInt(n: longint): pointer;
var
  p: PIntBox;
begin
  p := PIntBox(arenaAlloc(sizeof(longint)));
  p^ := n;
  boxInt := p;
end;

function unboxInt(p: pointer): longint;
begin
  if p = nil then
    exit(-1);
  unboxInt := PIntBox(p)^;
end;

procedure visitType(t: PIRType);
var
  i: longint;
begin
  if t = nil then
    exit;
  case t^.kind of
    ikArray: visitType(t^.elem);
    ikStruct:
      begin
        if t^.named then
        begin
          if mapHas(typeSeen, t^.id) then
            exit;
          mapPut(typeSeen, t^.id, t);
          listAdd(typeOrder, t);
        end;
        for i := 0 to t^.fields.count - 1 do
          visitType(PIRType(listGet(t^.fields, i)));
      end;
  else
    ;
  end;
end;

{ Writes p[0..len) to out, expanding markers (in pass 1 only the types
  are noted). }
procedure emitText(var ob: TBuf; p: PCharArray; len: int64);
var
  i, j: int64;
  c: char;
  id: longint;
  t: PIRType;
  box: PBufBox;
begin
  i := 0;
  {$R-}
  while i < len do
  begin
    c := p^[i];
    if (c >= #1) and (c <= #5) then
    begin
      j := i + 1;
      id := 0;
      while p^[j] <> c do
      begin
        id := id * 10 + ord(p^[j]) - ord('0');
        j := j + 1;
      end;
      i := j + 1;
      case c of
        #1:
          if not scanning then
            bufAddInt(ob, unboxInt(mapGet(defNumbers, id)));
        #2:
          begin
            box := PBufBox(listGet(atoms, id - 1));
            emitText(ob, box^.data, box^.len);
          end;
        #3:
          if not scanning then
            bufAddInt(ob, unboxInt(mapGet(globalNumbers, id)));
        #5:
          if not scanning then
          begin
            bufAddChar(ob, '!');
            bufAddInt(ob, PMeta(listGet(metas, id - 1))^.num);
          end;
      else
        begin
          t := PIRType(mapGet(typeById, id));
          if scanning then
            visitType(t)
          else
          begin
            bufAddChar(ob, '%');
            bufAdd(ob, quotedName(t^.name));
          end;
        end;
      end;
      continue;
    end;
    if not scanning then
      bufAddChar(ob, c);
    i := i + 1;
  end;
  {$R+}
end;

procedure emitStr(var ob: TBuf; const s: string);
begin
  if length(s) > 0 then
    emitText(ob, PCharArray(@s[1]), length(s));
end;

procedure emitBuf(var ob: TBuf; const b: TBuf);
begin
  emitText(ob, b.data, b.len);
end;

function paramList(f: PIRFunc; withNames: boolean): string;
var
  s: string;
  i: longint;
begin
  if f^.isDecl and (f^.paramText <> '') then
    exit(f^.paramText);
  s := '';
  for i := 0 to f^.params.count - 1 do
  begin
    if i > 0 then
      s := s + ', ';
    s := s + PIRType(listGet(f^.params, i))^.text;
    if withNames and (listGet(f^.argNames, i) <> nil) then
      s := s + ' %' + quotedName(nameStr(PName(listGet(f^.argNames, i))));
  end;
  paramList := s;
end;

procedure numberDefs(f: PIRFunc);
var
  n, k, j: longint;
  b: PIRBlock;
  i: PIRInst;
begin
  mapInit(defNumbers);
  n := 0;
  for k := 0 to f^.blocks.count - 1 do
  begin
    b := PIRBlock(listGet(f^.blocks, k));
    if k = 0 then
      for j := f^.entryTemps.count - 1 downto 0 do
      begin
        i := PIRInst(listGet(f^.entryTemps, j));
        if i^.defId > 0 then
        begin
          mapPut(defNumbers, i^.defId, boxInt(n));
          n := n + 1;
        end;
      end;
    for j := 0 to b^.insts.count - 1 do
    begin
      i := PIRInst(listGet(b^.insts, j));
      if i^.defId > 0 then
      begin
        mapPut(defNumbers, i^.defId, boxInt(n));
        n := n + 1;
      end;
    end;
  end;
end;

procedure emitInst(var ob: TBuf; i: PIRInst);
var
  k: longint;
begin
  for k := 0 to i^.records.count - 1 do
    emitStr(ob, '    #dbg_declare(' + PDbgRecord(listGet(i^.records, k))^.text + ')' + #10);
  emitStr(ob, '  ');
  emitBuf(ob, i^.text);
  if i^.isSwitch then
    emitStr(ob, #10 + '  ]');
  if i^.dbgLoc <> nil then
    emitStr(ob, ', !dbg ' + irMetaRef(i^.dbgLoc));
  if i^.unlikely then
    emitStr(ob, ', !prof ' + irMetaRef(branchWeights));
  emitStr(ob, #10);
end;

{ SlotTracker::CreateMetadataSlot: a pre-order walk of the operands }
var
  metaNext: longint;

procedure numberMeta(m: PMeta);
var
  i: longint;
begin
  if (m = nil) or (m^.num >= 0) then
    exit;
  m^.num := metaNext;
  metaNext := metaNext + 1;
  for i := 0 to m^.ops.count - 1 do
    numberMeta(PMeta(listGet(m^.ops, i)));
end;

procedure numberInstMeta(i: PIRInst);
var
  k: longint;
  r: PDbgRecord;
begin
  for k := 0 to i^.records.count - 1 do
  begin
    r := PDbgRecord(listGet(i^.records, k));
    numberMeta(r^.variable);
    numberMeta(r^.loc);
  end;
  numberMeta(i^.dbgLoc);
  if i^.unlikely then
    numberMeta(branchWeights);
end;

procedure numberAllMeta;
var
  i, k, j: longint;
  f: PIRFunc;
  b: PIRBlock;
  l: PPtrList;
begin
  metaNext := 0;
  for i := 0 to globals.count - 1 do
    numberMeta(PGlobal(listGet(globals, i))^.dbg);
  i := 0;
  while i < namedMetas.count do
  begin
    l := PPtrList(listGet(namedMetas, i + 1));
    for k := 0 to l^.count - 1 do
      numberMeta(PMeta(listGet(l^, k)));
    i := i + 2;
  end;
  for i := 0 to funcs.count - 1 do
  begin
    f := PIRFunc(listGet(funcs, i));
    numberMeta(f^.dbg);
    for k := 0 to f^.blocks.count - 1 do
    begin
      b := PIRBlock(listGet(f^.blocks, k));
      if k = 0 then
        for j := f^.entryTemps.count - 1 downto 0 do
          numberInstMeta(PIRInst(listGet(f^.entryTemps, j)));
      for j := 0 to b^.insts.count - 1 do
        numberInstMeta(PIRInst(listGet(b^.insts, j)));
    end;
  end;
end;

{ the number of characters on the last line of out }
function column(const ob: TBuf): longint;
var
  i: int64;
begin
  i := ob.len;
  while (i > 0) and (bufCharAt(ob, i - 1) <> #10) do
    i := i - 1;
  column := longint(ob.len - i);
end;

procedure emitFunction(var ob: TBuf; f: PIRFunc; attrNum: longint);
var
  k, j, col: longint;
  b: PIRBlock;
begin
  if f^.attrs <> '' then
    emitStr(ob, '; Function Attrs: ' + f^.attrs + #10);
  if f^.isDecl then
  begin
    emitStr(ob, 'declare ' + f^.ret^.text + ' ' + f^.name + '(' + paramList(f, false) + ')');
    if attrNum >= 0 then
      emitStr(ob, ' #' + intToStr(attrNum));
    emitStr(ob, #10);
    exit;
  end;
  numberDefs(f);
  emitStr(ob, 'define ');
  if f^.linkage <> '' then
    emitStr(ob, f^.linkage + ' ');
  emitStr(ob, f^.ret^.text + ' ' + f^.name + '(' + paramList(f, true) + ')');
  if attrNum >= 0 then
    emitStr(ob, ' #' + intToStr(attrNum));
  if f^.dbg <> nil then
    emitStr(ob, ' !dbg ' + irMetaRef(f^.dbg));
  emitStr(ob, ' {');
  for k := 0 to f^.blocks.count - 1 do
  begin
    b := PIRBlock(listGet(f^.blocks, k));
    emitStr(ob, #10 + quotedName(b^.name) + ':');
    if k > 0 then
    begin
      if not scanning then
      begin
        col := column(ob);
        if col < 50 then
          emitStr(ob, copy('                                                  ', 1, 50 - col))
        else
          emitStr(ob, ' ');
      end;
      emitStr(ob, ';');
      if b^.preds.count = 0 then
        emitStr(ob, ' No predecessors!')
      else
      begin
        emitStr(ob, ' preds = ');
        for j := b^.preds.count - 1 downto 0 do
        begin
          emitStr(ob, '%' + quotedName(PIRBlock(listGet(b^.preds, j))^.name));
          if j > 0 then
            emitStr(ob, ', ');
        end;
      end;
    end;
    emitStr(ob, #10);
    if k = 0 then
      for j := f^.entryTemps.count - 1 downto 0 do
        emitInst(ob, PIRInst(listGet(f^.entryTemps, j)));
    for j := 0 to b^.insts.count - 1 do
      emitInst(ob, PIRInst(listGet(b^.insts, j)));
  end;
  emitStr(ob, '}' + #10);
end;

procedure escaped(var ob: TBuf; const s: string);
var
  i: longint;
  c: char;
begin
  for i := 1 to length(s) do
  begin
    c := s[i];
    if c = '\' then
      bufAdd(ob, '\\')
    else if (c >= ' ') and (c <= '~') and (c <> '"') then
      bufAddChar(ob, c)
    else
    begin
      bufAddChar(ob, '\');
      bufAddChar(ob, hexDigit(ord(c) shr 4));
      bufAddChar(ob, hexDigit(ord(c) and 15));
    end;
  end;
end;

procedure irWriteModule(var outText: TBuf);
var
  attrNames: TPtrList;   { of PName: the attribute groups, numbered }
  scratch: TBuf;
  i, k, n, pass: longint;
  g: PGlobal;
  f: PIRFunc;
  t: PIRType;
  found: boolean;
  fnAttr: TIntMap;       { function index -> group number + 1 }
  l: PPtrList;
  m: PMeta;
  byNum: TPtrList;
begin
  listInit(typeOrder);
  mapInit(typeSeen);
  mapInit(globalNumbers);
  mapInit(fnAttr);
  listInit(attrNames);
  { unnamed globals are numbered in order }
  n := 0;
  for i := 0 to globals.count - 1 do
  begin
    g := PGlobal(listGet(globals, i));
    if g^.name = '' then
    begin
      mapPut(globalNumbers, g^.id, boxInt(n));
      n := n + 1;
    end;
  end;
  { attribute groups, in function order }
  for i := 0 to funcs.count - 1 do
  begin
    f := PIRFunc(listGet(funcs, i));
    if f^.attrs = '' then
      continue;
    found := false;
    for k := 0 to attrNames.count - 1 do
      if nameStr(PName(listGet(attrNames, k))) = f^.attrs then
      begin
        mapPut(fnAttr, i, boxInt(k));
        found := true;
      end;
    if not found then
    begin
      listAdd(attrNames, intern(f^.attrs));
      mapPut(fnAttr, i, boxInt(attrNames.count - 1));
    end;
  end;
  numberAllMeta;
  { pass 1 finds the named types in TypeFinder's order; pass 2 prints }
  bufInit(scratch);
  for pass := 1 to 2 do
  begin
    scanning := pass = 1;
    if not scanning then
    begin
      bufAdd(outText, '; ModuleID = ''' + modSource + '''' + #10);
      bufAdd(outText, 'source_filename = "');
      escaped(outText, modSource);
      bufAdd(outText, '"' + #10);
      if modLayout <> '' then
        bufAdd(outText, 'target datalayout = "' + modLayout + '"' + #10);
      if modTriple <> '' then
        bufAdd(outText, 'target triple = "' + modTriple + '"' + #10);
      if typeOrder.count > 0 then
      begin
        bufAddChar(outText, #10);
        for i := 0 to typeOrder.count - 1 do
        begin
          t := PIRType(listGet(typeOrder, i));
          bufAdd(outText, '%' + quotedName(t^.name) + ' = type ');
          emitStr(outText, fieldsText(t^.fields));
          bufAddChar(outText, #10);
        end;
      end;
    end;
    if globals.count > 0 then
      emitStr(outText, #10);
    for i := 0 to globals.count - 1 do
    begin
      g := PGlobal(listGet(globals, i));
      if g^.name = '' then
        emitStr(outText, '@' + #3 + intToStr(g^.id) + #3 + ' = ')
      else
        emitStr(outText, '@' + quotedName(g^.name) + ' = ');
      emitBuf(outText, g^.text);
      if g^.dbg <> nil then
        emitStr(outText, ', !dbg ' + irMetaRef(g^.dbg));
      emitStr(outText, #10);
    end;
    for i := 0 to funcs.count - 1 do
    begin
      f := PIRFunc(listGet(funcs, i));
      emitStr(outText, #10);
      emitFunction(outText, f, unboxInt(mapGet(fnAttr, i)));
    end;
  end;
  if attrNames.count > 0 then
  begin
    bufAddChar(outText, #10);
    for i := 0 to attrNames.count - 1 do
      bufAdd(outText, 'attributes #' + intToStr(i) + ' = { ' +
                      nameStr(PName(listGet(attrNames, i))) + ' }' + #10);
  end;
  { named metadata, then the nodes by number }
  scanning := false;
  if namedMetas.count > 0 then
  begin
    bufAddChar(outText, #10);
    i := 0;
    while i < namedMetas.count do
    begin
      bufAdd(outText, '!' + nameStr(PName(listGet(namedMetas, i))) + ' = !{');
      l := PPtrList(listGet(namedMetas, i + 1));
      for k := 0 to l^.count - 1 do
      begin
        if k > 0 then
          bufAdd(outText, ', ');
        emitStr(outText, irMetaRef(PMeta(listGet(l^, k))));
      end;
      bufAdd(outText, '}' + #10);
      i := i + 2;
    end;
  end;
  if metaNext > 0 then
  begin
    bufAddChar(outText, #10);
    listInit(byNum);
    for n := 0 to metaNext - 1 do
      listAdd(byNum, nil);
    for i := 0 to metas.count - 1 do
    begin
      m := PMeta(listGet(metas, i));
      if m^.num >= 0 then
        listSet(byNum, m^.num, m);
    end;
    for n := 0 to metaNext - 1 do
    begin
      m := PMeta(listGet(byNum, n));
      bufAdd(outText, '!' + intToStr(n) + ' = ');
      emitBuf(outText, m^.text);
      bufAddChar(outText, #10);
    end;
  end;
  bufFree(scratch);
end;

end.
