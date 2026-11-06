unit pcir;
{ A small builder for textual LLVM IR (SELFHOST.md decision D2: the
  self-hosted compiler writes .ll text and lets clang compile it).

  Types: TIRType with LLVM's layout for 64-bit targets (x86-64, AArch64):
  i1/i8 1 byte, i16 2, i32 4, i64 8, i128 and wider aligned to 16, double
  and ptr 8; arrays and structs as LLVM lays them out. Named structs are
  emitted as type definitions.

  Values: TV, a type and the text of the value ('%t12', '42', 'null',
  '@pas.x'). Instructions go to the current function's body; allocas to
  its entry block. Pointers are opaque (ptr). }

interface

uses pcsys, pcutil;

type
  TIRKind = (ikVoid, ikInt, ikDouble, ikPtr, ikArray, ikStruct);

  TInt64Array = array[0..$FFFFFF] of int64;
  PInt64Array = ^TInt64Array;

  PIRType = ^TIRType;
  TIRType = record
    kind: TIRKind;
    bits: longint;      { ikInt }
    count: int64;       { ikArray }
    elem: PIRType;      { ikArray }
    fields: TPtrList;   { ikStruct: of PIRType }
    offsets: PInt64Array; { ikStruct: byte offset of each field }
    text: string;       { 'i32', '[4 x i8]', a struct name or a literal struct }
    size, align: int64; { alloc size and ABI alignment }
  end;

  TV = record
    t: PIRType;
    v: string;
  end;

var
  irVoidT, irI1, irI8, irI16, irI32, irI64, irI256, irDoubleT, irPtrT: PIRType;

procedure irInit(const sourceName: string);

{ Types }
function irInt(bits: longint): PIRType;
function irArray(n: int64; elem: PIRType): PIRType;
{ A literal (unnamed) struct of the fields. }
function irStructLit(const fields: TPtrList): PIRType;
{ A named struct, emitted as a type definition (name made unique). }
function irStructNamed(const name: string; const fields: TPtrList): PIRType;
function irFieldOffset(t: PIRType; i: longint): int64;
function irSameType(a, b: PIRType): boolean;

{ Values }
function irV(t: PIRType; const v: string): TV;
function irConstInt(t: PIRType; v: int64): TV;
function irConstUInt(t: PIRType; v: int64): TV;    { v's bits as unsigned }
function irConstBool(b: boolean): TV;
function irConstDouble(x: double): TV;
function irNull: TV;
function irZero(t: PIRType): TV;
function irUndef(t: PIRType): TV;
{ Is v an integer constant (and its value)? }
function irIsConstInt(const v: TV; var value: int64): boolean;

{ Module }
{ A private constant holding bytes (no NUL added); returns '@name'. }
function irStringConstant(p: PCharArray; start, count: int64; nulTerminated: boolean): string;
procedure irGlobal(const name, linkage: string; t: PIRType; const init: string;
                   isConstant: boolean; align: longint);
{ the same with an initializer of any length }
procedure irGlobalBuf(const name, linkage: string; t: PIRType; const init: TBuf;
                      isConstant: boolean; align: longint);
{ c"..." for bytes (a [N x i8] constant) }
procedure irAddCString(var b: TBuf; p: PCharArray; start, count: int64);
{ An external function, declared once. }
procedure irDeclare(const name, decl: string);
function irIsDeclared(const name: string): boolean;

{ Functions: the header ("define internal i32 @f(ptr %link, i32 %x)") is
  given in a buffer, as parameter lists can be long. }
procedure irBeginFunction(const header: TBuf);
procedure irEndFunction;
function irNewLabel(const prefix: string): string;
procedure irStartBlock(const lbl: string);
function irCurrentBlock: string;
function irTerminated: boolean;
procedure irEmit(const s: string);
procedure irEmitBuf(const b: TBuf);
{ a terminator of any length (switch) }
procedure irEmitTermBuf(const b: TBuf);
function irTmp: string;
{ alloca in the entry block }
function irAlloca(t: PIRType; const hint: string): TV;

{ Instructions (return the result) }
function irBin(const op: string; const a, b: TV): TV;
{ the same with a result name taken earlier from irTmp (for phis) }
procedure irBinNamed(const res, op: string; const a, b: TV);
function irICmp(const pred: string; const a, b: TV): TV;
function irFCmp(const pred: string; const a, b: TV): TV;
function irCast(const op: string; const v: TV; dst: PIRType): TV;
function irLoad(t: PIRType; const p: TV): TV;
procedure irStore(const v, p: TV);
{ getelementptr inbounds i8, p, offset }
function irByteGEP(const p: TV; offset: int64): TV;
function irByteGEPv(const p, offset: TV): TV;
{ getelementptr inbounds T, p, i64 0, <idx> (arrays: idx is i64) }
function irArrayGEP(arr: PIRType; const p, idx: TV): TV;
function irElemGEP(elem: PIRType; const p, idx: TV): TV;
function irStructGEP(st: PIRType; const p: TV; field: longint): TV;
function irSelect(const c, a, b: TV): TV;
function irExtract(const agg: TV; idx: longint; t: PIRType): TV;
function irInsert(const agg, v: TV; idx: longint): TV;
{ Calls: arguments are collected in a buffer ("ty v, ty v"). }
procedure irArgsStart(var args: TBuf);
procedure irArgsAdd(var args: TBuf; const v: TV);
{ returns an ikVoid value for a void call }
function irCall(ret: PIRType; const callee: string; const args: TBuf): TV;
procedure irBr(const lbl: string);
procedure irCondBr(const c: TV; const t, f: string);
procedure irRet(const v: TV);
procedure irRetVoid;
procedure irUnreachable;
{ phi with two incoming values }
function irPhi2(t: PIRType; const v1, b1, v2, b2: string): TV;

{ The finished module text. }
procedure irWriteModule(var outText: TBuf);

implementation

var
  intTypes: TIntMap;     { bits -> PIRType }
  typeDefs: TBuf;        { the named struct definitions }
  globals: TBuf;
  funcs: TBuf;
  decls: TBuf;
  declared: TIntMap;     { name id -> non-nil once declared }
  namedCount: TIntMap;   { struct name id -> count }
  stringCount: longint;
  header: string;
  { the current function }
  fnHeader: TBuf;
  fnAllocas, fnBody: TBuf;
  tmpCount, labelCount: longint;
  curBlock: string;
  terminated: boolean;

function alignTo(n, a: int64): int64;
begin
  alignTo := (n + a - 1) div a * a;
end;

function newIRType(kind: TIRKind): PIRType;
var
  t: PIRType;
begin
  t := PIRType(arenaAlloc(sizeof(TIRType)));
  t^.kind := kind;
  listInit(t^.fields);
  newIRType := t;
end;

function irInt(bits: longint): PIRType;
var
  t: PIRType;
  bytes: int64;
begin
  t := PIRType(mapGet(intTypes, bits));
  if t <> nil then
    exit(t);
  t := newIRType(ikInt);
  t^.bits := bits;
  t^.text := 'i' + intToStr(bits);
  bytes := (bits + 7) div 8;
  { alloc size: the store size rounded up to the alignment }
  if bits <= 8 then
    t^.align := 1
  else if bits <= 16 then
    t^.align := 2
  else if bits <= 32 then
    t^.align := 4
  else if bits <= 64 then
    t^.align := 8
  else
    t^.align := 16;
  t^.size := alignTo(bytes, t^.align);
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

{ name as LLVM writes it: plain if it is a valid identifier, else quoted }
function quotedName(const name: string): string;
var
  i: longint;
  c: char;
  plain: boolean;
begin
  plain := (name <> '') and not ((name[1] >= '0') and (name[1] <= '9'));
  for i := 1 to length(name) do
  begin
    c := name[i];
    if not (((c >= 'a') and (c <= 'z')) or ((c >= 'A') and (c <= 'Z')) or
            ((c >= '0') and (c <= '9')) or (c = '.') or (c = '_') or (c = '$') or
            (c = '-')) then
      plain := false;
  end;
  if plain then
    quotedName := name
  else
    quotedName := '"' + name + '"';
end;

function irStructNamed(const name: string; const fields: TPtrList): PIRType;
var
  t: PIRType;
  i, n: longint;
  key: PName;
  counter: PIRType;
begin
  t := newIRType(ikStruct);
  for i := 0 to fields.count - 1 do
    listAdd(t^.fields, listGet(fields, i));
  key := intern(name);
  { the count is kept as an IR type's size (any non-nil pointer works) }
  counter := PIRType(mapGet(namedCount, key^.id));
  if counter = nil then
  begin
    counter := newIRType(ikVoid);
    counter^.size := 0;
    mapPut(namedCount, key^.id, counter);
  end;
  counter^.size := counter^.size + 1;
  n := longint(counter^.size);
  if n = 1 then
    t^.text := '%' + quotedName(name)
  else
    t^.text := '%' + quotedName(name + '.' + intToStr(n - 1));
  layoutStruct(t);
  bufAdd(typeDefs, t^.text + ' = type {');
  for i := 0 to t^.fields.count - 1 do
  begin
    if i > 0 then
      bufAddChar(typeDefs, ',');
    bufAdd(typeDefs, ' ' + PIRType(listGet(t^.fields, i))^.text);
  end;
  bufAdd(typeDefs, ' }' + #10);
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

{ --- Values --- }

function irV(t: PIRType; const v: string): TV;
var
  r: TV;
begin
  r.t := t;
  r.v := v;
  irV := r;
end;

function irConstInt(t: PIRType; v: int64): TV;
begin
  if t^.bits = 1 then
  begin
    if (v and 1) <> 0 then
      exit(irV(t, 'true'));
    exit(irV(t, 'false'));
  end;
  { the value of the low bits, as a signed number }
  if (t^.bits > 1) and (t^.bits < 64) then
  begin
    v := v and ((int64(1) shl t^.bits) - 1);
    if v >= (int64(1) shl (t^.bits - 1)) then
      v := v - (int64(1) shl t^.bits);
  end;
  irConstInt := irV(t, intToStr(v));
end;

function irConstUInt(t: PIRType; v: int64): TV;
begin
  irConstUInt := irV(t, uintToStr(v));
end;

function irConstBool(b: boolean): TV;
begin
  if b then
    irConstBool := irV(irI1, 'true')
  else
    irConstBool := irV(irI1, 'false');
end;

function hexDigit(v: longint): char;
begin
  if v < 10 then
    hexDigit := chr(ord('0') + v)
  else
    hexDigit := chr(ord('A') + v - 10);
end;

type
  TDoubleOverlay = record
    case boolean of
      true: (d: double);
      false: (bits: int64);
  end;

function irConstDouble(x: double): TV;
var
  o: TDoubleOverlay;
  s: string;
  i: longint;
  b: int64;
begin
  o.d := x;
  b := o.bits;
  s := '';
  for i := 1 to 16 do
  begin
    s := hexDigit(longint(b and 15)) + s;
    b := b shr 4;
  end;
  irConstDouble := irV(irDoubleT, '0x' + s);
end;

function irNull: TV;
begin
  irNull := irV(irPtrT, 'null');
end;

function irZero(t: PIRType): TV;
begin
  case t^.kind of
    ikInt:
      if t^.bits = 1 then
        irZero := irV(t, 'false')
      else
        irZero := irV(t, '0');
    ikDouble: irZero := irV(t, '0.0');
    ikPtr: irZero := irV(t, 'null');
  else
    irZero := irV(t, 'zeroinitializer');
  end;
end;

function irUndef(t: PIRType): TV;
begin
  irUndef := irV(t, 'undef');
end;

function irIsConstInt(const v: TV; var value: int64): boolean;
var
  i: longint;
  neg: boolean;
  s: string;
begin
  irIsConstInt := false;
  if v.t^.kind <> ikInt then
    exit;
  s := v.v;
  if s = 'true' then
  begin
    value := 1;
    exit(true);
  end;
  if s = 'false' then
  begin
    value := 0;
    exit(true);
  end;
  if (s = '') or (length(s) > 19) then
    exit;
  neg := s[1] = '-';
  i := 1;
  if neg then
    i := 2;
  if i > length(s) then
    exit;
  value := 0;
  while i <= length(s) do
  begin
    if (s[i] < '0') or (s[i] > '9') then
      exit;
    value := value * 10 + (ord(s[i]) - ord('0'));
    i := i + 1;
  end;
  if neg then
    value := -value;
  irIsConstInt := true;
end;

{ --- Module --- }

procedure irInit(const sourceName: string);
begin
  mapInit(intTypes);
  mapInit(declared);
  mapInit(namedCount);
  bufInit(typeDefs);
  bufInit(globals);
  bufInit(funcs);
  bufInit(decls);
  stringCount := 0;
  header := sourceName;
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
  irDoubleT^.align := 8;
  irPtrT := newIRType(ikPtr);
  irPtrT^.text := 'ptr';
  irPtrT^.size := 8;
  irPtrT^.align := 8;
  bufInit(fnAllocas);
  bufInit(fnBody);
  bufInit(fnHeader);
end;

procedure addCString(var b: TBuf; p: PCharArray; start, count: int64; nul: boolean);
var
  i: int64;
  c: char;
begin
  bufAdd(b, 'c"');
  {$R-}
  for i := start to start + count - 1 do
  begin
    c := p^[i];
    if (c >= ' ') and (c <= '~') and (c <> '"') and (c <> '\') then
      bufAddChar(b, c)
    else
    begin
      bufAddChar(b, '\');
      bufAddChar(b, hexDigit(ord(c) shr 4));
      bufAddChar(b, hexDigit(ord(c) and 15));
    end;
  end;
  {$R+}
  if nul then
    bufAdd(b, '\00');
  bufAddChar(b, '"');
end;

function irStringConstant(p: PCharArray; start, count: int64; nulTerminated: boolean): string;
var
  name: string;
  n: int64;
begin
  stringCount := stringCount + 1;
  name := '@.str.' + intToStr(stringCount);
  n := count;
  if nulTerminated then
    n := n + 1;
  bufAdd(globals, name + ' = private unnamed_addr constant [' + intToStr(n) + ' x i8] ');
  addCString(globals, p, start, count, nulTerminated);
  bufAdd(globals, ', align 1');
  bufAddChar(globals, #10);
  irStringConstant := name;
end;

procedure irGlobal(const name, linkage: string; t: PIRType; const init: string;
                   isConstant: boolean; align: longint);
begin
  bufAdd(globals, name + ' = ' + linkage);
  if isConstant then
    bufAdd(globals, ' constant ')
  else
    bufAdd(globals, ' global ');
  bufAdd(globals, t^.text + ' ' + init);
  if align > 0 then
    bufAdd(globals, ', align ' + intToStr(align));
  bufAddChar(globals, #10);
end;

procedure irGlobalBuf(const name, linkage: string; t: PIRType; const init: TBuf;
                      isConstant: boolean; align: longint);
begin
  bufAdd(globals, name + ' = ' + linkage);
  if isConstant then
    bufAdd(globals, ' constant ')
  else
    bufAdd(globals, ' global ');
  bufAdd(globals, t^.text + ' ');
  bufAddChars(globals, init.data, 0, init.len);
  if align > 0 then
    bufAdd(globals, ', align ' + intToStr(align));
  bufAddChar(globals, #10);
end;

procedure irAddCString(var b: TBuf; p: PCharArray; start, count: int64);
begin
  addCString(b, p, start, count, false);
end;

procedure irDeclare(const name, decl: string);
var
  key: PName;
begin
  key := intern(name);
  if mapHas(declared, key^.id) then
    exit;
  mapPut(declared, key^.id, key);
  bufAdd(decls, decl);
  bufAddChar(decls, #10);
end;

function irIsDeclared(const name: string): boolean;
begin
  irIsDeclared := mapHas(declared, intern(name)^.id);
end;

{ --- Functions --- }

procedure irBeginFunction(const header: TBuf);
begin
  bufClear(fnHeader);
  bufAddChars(fnHeader, header.data, 0, header.len);
  bufClear(fnAllocas);
  bufClear(fnBody);
  tmpCount := 0;
  labelCount := 0;
  curBlock := 'entry';
  terminated := false;
end;

procedure irEndFunction;
begin
  bufAddChars(funcs, fnHeader.data, 0, fnHeader.len);
  bufAdd(funcs, ' {');
  bufAdd(funcs, #10 + 'entry:' + #10);
  bufAddChars(funcs, fnAllocas.data, 0, fnAllocas.len);
  bufAddChars(funcs, fnBody.data, 0, fnBody.len);
  bufAdd(funcs, '}' + #10 + #10);
end;

function irNewLabel(const prefix: string): string;
begin
  labelCount := labelCount + 1;
  irNewLabel := prefix + '.' + intToStr(labelCount);
end;

procedure irStartBlock(const lbl: string);
begin
  if not terminated then
    { fall through: LLVM blocks need a terminator }
    bufAdd(fnBody, '  br label %' + lbl + #10);
  bufAdd(fnBody, lbl + ':' + #10);
  curBlock := lbl;
  terminated := false;
end;

function irCurrentBlock: string;
begin
  irCurrentBlock := curBlock;
end;

function irTerminated: boolean;
begin
  irTerminated := terminated;
end;

procedure irEmit(const s: string);
begin
  bufAdd(fnBody, '  ');
  bufAdd(fnBody, s);
  bufAddChar(fnBody, #10);
end;

procedure irEmitBuf(const b: TBuf);
begin
  bufAdd(fnBody, '  ');
  bufAddChars(fnBody, b.data, 0, b.len);
  bufAddChar(fnBody, #10);
end;

procedure irEmitTermBuf(const b: TBuf);
begin
  irEmitBuf(b);
  terminated := true;
end;

function irTmp: string;
begin
  tmpCount := tmpCount + 1;
  irTmp := '%t' + intToStr(tmpCount);
end;

function irAlloca(t: PIRType; const hint: string): TV;
var
  name: string;
begin
  tmpCount := tmpCount + 1;
  name := '%' + hint + '.' + intToStr(tmpCount);
  bufAdd(fnAllocas, '  ' + name + ' = alloca ' + t^.text + ', align ' +
         intToStr(t^.align) + #10);
  irAlloca := irV(irPtrT, name);
end;

{ --- Instructions --- }

function irBin(const op: string; const a, b: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = ' + op + ' ' + a.t^.text + ' ' + a.v + ', ' + b.v);
  irBin := irV(a.t, r);
end;

procedure irBinNamed(const res, op: string; const a, b: TV);
begin
  irEmit(res + ' = ' + op + ' ' + a.t^.text + ' ' + a.v + ', ' + b.v);
end;

function irICmp(const pred: string; const a, b: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = icmp ' + pred + ' ' + a.t^.text + ' ' + a.v + ', ' + b.v);
  irICmp := irV(irI1, r);
end;

function irFCmp(const pred: string; const a, b: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = fcmp ' + pred + ' double ' + a.v + ', ' + b.v);
  irFCmp := irV(irI1, r);
end;

function irCast(const op: string; const v: TV; dst: PIRType): TV;
var
  r: string;
begin
  if irSameType(v.t, dst) then
    exit(v);
  r := irTmp;
  irEmit(r + ' = ' + op + ' ' + v.t^.text + ' ' + v.v + ' to ' + dst^.text);
  irCast := irV(dst, r);
end;

function irLoad(t: PIRType; const p: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = load ' + t^.text + ', ptr ' + p.v + ', align 1');
  irLoad := irV(t, r);
end;

procedure irStore(const v, p: TV);
begin
  irEmit('store ' + v.t^.text + ' ' + v.v + ', ptr ' + p.v + ', align 1');
end;

function irByteGEP(const p: TV; offset: int64): TV;
var
  r: string;
begin
  if offset = 0 then
    exit(p);
  r := irTmp;
  irEmit(r + ' = getelementptr inbounds i8, ptr ' + p.v + ', i64 ' + intToStr(offset));
  irByteGEP := irV(irPtrT, r);
end;

function irByteGEPv(const p, offset: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = getelementptr inbounds i8, ptr ' + p.v + ', ' + offset.t^.text +
         ' ' + offset.v);
  irByteGEPv := irV(irPtrT, r);
end;

function irArrayGEP(arr: PIRType; const p, idx: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = getelementptr inbounds ' + arr^.text + ', ptr ' + p.v +
         ', i64 0, ' + idx.t^.text + ' ' + idx.v);
  irArrayGEP := irV(irPtrT, r);
end;

function irElemGEP(elem: PIRType; const p, idx: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = getelementptr inbounds ' + elem^.text + ', ptr ' + p.v + ', ' +
         idx.t^.text + ' ' + idx.v);
  irElemGEP := irV(irPtrT, r);
end;

function irStructGEP(st: PIRType; const p: TV; field: longint): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = getelementptr inbounds ' + st^.text + ', ptr ' + p.v +
         ', i32 0, i32 ' + intToStr(field));
  irStructGEP := irV(irPtrT, r);
end;

function irSelect(const c, a, b: TV): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = select i1 ' + c.v + ', ' + a.t^.text + ' ' + a.v + ', ' +
         b.t^.text + ' ' + b.v);
  irSelect := irV(a.t, r);
end;

function irExtract(const agg: TV; idx: longint; t: PIRType): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = extractvalue ' + agg.t^.text + ' ' + agg.v + ', ' + intToStr(idx));
  irExtract := irV(t, r);
end;

function irInsert(const agg, v: TV; idx: longint): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = insertvalue ' + agg.t^.text + ' ' + agg.v + ', ' + v.t^.text +
         ' ' + v.v + ', ' + intToStr(idx));
  irInsert := irV(agg.t, r);
end;

procedure irArgsStart(var args: TBuf);
begin
  bufInit(args);
end;

procedure irArgsAdd(var args: TBuf; const v: TV);
begin
  if args.len > 0 then
    bufAdd(args, ', ');
  bufAdd(args, v.t^.text + ' ' + v.v);
end;

function irCall(ret: PIRType; const callee: string; const args: TBuf): TV;
var
  r: string;
  line: TBuf;
begin
  bufInit(line);
  r := '';
  if ret^.kind <> ikVoid then
  begin
    r := irTmp;
    bufAdd(line, r + ' = ');
  end;
  bufAdd(line, 'call ' + ret^.text + ' ' + callee + '(');
  bufAddChars(line, args.data, 0, args.len);
  bufAddChar(line, ')');
  irEmitBuf(line);
  bufFree(line);
  irCall := irV(ret, r);
end;

procedure irBr(const lbl: string);
begin
  irEmit('br label %' + lbl);
  terminated := true;
end;

procedure irCondBr(const c: TV; const t, f: string);
begin
  irEmit('br i1 ' + c.v + ', label %' + t + ', label %' + f);
  terminated := true;
end;

procedure irRet(const v: TV);
begin
  irEmit('ret ' + v.t^.text + ' ' + v.v);
  terminated := true;
end;

procedure irRetVoid;
begin
  irEmit('ret void');
  terminated := true;
end;

procedure irUnreachable;
begin
  irEmit('unreachable');
  terminated := true;
end;

function irPhi2(t: PIRType; const v1, b1, v2, b2: string): TV;
var
  r: string;
begin
  r := irTmp;
  irEmit(r + ' = phi ' + t^.text + ' [ ' + v1 + ', %' + b1 + ' ], [ ' + v2 +
         ', %' + b2 + ' ]');
  irPhi2 := irV(t, r);
end;

procedure irWriteModule(var outText: TBuf);
begin
  bufAdd(outText, '; ModuleID = ''' + header + '''' + #10);
  bufAdd(outText, 'source_filename = "' + header + '"' + #10 + #10);
  bufAddChars(outText, typeDefs.data, 0, typeDefs.len);
  bufAddChar(outText, #10);
  bufAddChars(outText, globals.data, 0, globals.len);
  bufAddChar(outText, #10);
  bufAddChars(outText, funcs.data, 0, funcs.len);
  bufAddChars(outText, decls.data, 0, decls.len);
end;

end.
