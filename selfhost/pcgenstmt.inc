{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
{ pcgen, continued: statements, declarations and the program. Included at
  the end of pcgen's implementation. }

{ --- Read and write --- }

{ Splits off a leading file argument of read/write: returns the PasFile
  and the index of the first value argument; ft: the file type, or nil. }
function fileTarget(const args: TPtrList; input: boolean; var ft: PType;
                    var first: longint): TV;
var
  a: PNode;
begin
  ft := nil;
  first := 0;
  if args.count > 0 then
  begin
    a := PNode(listGet(args, 0));
    if (a^.typ <> nil) and (a^.typ^.kind = tyFile) then
    begin
      ft := a^.typ;
      first := 1;
      exit(loadFile(emitAddress(a)));
    end;
  end;
  if input then
    fileTarget := inputFile
  else
    fileTarget := outputFile;
end;

{ The address of a value convertible to type t: the designator itself if
  it has type t, else a temporary holding the converted value. }
function addressForValue(e: PNode; t: PType): TV;
var
  tmp, v: TV;
begin
  if (e^.typ = t) and (isStructured(t) or isStringType(t)) then
    exit(emitAddress(e));
  if isStructured(t) then
  begin
    tmp := irCreateTemp(memType(t), 'tmp');
    copyAggregate(tmp, emitAddress(e), t);
    exit(tmp);
  end;
  tmp := irCreateTemp(memType(t), 'tmp');
  v := convertForAssign(visitExpr(e), e^.typ, t, e^.b);
  storeScalar(tmp, v, t);
  addressForValue := tmp;
end;

{ The names of enumeration e's values as length-prefixed strings: a private
  constant [N x ptr], one per enumeration type. }
function enumNameTable(e: PType): TV;
var
  g: PTVBox;
  names: TPtrList;
  i: longint;
  n: PName;
begin
  g := PTVBox(mapGet(enumNameTables, e^.id));
  if g = nil then
  begin
    listInit(names);
    for i := 0 to e^.enumValues.count - 1 do
    begin
      n := PDecl(listGet(e^.enumValues, i))^.ident.spelling;
      listAdd(names, irBox(getPStrConstant(n^.chars, 0, n^.len)));
    end;
    g := irBox(irGlobal('.enumnames', 'private', irArray(names.count, irPtrT),
                        irConstArray(irArray(names.count, irPtrT), names),
                        true, true, false, 0));
    listFree(names);
    mapPut(enumNameTables, e^.id, g);
  end;
  enumNameTable := g^;
end;

{ write(e) of an enumeration's value (Turbo/FPC): its name, as declared,
  padded on the right to the field width. A value outside the type is a
  runtime error (always, as in Free Pascal). }
procedure emitWriteEnum(const f, v: TV; t, e: PType; const width: TV; loc: TLoc);
var
  x, bad, table, ignored: TV;
begin
  x := toI64(v, t);
  bad := irICmp('uge', x, i64c(e^.enumValues.count), '');
  emitRuntimeCheck(bad, 'invalid enumeration value', loc);
  table := enumNameTable(e);
  ignored := call4(classRuntime('__pas_write_enum', 'void', 'ptr,ptr,i64,i32'),
                   f, table, x, width, '');
end;

{ An expression that cannot call Pascal code: its evaluation cannot raise
  an exception, so it is safe to format it into the write buffer. Runtime
  calls (bounds checks, string concatenation) are fine: the end of the
  statement flushes the buffer, or the next statement discards it. }
function simpleWriteExpr(e: PNode): boolean;
begin
  simpleWriteExpr := false;
  if e = nil then
    exit;
  case e^.kind of
    nkParen:
      exit(simpleWriteExpr(e^.inner));
    nkIntegerLit, nkRealLit, nkStringLit:
      exit(true);
    nkNameRef:
      exit((e^.refWith = nil) and (e^.refDecl <> nil) and
           (e^.refDecl^.kind in [dkVar, dkConst]));
    nkUnary:
      if e^.uopDecl = nil then
        exit(simpleWriteExpr(e^.operand));
    nkBinary:
      if (e^.opDecl = nil) and simpleWriteExpr(e^.lhs) and
         simpleWriteExpr(e^.rhs) then
        exit(true);
    nkIndex:
      if (e^.idxProp = nil) and simpleWriteExpr(e^.base) and
         simpleWriteExpr(e^.index) then
        exit(true);
    nkField:
      if (e^.fieldDecl <> nil) and (e^.fieldDecl^.kind = dkField) then
        exit(simpleWriteExpr(e^.base));
  end;
end;

function simpleWriteArg(arg: PNode): boolean;
begin
  simpleWriteArg := false;
  if arg^.kind = nkFormatted then
  begin
    if not simpleWriteExpr(arg^.fmtValue) then
      exit;
    if (arg^.fmtWidth <> nil) and not simpleWriteExpr(arg^.fmtWidth) then
      exit;
    if (arg^.fmtPrecision <> nil) and
       not simpleWriteExpr(arg^.fmtPrecision) then
      exit;
    exit(true);
  end;
  simpleWriteArg := simpleWriteExpr(arg);
end;

procedure emitWriteArg(const f: TV; arg: PNode);
var
  value: PNode;
  width, prec, v, s, b, ignored: TV;
  t: PType;
begin
  value := arg;
  width := i32c(0); { no field width }
  prec := i32c(-1);
  if arg^.kind = nkFormatted then
  begin
    value := arg^.fmtValue;
    width := emitI32(arg^.fmtWidth);
    if arg^.fmtPrecision <> nil then
      prec := emitI32(arg^.fmtPrecision);
  end;
  t := value^.typ;
  if t^.kind = tyAnsiString then
  begin
    s := visitExpr(value);
    astrV3('__pas_write_astr', 'ptr,ptr,i32', f, s, width);
    exit;
  end;
  if t^.kind = tyPointer then { a pchar: the C string it points to }
  begin
    s := visitExpr(value);
    astrV3('__pas_write_cstr', 'ptr,ptr,i32', f, s, width);
    exit;
  end;
  if t^.kind = tyShortString then
  begin
    s := emitPStr(value);
    rtv3('__pas_write_pstr', f, s, width);
    exit;
  end;
  if isStringType(t) then
  begin
    s := emitAddress(value);
    rtv4('__pas_write_str', f, s, i64c(getStringLength(t)), width);
    exit;
  end;
  v := visitExpr(value);
  if t^.kind = tyReal then
    rtv4('__pas_write_real', f, v, width, prec)
  else if isCharBase(t) then
    rtv3('__pas_write_char', f, v, width)
  else if getBase(t)^.kind = tyWideChar then
    rtv3('__pas_write_char', f, wideToChar(v), width)
  else if isBoolBase(t) then
  begin
    b := irCast('zext', v, irI32, '');
    rtv3('__pas_write_bool', f, b, width);
  end
  else if getBase(t)^.kind = tyEnum then
    emitWriteEnum(f, v, t, getBase(t), width, value^.b)
  else if isQWord(getBase(t)) then
    ignored := call3(classRuntime('__pas_write_uint', 'void', 'ptr,i64,i32'),
                     f, v, width, '')
  else
    rtv3('__pas_write_int', f, toI64(v, t), width);
end;

{ Appends a compile-time constant write argument to a merged literal. Only
  ordinals: a real would need the runtime's exact formatting. }
function appendWriteConst(arg: PNode; var text: TBuf): boolean;
begin
  appendWriteConst := false;
  if arg^.kind = nkStringLit then
  begin
    if arg^.strWide <> 0 then
      exit;
    bufAddChars(text, arg^.strValue^.chars, 0, arg^.strValue^.len);
    exit(true);
  end;
  if arg^.kind = nkIntegerLit then
  begin
    if (arg^.typ = nil) or not isIntegerLike(arg^.typ) then
      exit;
    if isQWord(arg^.typ) then
      bufAdd(text, uintToStr(arg^.intValue))
    else
      bufAdd(text, intToStr(arg^.intValue));
    exit(true);
  end;
end;

procedure emitWrite(s: PNode; isWriteln: boolean);
var
  ft: PType;
  f, a: TV;
  first, i, j: longint;
  arg: PNode;
  text: TBuf;
  last, batch, newlineWritten: boolean;
begin
  f := fileTarget(s^.args, false, ft, first);
  if (ft <> nil) and not ft^.isText then
  begin
    for i := first to s^.args.count - 1 do
    begin
      a := addressForValue(PNode(listGet(s^.args, i)), ft^.fileElement);
      rtv2('__pas_write_elem', f, a);
    end;
    exit;
  end;
  batch := true;
  for i := first to s^.args.count - 1 do
    if not simpleWriteArg(PNode(listGet(s^.args, i))) then
    begin
      batch := false;
      break;
    end;
  if batch then
    rtv1('__pas_write_begin', f)
  else
    rtv1('__pas_write_discard', f);
  { adjacent string, character and ordinal literals become one write; a
    trailing literal of writeln gets the newline (items 19 and 2) }
  newlineWritten := false;
  i := first;
  while i < s^.args.count do
  begin
    bufInit(text);
    j := i;
    while j < s^.args.count do
    begin
      arg := PNode(listGet(s^.args, j));
      if not appendWriteConst(arg, text) then
        break;
      j := j + 1;
    end;
    if j > i then
    begin
      last := j = s^.args.count;
      if last and isWriteln then
        bufAddChar(text, #10);
      a := globalString(text.data, 0, text.len, '.str');
      rtv4('__pas_write_str', f, a, i64c(text.len), i32c(0));
      bufFree(text);
      if last and isWriteln then
      begin
        newlineWritten := true;
        break;
      end;
      i := j;
      continue;
    end;
    bufFree(text);
    emitWriteArg(f, PNode(listGet(s^.args, i)));
    i := i + 1;
  end;
  if isWriteln and not newlineWritten then
    rtv1('__pas_writeln', f);
  if batch then
    rtv1('__pas_write_end', f);
end;

procedure emitReadArg(const f: TV; arg: PNode);
var
  t: PType;
  addr, v, x, bad: TV;
  wide: boolean;
  tb: TOrdinalBounds;
  minV, maxV: int64;
begin
  t := arg^.typ;
  addr := emitAddress(arg);
  if t^.kind = tyAnsiString then
  begin
    astrV2('__pas_read_astr', 'ptr,ptr', f, addr);
    exit;
  end;
  if t^.kind = tyShortString then
  begin
    rtv3('__pas_read_str', f, addr, i32c(t^.maxLength));
    exit;
  end;
  if t^.kind = tyReal then
  begin
    irStore(call1(rtFn('__pas_read_real'), f, ''), addr);
    exit;
  end;
  if isCharBase(t) then
  begin
    v := call1(rtFn('__pas_read_char'), f, '');
    emitRangeCheck(v, getBase(t), t, arg^.b);
    irStore(v, addr);
    exit;
  end;
  if isQWord(t) then { all of 0 .. 2^64-1 }
  begin
    v := call1(classRuntime('__pas_read_uint64', 'i64', 'ptr'), f, '');
    irStore(v, addr);
    exit;
  end;
  { integers: read as i32 (or i64 if needed), then checked against the
    target type }
  wide := (intBits(t) = 64) or not isSignedOrdinal(t);
  if wide then
  begin
    v := call1(rtFn('__pas_read_int64'), f, '');
    minV := -9223372036854775807 - 1;
    maxV := 9223372036854775807;
  end
  else
  begin
    v := call1(rtFn('__pas_read_int'), f, '');
    minV := -2147483648;
    maxV := 2147483647;
  end;
  x := irCast('sext', v, irI64, '');
  tb := getOrdinalBounds(t);
  if rangeChecks(arg^.b) and ((tb.lo > minV) or (tb.hi < maxV)) then
  begin
    bad := outsideRange(x, tb.lo, tb.hi);
    emitRuntimeCheck(bad, 'value out of range', arg^.b);
  end;
  irStore(irCast('trunc', x, regType(t), ''), addr);
end;

procedure emitRead(s: PNode; isReadln: boolean);
var
  ft, elem: PType;
  f, tmp, v, addr: TV;
  first, i: longint;
  arg: PNode;
begin
  f := fileTarget(s^.args, true, ft, first);
  if (ft <> nil) and not ft^.isText then
  begin
    elem := ft^.fileElement;
    for i := first to s^.args.count - 1 do
    begin
      arg := PNode(listGet(s^.args, i));
      if arg^.typ = elem then
      begin
        rtv2('__pas_read_elem', f, emitAddress(arg));
        continue;
      end;
      tmp := irCreateTemp(memType(elem), 'tmp');
      rtv2('__pas_read_elem', f, tmp);
      v := convertForAssign(loadScalar(tmp, elem, ''), elem, arg^.typ, arg^.b);
      addr := emitAddress(arg);
      storeScalar(addr, v, arg^.typ);
    end;
    exit;
  end;
  for i := first to s^.args.count - 1 do
    emitReadArg(f, PNode(listGet(s^.args, i)));
  if isReadln then
    rtv1('__pas_readln', f);
end;

{ pack(a, i, z) / unpack(z, a, i): packed and unpacked arrays share a
  representation, so both are a bounds-checked memcpy. }
procedure emitPackUnpack(s: PNode; isPack: boolean);
var
  unpacked, index, packed_: PNode;
  ua, pa: PType;
  ub, pb: TOrdinalBounds;
  count, bytes: int64;
  i, bad, offset, base, elem, packedAddr: TV;
begin
  if isPack then
  begin
    unpacked := PNode(listGet(s^.args, 0));
    index := PNode(listGet(s^.args, 1));
    packed_ := PNode(listGet(s^.args, 2));
  end
  else
  begin
    packed_ := PNode(listGet(s^.args, 0));
    unpacked := PNode(listGet(s^.args, 1));
    index := PNode(listGet(s^.args, 2));
  end;
  ua := unpacked^.typ;
  pa := packed_^.typ;
  ub := getOrdinalBounds(ua^.indexType);
  pb := getOrdinalBounds(pa^.indexType);
  count := pb.hi - pb.lo + 1;
  i := toI64(visitExpr(index), index^.typ);
  if rangeChecks(index^.b) then
  begin
    bad := outsideRange(i, ub.lo, ub.hi - count + 1);
    if isPack then
      emitRuntimeCheck(bad, '''pack'' index out of range', index^.b)
    else
      emitRuntimeCheck(bad, '''unpack'' index out of range', index^.b);
  end;
  base := emitAddress(unpacked);
  offset := irBin('sub', i, i64c(ub.lo), '');
  elem := irGEP2(memType(ua), base, i64c(0), offset, '');
  packedAddr := emitAddress(packed_);
  bytes := count * sizeOf_(ua^.elementType);
  if isPack then
    emitMemCpy(packedAddr, elem, i64c(bytes))
  else
    emitMemCpy(elem, packedAddr, i64c(bytes));
end;

{ --- goto --- }

function labelBlock(l: PDecl): PIRBlock;
begin
  if l^.cgBlock = nil then
    l^.cgBlock := irNewBlock('label.' + nm(l^.ident.name));
  labelBlock := PIRBlock(l^.cgBlock);
end;

function hasNonLocalLabels(blk: PBlock): boolean;
var
  i: longint;
  d: PDecl;
begin
  hasNonLocalLabels := false;
  for i := 0 to blk^.decls.count - 1 do
  begin
    d := PDecl(listGet(blk^.decls, i));
    if (d^.kind = dkLabel) and d^.labelNonLocal then
      exit(true);
  end;
end;

{ At the entry of a block targeted by non-local gotos:
  switch (_setjmp(buf)) case k: goto label_k }
procedure emitSetJmpDispatch(blk: PBlock; const buf: TV);
var
  r: TV;
  start: PIRBlock;
  sw: PIRInst;
  i: longint;
  d: PDecl;
begin
  r := call1(rtFn(setJmpName), buf, 'jmp');
  start := irNewBlock('body');
  sw := irSwitch(r, start);
  for i := 0 to blk^.decls.count - 1 do
  begin
    d := PDecl(listGet(blk^.decls, i));
    if (d^.kind = dkLabel) and d^.labelNonLocal then
      irAddCase(sw, i32c(d^.labelIndex), labelBlock(d));
  end;
  irSetInsertPoint(start);
end;

{ --- Statements --- }

{ The innermost cleanup (a PCleanup), or nil. }
function topCleanup: pointer;
begin
  if cleanups.count = 0 then
    topCleanup := nil
  else
    topCleanup := listGet(cleanups, cleanups.count - 1);
end;

procedure pushLoop(continueB, breakB: PIRBlock);
begin
  listAdd(loopBlocks, continueB);
  listAdd(loopBlocks, breakB);
  listAdd(loopBlocks, topCleanup);
end;

procedure popLoop;
var
  ignored: pointer;
begin
  ignored := listPop(loopBlocks);
  ignored := listPop(loopBlocks);
  ignored := listPop(loopBlocks);
end;

{ Branches to target (exit, break, continue), leaving the cleanups inside
  stopAt (nil: all of them) on the way. }
procedure emitJump(target: PIRBlock; stopAt: pointer);
var
  i: longint;
  c: PCleanup;
  cont: PIRBlock;
begin
  i := cleanups.count;
  while (i > 0) and (listGet(cleanups, i - 1) <> stopAt) do
  begin
    c := PCleanup(listGet(cleanups, i - 1));
    excCall1('__pas_exc_pop', c^.frame);
    if c^.kind = clHandler then
      excCall1('__pas_free_object', c^.obj)
    else if c^.kind = clFinally then
    begin
      irStore(i32c(2 + c^.exits.count), c^.state);
      cont := irNewBlock('finally.cont');
      listAdd(c^.exits, cont);
      irBr(c^.finallyB);
      irSetInsertPoint(cont);
    end;
    i := i - 1;
  end;
  irBr(target);
end;

function newCleanup(kind: TCleanupKind; const frame: TV): PCleanup;
var
  c: PCleanup;
begin
  c := PCleanup(arenaAlloc(sizeof(TCleanup)));
  c^.kind := kind;
  c^.frame := frame;
  c^.obj := irNull;
  c^.state := irNull;
  c^.finallyB := nil;
  listInit(c^.exits);
  newCleanup := c;
end;

{ The property an assignment targets, or nil. }
function assignedProperty(target: PNode): PDecl;
begin
  assignedProperty := nil;
  if (target^.kind = nkNameRef) and (target^.refDecl^.kind = dkProperty) then
    assignedProperty := target^.refDecl
  else if (target^.kind = nkField) and (target^.fieldDecl <> nil) and
          (target^.fieldDecl^.kind = dkProperty) then
    assignedProperty := target^.fieldDecl
  else if target^.kind = nkIndex then
    assignedProperty := target^.idxProp;
end;

{ target := value for a property target: stores into its field or calls
  its write procedure. }
procedure emitPropertyWrite(p: PDecl; target, value: PNode);
var
  obj, addr, ignored: TV;
  c: PType;
  args, noIndexes: TPtrList;
begin
  if p^.propIsClass then { a class variable or a static method }
  begin
    if p^.writeDecl^.kind = dkRoutine then
    begin
      listInit(noIndexes);
      if target^.kind = nkIndex then
        accessorArgs(p, target^.idxArgs, value, args)
      else
        accessorArgs(p, noIndexes, value, args);
      ignored := emitCall(p^.writeDecl, args);
      exit;
    end;
    addr := PTVBox(p^.writeDecl^.cgValue)^;
  end
  else
  begin
    if target^.kind = nkIndex then
      obj := propertyObject(target, c)
    else if target^.kind = nkNameRef then
    begin
      obj := withBase(target^.refWith);
      c := target^.refWith^.rec^.typ;
    end
    else
    begin
      if target^.base^.typ^.kind = tyInterface then { its setter }
      begin
        listInit(noIndexes);
        accessorArgs(p, noIndexes, value, args);
        ignored := emitInterfaceCall(p^.writeDecl, target^.base, target^.fieldName.loc,
                                     args);
        exit;
      end;
      obj := memberObject(target^.base);
      c := target^.base^.typ;
    end;
    if p^.writeDecl^.kind = dkRoutine then
    begin
      listInit(noIndexes);
      if target^.kind = nkIndex then
        accessorArgs(p, target^.idxArgs, value, args)
      else
        accessorArgs(p, noIndexes, value, args);
      ignored := emitMethodCall(p^.writeDecl, obj, args, false, target^.b);
      listFree(args);
      exit;
    end;
    addr := fieldAddress(obj, c, p^.writeDecl);
  end;
  storeExpr(addr, p^.propType, value);
end;

{ If value is target + x, for a short string variable target named in both
  places: x. }
function appendedTo(target, value: PNode): PNode;
var
  l: PNode;
begin
  appendedTo := nil;
  if (target^.kind <> nkNameRef) or (value^.kind <> nkBinary) or (value^.bop <> boAdd) or
     (value^.opDecl <> nil) or (value^.typ^.kind <> tyShortString) then
    exit;
  l := value^.lhs;
  if (l^.kind <> nkNameRef) or (target^.refDecl = nil) or (target^.refDecl^.kind <> dkVar) or
     (l^.refDecl <> target^.refDecl) or (target^.refWith <> nil) or (l^.refWith <> nil) then
    exit;
  appendedTo := value^.rhs;
end;

function skipParensNode(e: PNode): PNode;
begin
  while e^.kind = nkParen do
    e := e^.inner;
  skipParensNode := e;
end;

function isTargetNameNode(e: PNode; d: PDecl): boolean;
begin
  e := skipParensNode(e);
  isTargetNameNode := (e^.kind = nkNameRef) and (e^.refDecl = d) and
                      (e^.refWith = nil);
end;

{ Is e, after parentheses and ansistring casts, the variable d itself? }
function isDirectTargetRefNode(e: PNode; d: PDecl): boolean;
begin
  e := skipParensNode(e);
  if (e^.kind = nkCall) and (e^.typ <> nil) and (e^.typ^.kind = tyAnsiString) and
     (e^.args.count = 1) and
     (e^.isVarCast or ((e^.callDecl <> nil) and (e^.callDecl^.kind = dkType))) then
    exit(isDirectTargetRefNode(PNode(listGet(e^.args, 0)), d));
  isDirectTargetRefNode := isTargetNameNode(e, d);
end;

{ The right operands of a left-nested ansistring addition whose leftmost
  operand is d: s + a + b gives a and b. }
function ansiAppends(value: PNode; d: PDecl; var parts: TPtrList): boolean;
begin
  ansiAppends := false;
  if (value^.kind <> nkBinary) or (value^.bop <> boAdd) or
     (value^.opDecl <> nil) or (value^.typ^.kind <> tyAnsiString) then
    exit;
  if not isTargetNameNode(value^.lhs, d) then
    if not ansiAppends(value^.lhs, d, parts) then
      exit;
  listAdd(parts, value^.rhs);
  ansiAppends := true;
end;

{ A direct call of a function (or through a procedural value) whose result
  is an ansistring: the reference it returns is new, so an assignment can
  move it instead of counting it and releasing it later. }
function isFreshAnsiCall(value: PNode): boolean;
begin
  isFreshAnsiCall := false;
  if (value^.typ = nil) or (value^.typ^.kind <> tyAnsiString) then
    exit;
  if value^.kind = nkInherited then
    exit(true);
  if not (value^.kind in [nkCall, nkIndirectCall]) or value^.isVarCast or
     (value^.methodCast <> 0) then
    exit;
  if (value^.calleeExpr <> nil) or (value^.callValue <> nil) then
    exit(true);
  if value^.callDecl <> nil then
    if value^.callDecl^.kind in [dkRoutine, dkVar] then
      exit(true);
end;

function emitResultInPlace(target, value: PNode; tt: PType): boolean; forward;

{ Ranges of local variables (optimization3 item 4); after the pin
  analysis. }
function plainAssignTarget(e: PNode): PDecl; forward;
procedure setVarRange(v: PDecl; known: boolean; lo, hi: int64); forward;
procedure rangeLocalsWrittenBy(s: PNode; var outl: TPtrList); forward;
procedure forgetRanges(const l: TPtrList); forward;
function accumulatorSteps(s: PNode; v: PDecl; var dLo, dHi: int64): boolean; forward;
procedure initRangeLocals(r: PDecl); forward;
procedure endRangeLocals; forward;

procedure visitAssignInner(s: PNode);
var
  more: PNode;
  target, value: PNode;
  tt: PType;
  dst, src, pair, v: TV;
  p: PDecl;
  take, safe: boolean;
  dV: PDecl;
  parts, vals: TPtrList;
  k: longint;
begin
  target := s^.target;
  value := s^.value;
  tt := target^.typ;
  p := assignedProperty(target);
  if p <> nil then
  begin
    emitPropertyWrite(p, target, value);
    exit;
  end;
  if tt^.kind = tyShortString then
  begin
    dst := emitAddress(target);
    more := appendedTo(target, value);
    if more <> nil then
    begin
      { s := s + x: x goes to the end of s, without a temporary }
      src := emitPStr(more);
      astrV3('__pas_str_append', 'ptr,i32,ptr', dst, i32c(tt^.maxLength), src);
      exit;
    end;
    emitStringAssign(dst, tt, value);
    exit;
  end;
  if tt^.kind = tyAnsiString then
  begin
    dV := nil;
    if (target^.kind = nkNameRef) and (target^.refWith = nil) and
       (target^.refDecl <> nil) and (target^.refDecl^.kind = dkVar) then
      dV := target^.refDecl;
    listInit(parts);
    if (dV <> nil) and ansiAppends(value, dV, parts) then
    begin
      if parts.count = 1 then
      begin
        { s := s + x: x goes to the end of s, in place when s is unique }
        dst := emitAddress(target);
        more := PNode(listGet(parts, 0));
        if isCharLike(more^.typ) then
          astrV2('__pas_astr_append_char', 'ptr,i8', dst, emitCharValue(more))
        else
          astrV2('__pas_astr_append', 'ptr,ptr', dst, emitAnsi(more));
        listFree(parts);
        exit;
      end;
      { s + a + b ...: evaluate every part before changing s, then append
        them in order. (A bare s among the parts would read it after the
        first append; keep the general concatenation for that.) }
      safe := true;
      for k := 0 to parts.count - 1 do
        if isDirectTargetRefNode(PNode(listGet(parts, k)), dV) then
          safe := false;
      if safe then
      begin
        dst := emitAddress(target);
        listInit(vals);
        for k := 0 to parts.count - 1 do
        begin
          more := PNode(listGet(parts, k));
          if isCharLike(more^.typ) then
            listAdd(vals, irBox(emitCharValue(more)))
          else
            listAdd(vals, irBox(emitAnsi(more)));
        end;
        for k := 0 to vals.count - 1 do
        begin
          more := PNode(listGet(parts, k));
          if isCharLike(more^.typ) then
            astrV2('__pas_astr_append_char', 'ptr,i8', dst,
                   PTVBox(listGet(vals, k))^)
          else
            astrV2('__pas_astr_append', 'ptr,ptr', dst,
                   PTVBox(listGet(vals, k))^);
        end;
        listFree(vals);
        listFree(parts);
        exit;
      end;
    end;
    listFree(parts);
    if (dV <> nil) and (value^.kind = nkBinary) and (value^.bop = boAdd) and
       (value^.opDecl = nil) and isTargetNameNode(value^.rhs, dV) then
    begin
      { s := x + s: x goes to the front of s, in place when s is unique and
        has spare capacity }
      dst := emitAddress(target);
      more := value^.lhs;
      if isCharLike(more^.typ) then
        astrV2('__pas_astr_prepend_char', 'ptr,i8', dst, emitCharValue(more))
      else
        astrV2('__pas_astr_prepend', 'ptr,ptr', dst, emitAnsi(more));
      exit;
    end;
    take := isFreshAnsiCall(value) and (target^.kind = nkNameRef);
    if take then
    begin
      noHoldAnsiResult := true;
      tookAnsiResult := false;
    end;
    v := emitAnsi(value);
    if take then
    begin
      noHoldAnsiResult := false;
      take := tookAnsiResult;
    end;
    dst := emitAddress(target);
    if take then
    begin
      astrV2('__pas_astr_take', 'ptr,ptr', dst, v);
      exit;
    end;
    storeValue(dst, v, tt);
    exit;
  end;
  if isStructured(tt) then
  begin
    if emitResultInPlace(target, value, tt) then
      exit;
    dst := emitAddress(target);
    src := emitAddress(value);
    copyValue(dst, src, tt);
    exit;
  end;
  if tt^.kind = tyRoutine then
  begin
    { a procedural variable (Turbo/FPC) }
    pair := emitRoutinePair(value, tt);
    storeValue(emitAddress(target), pair, tt); { (a closure: counted) }
    exit;
  end;
  v := convertForAssign(visitExpr(value), value^.typ, tt, value^.b);
  if (target^.kind = nkNameRef) and (target^.refDecl <> nil) and
     (target^.refDecl^.kind = dkVar) and target^.refDecl^.isOwned then
  begin
    { an owned variable: frees the object it had }
    dst := emitAddress(target);
    pair := call2(classRuntime('__pas_owned_set', 'void', 'ptr,ptr'), dst, v, '');
    exit;
  end;
  if isBitPackedComponent(target) then
  begin
    bitStore(target, v);
    exit;
  end;
  storeValue(emitAddress(target), v, tt);
  assignStored := v;
  assignStoredSet := true;
end;

{ target := value. A variable of rangeLocals gets the stored value's
  range from now on (optimization3 item 4), else none. }
procedure visitAssign(s: PNode);
var
  rangeVar: PDecl;
  stored: boolean;
begin
  rangeVar := plainAssignTarget(s^.target);
  if (rangeVar <> nil) and not rangeVar^.cgRangeLocal then
    rangeVar := nil;
  assignStoredSet := false;
  visitAssignInner(s);
  stored := assignStoredSet;
  assignStoredSet := false;
  if rangeVar <> nil then
  begin
    if stored then
      setVarRange(rangeVar, assignStored.rKnown, assignStored.rLo, assignStored.rHi)
    else
      setVarRange(rangeVar, false, 0, 0);
  end;
end;

{ exit(value): sets the result first }
procedure emitSetResult(value: PNode);
var
  res: PDecl;
  rt: PType;
  addr, src, v: TV;
begin
  res := curRoutine^.resultVar;
  rt := res^.varType;
  addr := varAddress(res);
  if rt^.kind = tyShortString then
    emitStringAssign(addr, rt, value)
  else if rt^.kind = tyAnsiString then
    storeValue(addr, emitAnsi(value), rt)
  else if isMethodPointer(rt) or isClosureType(rt) then
    storeValue(addr, emitRoutinePair(value, rt), rt)
  else if isStructured(rt) then
  begin
    src := emitAddress(value);
    copyValue(addr, src, rt);
  end
  else
  begin
    v := convertForAssign(visitExpr(value), value^.typ, rt, value^.b);
    storeValue(addr, v, rt);
  end;
end;

procedure emitIncDec(s: PNode; isInc: boolean);
var
  varE, stepE: PNode;
  t: PType;
  addr, old, step, nv, wide, old64, neg, bad: TV;
  loc: TLoc;
  bd: TOrdinalBounds;
  check: boolean;
begin
  varE := PNode(listGet(s^.args, 0));
  t := varE^.typ;
  addr := emitAddress(varE);
  old := loadScalar(addr, t, '');
  if s^.args.count = 2 then
  begin
    stepE := PNode(listGet(s^.args, 1));
    step := toI64(visitExpr(stepE), stepE^.typ);
  end
  else
    step := i64c(1);
  loc := s^.b;
  if t^.kind = tyPointer then { by n of its pointee }
  begin
    if not isInc then
      step := irBin('sub', i64c(0), step, '');
    if pointerMathChecked(loc) then
      emitPointerAddCheck(t, old, step, loc);
    nv := irGEP1(pointeeMemType(t), old, step, false, '');
    irStore(nv, addr);
    exit;
  end;
  if isIntegerLike(t) and (intBits(t) = 64) then
  begin
    { no wider type to check in (qword: unsigned) }
    if isInc then
      nv := emitCheckedArith('add', isSignedOrdinal(t), old, step, loc,
                             nm(s^.callee.name))
    else
      nv := emitCheckedArith('sub', isSignedOrdinal(t), old, step, loc,
                             nm(s^.callee.name));
    emitRangeCheck(nv, getBase(t), t, loc);
    storeScalar(addr, nv, t);
    exit;
  end;
  { computed in i64 so the range check sees the true result }
  old64 := toI64(old, t);
  if isInc then
    wide := irBin('add', old64, step, '')
  else
  begin
    neg := irBin('sub', i64c(0), step, '');
    wide := irBin('add', old64, neg, '');
  end;
  if isIntegerLike(t) then
    check := overflowChecks(loc) or
             (rangeChecks(loc) and not (wraps(loc) and (t^.kind = tyInteger)))
  else
    check := rangeChecks(loc);
  if check then
  begin
    bd := getOrdinalBounds(t);
    bad := outsideRange(wide, bd.lo, bd.hi);
    if isIntegerLike(t) then
      emitOverflowCheck(bad, 'integer overflow in ''' + nm(s^.callee.name) + '''', loc)
    else
      emitRuntimeCheck(bad, 'value out of range', loc);
  end;
  storeScalar(addr, irCast('trunc', wide, regType(t), ''), t);
end;

{ setlength of the array variable at addr, of type t: ns[idx] elements,
  each of them (arrays) set to the next lengths. }
procedure emitDynSetLength(const addr: TV; t: PType; const ns: TPtrList; idx: longint);
var
  info, n, data, i, next: TV;
  pre, loopB, doneB: PIRBlock;
  phi: PIRInst;
begin
  info := dynInfoOf(t);
  n := PTVBox(listGet(ns, idx))^;
  astrV3('__pas_dyn_setlength', 'ptr,ptr,i64', addr, info, n);
  if idx = ns.count - 1 then
    exit;
  data := irLoad(irPtrT, addr, 'data');
  pre := irInsertBlock;
  loopB := irNewBlock('dim.loop');
  doneB := irNewBlock('dim.done');
  irCondBr(irICmp('eq', n, i64c(0), ''), doneB, loopB, false);
  irSetInsertPoint(loopB);
  i := irPhi(irI64, 'i', phi);
  irAddIncoming(phi, i64c(0), pre);
  emitDynSetLength(irGEP1(irPtrT, data, i, true, ''), t^.dynElement, ns, idx + 1);
  next := irBin('add', i, i64c(1), '');
  irAddIncoming(phi, next, irInsertBlock);
  irCondBr(irICmp('eq', next, n, ''), doneB, loopB, false);
  irSetInsertPoint(doneB);
end;

procedure visitProcCall(s: PNode);
var
  d: PDecl;
  k: TBuiltinKind;
  ignored, f, a0, a1, a2, mem, sz, old, dst, cnt, neg, v, code, p, src: TV;
  done: TV;
  arg, countE: PNode;
  isFill: boolean;
  pointee: PType;
  dims: TPtrList; { of PTVBox: setlength's lengths }
  i: longint;
begin
  if s^.callValue <> nil then { a procedural field or property }
  begin
    ignored := emitCallThrough(visitExpr(s^.callValue), s^.callValue^.typ, s^.args, s^.b);
    exit;
  end;
  d := s^.callDecl;
  if d^.kind = dkRoutine then
  begin
    if d^.isStatic then { a record's static method }
      ignored := emitCall(d, s^.args)
    else if d^.methodKind <> mtNone then { through Self or a with statement's object }
      ignored := emitMethodCallC(d, withBase(s^.callWith), s^.args, false, s^.b,
                                 withIsClass(s^.callWith))
    else
      ignored := emitCall(d, s^.args);
    exit;
  end;
  if d^.kind = dkVar then
  begin
    ignored := emitIndirectCall(d, s^.args);
    exit;
  end;
  k := d^.builtin;
  if k = bkWait then { Wait(t); }
  begin
    ignored := emitAwait(visitExpr(PNode(listGet(s^.args, 0))),
                         PNode(listGet(s^.args, 0))^.typ);
    exit;
  end;
  case k of
    bkWrite, bkWriteln: emitWrite(s, k = bkWriteln);
    bkRead, bkReadln: emitRead(s, k = bkReadln);
    bkReset, bkRewrite, bkGet, bkPut, bkPage:
      if ((k = bkReset) or (k = bkRewrite)) and
         isUntypedFileType(PNode(listGet(s^.args, 0))^.typ) then
      begin
        { an untyped file's: with its record size (128 if not given) }
        f := loadFile(emitAddress(PNode(listGet(s^.args, 0))));
        if s^.args.count = 2 then
        begin
          arg := PNode(listGet(s^.args, 1));
          sz := toI64(visitExpr(arg), arg^.typ);
        end
        else
          sz := i64c(128);
        if k = bkReset then
          ignored := call2(classRuntime('__pas_reset_size', 'void', 'ptr,i64'), f, sz, '')
        else
          ignored := call2(classRuntime('__pas_rewrite_size', 'void', 'ptr,i64'), f, sz, '');
      end
      else
      begin
        if s^.args.count = 0 then
          f := outputFile
        else
          f := loadFile(emitAddress(PNode(listGet(s^.args, 0))));
        case k of
          bkReset: rtv1('__pas_reset', f);
          bkRewrite: rtv1('__pas_rewrite', f);
          bkGet: rtv1('__pas_get', f);
          bkPut: rtv1('__pas_put', f);
        else
          rtv1('__pas_page', f);
        end;
      end;
    bkClose, bkCloseFile:
      begin
        f := loadFile(emitAddress(PNode(listGet(s^.args, 0))));
        ignored := call1(classRuntime('__pas_close', 'void', 'ptr'), f, '');
      end;
    bkFlush:
      begin
        f := loadFile(emitAddress(PNode(listGet(s^.args, 0))));
        ignored := call1(classRuntime('__pas_flush', 'void', 'ptr'), f, '');
      end;
    bkAssign, bkAssignFile:
      begin
        f := loadFile(emitAddress(PNode(listGet(s^.args, 0))));
        a1 := emitAnsi(PNode(listGet(s^.args, 1)));
        ignored := call2(classRuntime('__pas_assign', 'void', 'ptr,ptr'), f, a1, '');
      end;
    bkBlockRead, bkBlockWrite:
      begin
        f := loadFile(emitAddress(PNode(listGet(s^.args, 0))));
        a1 := emitAddress(PNode(listGet(s^.args, 1)));
        arg := PNode(listGet(s^.args, 2));
        cnt := toI64(visitExpr(arg), arg^.typ);
        if k = bkBlockRead then
          done := call4(classRuntime('__pas_blockread', 'i64', 'ptr,ptr,i64,i32'),
                        f, a1, cnt, i32c(ord(s^.args.count = 3)), '')
        else
          done := call4(classRuntime('__pas_blockwrite', 'i64', 'ptr,ptr,i64,i32'),
                        f, a1, cnt, i32c(ord(s^.args.count = 3)), '');
        if s^.args.count = 4 then { the records done }
        begin
          arg := PNode(listGet(s^.args, 3));
          if not irSameType(done.t, regType(arg^.typ)) then
            done := irCast('trunc', done, regType(arg^.typ), '');
          storeScalar(emitAddress(arg), done, arg^.typ);
        end;
      end;
    bkSeek:
      begin
        f := loadFile(emitAddress(PNode(listGet(s^.args, 0))));
        arg := PNode(listGet(s^.args, 1));
        a1 := toI64(visitExpr(arg), arg^.typ);
        ignored := call2(classRuntime('__pas_seek', 'void', 'ptr,i64'), f, a1, '');
      end;
    bkPack, bkUnpack: emitPackUnpack(s, k = bkPack);
    bkBreak, bkContinue, bkExit:
      begin
        if s^.args.count > 0 then
          emitSetResult(PNode(listGet(s^.args, 0)));
        if k = bkExit then
          emitJump(exitBlock, nil)
        else if k = bkContinue then
          emitJump(PIRBlock(listGet(loopBlocks, loopBlocks.count - 3)),
                   listGet(loopBlocks, loopBlocks.count - 1))
        else
          emitJump(PIRBlock(listGet(loopBlocks, loopBlocks.count - 2)),
                   listGet(loopBlocks, loopBlocks.count - 1));
        irSetInsertPoint(irNewBlock('after.jump')); { unreachable code follows }
      end;
    bkSetLength:
      begin
        arg := PNode(listGet(s^.args, 0));
        a0 := emitAddress(arg);
        if arg^.typ^.kind = tyDynArray then
        begin
          listInit(dims);
          for i := 1 to s^.args.count - 1 do
            listAdd(dims, irBox(toI64(visitExpr(PNode(listGet(s^.args, i))),
                                      PNode(listGet(s^.args, i))^.typ)));
          emitDynSetLength(a0, arg^.typ, dims, 0);
          listFree(dims);
          exit;
        end;
        a1 := emitI32(PNode(listGet(s^.args, 1)));
        if arg^.typ^.kind = tyAnsiString then
          astrV2('__pas_astr_setlength', 'ptr,i32', a0, a1)
        else
          astrV3('__pas_str_setlength', 'ptr,i32,i32', a0, i32c(arg^.typ^.maxLength), a1);
      end;
    bkDelete:
      begin
        a0 := emitAddress(PNode(listGet(s^.args, 0)));
        a1 := emitI32(PNode(listGet(s^.args, 1)));
        a2 := emitI32(PNode(listGet(s^.args, 2)));
        if PNode(listGet(s^.args, 0))^.typ^.kind = tyAnsiString then
          astrV3('__pas_astr_delete', 'ptr,i32,i32', a0, a1, a2)
        else
          rtv3('__pas_str_delete', a0, a1, a2);
      end;
    bkInsert:
      begin
        arg := PNode(listGet(s^.args, 1));
        if arg^.typ^.kind = tyAnsiString then
        begin
          a0 := emitAnsi(PNode(listGet(s^.args, 0)));
          a1 := emitAddress(arg);
          a2 := emitI32(PNode(listGet(s^.args, 2)));
          astrV3('__pas_astr_insert', 'ptr,ptr,i32', a0, a1, a2);
          exit;
        end;
        a0 := emitPStr(PNode(listGet(s^.args, 0)));
        a1 := emitAddress(arg);
        a2 := emitI32(PNode(listGet(s^.args, 2)));
        rtv4('__pas_str_insert', a0, a1, i32c(arg^.typ^.maxLength), a2);
      end;
    bkInc, bkDec: emitIncDec(s, k = bkInc);
    bkGetMem:
      begin
        arg := PNode(listGet(s^.args, 1));
        sz := toI64(visitExpr(arg), arg^.typ);
        mem := call1(rtFn('__pas_new'), sz, '');
        irStore(mem, emitAddress(PNode(listGet(s^.args, 0))));
      end;
    bkFreeMem:
      rtv1('__pas_freemem', visitExpr(PNode(listGet(s^.args, 0))));
    bkReallocMem:
      begin
        a0 := emitAddress(PNode(listGet(s^.args, 0)));
        arg := PNode(listGet(s^.args, 1));
        sz := toI64(visitExpr(arg), arg^.typ);
        old := irLoad(irPtrT, a0, '');
        mem := call2(rtFn('__pas_realloc'), old, sz, '');
        irStore(mem, a0);
      end;
    bkFillChar, bkMove:
      begin
        { a negative count does nothing }
        isFill := k = bkFillChar;
        if isFill then
        begin
          dst := emitAddress(PNode(listGet(s^.args, 0)));
          countE := PNode(listGet(s^.args, 1));
        end
        else
        begin
          dst := emitAddress(PNode(listGet(s^.args, 1)));
          countE := PNode(listGet(s^.args, 2));
        end;
        cnt := toI64(visitExpr(countE), countE^.typ);
        neg := irICmp('slt', cnt, i64c(0), '');
        cnt := irSelect(neg, i64c(0), cnt, '');
        if isFill then
        begin
          arg := PNode(listGet(s^.args, 2));
          v := irCast('trunc', toI64(visitExpr(arg), arg^.typ), irI8, '');
          emitMemSet(dst, v, cnt, 1);
        end
        else
        begin
          src := emitAddress(PNode(listGet(s^.args, 0)));
          emitMemMove(dst, src, cnt);
        end;
      end;
    bkVectorStore: { at any address (alignment 1) }
      begin
        p := visitExpr(PNode(listGet(s^.args, 0)));
        irStoreAlign(visitExpr(PNode(listGet(s^.args, 1))), p, 1);
      end;
    bkHalt:
      begin
        if s^.args.count = 0 then
          code := i32c(0)
        else
          code := emitI32(PNode(listGet(s^.args, 0)));
        rtv1('__pas_halt', code);
        irUnreachable;
        irSetInsertPoint(irNewBlock('after.halt')); { unreachable code follows }
      end;
    bkNew:
      begin
        arg := PNode(listGet(s^.args, 0));
        pointee := arg^.typ^.pointee;
        mem := call1(rtFn('__pas_new'), i64c(sizeOf_(pointee)), '');
        emitFileLifetime(mem, pointee, true, irNull);
        irStore(mem, emitAddress(arg));
      end;
    bkDispose:
      begin
        arg := PNode(listGet(s^.args, 0));
        p := visitExpr(arg);
        pointee := arg^.typ^.pointee;
        if containsFile(pointee) then
        begin
          if rangeChecks(arg^.b) then
            emitRuntimeCheck(irICmp('eq', p, irNull, ''), '''dispose'' of a nil pointer',
                             arg^.b);
          emitFileLifetime(p, pointee, false, irNull);
        end;
        if isManaged(pointee) then
        begin
          if rangeChecks(arg^.b) then
            emitRuntimeCheck(irICmp('eq', p, irNull, ''), '''dispose'' of a nil pointer',
                             arg^.b);
          finalizeValue(p, pointee);
        end;
        rtv1('__pas_dispose', p);
      end;
  else
    ; { Sema allows only procedures here }
  end;
end;

procedure visitIf(s: PNode);
var
  c: TV;
  thenB, endB, elseB: PIRBlock;
begin
  c := visitExpr(s^.cond);
  thenB := irNewBlock('if.then');
  endB := irNewBlock('if.end');
  elseB := endB;
  if s^.elseStmt <> nil then
    elseB := irNewBlock('if.else');
  irCondBr(c, thenB, elseB, false);
  irSetInsertPoint(thenB);
  visitStmt(s^.thenStmt);
  irBr(endB);
  if s^.elseStmt <> nil then
  begin
    irSetInsertPoint(elseB);
    visitStmt(s^.elseStmt);
    irBr(endB);
  end;
  irSetInsertPoint(endB);
end;

procedure visitWhile(s: PNode);
var
  condB, bodyB, endB: PIRBlock;
begin
  condB := irNewBlock('while.cond');
  bodyB := irNewBlock('while.body');
  endB := irNewBlock('while.end');
  irBr(condB);
  irSetInsertPoint(condB);
  irCondBr(visitExpr(s^.cond), bodyB, endB, false);
  irSetInsertPoint(bodyB);
  pushLoop(condB, endB);
  visitStmt(s^.thenStmt);
  popLoop;
  irBr(condB);
  irSetInsertPoint(endB);
end;

procedure visitStmtList(const l: TPtrList); forward;

procedure visitRepeat(s: PNode);
var
  bodyB, condB, endB: PIRBlock;
begin
  bodyB := irNewBlock('repeat.body');
  condB := irNewBlock('repeat.cond');
  endB := irNewBlock('repeat.end');
  irBr(bodyB);
  irSetInsertPoint(bodyB);
  pushLoop(condB, endB);
  visitStmtList(s^.stmts);
  popLoop;
  irBr(condB);
  irSetInsertPoint(condB);
  irCondBr(visitExpr(s^.untilCond), endB, bodyB, false);
  irSetInsertPoint(endB);
end;

{   start = Init; final = Final            (each evaluated once)
    if not (start <= final) goto end       (>= for downto)
    [range-check start and final against the control variable's type]
  loop:
    i = phi [start, pre], [next, inc]; Var := i
    Body
    if i = final goto end                  (test before stepping: no
  inc:                                      overflow at maxint)
    next = i + 1 (- 1); goto loop }
type
  { a for loop's accumulator (optimization3 item 4): x := x + c in its
    body, from a start range; its bound, and the loop bound it had }
  PAccumulator = ^TAccumulator;
  TAccumulator = record
    v: PDecl;
    startLo, startHi, dLo, dHi, lo, hi: int64;
    bounded, oldKnown: boolean;
    oldLo, oldHi: int64;
  end;

procedure visitFor(s: PNode);
var
  v, a: PDecl;
  varTy: PType;
  down: boolean;
  start, final, start64, final64, enter, i, done, next: TV;
  checkB, loopB, latchB, incB, endB, preB: PIRBlock;
  phi: PIRInst;
  oldKnown: boolean;
  oldLo, oldHi: int64;
  written, accs: TPtrList;
  acc: PAccumulator;
  k: longint;
  dLo, dHi, trips, tyLo, tyHi, x: int64;
  overflow, fits: boolean;
begin
  v := s^.forVar^.refDecl;
  varTy := v^.varType;
  down := s^.isDownto;
  { Variables of rangeLocals the loop writes: their ranges are forgotten in
    it and after it, except for accumulators (x := x + c), which stay
    within their start value plus the most the loop can add. }
  listInit(written);
  listInit(accs);
  if rangeLocals.count > 0 then
  begin
    listFree(written);
    rangeLocalsWrittenBy(s, written);
    for k := 0 to written.count - 1 do
    begin
      a := PDecl(listGet(written, k));
      dLo := 0;
      dHi := 0;
      if a^.cgRangeKnown and accumulatorSteps(s^.forBody, a, dLo, dHi) then
      begin
        acc := PAccumulator(arenaAlloc(sizeof(TAccumulator)));
        acc^.v := a;
        acc^.startLo := a^.cgRangeLo;
        acc^.startHi := a^.cgRangeHi;
        acc^.dLo := dLo;
        acc^.dHi := dHi;
        acc^.bounded := false;
        listAdd(accs, acc);
      end;
    end;
  end;
  start := visitExpr(s^.forInit);
  final := visitExpr(s^.forFinal);
  { the bounds may have other integer types than the variable: compare
    them as i64, check them, then convert them to the variable's type }
  start64 := toI64(start, s^.forInit^.typ);
  final64 := toI64(final, s^.forFinal^.typ);
  { (a qword variable: its bounds compare unsigned) }
  if isQWord(varTy) then
  begin
    if down then
      enter := irICmp('uge', start64, final64, '')
    else
      enter := irICmp('ule', start64, final64, '');
  end
  else if down then
    enter := irICmp('sge', start64, final64, '')
  else
    enter := irICmp('sle', start64, final64, '');
  checkB := irNewBlock('for.check');
  loopB := irNewBlock('for.body');
  latchB := irNewBlock('for.latch');
  incB := irNewBlock('for.inc');
  endB := irNewBlock('for.end');
  irCondBr(enter, checkB, endB, false);

  { ISO 6.8.3.9: if the loop runs, both bounds must be assignment
    compatible with the control variable }
  irSetInsertPoint(checkB);
  emitRangeCheckMsg(start, s^.forInit^.typ, varTy, s^.forInit^.b,
                    '''for'' loop bound out of range');
  emitRangeCheckMsg(final, s^.forFinal^.typ, varTy, s^.forFinal^.b,
                    '''for'' loop bound out of range');
  start := convertOrdinal(start, s^.forInit^.typ, varTy);
  final := convertOrdinal(final, s^.forFinal^.typ, varTy);
  preB := irInsertBlock;
  irBr(loopB);

  irSetInsertPoint(loopB);
  i := irPhi(start.t, nm(v^.ident.name), phi);
  irAddIncoming(phi, start, preB);
  storeScalar(varAddress(v), i, varTy);
  oldKnown := v^.cgRangeKnown;
  oldLo := v^.cgRangeLo;
  oldHi := v^.cgRangeHi;
  if isSignedOrdinal(varTy) and start.rKnown and final.rKnown then
  begin
    v^.cgRangeKnown := true;
    if start.rLo < final.rLo then
      v^.cgRangeLo := start.rLo
    else
      v^.cgRangeLo := final.rLo;
    if start.rHi > final.rHi then
      v^.cgRangeHi := start.rHi
    else
      v^.cgRangeHi := final.rHi;
  end
  else
    v^.cgRangeKnown := false;
  { At most trips iterations: an accumulator stays within its start range
    plus trips times the most one iteration adds (or subtracts), at every
    point of the loop and after it. Only a bound that fits its type counts
    (so that no step can have wrapped), computed in int64 (an overflow: no
    bound). }
  if (accs.count > 0) and isSignedOrdinal(varTy) and start.rKnown and
     final.rKnown then
  begin
    if down then
      overflow := subOverflows(start.rHi, final.rLo, trips)
    else
      overflow := subOverflows(final.rHi, start.rLo, trips);
    if not overflow then
      overflow := addOverflows(trips, 1, trips);
    if not overflow and (trips < 0) then
      trips := 0;
    for k := 0 to accs.count - 1 do
    begin
      acc := PAccumulator(listGet(accs, k));
      fits := not overflow and
              signedTypeRange(regType(acc^.v^.varType)^.bits, tyLo, tyHi);
      if fits then
        fits := not mulOverflows(trips, acc^.dLo, x) and
                not addOverflows(acc^.startLo, x, acc^.lo);
      if fits then
        fits := not mulOverflows(trips, acc^.dHi, x) and
                not addOverflows(acc^.startHi, x, acc^.hi);
      acc^.bounded := fits and (acc^.lo >= tyLo) and (acc^.hi <= tyHi);
    end;
  end;
  forgetRanges(written);
  for k := 0 to accs.count - 1 do
  begin
    acc := PAccumulator(listGet(accs, k));
    if acc^.bounded then
    begin
      a := acc^.v;
      acc^.oldKnown := a^.cgLoopKnown;
      acc^.oldLo := a^.cgLoopLo;
      acc^.oldHi := a^.cgLoopHi;
      a^.cgLoopKnown := true;
      a^.cgLoopLo := acc^.lo;
      a^.cgLoopHi := acc^.hi;
      setVarRange(a, true, acc^.lo, acc^.hi);
    end;
  end;
  pushLoop(latchB, endB);
  visitStmt(s^.forBody);
  popLoop;
  v^.cgRangeKnown := oldKnown;
  v^.cgRangeLo := oldLo;
  v^.cgRangeHi := oldHi;
  for k := 0 to accs.count - 1 do
  begin
    acc := PAccumulator(listGet(accs, k));
    if acc^.bounded then
    begin
      a := acc^.v;
      a^.cgLoopKnown := acc^.oldKnown;
      a^.cgLoopLo := acc^.oldLo;
      a^.cgLoopHi := acc^.oldHi;
    end;
  end;
  forgetRanges(written);
  for k := 0 to accs.count - 1 do
  begin
    acc := PAccumulator(listGet(accs, k));
    if acc^.bounded then
      setVarRange(acc^.v, true, acc^.lo, acc^.hi);
  end;
  listFree(written);
  listFree(accs);
  irBr(latchB);
  irSetInsertPoint(latchB);
  done := irICmp('eq', i, final, 'for.done');
  irCondBr(done, endB, incB, false);

  irSetInsertPoint(incB);
  if down then
    next := irBin('sub', i, irConstInt(i.t, 1), 'for.next')
  else
    next := irBin('add', i, irConstInt(i.t, 1), 'for.next');
  irAddIncoming(phi, next, incB);
  irBr(loopB);
  irSetInsertPoint(endB);
end;

type
  { a try-finally being emitted: tryFinallyBegin, the body, tryFinallyMiddle,
    the finally part, tryFinallyEnd }
  TTryFinally = record
    raisedB, finallyB, endB: PIRBlock;
    state, f: TV;
    c: PCleanup;
  end;

procedure tryFinallyBegin(var t: TTryFinally);
begin
  t.raisedB := irNewBlock('finally.raise');
  t.finallyB := irNewBlock('finally');
  t.endB := irNewBlock('try.end');
  t.state := irCreateTemp(irI32, 'finally.state');
  t.f := emitProtect(t.raisedB, 'try.body');
  t.c := newCleanup(clFinally, t.f);
  t.c^.state := t.state;
  t.c^.finallyB := t.finallyB;
  listAdd(cleanups, t.c);
end;

procedure tryFinallyMiddle(var t: TTryFinally);
var
  ignored: pointer;
begin
  ignored := listPop(cleanups);
  excCall1('__pas_exc_pop', t.f);
  irStore(i32c(0), t.state);
  irBr(t.finallyB);

  irSetInsertPoint(t.raisedB);
  irStore(i32c(1), t.state);
  irBr(t.finallyB);

  irSetInsertPoint(t.finallyB);
end;

procedure tryFinallyEnd(var t: TTryFinally);
var
  reraiseB: PIRBlock;
  sw: PIRInst;
  k: longint;
begin
  reraiseB := irNewBlock('finally.reraise');
  sw := irSwitch(irLoad(irI32, t.state, 'state'), t.endB);
  irAddCase(sw, i32c(1), reraiseB);
  for k := 0 to t.c^.exits.count - 1 do
    irAddCase(sw, i32c(2 + k), PIRBlock(listGet(t.c^.exits, k)));
  irSetInsertPoint(reraiseB);
  excCall1('__pas_reraise', t.f);
  irUnreachable;
  irSetInsertPoint(t.endB);
end;

type
  { a for-in loop over i64 indexes being emitted (forInLoopBegin, an
    iteration, forInLoopEnd) }
  TForInLoop = record
    loopB, latchB, incB, endB: PIRBlock;
    phi: PIRInst;
    i, hi: TV;
  end;

{ for i := lo to hi (signed; nothing if lo > hi), with break and continue:
  the iteration may branch to latchB to skip the rest of it. }
procedure forInLoopBegin(var l: TForInLoop; const lo, hi: TV);
var
  preB: PIRBlock;
begin
  l.loopB := irNewBlock('forin.body');
  l.latchB := irNewBlock('forin.latch');
  l.incB := irNewBlock('forin.inc');
  l.endB := irNewBlock('forin.end');
  l.hi := hi;
  preB := irInsertBlock;
  irCondBr(irICmp('sle', lo, hi, ''), l.loopB, l.endB, false);
  irSetInsertPoint(l.loopB);
  l.i := irPhi(irI64, 'in.index', l.phi);
  irAddIncoming(l.phi, lo, preB);
  pushLoop(l.latchB, l.endB);
end;

procedure forInLoopEnd(var l: TForInLoop);
begin
  popLoop;
  irBr(l.latchB);
  irSetInsertPoint(l.latchB);
  irCondBr(irICmp('eq', l.i, l.hi, 'in.done'), l.endB, l.incB, false);
  irSetInsertPoint(l.incB);
  irAddIncoming(l.phi, irBin('add', l.i, i64c(1), 'in.next'), l.incB);
  irBr(l.loopB);
  irSetInsertPoint(l.endB);
end;

{ One iteration of for-in loop s: the element variable bound to elemAddr,
  variable := element, the body. }
procedure forInIteration(s: PNode; const elemAddr: TV);
begin
  if s^.inElemVar <> nil then
    s^.inElemVar^.cgValue := irBox(elemAddr);
  if s^.inStep <> nil then
    visitStmt(s^.inStep);
  visitStmt(s^.inBody);
end;

{ for-in over elements lo..hi of type elemTy at base. }
procedure forInIndexed(s: PNode; const lo, hi: TV; elemTy: PIRType; const base: TV);
var
  l: TForInLoop;
begin
  forInLoopBegin(l, lo, hi);
  forInIteration(s, irGEP1(elemTy, base, l.i, true, 'in.elem'));
  forInLoopEnd(l);
end;

{   while e.MoveNext do begin variable := e.Current; Body end }
procedure emitEnumeratorLoop(s: PNode);
var
  condB, bodyB, endB: PIRBlock;
begin
  condB := irNewBlock('forin.cond');
  bodyB := irNewBlock('forin.body');
  endB := irNewBlock('forin.end');
  irBr(condB);
  irSetInsertPoint(condB);
  irCondBr(visitExpr(s^.inMoveNext), bodyB, endB, false);
  irSetInsertPoint(bodyB);
  pushLoop(condB, endB);
  if s^.inStep <> nil then
    visitStmt(s^.inStep);
  visitStmt(s^.inBody);
  popLoop;
  irBr(condB);
  irSetInsertPoint(endB);
end;

procedure emitForInEnumerator(s: PNode);
var
  c: PNode;
  ct, et: PType;
  slot: TV;
  t: TTryFinally;
begin
  c := s^.inColl;
  ct := c^.typ;
  if (ct^.kind = tyClass) or (ct^.kind = tyInterface) then
  begin
    slot := irCreateTemp(memType(ct), 'in.coll');
    irStore(visitExpr(c), slot);
    s^.inCollVar^.cgValue := irBox(slot);
  end
  else { a record: its methods get its address }
    s^.inCollVar^.cgValue := irBox(emitAddress(c));
  et := s^.inEnumVar^.varType;
  if (et^.kind <> tyClass) and (et^.kind <> tyInterface) then
  begin
    s^.inEnumVar^.cgValue := irBox(emitAddress(s^.inGetEnum));
    emitEnumeratorLoop(s);
    exit;
  end;
  slot := irCreateTemp(memType(et), 'in.enum');
  irStore(visitExpr(s^.inGetEnum), slot);
  s^.inEnumVar^.cgValue := irBox(slot);
  if et^.kind <> tyClass then
  begin
    emitEnumeratorLoop(s);
    exit;
  end;
  tryFinallyBegin(t);
  emitEnumeratorLoop(s);
  tryFinallyMiddle(t);
  emitFree(irLoad(irPtrT, slot, 'enum'));
  tryFinallyEnd(t);
end;

{   [hold the collection] lo, hi = its first and last index (i64)
    if lo > hi goto end
  loop:
    i = phi [lo, pre], [next, inc]
    [bind the element variable to element i]; variable := element; Body
  latch:                                    (continue)
    if i = hi goto end                      (test before stepping)
  inc:
    next = i + 1; goto loop
  An enumerator: e := Coll.GetEnumerator; while e.MoveNext do begin
  variable := e.Current; Body end, a class's e freed in a finally part. }
procedure visitForIn(s: PNode);
var
  c: PNode;
  elem: PType;
  base, desc, data, hi, v, n, slot, bit: TV;
  bd: TOrdinalBounds;
  lo64, hi64: int64;
  l: TForInLoop;
  member: PIRBlock;
  dyn: boolean;
  tmp: TV;
  br: TBitRef;
  condB, bodyB, endB: PIRBlock;
begin
  c := s^.inColl;
  elem := s^.inElemType;
  case s^.inKind of
    fikEnumerator:
      emitForInEnumerator(s);
    fikIterator:
      begin
        { held during the loop (and released after it, which stops it if
          it has more values: its finally parts run) }
        v := visitExpr(c);
        ignoredW := call1(classRuntime('__pas_gen_addref', 'void', 'ptr'), v, '');
        v := holdDynResult(v, c^.typ);
        condB := irNewBlock('forin.cond');
        bodyB := irNewBlock('forin.body');
        endB := irNewBlock('forin.end');
        irBr(condB);
        irSetInsertPoint(condB);
        irCondBr(emitIteratorNext(v), bodyB, endB, false);
        irSetInsertPoint(bodyB);
        pushLoop(condB, endB);
        forInIteration(s, iteratorValueAddr(v));
        popLoop;
        irBr(condB);
        irSetInsertPoint(endB);
      end;
    fikArray:
      begin
        base := emitAddress(c);
        bd := getOrdinalBounds(c^.typ^.indexType);
        if hasBitElements(c^.typ) then { each element's bits, copied }
        begin
          tmp := irCreateTemp(memType(c^.typ^.elementType), 'in.bits');
          forInLoopBegin(l, i64c(0), i64c(bd.hi - bd.lo));
          br.base := base;
          br.bit := irBin('mul', l.i, i64c(bitPackedWidth(c^.typ^.elementType)), 'bit');
          br.hasBit := true;
          br.constBit := 0;
          br.size := sizeOf_(c^.typ);
          br.t := c^.typ^.elementType;
          storeScalar(tmp, bitLoadRef(br), br.t);
          forInIteration(s, tmp);
          forInLoopEnd(l);
          exit;
        end;
        forInLoopBegin(l, i64c(0), i64c(bd.hi - bd.lo));
        forInIteration(s, irGEP2(memType(c^.typ), base, i64c(0), l.i, 'in.elem'));
        forInLoopEnd(l);
      end;
    fikOpenArray:
      begin
        desc := openDescriptor(c);
        data := irLoad(irPtrT, desc, 'data');
        hi := irBin('sub', openCount(desc), i64c(1), 'in.hi');
        forInIndexed(s, i64c(0), hi, memType(elem), data);
      end;
    fikDynArray, fikAnsiString:
      begin
        { a reference is held during the loop: changing the variable does
          not change the elements enumerated, as in Free Pascal }
        dyn := s^.inKind = fikDynArray;
        v := visitExpr(c);
        if dyn then
        begin
          astrV1('__pas_dyn_addref', v);
          v := holdDynResult(v, c^.typ);
          n := dynLength(v);
        end
        else
        begin
          astrV1('__pas_astr_addref', v);
          v := holdResult(v);
          n := irCast('zext', call1(classRuntime('__pas_astr_length', 'i32', 'ptr'), v, ''),
                      irI64, '');
        end;
        forInIndexed(s, i64c(0), irBin('sub', n, i64c(1), 'in.hi'), memType(elem), v);
      end;
    fikShortString:
      begin
        base := emitPStr(c); { its characters are 1..length }
        n := irCast('zext', irLoad(irI8, base, 'length'), irI64, '');
        forInIndexed(s, i64c(1), n, irI8, base);
      end;
    fikStringConst:
      begin
        base := emitAddress(c);
        forInIndexed(s, i64c(0), i64c(getStringLength(c^.typ) - 1), irI8, base);
      end;
    fikSet:
      begin
        { the members in order: the set is evaluated once }
        v := visitExpr(c);
        lo64 := 0;
        hi64 := -1; { [] has none }
        slot := irNull;
        if elem <> nil then
        begin
          bd := getOrdinalBounds(elem);
          lo64 := bd.lo;
          if lo64 < 0 then
            lo64 := 0;
          hi64 := bd.hi;
          if hi64 > setStorageBits(c^.typ) - 1 then
            hi64 := setStorageBits(c^.typ) - 1;
          slot := irCreateTemp(memType(elem), 'in.elem');
        end;
        forInLoopBegin(l, i64c(lo64), i64c(hi64));
        bit := irCast('trunc',
                      irBin('lshr', v, setCast(l.i, 64, setStorageBits(c^.typ)), ''),
                      irI1, 'in.member');
        member := irNewBlock('forin.member');
        irCondBr(bit, member, l.latchB, false);
        irSetInsertPoint(member);
        if elem <> nil then
          storeScalar(slot, irCast('trunc', l.i, regType(elem), ''), elem);
        forInIteration(s, slot);
        forInLoopEnd(l);
      end;
    fikOrdinals:
      begin
        bd := getOrdinalBounds(elem);
        slot := irCreateTemp(memType(elem), 'in.elem');
        forInLoopBegin(l, i64c(bd.lo), i64c(bd.hi));
        storeScalar(slot, irCast('trunc', l.i, regType(elem), ''), elem);
        forInIteration(s, slot);
        forInLoopEnd(l);
      end;
  else
    ; { (errors were reported) }
  end;
end;

procedure visitCase(s: PNode);
const
  maxExpandedRange = 256;
var
  sel, lo, hi, inRange: TV;
  signed: boolean;
  defaultB, endB, armB, nextB: PIRBlock;
  largeRanges: TPtrList;
  sw: PIRInst;
  i, j: longint;
  arm: PCaseArm;
  cl: PCaseLabel;
  v, span, k: int64;
  lr: PLargeRange;
begin
  sel := visitExpr(s^.selector);
  signed := isSignedOrdinal(s^.selector^.typ);
  defaultB := irNewBlock('case.default');
  endB := irNewBlock('case.end');
  sw := irSwitch(sel, defaultB);
  { ranges (Turbo/FPC) become switch cases when small; large ones are
    tested with comparisons in the default block }
  listInit(largeRanges);
  for i := 0 to s^.arms.count - 1 do
  begin
    arm := PCaseArm(listGet(s^.arms, i));
    armB := irNewBlock('case.arm');
    for j := 0 to arm^.labels.count - 1 do
    begin
      cl := PCaseLabel(listGet(arm^.labels, j));
      { the number of values less one, unsigned (for qword's labels) }
      {$Q-}{$R-}
      span := cl^.hiValue - cl^.loValue;
      {$Q+}{$R+}
      if (span < 0) or (span >= maxExpandedRange) then
      begin
        lr := PLargeRange(arenaAlloc(sizeof(TLargeRange)));
        lr^.lo := cl^.loValue;
        lr^.hi := cl^.hiValue;
        lr^.target := armB;
        listAdd(largeRanges, lr);
        continue;
      end;
      for k := 0 to span do
      begin
        {$Q-}{$R-}
        v := cl^.loValue + k;
        {$Q+}{$R+}
        irAddCase(sw, irConstInt(sel.t, v), armB);
      end;
    end;
    irSetInsertPoint(armB);
    visitStmt(arm^.body);
    irBr(endB);
  end;

  irSetInsertPoint(defaultB);
  for i := 0 to largeRanges.count - 1 do
  begin
    lr := PLargeRange(listGet(largeRanges, i));
    if signed then
    begin
      lo := irICmp('sge', sel, irConstInt(sel.t, lr^.lo), '');
      hi := irICmp('sle', sel, irConstInt(sel.t, lr^.hi), '');
    end
    else
    begin
      lo := irICmp('uge', sel, irConstInt(sel.t, lr^.lo), '');
      hi := irICmp('ule', sel, irConstInt(sel.t, lr^.hi), '');
    end;
    inRange := irBin('and', lo, hi, '');
    nextB := irNewBlock('case.range');
    irCondBr(inRange, lr^.target, nextB, false);
    irSetInsertPoint(nextB);
  end;
  if s^.elseLoc <> noLoc then
  begin
    visitStmtList(s^.elseBody);
    irBr(endB);
  end
  else if mode = lmISO then
    { ISO 7185 6.8.3.5: an error if no label equals the selector }
    emitRuntimeError('no ''case'' label matches the selector value', s^.b)
  else
    irBr(endB); { Turbo/FPC: nothing }
  irSetInsertPoint(endB);
  listFree(largeRanges);
end;

procedure visitGoto(s: PNode);
var
  l, owner: PDecl;
  buf, frame: TV;
begin
  l := s^.lblDecl;
  if l^.labelOwner = curRoutine then
    irBr(labelBlock(l))
  else
  begin
    { non-local: unwind to the owner's activation with _longjmp }
    owner := l^.labelOwner;
    if owner <> nil then
    begin
      frame := framePtrFor(owner);
      buf := irStructGEP(frameType(owner), frame, owner^.cgJmpSlot, '');
    end
    else
      buf := mainJmpBuf;
    rtv2(longJmpName, buf, i32c(l^.labelIndex));
    irUnreachable;
  end;
  irSetInsertPoint(irNewBlock('after.goto')); { unreachable code follows }
end;

procedure visitWith(s: PNode);
var
  i: longint;
  w: PWithContext;
begin
  { the record designators are evaluated once, on entry (ISO 6.8.3.10) }
  for i := 0 to s^.withRecords.count - 1 do
  begin
    w := PWithContext(listGet(s^.withRecords, i));
    { an object (objfpc) is evaluated, a record's address taken }
    if w^.rec^.typ^.kind = tyClass then
      w^.cgAddr := irBox(emitObject(w^.rec))
    else
      w^.cgAddr := irBox(emitAddress(w^.rec));
  end;
  visitStmt(s^.withBody);
end;

{ --- Exceptions (objfpc) --- }


{   push F; if (_setjmp(F)) goto except;  body; pop F; goto end
  except:
    exc = F.object; push H; if (_setjmp(H)) goto escape
    if (exc is C1) [ E1 := exc; handler1; pop H; free exc; goto end ] ...
    [else part; pop H; free exc; goto end]  or  pop H; reraise F
  escape: (an exception left a handler) free exc unless it is the one
    raised; reraise it }
procedure emitTryExcept(s: PNode);
var
  exceptB, endB, escapeB, doB, nextB: PIRBlock;
  f, obj, h, isv, slot: TV;
  c: PCleanup;
  ignored: pointer;
  i: longint;
  hd: PExceptHandler;

  procedure handlerDone; { a handler ran to its end }
  begin
    excCall1('__pas_exc_pop', h);
    excCall1('__pas_free_object', obj);
    irBr(endB);
  end;

begin
  exceptB := irNewBlock('try.except');
  endB := irNewBlock('try.end');
  f := emitProtect(exceptB, 'try.body');
  listAdd(cleanups, newCleanup(clTryBody, f));
  visitStmtList(s^.tryBody);
  ignored := listPop(cleanups);
  excCall1('__pas_exc_pop', f);
  irBr(endB);

  irSetInsertPoint(exceptB);
  obj := irLoad(irPtrT, irStructGEP(excFrameT, f, 1, ''), 'exc');
  escapeB := irNewBlock('handler.raise');
  h := emitProtect(escapeB, 'handler.match');
  c := newCleanup(clHandler, h);
  c^.obj := obj;
  listAdd(cleanups, c);
  listAdd(handledFrames, irBox(f));
  for i := 0 to s^.handlers.count - 1 do
  begin
    hd := PExceptHandler(listGet(s^.handlers, i));
    isv := irLoad(irPtrT, obj, 'vmt');
    isv := classIsVMT(isv, hd^.hClass);
    doB := irNewBlock('handler.do');
    nextB := irNewBlock('handler.next');
    irCondBr(isv, doB, nextB, false);
    irSetInsertPoint(doB);
    if hd^.hVar <> nil then
    begin
      slot := irCreateTemp(irPtrT, nm(hd^.hVar^.ident.name));
      irStore(obj, slot);
      hd^.hVar^.cgValue := irBox(slot);
    end;
    visitStmt(hd^.hBody);
    handlerDone;
    irSetInsertPoint(nextB);
  end;
  if (s^.handlers.count = 0) or (s^.tryElseLoc <> noLoc) then
  begin
    visitStmtList(s^.partBody);
    handlerDone;
  end
  else
  begin { no handler matches: on to the next try statement }
    excCall1('__pas_exc_pop', h);
    excCall1('__pas_reraise', f);
    irUnreachable;
  end;
  ignored := listPop(handledFrames);
  ignored := listPop(cleanups);

  irSetInsertPoint(escapeB);
  excCall2('__pas_reraise_free', h, obj);
  irUnreachable;
  irSetInsertPoint(endB);
end;

{   push F; if (_setjmp(F)) goto raised;  body; pop F; state = 0
  finally:
    finally part
    switch (state) [0: end; 1: reraise F; 2 + k: exit k (exit, break or
      continue out of the body)]
  raised: state = 1; goto finally }
{ The finally part of a routine with owned variables (owned, of PDecl):
  frees their objects, the last declared first. A destructor's exception
  does not stop the others; the first one is raised afterwards, unless an
  exception is already leaving the routine (state 1), which goes on. }
procedure emitFreeOwned(const owned: TPtrList; const state: TV);
var
  i: longint;
  pending, raised, ignored: TV;
begin
  pending := irCreateTemp(ownedExcT, 'owned.exc');
  irStore(irZero(ownedExcT), pending);
  for i := owned.count - 1 downto 0 do
    ignored := call2(classRuntime('__pas_owned_free', 'void', 'ptr,ptr'),
                     varAddress(PDecl(listGet(owned, i))), pending, '');
  raised := irICmp('eq', irLoad(irI32, state, 'state'), i32c(1), 'raised');
  raised := irCast('zext', raised, irI32, '');
  ignored := call2(classRuntime('__pas_owned_done', 'void', 'ptr,i32'), pending,
                   raised, '');
end;

{ The statements of sequence l from the one at index from. After one that
  declares owned variables, the rest is the protected part of a
  try-finally that frees them: their scope ends with the sequence, however
  it is left. }
procedure emitSequenceFrom(const l: TPtrList; from: longint);
var
  i, j: longint;
  d: PNode;
  v: PDecl;
  owned: TPtrList;
  t: TTryFinally;
begin
  for i := from to l.count - 1 do
  begin
    d := PNode(listGet(l, i));
    visitStmt(d);
    if d^.kind <> nkVarDecl then
      continue;
    listInit(owned);
    for j := 0 to d^.vdVars.count - 1 do
    begin
      v := PDecl(listGet(d^.vdVars, j));
      if v^.isOwned then
        listAdd(owned, v);
    end;
    if owned.count = 0 then
    begin
      listFree(owned);
      continue;
    end;
    tryFinallyBegin(t);
    emitSequenceFrom(l, i + 1);
    tryFinallyMiddle(t);
    emitFreeOwned(owned, t.state);
    tryFinallyEnd(t);
    listFree(owned);
    exit;
  end;
end;

procedure visitStmtList(const l: TPtrList);
begin
  emitSequenceFrom(l, 0);
end;

{ (a, b) := e;  var (a, b) := e;  -- the value kept in its hidden variable
  (a tuple written out: built in its own), then the assignments Sema made. }
procedure visitDestructureStmt(s: PNode);
var
  i: longint;
  ignored: TV;
begin
  if s^.dsInit <> nil then
    visitStmt(s^.dsInit)
  else if s^.dsAssigns.count > 0 then
    ignored := emitAddress(s^.dsValue);
  for i := 0 to s^.dsAssigns.count - 1 do
    visitStmt(PNode(listGet(s^.dsAssigns, i)));
end;

{ var x: T [:= e]: x is set each time the statement runs, to e or to zero
  (what it held, in an earlier iteration, is released). }
procedure visitVarDeclStmt(s: PNode);
var
  i: longint;
  v: PDecl;
  t: PType;
  addr, ignored: TV;
begin
  if s^.vdAssign <> nil then
  begin
    visitStmt(s^.vdAssign);
    exit;
  end;
  for i := 0 to s^.vdVars.count - 1 do
  begin
    v := PDecl(listGet(s^.vdVars, i));
    t := v^.varType;
    addr := varAddress(v);
    if v^.isOwned then
    begin
      ignored := call2(classRuntime('__pas_owned_set', 'void', 'ptr,ptr'), addr,
                       irNull, '');
      continue;
    end;
    if containsFile(t) then
      continue; { (opened when its routine starts) }
    if isManaged(t) then
      finalizeValue(addr, t);
    emitZeroInit(addr, t);
  end;
end;

procedure visitScopeStmt(s: PNode);
var
  t: TTryFinally;
  token, scope, slot, raised: TV;
begin
  if (s^.scopeToken <> nil) and (s^.scopeToken^.typ^.kind <> tyNil) then
    token := visitExpr(s^.scopeToken)
  else
    token := irNull;
  scope := call1(classRuntime('__pas_scope_enter', 'ptr', 'ptr'), token, 'scope');
  slot := irCreateTemp(irPtrT, 'scope.slot');
  irStore(scope, slot);
  pinInMemory(slot);
  tryFinallyBegin(t);
  visitStmtList(s^.scopeBody);
  tryFinallyMiddle(t);
  raised := irICmp('eq', irLoad(irI32, t.state, 'state'), i32c(1), 'raised');
  ignoredW := call2(classRuntime('__pas_scope_leave', 'void', 'ptr,i32'),
                    irLoad(irPtrT, slot, 'scope'), irCast('zext', raised, irI32, ''), '');
  tryFinallyEnd(t);
end;

{ yield e / yield break / yield from it. Each value is assigned into the
  iterator's slot: what it held is released. }
procedure visitYieldStmt(s: PNode);
var
  et, srcEt, ct: PType;
  slot, held, src: TV;
  e: PNode;
  t: TTryFinally;
  condB, bodyB, endB: PIRBlock;
begin
  et := curRoutine^.iterType^.iterElement;
  if s^.yieldValue = nil then { yield break }
  begin
    emitJump(exitBlock, nil);
    irSetInsertPoint(irNewBlock('after.jump')); { unreachable code follows }
    exit;
  end;
  if not s^.yieldFrom then
  begin
    slot := call0(classRuntime('__pas_gen_slot', 'ptr', ''), 'slot');
    storeExpr(slot, et, s^.yieldValue);
    ignoredW := call0(classRuntime('__pas_gen_yield', 'void', ''), '');
    exit;
  end;
  e := s^.yieldValue;
  ct := e^.typ;
  srcEt := ct^.iterElement;
  held := irCreateTemp(irPtrT, 'from');
  irStore(irZero(irPtrT), held);
  storeValue(held, visitExpr(e), ct); { (counted) }
  tryFinallyBegin(t);
  condB := irNewBlock('from.cond');
  bodyB := irNewBlock('from.body');
  endB := irNewBlock('from.end');
  irBr(condB);
  irSetInsertPoint(condB);
  irCondBr(emitIteratorNext(irLoad(irPtrT, held, 'from')), bodyB, endB, false);
  irSetInsertPoint(bodyB);
  src := iteratorValueAddr(irLoad(irPtrT, held, 'from'));
  slot := call0(classRuntime('__pas_gen_slot', 'ptr', ''), 'slot');
  if isStructured(et) then { (the same type: Sema) }
    copyValue(slot, src, et)
  else
    storeValue(slot, convertForAssign(loadScalar(src, srcEt, ''), srcEt, et, s^.b), et);
  ignoredW := call0(classRuntime('__pas_gen_yield', 'void', ''), '');
  irBr(condB);
  irSetInsertPoint(endB);
  tryFinallyMiddle(t);
  finalizeValue(held, ct);
  tryFinallyEnd(t);
end;

procedure visitDetachedStmt(s: PNode);
var
  t, fn, link, pair: TV;
begin
  t := visitExpr(s^.detCall);
  fn := irNull;
  link := irNull;
  if s^.detHandler <> nil then
  begin
    pair := emitRoutineValue(s^.detHandler);
    fn := irExtract(pair, 0, irPtrT, 'handler');
    link := irExtract(pair, 1, irPtrT, 'link');
  end;
  ignoredW := call3(classRuntime('__pas_task_detach', 'void', 'ptr,ptr,ptr'), t, fn, link, '');
end;

procedure emitTryFinally(s: PNode);
var
  t: TTryFinally;
begin
  tryFinallyBegin(t);
  visitStmtList(s^.tryBody);
  tryFinallyMiddle(t);
  visitStmtList(s^.partBody);
  tryFinallyEnd(t);
end;

procedure visitRaise(s: PNode);
var
  obj, addr, ignored: TV;
begin
  if s^.raiseObj = nil then
    excCall1('__pas_reraise', PTVBox(listGet(handledFrames, handledFrames.count - 1))^)
  else if s^.raiseAddr <> nil then
  begin
    { Free Pascal's order: the frame (only evaluated), the address, the
      object }
    if s^.raiseFrame <> nil then
      ignored := visitExpr(s^.raiseFrame);
    addr := visitExpr(s^.raiseAddr);
    obj := visitExpr(s^.raiseObj);
    ignored := call3(excRuntime('__pas_raise_at'), obj, whereText(s^.b), addr, '');
  end
  else
  begin
    obj := visitExpr(s^.raiseObj);
    excCall2('__pas_raise', obj, whereText(s^.b));
  end;
  irUnreachable;
  irSetInsertPoint(irNewBlock('after.raise')); { unreachable code follows }
end;

procedure visitStmt(s: PNode);
var
  mark, stringMark: longint;
  ignored: TV;
  b: PIRBlock;
  written: TPtrList;
  track: boolean;
begin
  { statements carry their source position into the debug information }
  setDebugLoc(s^.b);
  mark := liveTemps.count;
  stringMark := stringTemps.count;
  { The ranges of rangeLocals (optimization3 item 4) hold from store to
    store in straight-line code. A statement with other control flow
    forgets those of the variables it writes when it ends, since they
    depend on the path taken; and when it starts, if code in it may run
    again or be reached from the middle of it (a loop, a try statement). A
    for loop does its own (visitFor). }
  track := (rangeLocals.count > 0) and
           not (s^.kind in [nkAssign, nkCompound, nkProcCall, nkEmpty, nkFor]);
  if track then
  begin
    rangeLocalsWrittenBy(s, written);
    if not (s^.kind in [nkIf, nkCase, nkWith]) then
      forgetRanges(written);
  end;
  case s^.kind of
    nkEmpty: ;
    nkCompound: visitStmtList(s^.stmts);
    nkAssign: visitAssign(s);
    nkProcCall: visitProcCall(s);
    nkIndirectCallStmt: ignored := visitExpr(s^.calleeExpr);
    nkIf: visitIf(s);
    nkForIn: visitForIn(s);
    nkWhile: visitWhile(s);
    nkRepeat: visitRepeat(s);
    nkFor: visitFor(s);
    nkCase: visitCase(s);
    nkLabeled:
      begin
        b := labelBlock(s^.lblDecl);
        irBr(b);
        irSetInsertPoint(b);
        visitStmt(s^.lblBody);
      end;
    nkGoto: visitGoto(s);
    nkWith: visitWith(s);
    nkTry:
      if s^.isFinally then
        emitTryFinally(s)
      else
        emitTryExcept(s);
    nkRaise: visitRaise(s);
    nkVarDecl: visitVarDeclStmt(s);
    nkDestructure: visitDestructureStmt(s);
    nkExprStmt: ignoredW := visitExpr(s^.exprStmtExpr);
    nkScope: visitScopeStmt(s);
    nkDetached: visitDetachedStmt(s);
    nkYield: visitYieldStmt(s);
  else
    ;
  end;
  if track then
  begin
    forgetRanges(written);
    listFree(written);
  end;
  releaseTemps(mark); { the results held by its expressions }
  releaseStringTemps(stringMark);
end;

{ --- Declarations --- }

{ The bytes of a string or char constant for an object of type t (memory
  type ty): length-prefixed for short strings, padded with NULs or cut to
  the size (initializerForString). }
function stringInit(p: PCharArray; start, count: int64; ty: PIRType; t: PType): TV;
var
  bytes: TBuf;
  n: int64;
begin
  bufInit(bytes);
  if t^.kind = tyShortString then
  begin
    n := imin(count, t^.maxLength);
    bufAddChar(bytes, chr(n));
    bufAddChars(bytes, p, start, n);
  end
  else
    bufAddChars(bytes, p, start, count);
  while bytes.len < ty^.count do
    bufAddChar(bytes, #0);
  stringInit := irConstBytes(bytes.data, 0, ty^.count);
  bufFree(bytes);
end;

{ The constant of memory type ty whose bytes are those of s from offset
  (integers little-endian; bytes past the end are 0). }
function bytesConstant(s: PName; offset: int64; ty: PIRType): TV;
var
  v, i: int64;
  b: longint;
  elems: TPtrList;
begin
  case ty^.kind of
    ikInt:
      begin
        v := 0;
        for i := ty^.bits div 8 - 1 downto 0 do
        begin
          if offset + i < s^.len then
            b := ord(s^.chars^[offset + i])
          else
            b := 0;
          v := (v shl 8) or b;
        end;
        bytesConstant := irConstInt(ty, v);
      end;
    ikArray:
      begin
        listInit(elems);
        for i := 0 to ty^.count - 1 do
          listAdd(elems, irBox(bytesConstant(s, offset + i * ty^.elem^.size, ty^.elem)));
        bytesConstant := irConstArray(ty, elems);
        listFree(elems);
      end;
    ikStruct:
      begin
        listInit(elems);
        for i := 0 to ty^.fields.count - 1 do
          listAdd(elems, irBox(bytesConstant(s, offset + ty^.offsets^[i],
                                             PIRType(listGet(ty^.fields, i)))));
        bytesConstant := irConstStruct(ty, elems);
        listFree(elems);
      end;
  else
    bytesConstant := irZero(ty);
  end;
end;

function scalarConstant(const c: TConstValue; t: PType; ty: PIRType): TV;
var
  x: double;
  ch: char;
begin
  case c.kind of
    ckOrdinal:
      if t^.kind = tyReal then
      begin
        x := c.intVal;
        scalarConstant := irConstDouble(x);
      end
      else if t^.kind = tyAnsiString then
      begin
        ch := chr(c.intVal and 255);
        scalarConstant := getAnsiConstant(PCharArray(@ch), 0, 1);
      end
      else if t^.kind = tyShortString then
      begin
        { a char }
        ch := chr(c.intVal and 255);
        scalarConstant := stringInit(PCharArray(@ch), 0, 1, ty, t);
      end
      else
        scalarConstant := irConstInt(ty, c.intVal);
    ckReal: scalarConstant := irConstDouble(c.realVal);
    ckString:
      if t^.kind = tyAnsiString then
        scalarConstant := getAnsiConstant(c.strVal^.chars, 0, c.strVal^.len)
      else if t^.kind = tyRecord then { a TGUID from its string: its bytes }
        scalarConstant := bytesConstant(c.strVal, 0, ty)
      else
        scalarConstant := stringInit(c.strVal^.chars, 0, c.strVal^.len, ty, t);
  else
    scalarConstant := irZero(ty);
  end;
end;

{ The constant for a typed-constant initializer init of type t (memory
  type ty); folded is Sema's value for non-aggregates. }
{ Does t have a record laid out with a field alignment? }
function containsAlignedRecord(t: PType): boolean;
var
  i: longint;
  f: PDecl;
begin
  if t^.kind = tyArray then
    exit(containsAlignedRecord(t^.elementType));
  if t^.kind = tyRecord then
  begin
    if fieldAlignment(t) <> 0 then
      exit(true);
    for i := 0 to t^.allFields.count - 1 do
    begin
      f := PDecl(listGet(t^.allFields, i));
      if (f^.fieldType <> nil) and containsAlignedRecord(f^.fieldType) then
        exit(true);
    end;
  end;
  containsAlignedRecord := false;
end;

{ Writes the bytes of a typed constant's value (init, or folded for a
  scalar) of type t into bytes from offset (little-endian): ordinals,
  reals, short strings and char arrays, and arrays and records of them. }
procedure writeConstantBytes(init: PNode; const folded: TConstValue; t: PType;
                             bytes: PCharArray; offset: int64);
type
  TRealBits = record
    case boolean of
      true: (d: double);
      false: (bits: int64);
  end;
var
  i: longint;
  el: PAggElement;
  elem: PType;
  stride, size: int64;
  f: PDecl;
  ignored: PIRType;
  rb: TRealBits;
  ch: char;

  procedure putInt(v: int64);
  var
    b: int64;
  begin
    for b := 0 to size - 1 do
      bytes^[offset + b] := chr(longint((v shr (8 * b)) and 255));
  end;

  procedure putChars(p: PCharArray; n: int64);
  var
    b: int64;
  begin
    if t^.kind = tyShortString then
    begin
      if n > t^.maxLength then
        n := t^.maxLength;
      bytes^[offset] := chr(longint(n));
      for b := 0 to n - 1 do
        bytes^[offset + 1 + b] := p^[b];
      exit;
    end;
    b := 0;
    while (b < n) and (b < size) do
    begin
      bytes^[offset + b] := p^[b];
      b := b + 1;
    end;
  end;

begin
  if (init^.kind = nkAggregate) and (t^.kind = tyArray) then
  begin
    elem := t^.elementType;
    stride := sizeOf_(elem);
    for i := 0 to init^.aggElems.count - 1 do
    begin
      el := PAggElement(listGet(init^.aggElems, i));
      writeConstantBytes(el^.value, el^.folded, elem, bytes, offset + stride * i);
    end;
    exit;
  end;
  if (init^.kind = nkAggregate) and (t^.kind = tyRecord) then
  begin
    ignored := memType(t); { (its field offsets) }
    for i := 0 to init^.aggElems.count - 1 do
    begin
      el := PAggElement(listGet(init^.aggElems, i));
      f := findField(t, el^.field.name);
      if f <> nil then
        writeConstantBytes(el^.value, el^.folded, f^.fieldType, bytes,
                           offset + f^.cgOffset);
    end;
    exit;
  end;
  if init^.kind = nkAggregate then { (a scalar in parentheses) }
  begin
    el := PAggElement(listGet(init^.aggElems, 0));
    writeConstantBytes(el^.value, folded, t, bytes, offset);
    exit;
  end;
  size := sizeOf_(t);
  case folded.kind of
    ckOrdinal:
      if t^.kind = tyReal then
      begin
        rb.d := folded.intVal;
        putInt(rb.bits);
      end
      else if (t^.kind = tyShortString) or isStringType(t) then
      begin
        ch := chr(longint(folded.intVal and 255));
        putChars(PCharArray(@ch), 1);
      end
      else
        putInt(folded.intVal);
    ckReal:
      begin
        rb.d := folded.realVal;
        putInt(rb.bits);
      end;
    ckString:
      putChars(folded.strVal^.chars, folded.strVal^.len);
  else
    begin
    end; { none }
  end;
end;

{ A procedural constant: nil, a routine, or a lambda (with no frame and no
  object), as (function, nil). }
function routineConstant(init: PNode; ty: PIRType): TV;
var
  r: PDecl;
  elems: TPtrList;
begin
  r := nil;
  if init^.kind = nkAddrOf then
  begin
    if init^.addrLambda <> nil then
      r := init^.addrLambda
    else if init^.addrCode <> nil then
      r := init^.addrCode
    else if (init^.inner^.refDecl <> nil) and (init^.inner^.refDecl^.kind = dkRoutine) then
      r := init^.inner^.refDecl;
  end;
  if r = nil then { nil }
    exit(irZero(ty));
  r := canon(r);
  listInit(elems);
  listAdd(elems, irBox(irFuncRef(routineEntry(r))));
  listAdd(elems, irBox(irNull));
  routineConstant := irConstStruct(ty, elems);
  listFree(elems);
end;

function constantFor(init: PNode; const folded: TConstValue; t: PType; ty: PIRType): TV;
var
  i, j: longint;
  el, match: PAggElement;
  f: PDecl;
  fty: PIRType;
  elems: TPtrList;
  img: TBuf;
  n: int64;
begin
  if t^.kind = tyRoutine then
    exit(routineConstant(init, ty));
  if containsAlignedRecord(t) then { its bytes (Sema checked their types) }
  begin
    bufInit(img);
    n := sizeOf_(t);
    for i := 1 to n do
      bufAddChar(img, #0);
    writeConstantBytes(init, folded, t, img.data, 0);
    constantFor := bytesConstant(internChars(img.data, 0, n), 0, ty);
    bufFree(img);
    exit;
  end;
  if init^.kind <> nkAggregate then
    exit(scalarConstant(folded, t, ty));
  listInit(elems);
  if t^.kind = tyArray then
  begin
    for i := 0 to init^.aggElems.count - 1 do
    begin
      el := PAggElement(listGet(init^.aggElems, i));
      listAdd(elems, irBox(constantFor(el^.value, el^.folded, t^.elementType, ty^.elem)));
    end;
    constantFor := irConstArray(ty, elems);
    listFree(elems);
    exit;
  end;
  if t^.kind = tyRecord then
  begin
    { fixed fields only (Sema rejects variant parts); missing ones are 0 }
    for i := 0 to t^.fieldList^.fixed.count - 1 do
    begin
      f := PDecl(listGet(t^.fieldList^.fixed, i));
      fty := PIRType(listGet(ty^.fields, i));
      match := nil;
      for j := 0 to init^.aggElems.count - 1 do
      begin
        el := PAggElement(listGet(init^.aggElems, j));
        if el^.field.name = f^.ident.name then
          match := el;
      end;
      if match <> nil then
        listAdd(elems, irBox(constantFor(match^.value, match^.folded, f^.fieldType, fty)))
      else
        listAdd(elems, irBox(irZero(fty)));
    end;
    constantFor := irConstStruct(ty, elems);
    listFree(elems);
    exit;
  end;
  { a scalar in parentheses }
  listFree(elems);
  el := PAggElement(listGet(init^.aggElems, 0));
  constantFor := constantFor(el^.value, folded, t, ty);
end;

{ A static variable: a typed constant, or a variable of the program or of
  a unit (zero-initialized: ISO leaves them undefined; this makes programs
  deterministic at no cost). }
function definedClass(d: PDecl): PType; forward;

{ A program, unit or class variable (pas.<class>.<name>), or a typed
  constant. }
procedure emitStaticVar(v: PDecl);
var
  ty: PIRType;
  init: TV;
  name: string;
  deferred: boolean;
begin
  ty := memType(v^.varType);
  if v^.cgImported then
    { a variable of a separately compiled unit: declared only }
    v^.cgValue := irBox(irGlobal(nm(v^.cgExport), 'external', ty, irNull, false, false,
                                 true, 0))
  else
  begin
    { one holding procedural values gets its value once the routines are
      declared (finishStaticInits) }
    deferred := (v^.init <> nil) and containsRoutineType(v^.varType);
    if deferred then
    begin
      init := irZero(ty); { (none yet: irSetGlobalInit) }
      listAdd(deferredInits, v);
    end
    else if v^.init <> nil then
      init := constantFor(v^.init, v^.initValue, v^.varType, ty)
    else
      init := irZero(ty);
    if v^.cgExport <> nil then
      v^.cgValue := irBox(irGlobal(nm(v^.cgExport), '', ty, init, false, false, deferred, 0))
    else
    begin
      if v^.isClassVar then
        name := 'pas.' + lowerStr(nm(v^.varClass^.declName)) + '.' + nm(v^.ident.name)
      else
        name := 'pas.' + nm(v^.ident.name);
      v^.cgValue := irBox(irGlobal(name, 'internal', ty, init, false, false, deferred, 0));
    end;
  end;
  { class variables have no debug information }
  if dbgOn and not v^.cgImported and not v^.isClassVar then
    declareGlobalDebugInfo(v, PTVBox(v^.cgValue)^);
end;

function advancedRecord(d: PDecl): PType; forward;

{ decls, each followed by the types declared in it when it is a class or an
  advanced record (recursively). }
procedure addWithNested(d: PDecl; var outList: TPtrList);
var
  c: PType;
  j: longint;
  m: PDecl;
begin
  listAdd(outList, d);
  c := definedClass(d);
  if c = nil then
    c := advancedRecord(d);
  if c <> nil then
    for j := 0 to c^.classMembers.count - 1 do
    begin
      m := PDecl(listGet(c^.classMembers, j));
      if m^.kind = dkType then
        addWithNested(m, outList);
    end;
end;

function withNested(const decls: TPtrList): TPtrList;
var
  outList: TPtrList;
  i: longint;
begin
  listInit(outList);
  for i := 0 to decls.count - 1 do
    addWithNested(PDecl(listGet(decls, i)), outList);
  withNested := outList;
end;

procedure emitGlobals(blk: PBlock);
var
  g: TV;
  i, j: longint;
  d, m: PDecl;
  c: PType;
  decls: TPtrList;
begin
  decls := withNested(blk^.decls);
  for i := 0 to decls.count - 1 do
  begin
    d := PDecl(listGet(decls, i));
    c := definedClass(d);
    if c = nil then
      c := advancedRecord(d); { (an advanced record's, a helper's) }
    if c <> nil then { its class variables }
    begin
      for j := 0 to c^.classMembers.count - 1 do
      begin
        m := PDecl(listGet(c^.classMembers, j));
        if m^.kind = dkVar then
          emitStaticVar(m);
      end;
      continue;
    end;
    if (d^.kind = dkVar) and (d^.absTarget = nil) and d^.varExternal then
    begin
      { a C variable: its symbol, defined elsewhere }
      if not isErrno(d) then
      begin
        if not irFindGlobal(externalVarName(d), g) then
          g := irGlobal(externalVarName(d), 'external', memType(d^.varType), irNull,
                        false, false, true, 0);
        d^.cgValue := irBox(g);
      end;
      continue;
    end;
    if (d^.kind = dkVar) and (d^.absTarget = nil) then
      emitStaticVar(d);
  end;
end;

function paramIRType(p: PDecl): PIRType;
begin
  if (p^.varKind = vkVarParam) or isStructured(p^.varType) or
     (p^.varType^.kind = tyUntyped) or isConstRefPtr(p) then
    paramIRType := irPtrT
  else
    paramIRType := regType(p^.varType);
end;

{ An external (C) routine: no static link, C types (getOrInsertFunction). }
procedure declareExternal(r: PDecl);
var
  sym: string;
  i: longint;
  p: PDecl;
  ret: PIRType;
  params: TPtrList;
  f: PIRFunc;
begin
  if r^.externalName <> nil then
    sym := nm(r^.externalName)
  else
    sym := nm(r^.ident.spelling);
  f := irFindFunction(sym);
  if f = nil then
  begin
    listInit(params);
    for i := 0 to r^.params.count - 1 do
    begin
      p := PDecl(listGet(r^.params, i));
      if (p^.varKind = vkVarParam) or (p^.varType^.kind = tyRoutine) or
         (p^.varType^.kind = tyUntyped) or isConstRefPtr(p) then
        listAdd(params, irPtrT) { (a callback: a function pointer) }
      else
        listAdd(params, regType(p^.varType));
    end;
    ret := retTypeOf(nil);
    if r^.isFunction then
      ret := regType(r^.resultType);
    if r^.isVarArgs then
      f := irDeclareFunction(sym, ret, params, '', irVarArgParams(params))
    else
      f := irDeclareFunction(sym, ret, params, '', '');
    listFree(params);
  end;
  r^.cgFunc := f;
end;

{ The class d (a type declaration) defines, not a forward one, or nil. }
{ the interface d defines (not a forward declaration), or nil }
function definedInterface(d: PDecl): PType;
begin
  definedInterface := nil;
  if (d^.kind = dkType) and (d^.typeSpec <> nil) and
     (d^.typeSpec^.kind = tsInterface) and not d^.typeSpec^.isForwardIntf and
     (d^.declType <> nil) and (d^.declType^.kind = tyInterface) then
    definedInterface := d^.declType;
end;

{ The member table of the advanced record d declares, or nil. }
function advancedRecord(d: PDecl): PType;
begin
  advancedRecord := nil;
  if (d^.kind = dkType) and (d^.typeSpec <> nil) and (d^.typeSpec^.kind = tsHelper) then
  begin
    if (d^.declType <> nil) and (d^.declType^.kind = tyClass) then
      advancedRecord := d^.declType;
    exit;
  end;
  if (d^.kind = dkType) and (d^.declType <> nil) and (d^.declType^.kind = tyRecord) then
    advancedRecord := d^.declType^.memberTable;
end;

function definedClass(d: PDecl): PType;
begin
  definedClass := nil;
  if (d^.kind = dkType) and (d^.typeSpec <> nil) and (d^.typeSpec^.kind = tsClass) and
     not d^.typeSpec^.isForwardClass and (d^.declType <> nil) and
     (d^.declType^.kind = tyClass) then
    definedClass := d^.declType;
end;

{ The parameter types of routine r's function: static link, [Self], the
  parameters. }
procedure routineParamTypes(r: PDecl; var params: TPtrList);
var
  j: longint;
begin
  if needsLink(r) then
    listAdd(params, irPtrT);
  if hasSelf(r) then
    listAdd(params, irPtrT);
  if r^.isFunction and returnsByPointer(r^.resultType) then
    listAdd(params, irPtrT); { where the result goes }
  for j := 0 to r^.params.count - 1 do
    listAdd(params, paramIRType(PDecl(listGet(r^.params, j))));
end;

function routineRetType(r: PDecl): PIRType;
begin
  if r^.isFunction and not returnsByPointer(r^.resultType) then
    routineRetType := regType(r^.resultType)
  else
    routineRetType := irVoidT;
end;

{ A class: its VMT (defined once all methods are declared), and for a
  class of a separately compiled unit its methods' declarations. }
procedure declareClassCG(d: PDecl; c: PType);
var
  ty: PIRType;
  name, linkage: string;
  i: longint;
  m: PDecl;
  params: TPtrList;
begin
  listAdd(allClasses, c);
  ty := irArray(6 + c^.classVTable.count, irPtrT);
  if d^.cgExport = nil then
  begin
    name := 'vmt.' + lowerStr(nm(c^.declName));
    linkage := 'internal';
  end
  else
  begin
    name := nm(d^.cgExport);
    linkage := '';
  end;
  if d^.cgImported then
    linkage := 'external';
  mapPut(vmts, c^.id, irBox(irGlobal(name, linkage, ty, irNull, true, false, true, 0)));
  if not d^.cgImported then
  begin
    listAdd(vmtsToDefine, c);
    exit;
  end;
  for i := 0 to c^.classMembers.count - 1 do
  begin
    m := PDecl(listGet(c^.classMembers, i));
    if (m^.kind = dkRoutine) and not m^.isAbstract and (m^.memberKey = nil) then
    begin
      listInit(params);
      routineParamTypes(m, params);
      m^.cgFunc := irDeclareFunction(nm(m^.cgExport), routineRetType(m), params, '', '');
      listFree(params);
      if (m^.asyncType <> nil) or (m^.iterType <> nil) then
        declareAsync(m, nm(m^.cgExport), true, true);
    end;
  end;
end;

{ An interface's iid: a constant holding its name (length-prefixed),
  pas.<unit>$<name>$iid for an interface of a unit's interface. }
procedure declareInterfaceCG(d: PDecl; t: PType);
var
  b: TBuf;
  name, linkage: string;
  init: TV;
  ty: PIRType;
begin
  bufInit(b);
  bufAddChar(b, chr(t^.declName^.len));
  bufAddChars(b, t^.declName^.chars, 0, t^.declName^.len);
  ty := irArray(b.len, irI8);
  init := irConstBytes(b.data, 0, b.len);
  if d^.cgExport = nil then
  begin
    name := 'iid.' + lowerStr(nm(t^.declName));
    linkage := 'internal';
  end
  else
  begin
    name := nm(d^.cgExport);
    linkage := '';
  end;
  if d^.cgImported then
  begin
    linkage := 'external';
    init := irNull;
  end;
  mapPut(iids, t^.id, irBox(irGlobal(name, linkage, ty, init, true, false,
                                     d^.cgImported, 0)));
  bufFree(b);
end;

{ The interface table of class c (null without interfaces): its
  interfaces' iids and method tables. }
function interfaceTable(c: PType): TV;
var
  name: string;
  entries, methods, params: TPtrList;
  k, j: longint;
  impl: PInterfaceImpl;
  r: PDecl;
  t, cleanupFn: TV;
  cleanup: boolean;
  f: PIRFunc;
begin
  cleanup := hasManagedFields(c);
  if (c^.classInterfaces.count = 0) and not cleanup then
    exit(irNull);
  name := lowerStr(nm(c^.declName));
  cleanupFn := irNull;
  if cleanup then
  begin
    listInit(params);
    listAdd(params, irPtrT);
    f := irCreateFunction('cleanup.' + name, 'internal', irVoidT, params);
    irSetArgName(f, 0, 'self');
    listAdd(cleanupClasses, c);
    listAdd(cleanupFns, f);
    cleanupFn := irFuncRef(f);
  end;
  listInit(entries);
  listAdd(entries, irBox(irConstIntToPtr(c^.classInterfaces.count)));
  listAdd(entries, irBox(cleanupFn));
  for k := 0 to c^.classInterfaces.count - 1 do
  begin
    impl := PInterfaceImpl(listGet(c^.classInterfaces, k));
    listInit(methods);
    for j := 0 to impl^.methods.count - 1 do
    begin
      r := PDecl(listGet(impl^.methods, j));
      if r^.isAbstract then
        listAdd(methods, irBox(irFuncRef(classRuntime('__pas_abstract_error', 'void', ''))))
      else
        listAdd(methods, irBox(irFuncRef(fnOf(r))));
    end;
    t := irGlobal('itable.' + name + '.' + lowerStr(nm(impl^.intf^.declName)), 'internal',
                  irArray(methods.count, irPtrT),
                  irConstArray(irArray(methods.count, irPtrT), methods), true, false,
                  false, 0);
    listFree(methods);
    listAdd(entries, irBox(iidOf(impl^.intf)));
    listAdd(entries, irBox(t));
  end;
  interfaceTable := irGlobal('intfs.' + name, 'internal', irArray(entries.count, irPtrT),
                             irConstArray(irArray(entries.count, irPtrT), entries),
                             true, false, false, 0);
  listFree(entries);
end;

{ Fills in the VMTs: parent VMT, class name, virtual methods. }
{ The static variables holding procedural values: their initial values,
  now that the routines are declared. }
procedure finishStaticInits;
var
  i: longint;
  v: PDecl;
begin
  for i := 0 to deferredInits.count - 1 do
  begin
    v := PDecl(listGet(deferredInits, i));
    irSetGlobalInit(PTVBox(v^.cgValue)^,
                    constantFor(v^.init, v^.initValue, v^.varType, memType(v^.varType)));
  end;
  listClear(deferredInits);
end;

procedure defineVMTs;
var
  i, j: longint;
  c, a: PType;
  r: PDecl;
  elems, anc, ancElems: TPtrList;
  g: PTVBox;
  ancTy, vmtTy: PIRType;
  ancG: TV;
begin
  for i := 0 to vmtsToDefine.count - 1 do
  begin
    c := PType(listGet(vmtsToDefine, i));
    listInit(elems);
    listAdd(elems, irBox(vmtOf(c^.classParent)));
    listAdd(elems, irBox(getPStrConstant(c^.declName^.chars, 0, c^.declName^.len)));
    listAdd(elems, irBox(irConstIntToPtr(instanceSize(c))));
    listAdd(elems, irBox(interfaceTable(c)));
    { the ancestor display: the VMTs from the root down to c (item 12) }
    listInit(anc);
    a := c;
    while a <> nil do
    begin
      listAdd(anc, irBox(vmtOf(a)));
      a := a^.classParent;
    end;
    listInit(ancElems);
    for j := anc.count - 1 downto 0 do
      listAdd(ancElems, listGet(anc, j));
    ancTy := irArray(anc.count, irPtrT);
    ancG := irGlobal('ancestors.' + lowerStr(nm(c^.declName)), 'internal', ancTy,
                     irConstArray(ancTy, ancElems), true, false, false, 8);
    listFree(ancElems);
    listAdd(elems, irBox(irConstIntToPtr(anc.count - 1)));
    listAdd(elems, irBox(ancG));
    for j := 0 to c^.classVTable.count - 1 do
    begin
      r := PDecl(listGet(c^.classVTable, j));
      if r^.objMethod = obDestroy then
        listAdd(elems, irBox(irFuncRef(classRuntime('__pas_tobject_destroy', 'void',
                                                    'ptr,ptr'))))
      else if r^.isAbstract then
        listAdd(elems, irBox(irFuncRef(classRuntime('__pas_abstract_error', 'void', ''))))
      else
        listAdd(elems, irBox(irFuncRef(fnOf(r))));
    end;
    vmtTy := irArray(elems.count, irPtrT);
    g := PTVBox(mapGet(vmts, c^.id));
    irSetGlobalInit(g^, irConstArray(vmtTy, elems));
    listFree(elems);
    listFree(anc);
  end;
  listClear(vmtsToDefine);
end;

{ --- Pin analysis (optimization2 item 3) -------------------------------- }

{ The variable whose storage a write through e changes: x, r.f and a[i]
  write x, r and a (not the index). An absolute variable writes its root. }
procedure paAddTarget(e: PNode; var outSet: TPtrList); forward;

procedure paAddTarget(e: PNode; var outSet: TPtrList);
begin
  if e = nil then
    exit;
  case e^.kind of
    nkParen:
      paAddTarget(e^.inner, outSet);
    nkNameRef:
      if (e^.refDecl <> nil) and (e^.refDecl^.kind = dkVar) then
      begin
        if e^.refDecl^.absRoot <> nil then
          listAddUnique(outSet, e^.refDecl^.absRoot)
        else
          listAddUnique(outSet, e^.refDecl);
      end
      else if e^.refWith <> nil then
        paAddTarget(e^.refWith^.rec, outSet);
    nkField:
      paAddTarget(e^.base, outSet);
    nkIndex:
      paAddTarget(e^.base, outSet);
    nkDeref:
      { the pointee changes; keep the pointer in memory too }
      paAddTarget(e^.inner, outSet);
  end;
end;

procedure paExpr(e: PNode; var used, written: TPtrList;
                 collectUsed, collectWritten: boolean); forward;
procedure paStmt(s: PNode; var used, written: TPtrList;
                 collectUsed, collectWritten: boolean); forward;

{ The variables an assignment target reads to compute its address: a[i]
  reads a and i, p^ reads p, r.f reads r, but x := e does not read x. }
procedure paAddTargetReads(e: PNode; var used: TPtrList);
begin
  if e = nil then
    exit;
  case e^.kind of
    nkParen:
      paAddTargetReads(e^.inner, used);
    nkNameRef:
      ;
    nkField:
      paExpr(e^.base, used, paDummy, true, false);
    nkIndex:
      begin
        paExpr(e^.base, used, paDummy, true, false);
        paExpr(e^.index, used, paDummy, true, false);
      end;
    nkDeref:
      paExpr(e^.inner, used, paDummy, true, false);
  end;
end;

procedure paList(const l: TPtrList; var used, written: TPtrList;
                 collectUsed, collectWritten: boolean);
var
  i: longint;
begin
  for i := 0 to l.count - 1 do
    paStmt(PNode(listGet(l, i)), used, written, collectUsed, collectWritten);
end;

procedure paCallWrites(d: PDecl; const args: TPtrList; var written: TPtrList);
var
  i: longint;
  p: PDecl;
  procedure mark(idx: longint);
  begin
    if idx < args.count then
      paAddTarget(PNode(listGet(args, idx)), written);
  end;
begin
  if (d <> nil) and (d^.kind = dkBuiltin) then
  begin
    case d^.builtin of
      bkWrite, bkWriteln, bkHalt, bkBreak, bkContinue:
        exit;
      bkExit:
        if (args.count > 0) and (paCurResult <> nil) then
          listAddUnique(written, paCurResult);
      bkInc, bkDec, bkSetLength, bkDelete, bkInsert, bkGetMem, bkFreeMem,
      bkReallocMem, bkFillChar, bkNew, bkDispose, bkAssign, bkAssignFile,
      bkReset, bkRewrite, bkGet, bkPut, bkPage, bkClose, bkCloseFile,
      bkFlush, bkSeek:
        mark(0);
      bkMove:
        mark(1);
      bkBlockRead, bkBlockWrite:
        for i := 1 to args.count - 1 do
          mark(i);
      bkPack, bkUnpack:
        for i := 0 to args.count - 1 do
          mark(i);
    else
      for i := 0 to args.count - 1 do
        mark(i);
    end;
    exit;
  end;
  if (d <> nil) and (d^.kind = dkRoutine) then
  begin
    for i := 0 to args.count - 1 do
    begin
      if i >= d^.params.count then
        break;
      p := PDecl(listGet(d^.params, i));
      if (p^.varKind = vkVarParam) or p^.isOut then
        mark(i);
    end;
    exit;
  end;
  { a procedural value or an unknown declaration }
  for i := 0 to args.count - 1 do
    mark(i);
end;

procedure paExpr(e: PNode; var used, written: TPtrList;
                 collectUsed, collectWritten: boolean);
var
  i: longint;
  el: PAggElement;
  se: PSetElement;
begin
  if e = nil then
    exit;
  case e^.kind of
    nkNameRef:
      if (e^.refDecl <> nil) and (e^.refDecl^.kind = dkVar) then
      begin
        if collectUsed then
          if e^.refDecl^.absRoot <> nil then
            listAddUnique(used, e^.refDecl^.absRoot)
          else
            listAddUnique(used, e^.refDecl);
      end
      else if e^.refWith <> nil then
        paExpr(e^.refWith^.rec, used, written, collectUsed, collectWritten);
    nkAddrOf:
      begin
        { the address escapes: a call inside the protected code may write it }
        if collectWritten then
          paAddTarget(e^.inner, written);
        paExpr(e^.inner, used, written, collectUsed, collectWritten);
      end;
    nkParen, nkDeref, nkMove:
      paExpr(e^.inner, used, written, collectUsed, collectWritten);
    nkUnary:
      paExpr(e^.operand, used, written, collectUsed, collectWritten);
    nkBinary:
      begin
        paExpr(e^.lhs, used, written, collectUsed, collectWritten);
        paExpr(e^.rhs, used, written, collectUsed, collectWritten);
      end;
    nkCall:
      begin
        if collectWritten then
        begin
          paCallWrites(e^.callDecl, e^.args, written);
          if e^.callWith <> nil then
            paAddTarget(e^.callWith^.rec, written);
          paAddTarget(e^.calleeExpr, written);
        end;
        if e^.callWith <> nil then
          paExpr(e^.callWith^.rec, used, written, collectUsed, collectWritten);
        paExpr(e^.calleeExpr, used, written, collectUsed, collectWritten);
        for i := 0 to e^.args.count - 1 do
          paExpr(PNode(listGet(e^.args, i)), used, written,
                 collectUsed, collectWritten);
      end;
    nkIndirectCall:
      begin
        if collectWritten then
        begin
          for i := 0 to e^.args.count - 1 do
            paAddTarget(PNode(listGet(e^.args, i)), written);
          paAddTarget(e^.calleeExpr, written);
        end;
        paExpr(e^.calleeExpr, used, written, collectUsed, collectWritten);
        for i := 0 to e^.args.count - 1 do
          paExpr(PNode(listGet(e^.args, i)), used, written,
                 collectUsed, collectWritten);
      end;
    nkFormatted:
      begin
        paExpr(e^.fmtValue, used, written, collectUsed, collectWritten);
        paExpr(e^.fmtWidth, used, written, collectUsed, collectWritten);
        paExpr(e^.fmtPrecision, used, written, collectUsed, collectWritten);
      end;
    nkIndex:
      begin
        paExpr(e^.base, used, written, collectUsed, collectWritten);
        paExpr(e^.index, used, written, collectUsed, collectWritten);
      end;
    nkField:
      paExpr(e^.base, used, written, collectUsed, collectWritten);
    nkSetCtor:
      for i := 0 to e^.setElems.count - 1 do
      begin
        se := PSetElement(listGet(e^.setElems, i));
        paExpr(se^.lo, used, written, collectUsed, collectWritten);
        paExpr(se^.hi, used, written, collectUsed, collectWritten);
      end;
    nkAggregate:
      for i := 0 to e^.aggElems.count - 1 do
      begin
        el := PAggElement(listGet(e^.aggElems, i));
        paExpr(el^.value, used, written, collectUsed, collectWritten);
      end;
    nkTemplate:
      for i := 0 to e^.tmplParts.count - 1 do
        paExpr(PNode(listGet(e^.tmplParts, i)), used, written,
               collectUsed, collectWritten);
    nkTuple:
      for i := 0 to e^.tupElems.count - 1 do
        paExpr(PNode(listGet(e^.tupElems, i)), used, written,
               collectUsed, collectWritten);
    nkNamedArg:
      paExpr(e^.naValue, used, written, collectUsed, collectWritten);
    nkCondExpr:
      begin
        paExpr(e^.ceCond, used, written, collectUsed, collectWritten);
        paExpr(e^.ceThen, used, written, collectUsed, collectWritten);
        paExpr(e^.ceElse, used, written, collectUsed, collectWritten);
      end;
    nkAwait:
      paExpr(e^.awaitOperand, used, written, collectUsed, collectWritten);
    nkSpawn:
      paExpr(e^.spawnCall, used, written, collectUsed, collectWritten);
    nkSafeNav:
      begin
        paExpr(e^.navBase, used, written, collectUsed, collectWritten);
        paExpr(e^.navAccess, used, written, collectUsed, collectWritten);
      end;
    nkCoalesce:
      begin
        paExpr(e^.coLeft, used, written, collectUsed, collectWritten);
        paExpr(e^.coRight, used, written, collectUsed, collectWritten);
      end;
    nkInherited:
      for i := 0 to e^.inhArgs.count - 1 do
        paExpr(PNode(listGet(e^.inhArgs, i)), used, written,
               collectUsed, collectWritten);
  end;
end;

procedure paStmt(s: PNode; var used, written: TPtrList;
                 collectUsed, collectWritten: boolean);
var
  i, j: longint;
  arm: PCaseArm;
  lbl: PCaseLabel;
  w: PWithContext;
  h: PExceptHandler;
begin
  if s = nil then
    exit;
  case s^.kind of
    nkEmpty, nkGoto:
      ;
    nkCompound:
      paList(s^.stmts, used, written, collectUsed, collectWritten);
    nkProcCall:
      begin
        if collectWritten then
        begin
          paCallWrites(s^.callDecl, s^.args, written);
          if s^.callWith <> nil then
            paAddTarget(s^.callWith^.rec, written);
          paAddTarget(s^.calleeExpr, written);
        end;
        if s^.callWith <> nil then
          paExpr(s^.callWith^.rec, used, written, collectUsed, collectWritten);
        paExpr(s^.calleeExpr, used, written, collectUsed, collectWritten);
        for i := 0 to s^.args.count - 1 do
          paExpr(PNode(listGet(s^.args, i)), used, written,
                 collectUsed, collectWritten);
      end;
    nkIndirectCallStmt:
      paExpr(s^.calleeExpr, used, written, collectUsed, collectWritten);
    nkAssign:
      begin
        if collectWritten and
           not (paSkipPlain and (plainAssignTarget(s^.target) <> nil)) then
          paAddTarget(s^.target, written);
        if collectUsed then
        begin
          if s^.assignCompound then
            paExpr(s^.target, used, written, collectUsed, collectWritten)
          else
            paAddTargetReads(s^.target, used);
        end;
        paExpr(s^.value, used, written, collectUsed, collectWritten);
      end;
    nkIf:
      begin
        paExpr(s^.cond, used, written, collectUsed, collectWritten);
        paStmt(s^.thenStmt, used, written, collectUsed, collectWritten);
        paStmt(s^.elseStmt, used, written, collectUsed, collectWritten);
      end;
    nkWhile:
      begin
        paExpr(s^.cond, used, written, collectUsed, collectWritten);
        paStmt(s^.thenStmt, used, written, collectUsed, collectWritten);
      end;
    nkRepeat:
      begin
        paList(s^.stmts, used, written, collectUsed, collectWritten);
        paExpr(s^.untilCond, used, written, collectUsed, collectWritten);
      end;
    nkFor:
      begin
        if collectWritten then
        begin
          paAddTarget(s^.forVar, written);
          if s^.forInline <> nil then
            listAddUnique(written, s^.forInline);
        end;
        paExpr(s^.forVar, used, written, collectUsed, collectWritten);
        paExpr(s^.forInit, used, written, collectUsed, collectWritten);
        paExpr(s^.forFinal, used, written, collectUsed, collectWritten);
        paStmt(s^.forBody, used, written, collectUsed, collectWritten);
      end;
    nkForIn:
      begin
        if collectWritten then
        begin
          paAddTarget(s^.inVar, written);
          if s^.inInline <> nil then
            listAddUnique(written, s^.inInline);
          if s^.inElemVar <> nil then
            listAddUnique(written, s^.inElemVar);
          if s^.inCollVar <> nil then
            listAddUnique(written, s^.inCollVar);
          if s^.inEnumVar <> nil then
            listAddUnique(written, s^.inEnumVar);
        end;
        paExpr(s^.inVar, used, written, collectUsed, collectWritten);
        paExpr(s^.inColl, used, written, collectUsed, collectWritten);
        paStmt(s^.inStep, used, written, collectUsed, collectWritten);
        paExpr(s^.inGetEnum, used, written, collectUsed, collectWritten);
        paExpr(s^.inMoveNext, used, written, collectUsed, collectWritten);
        paStmt(s^.inBody, used, written, collectUsed, collectWritten);
      end;
    nkCase:
      begin
        paExpr(s^.selector, used, written, collectUsed, collectWritten);
        for i := 0 to s^.arms.count - 1 do
        begin
          arm := PCaseArm(listGet(s^.arms, i));
          for j := 0 to arm^.labels.count - 1 do
          begin
            lbl := PCaseLabel(listGet(arm^.labels, j));
            paExpr(lbl^.lo, used, written, collectUsed, collectWritten);
            paExpr(lbl^.hi, used, written, collectUsed, collectWritten);
          end;
          paStmt(arm^.body, used, written, collectUsed, collectWritten);
        end;
        paList(s^.elseBody, used, written, collectUsed, collectWritten);
      end;
    nkWith:
      begin
        for i := 0 to s^.withRecords.count - 1 do
        begin
          w := PWithContext(listGet(s^.withRecords, i));
          paExpr(w^.rec, used, written, collectUsed, collectWritten);
        end;
        paStmt(s^.withBody, used, written, collectUsed, collectWritten);
      end;
    nkLabeled:
      paStmt(s^.lblBody, used, written, collectUsed, collectWritten);
    nkTry:
      begin
        paList(s^.tryBody, used, written, collectUsed, collectWritten);
        for i := 0 to s^.handlers.count - 1 do
        begin
          h := PExceptHandler(listGet(s^.handlers, i));
          paStmt(h^.hBody, used, written, collectUsed, collectWritten);
        end;
        paList(s^.partBody, used, written, collectUsed, collectWritten);
      end;
    nkRaise:
      begin
        paExpr(s^.raiseObj, used, written, collectUsed, collectWritten);
        paExpr(s^.raiseAddr, used, written, collectUsed, collectWritten);
        paExpr(s^.raiseFrame, used, written, collectUsed, collectWritten);
      end;
    nkVarDecl:
      begin
        if collectWritten then
          for i := 0 to s^.vdVars.count - 1 do
            listAddUnique(written, PDecl(listGet(s^.vdVars, i)));
        paExpr(s^.vdInit, used, written, collectUsed, collectWritten);
      end;
    nkDestructure:
      begin
        if collectWritten then
        begin
          for i := 0 to s^.dsVars.count - 1 do
            listAddUnique(written, PDecl(listGet(s^.dsVars, i)));
          for i := 0 to s^.dsTargets.count - 1 do
            paAddTarget(PNode(listGet(s^.dsTargets, i)), written);
        end;
        paExpr(s^.dsValue, used, written, collectUsed, collectWritten);
      end;
    nkExprStmt:
      paExpr(s^.exprStmtExpr, used, written, collectUsed, collectWritten);
    nkScope:
      begin
        paExpr(s^.scopeToken, used, written, collectUsed, collectWritten);
        paList(s^.scopeBody, used, written, collectUsed, collectWritten);
      end;
    nkDetached:
      begin
        paExpr(s^.detHandler, used, written, collectUsed, collectWritten);
        paExpr(s^.detCall, used, written, collectUsed, collectWritten);
      end;
    nkYield:
      paExpr(s^.yieldValue, used, written, collectUsed, collectWritten);
  end;
end;

{ --- Ranges of local variables (optimization3 item 4) ------------------ }

{ The variable of a plain x := e: a bare variable name, not reached
  through with and not an absolute overlay. }
function plainAssignTarget(e: PNode): PDecl;
var
  d: PDecl;
begin
  plainAssignTarget := nil;
  if (e = nil) or (e^.kind <> nkNameRef) or (e^.refWith <> nil) then
    exit;
  d := e^.refDecl;
  if (d <> nil) and (d^.kind = dkVar) and (d^.absRoot = nil) then
    plainAssignTarget := d;
end;

{ Sets (or, not known, forgets) the range of a variable of rangeLocals.
  Inside a loop that bounds it, a forgotten range is that bound. }
procedure setVarRange(v: PDecl; known: boolean; lo, hi: int64);
begin
  if not known and v^.cgLoopKnown then
  begin
    known := true;
    lo := v^.cgLoopLo;
    hi := v^.cgLoopHi;
  end;
  v^.cgRangeKnown := known;
  if known then
  begin
    v^.cgRangeLo := lo;
    v^.cgRangeHi := hi;
  end;
end;

{ The variables of rangeLocals that s may write. }
procedure rangeLocalsWrittenBy(s: PNode; var outl: TPtrList);
var
  used, all: TPtrList;
  i: longint;
  v: PDecl;
begin
  listInit(used);
  listInit(all);
  paStmt(s, used, all, false, true);
  listInit(outl);
  for i := 0 to all.count - 1 do
  begin
    v := PDecl(listGet(all, i));
    if v^.cgRangeLocal then
      listAdd(outl, v);
  end;
  listFree(used);
  listFree(all);
end;

procedure forgetRanges(const l: TPtrList);
var
  i: longint;
begin
  for i := 0 to l.count - 1 do
    setVarRange(PDecl(listGet(l, i)), false, 0, 0);
end;

{ An integer constant written as a literal, a named constant or one of
  those negated: its value. }
function constIntOf(e: PNode; var value: int64): boolean;
var
  x: int64;
  d: PDecl;
begin
  constIntOf := false;
  if e = nil then
    exit;
  case e^.kind of
    nkParen:
      constIntOf := constIntOf(e^.inner, value);
    nkIntegerLit:
      { (as an unsigned 64-bit value: above high(int64), none) }
      if e^.intValue >= 0 then
      begin
        value := e^.intValue;
        constIntOf := true;
      end;
    nkUnary:
      if (e^.uop <> uoNot) and constIntOf(e^.operand, x) and
         (x <> low(int64)) then
      begin
        if e^.uop = uoMinus then
          value := -x
        else
          value := x;
        constIntOf := true;
      end;
    nkNameRef:
      begin
        d := e^.refDecl;
        if (d <> nil) and (d^.kind = dkConst) and
           (d^.constValue.kind = ckOrdinal) and (d^.constType <> nil) and
           isIntegerLike(d^.constType) then
        begin
          value := d^.constValue.intVal;
          constIntOf := true;
        end;
      end;
  end;
end;

{ v := v + c, v := c + v or v := v - c, for a constant c: its step. }
function accumulatorStep(s: PNode; v: PDecl; var step: int64): boolean;
var
  e: PNode;
begin
  accumulatorStep := false;
  e := s^.value;
  if (e = nil) or (e^.kind <> nkBinary) then
    exit;
  if e^.bop = boAdd then
  begin
    if (plainAssignTarget(e^.lhs) = v) and constIntOf(e^.rhs, step) then
      exit(true);
    accumulatorStep := (plainAssignTarget(e^.rhs) = v) and constIntOf(e^.lhs, step);
  end
  else if (e^.bop = boSub) and (plainAssignTarget(e^.lhs) = v) and
          constIntOf(e^.rhs, step) and (step <> low(int64)) then
  begin
    step := -step;
    accumulatorStep := true;
  end;
end;

{ Does every write of v in s (a for loop's body) add a constant to it, each
  at most once per iteration (not inside an inner loop)? Adds the possible
  change per iteration to dLo..dHi; false if that overflows int64. }
function accumulatorSteps(s: PNode; v: PDecl; var dLo, dHi: int64): boolean;
var
  i: longint;
  step, r: int64;
  arm: PCaseArm;
  used, w: TPtrList;
begin
  accumulatorSteps := true;
  if s = nil then
    exit;
  case s^.kind of
    nkCompound:
      begin
        for i := 0 to s^.stmts.count - 1 do
          if not accumulatorSteps(PNode(listGet(s^.stmts, i)), v, dLo, dHi) then
            exit(false);
      end;
    nkAssign:
      begin
        if plainAssignTarget(s^.target) <> v then
          exit; { (v is only written by assignments to it) }
        if not accumulatorStep(s, v, step) then
          exit(false);
        if step < 0 then
        begin
          if addOverflows(dLo, step, r) then
            exit(false);
          dLo := r;
        end
        else
        begin
          if addOverflows(dHi, step, r) then
            exit(false);
          dHi := r;
        end;
      end;
    nkIf:
      accumulatorSteps := accumulatorSteps(s^.thenStmt, v, dLo, dHi) and
                          accumulatorSteps(s^.elseStmt, v, dLo, dHi);
    nkCase:
      begin
        for i := 0 to s^.arms.count - 1 do
        begin
          arm := PCaseArm(listGet(s^.arms, i));
          if not accumulatorSteps(arm^.body, v, dLo, dHi) then
            exit(false);
        end;
        for i := 0 to s^.elseBody.count - 1 do
          if not accumulatorSteps(PNode(listGet(s^.elseBody, i)), v, dLo, dHi) then
            exit(false);
      end;
  else
    begin
      { anything else (an inner loop, a try statement...): v not written }
      listInit(used);
      listInit(w);
      paStmt(s, used, w, false, true);
      accumulatorSteps := not listHasPtr(w, v);
      listFree(used);
      listFree(w);
    end;
  end;
end;

{ Chooses rangeLocals for r, each starting at 0 (the value every scalar
  local starts with, see emitZeroInit). None in a routine with a label (a
  goto may arrive with any value). }
procedure initRangeLocals(r: PDecl);
var
  i: longint;
  v: PDecl;
  used, other: TPtrList;
begin
  listClear(rangeLocals);
  for i := 0 to r^.body^.decls.count - 1 do
    if PDecl(listGet(r^.body^.decls, i))^.kind = dkLabel then
      exit;
  listInit(used);
  listInit(other);
  paSkipPlain := true;
  paStmt(r^.body^.body, used, other, false, true);
  paSkipPlain := false;
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if (v^.kind <> dkVar) or (v^.varKind <> vkVariable) or
       ((v^.init <> nil) and not v^.isInitVar) or v^.isInitVar or
       v^.isInlineVar or (v^.absTarget <> nil) or v^.captured or
       v^.isOwned or listHasPtr(other, v) or (v^.varType = nil) then
      continue;
    if not isOrdinal(v^.varType) or not isSignedOrdinal(v^.varType) then
      continue;
    v^.cgRangeLocal := true;
    v^.cgRangeKnown := true;
    v^.cgRangeLo := 0;
    v^.cgRangeHi := 0;
    listAdd(rangeLocals, v);
  end;
  listFree(used);
  listFree(other);
end;

{ After the routine's body: nothing followed any more. }
procedure endRangeLocals;
var
  i: longint;
  v: PDecl;
begin
  for i := 0 to rangeLocals.count - 1 do
  begin
    v := PDecl(listGet(rangeLocals, i));
    v^.cgRangeLocal := false;
    v^.cgRangeKnown := false;
    v^.cgLoopKnown := false;
  end;
  listClear(rangeLocals);
end;

procedure paPinStmt(s: PNode; const after: TPtrList; var pins: TPtrList); forward;

procedure paPinList(const l: TPtrList; const after: TPtrList;
                    var pins: TPtrList);
var
  i: longint;
  a: TPtrList;
begin
  listInit(a);
  listAppendUnique(a, after);
  for i := l.count - 1 downto 0 do
  begin
    paPinStmt(PNode(listGet(l, i)), a, pins);
    paStmt(PNode(listGet(l, i)), a, paDummy, true, false);
  end;
  listFree(a);
end;

procedure paPinStmt(s: PNode; const after: TPtrList; var pins: TPtrList);
var
  i: longint;
  landing, written, loopAfter, dummy: TPtrList;
  arm: PCaseArm;
  h: PExceptHandler;
  v: PDecl;
begin
  if s = nil then
    exit;
  case s^.kind of
    nkCompound:
      paPinList(s^.stmts, after, pins);
    nkTry:
      begin
        listInit(landing);
        listAppendUnique(landing, after);
        listInit(dummy);
        listInit(written);
        for i := 0 to s^.handlers.count - 1 do
        begin
          h := PExceptHandler(listGet(s^.handlers, i));
          paStmt(h^.hBody, landing, dummy, true, false);
        end;
        paList(s^.partBody, landing, dummy, true, false);
        paList(s^.tryBody, dummy, written, false, true);
        for i := 0 to written.count - 1 do
        begin
          v := PDecl(listGet(written, i));
          if listHasPtr(landing, v) then
            listAddUnique(pins, v);
        end;
        paPinList(s^.tryBody, landing, pins);
        for i := 0 to s^.handlers.count - 1 do
        begin
          h := PExceptHandler(listGet(s^.handlers, i));
          paPinStmt(h^.hBody, after, pins);
        end;
        paPinList(s^.partBody, after, pins);
        listFree(landing);
        listFree(written);
      end;
    nkIf:
      begin
        paPinStmt(s^.thenStmt, after, pins);
        paPinStmt(s^.elseStmt, after, pins);
      end;
    nkWhile:
      begin
        listInit(loopAfter);
        listAppendUnique(loopAfter, after);
        paExpr(s^.cond, loopAfter, paDummy, true, false);
        paStmt(s^.thenStmt, loopAfter, paDummy, true, false);
        paPinStmt(s^.thenStmt, loopAfter, pins);
        listFree(loopAfter);
      end;
    nkRepeat:
      begin
        listInit(loopAfter);
        listAppendUnique(loopAfter, after);
        paList(s^.stmts, loopAfter, paDummy, true, false);
        paExpr(s^.untilCond, loopAfter, paDummy, true, false);
        paPinList(s^.stmts, loopAfter, pins);
        listFree(loopAfter);
      end;
    nkFor:
      begin
        listInit(loopAfter);
        listAppendUnique(loopAfter, after);
        paExpr(s^.forVar, loopAfter, paDummy, true, false);
        paExpr(s^.forInit, loopAfter, paDummy, true, false);
        paExpr(s^.forFinal, loopAfter, paDummy, true, false);
        paStmt(s^.forBody, loopAfter, paDummy, true, false);
        paPinStmt(s^.forBody, loopAfter, pins);
        listFree(loopAfter);
      end;
    nkForIn:
      begin
        listInit(loopAfter);
        listAppendUnique(loopAfter, after);
        paExpr(s^.inVar, loopAfter, paDummy, true, false);
        paExpr(s^.inColl, loopAfter, paDummy, true, false);
        paStmt(s^.inBody, loopAfter, paDummy, true, false);
        paPinStmt(s^.inBody, loopAfter, pins);
        listFree(loopAfter);
      end;
    nkCase:
      begin
        for i := 0 to s^.arms.count - 1 do
        begin
          arm := PCaseArm(listGet(s^.arms, i));
          paPinStmt(arm^.body, after, pins);
        end;
        paPinList(s^.elseBody, after, pins);
      end;
    nkWith:
      paPinStmt(s^.withBody, after, pins);
    nkLabeled:
      paPinStmt(s^.lblBody, after, pins);
    nkScope:
      paPinList(s^.scopeBody, after, pins);
  end;
end;

{ optimization3 item 1: `Result := F(...)` where F returns a record by
  pointer: this function's own result pointer is passed on to F, instead of
  a temporary that is then copied. The pointer is `noalias` (the caller's
  fresh temporary), so nothing else can see the record while F writes it.
  Not done when the routine has a try or a non-local label (an exception
  caught here could see a half-written result), when the result variable
  is captured (a nested routine could read it), or when an argument
  mentions the result (F zeroes and fills it before it copies its value
  parameters). Returns whether the assignment was emitted. }
function emitResultInPlace(target, value: PNode; tt: PType): boolean;
var
  rv, r: PDecl;
  c: PNode;
  used: TPtrList;
  i: longint;
  dst, res: TV;
begin
  emitResultInPlace := false;
  if (not returnsByPointer(tt)) or (curRoutine = nil) or pinTemps then
    exit;
  if not curRoutine^.isFunction then
    exit;
  rv := curRoutine^.resultVar;
  if (target^.kind <> nkNameRef) or (target^.refWith <> nil) or
     (target^.refDecl <> rv) or (not isByRef(rv)) or rv^.cgInFrame or
     rv^.captured then
    exit;
  c := value;
  while c^.kind = nkParen do
    c := c^.inner;
  if (c^.kind <> nkCall) or (c^.methodCast <> 0) or c^.isVarCast or
     (c^.callValue <> nil) or (c^.callWith <> nil) then
    exit;
  r := c^.callDecl;
  if (r = nil) or (r^.kind <> dkRoutine) then
    exit;
  if (not r^.isFunction) or (r^.methodKind <> mtNone) or r^.isStatic or
     r^.isExternal or (r^.asyncType <> nil) or (r^.iterType <> nil) or
     (r^.resultType <> tt) then
    exit;
  listInit(used);
  for i := 0 to c^.args.count - 1 do
    paExpr(PNode(listGet(c^.args, i)), used, paDummy, true, false);
  for i := 0 to r^.params.count - 1 do
    paExpr(PDecl(listGet(r^.params, i))^.paramDefault, used, paDummy, true, false);
  if listHasPtr(used, rv) then
  begin
    listFree(used);
    exit;
  end;
  listFree(used);
  dst := emitAddress(target);
  resultDst := dst;
  hasResultDst := true;
  res := emitCall(r, c^.args);
  emitResultInPlace := true;
end;

procedure computePinVars(r: PDecl; var pins: TPtrList);
var
  after, writtenAll: TPtrList;
  i: longint;
  v: PDecl;
  owned: boolean;
begin
  listInit(pins);
  listInit(after);
  paCurResult := nil;
  if r^.isFunction and (r^.resultVar <> nil) then
  begin
    paCurResult := r^.resultVar;
    listAddUnique(after, r^.resultVar);
  end;
  { owned variables are freed by an implicit try-finally around the body;
    its landing reads them, and any write to one happens inside it }
  owned := false;
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if v^.kind = dkVar then
    begin
      { a file local is closed on the way out, after the landing }
      if (v^.varType <> nil) and containsFile(v^.varType) then
        listAddUnique(after, v);
      if v^.isOwned then
      begin
        listAddUnique(after, v);
        owned := true;
      end;
    end;
  end;
  if owned then
  begin
    listInit(writtenAll);
    paStmt(r^.body^.body, paDummy, writtenAll, false, true);
    for i := 0 to writtenAll.count - 1 do
    begin
      v := PDecl(listGet(writtenAll, i));
      if listHasPtr(after, v) then
        listAddUnique(pins, v);
    end;
    listFree(writtenAll);
  end;
  paPinStmt(r^.body^.body, after, pins);
  listFree(after);
end;

procedure declareRoutine(r: PDecl); forward;

{ Lays out the frames of the routines declared in blk (recursively),
  creates their functions and queues their bodies, and declares the
  classes' VMTs. A method is declared where its implementation is. }
{ The C-callable entry of a cdecl routine r at program or unit level (its
  procedural values point to it): cthunk.<sym>, which calls r with a null
  static link; created right after r, its body emitted with the
  routines'. }
{ The function a procedural value of r points to: the routine itself when
  it takes the link (an external routine, a nested one, or a cdecl one's
  C entry), else a thunk with the procedural value's (link, params)
  signature that ignores the link (item 20). }
function routineEntry(r: PDecl): PIRFunc;
var
  params: TPtrList;
  f, target: PIRFunc;
  i, argNo: longint;
  p: PDecl;
begin
  r := canon(r);
  if r^.cgThunk <> nil then
    exit(PIRFunc(r^.cgThunk));
  if r^.isExternal then
    exit(PIRFunc(r^.cgFunc));
  if needsLink(r) then
    exit(fnOf(r));
  target := fnOf(r);
  listInit(params);
  listAdd(params, irPtrT);
  if r^.isFunction and returnsByPointer(r^.resultType) then
    listAdd(params, irPtrT);
  for i := 0 to r^.params.count - 1 do
    listAdd(params, paramIRType(PDecl(listGet(r^.params, i))));
  f := irCreateFunction('linkthunk.' + mangle(r), 'internal',
                        routineRetType(r), params);
  listFree(params);
  irSetArgName(f, 0, 'link');
  argNo := 1;
  if r^.isFunction and returnsByPointer(r^.resultType) then
  begin
    irSetArgAttrs(f, argNo, resultParamAttr(r^.resultType));
    argNo := argNo + 1;
  end;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    irSetArgName(f, argNo + i, nm(p^.ident.name));
  end;
  r^.cgThunk := f;
  listAdd(thunksToEmit, r);
  routineEntry := f;
end;

procedure declareCThunk(r: PDecl; const sym: string);
var
  params: TPtrList;
  target, t: PIRFunc;
  i, first: longint;
begin
  if not isCDecl(r) or (r^.methodKind <> mtNone) or (r^.parent <> nil) then
    exit;
  target := fnOf(r);
  listInit(params);
  for i := 0 to target^.params.count - 1 do
    listAdd(params, listGet(target^.params, i));
  t := irCreateFunction('cthunk.' + sym, 'internal', target^.ret, params);
  listFree(params);
  first := 0;
  if r^.isFunction and returnsByPointer(r^.resultType) then
  begin
    first := 1;
    irSetArgAttrs(t, 0, resultParamAttr(r^.resultType));
  end;
  for i := 0 to r^.params.count - 1 do
    irSetArgName(t, i + first, nm(PDecl(listGet(r^.params, i))^.ident.name));
  r^.cgThunk := t;
  listAdd(thunksToEmit, r);
end;

procedure emitCThunk(r: PDecl);
var
  t: PIRFunc;
  buf: TBuf;
  i: longint;
  res: TV;
begin
  t := PIRFunc(r^.cgThunk);
  curFn := t;
  curRoutine := nil;
  curSP := nil;
  irBeginBody(t);
  irSetDebugLoc(nil);
  irSetInsertPoint(irNewBlock('entry'));
  bufInit(buf);
  if isCDecl(r) then
  begin
    for i := 0 to t^.params.count - 1 do
      if (i = 0) and r^.isFunction and returnsByPointer(r^.resultType) then
        irArgsAddAttr(buf, irArg(t, i), resultCallAttr(r^.resultType))
      else
        irArgsAdd(buf, irArg(t, i));
  end
  else { a link thunk: skip its link argument }
  begin
    for i := 1 to t^.params.count - 1 do
      if (i = 1) and r^.isFunction and returnsByPointer(r^.resultType) then
        irArgsAddAttr(buf, irArg(t, i), resultCallAttr(r^.resultType))
      else
        irArgsAdd(buf, irArg(t, i));
  end;
  res := irCall(fnOf(r), buf, '');
  bufFree(buf);
  if t^.ret^.kind = ikVoid then
    irRetVoid
  else
    irRet(res);
end;

procedure emitCThunks;
var
  i: longint;
begin
  for i := 0 to thunksToEmit.count - 1 do
    emitCThunk(PDecl(listGet(thunksToEmit, i)));
  listClear(thunksToEmit);
end;

procedure declareRoutines(blk: PBlock);
var
  i, j: longint;
  r, m: PDecl;
  c: PType;
  params, decls: TPtrList;
begin
  decls := withNested(blk^.decls);
  for i := 0 to decls.count - 1 do
  begin
    r := PDecl(listGet(decls, i));
    c := definedClass(r);
    if c <> nil then
    begin
      declareClassCG(r, c);
      continue;
    end;
    c := definedInterface(r);
    if c <> nil then
    begin
      declareInterfaceCG(r, c);
      continue;
    end;
    c := advancedRecord(r);
    if c <> nil then
    begin
      { a record of a separately compiled unit: its methods, declared }
      for j := 0 to c^.classMembers.count - 1 do
      begin
        m := PDecl(listGet(c^.classMembers, j));
        if (m^.kind = dkRoutine) and m^.cgImported then
        begin
          listInit(params);
          routineParamTypes(m, params);
          m^.cgFunc := irDeclareFunction(nm(m^.cgExport), routineRetType(m), params,
                                         '', '');
          if m^.isFunction and returnsByPointer(m^.resultType) then
            irSetArgAttrs(PIRFunc(m^.cgFunc), resultArgIndex(m),
                          resultParamAttr(m^.resultType));
          listFree(params);
        end;
      end;
      continue;
    end;
    if (r^.kind = dkRoutine) and (r^.canonical <> nil) and
       (r^.canonical^.methodKind <> mtNone) then
    begin
      declareRoutine(r^.canonical); { a method's implementation }
      continue;
    end;
    if (r^.kind <> dkRoutine) or (r^.canonical <> nil) or r^.isGeneric then
      continue; { the forward declaration is the canonical node; a generic is
                  compiled as its specializations }
    if r^.isExternal then
    begin
      declareExternal(r);
      continue;
    end;
    if r^.cgImported then
    begin
      { a routine of a separately compiled unit: declared only }
      listInit(params);
      routineParamTypes(r, params);
      r^.cgFunc := irDeclareFunction(nm(r^.cgExport), routineRetType(r), params, '', '');
      if r^.isFunction and returnsByPointer(r^.resultType) then
        irSetArgAttrs(PIRFunc(r^.cgFunc), resultArgIndex(r),
                      resultParamAttr(r^.resultType));
      listFree(params);
      declareCThunk(r, nm(r^.cgExport));
      if (r^.asyncType <> nil) or (r^.iterType <> nil) then
        declareAsync(r, nm(r^.cgExport), true, true);
      continue;
    end;
    declareRoutine(r);
  end;
end;

{ The function and frame of routine r (of a method: its canonical
  declaration): frame [static link], [Self], parameters, locals, [result],
  [jmp_buf]. Every routine takes its static link first (null at the outer
  level), so procedural parameters can call any routine the same way. }
procedure declareRoutine(r: PDecl);
var
  j, argNo: longint;
  v, p: PDecl;
  slots, params, pinVars: TPtrList;
  name, dbgName: string;
  f: PIRFunc;
  ret: PIRType;
  loc: TLoc;
  hasNested, pinAll: boolean;

  procedure addSlot(x: PDecl);
  begin
    x^.cgLocal := nil;
    x^.cgInFrame := inFrame(x, pinAll, pinVars);
    if not x^.cgInFrame then
      exit;
    x^.cgSlot := slots.count;
    if isByRef(x) then
      listAdd(slots, irPtrT)
    else
      listAdd(slots, memType(x^.varType));
  end;

begin
  begin
    { frame record: [static link], then those of [Self], params, locals
      and [result] that have to be in it (see inFrame); the link is there
      for the routines nested in r, which follow it to r's parents }
    listInit(slots);
    hasNested := false;
    for j := 0 to r^.body^.decls.count - 1 do
    begin
      v := PDecl(listGet(r^.body^.decls, j));
      if (v^.kind = dkRoutine) and not v^.isExternal then
        hasNested := true;
    end;
    r^.cgLinkInFrame := (r^.parent <> nil) and hasNested;
    if r^.cgLinkInFrame then
      listAdd(slots, irPtrT);
    pinAll := hasNonLocalLabels(r^.body);
    listInit(pinVars);
    if not pinAll and r^.hasTry then
      computePinVars(r, pinVars);
    if hasSelf(r) then
      addSlot(r^.selfVar);
    for j := 0 to r^.params.count - 1 do
      addSlot(PDecl(listGet(r^.params, j)));
    for j := 0 to r^.body^.decls.count - 1 do
    begin
      v := PDecl(listGet(r^.body^.decls, j));
      if (v^.kind <> dkVar) or (v^.absTarget <> nil) then
        continue;
      if (v^.init <> nil) and not v^.isInitVar then
        emitStaticVar(v) { static storage, local scope }
      else
        addSlot(v);
    end;
    if r^.isFunction then
      addSlot(r^.resultVar);
    listFree(pinVars);
    name := mangle(r);
    r^.cgFrame := nil;
    if slots.count > 0 then
      r^.cgFrame := irStructNamed('frame.' + copy(name, 5, 255), slots);
    r^.cgJmpSlot := -1;
    if hasNonLocalLabels(r^.body) then
    begin
      r^.cgJmpSlot := slots.count;
      listAdd(slots, jmpBufT);
      r^.cgFrame := irStructNamed('frame.' + copy(name, 5, 255) + '.jb', slots);
    end;
    listFree(slots);

    listInit(params);
    routineParamTypes(r, params);
    ret := routineRetType(r);
    if r^.cgExport <> nil then
      f := irCreateFunction(nm(r^.cgExport), '', ret, params)
    else
      f := irCreateFunction(name, 'internal', ret, params);
    listFree(params);
    argNo := 0;
    if needsLink(r) then
    begin
      irSetArgName(f, 0, 'link');
      argNo := 1;
    end;
    if r^.isFunction and returnsByPointer(r^.resultType) then
    begin
      irSetArgName(f, argNo, 'result.ptr');
      irSetArgAttrs(f, argNo, resultParamAttr(r^.resultType));
      argNo := argNo + 1;
    end;
    if hasSelf(r) then
    begin
      irSetArgName(f, argNo, 'self');
      if (r^.ownerClass <> nil) and
         ((r^.ownerClass^.classRecord <> nil) or
          (r^.ownerClass^.classHelperFor <> nil)) then
        irSetArgAttrs(f, argNo, byRefParamAttr(r^.selfVar^.varType));
      argNo := argNo + 1;
    end;
    for j := 0 to r^.params.count - 1 do
    begin
      p := PDecl(listGet(r^.params, j));
      irSetArgName(f, j + argNo, nm(p^.ident.name));
      if isByRef(p) then
        irSetArgAttrs(f, j + argNo, byRefParamAttr(p^.varType));
    end;
    if r^.isInline then { the inline directive }
      f^.attrs := 'inlinehint nounwind';
    r^.cgFunc := f;
    if r^.cgExport <> nil then
      declareCThunk(r, nm(r^.cgExport))
    else
      declareCThunk(r, name);
    if (r^.asyncType <> nil) or (r^.iterType <> nil) then
    begin
      if r^.cgExport <> nil then
        declareAsync(r, nm(r^.cgExport), true, false)
      else
        declareAsync(r, name, false, false);
    end;
    if dbgOn then
    begin
      { file scope even for nested routines: lexical nesting would make
        debuggers look up enclosing variables through DW_AT_static_link,
        which LLVM cannot emit }
      loc := bodyLoc(r);
      dbgName := nm(r^.ident.spelling);
      if r^.methodKind <> mtNone then
        dbgName := nm(r^.ownerClass^.declName) + '.' + dbgName;
      if r^.isFunction then
        r^.cgDbg := createSubprogram(f, dbgName, diFileFor(loc), lineOf(loc),
                                     r^.resultType, r^.params, diFileFor(loc),
                                     r^.selfVar)
      else
        r^.cgDbg := createSubprogram(f, dbgName, diFileFor(loc), lineOf(loc), nil,
                                     r^.params, diFileFor(loc), r^.selfVar);
    end;
    listAdd(routinesToEmit, r);
    declareRoutines(r^.body);
  end;
end;

{ The cleanup function of class c: releases its own fields' COM
  interfaces (the runtime calls it for each class of a freed object). }
procedure emitCleanup(c: PType; f: PIRFunc);
var
  k: longint;
  d: PDecl;
begin
  curFn := f;
  resetAddrAligns;
  curRoutine := nil;
  curSP := nil;
  irBeginBody(f);
  irSetDebugLoc(nil);
  irSetInsertPoint(irNewBlock('entry'));
  for k := 0 to c^.classMembers.count - 1 do
  begin
    d := PDecl(listGet(c^.classMembers, k));
    if (d^.kind = dkField) and isManaged(d^.fieldType) then
      finalizeValue(fieldAddress(irArg(f, 0), c, d), d^.fieldType);
  end;
  irRetVoid;
end;

{ The element functions of the array types used: last of all, as any
  code, the main program's too, can be the first to use a type. }
procedure emitDynOps;
var
  k: longint;
  f: PIRFunc;
  elem: PType;
begin
  for k := 0 to dynOpFns.count - 1 do
  begin
    f := PIRFunc(listGet(dynOpFns, k));
    elem := PType(listGet(dynOpElems, k));
    curFn := f;
    resetAddrAligns;
    curRoutine := nil;
    curSP := nil;
    irBeginBody(f);
    irSetDebugLoc(nil);
    irSetInsertPoint(irNewBlock('entry'));
    if k mod 2 = 0 then
      finalizeValue(irArg(f, 0), elem)
    else
      addRefValue(irArg(f, 0), elem);
    irRetVoid;
  end;
  listClear(dynOpFns);
  listClear(dynOpElems);
end;

procedure emitCleanups;
var
  k: longint;
begin
  for k := 0 to cleanupClasses.count - 1 do
    emitCleanup(PType(listGet(cleanupClasses, k)), PIRFunc(listGet(cleanupFns, k)));
  listClear(cleanupClasses);
  listClear(cleanupFns);
end;

{ The variables of r's frame that hold managed values r releases when it
  returns: its value parameters (one more reference each) and local
  variables. }
procedure managedFrameVars(r: PDecl; var vars: TPtrList);
var
  i: longint;
  v: PDecl;
begin
  for i := 0 to r^.params.count - 1 do
  begin
    v := PDecl(listGet(r^.params, i));
    if not isByRef(v) and not (v^.varKind in [vkConstParam, vkVarParam]) and
       isManaged(v^.varType) then
      listAdd(vars, v);
  end;
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if (v^.kind = dkVar) and ((v^.init = nil) or v^.isInitVar) and
       (v^.absTarget = nil) and isManaged(v^.varType) then
      listAdd(vars, v);
  end;
end;

{ Does r's frame hold its static link, to a frame on the heap? }
function parentFrameOnHeap(r: PDecl): boolean;
begin
  parentFrameOnHeap := r^.cgLinkInFrame and (r^.parent <> nil) and
                       r^.parent^.heapFrame and (frameType(r^.parent) <> nil);
end;

{ __pas_frame_new, __pas_frame_addref / release (runtime/io.c). }
function frameRuntime(const name: string): PIRFunc;
begin
  if name = '__pas_frame_new' then
    frameRuntime := classRuntime(name, 'ptr', 'i64,ptr')
  else
    frameRuntime := classRuntime(name, 'void', 'ptr');
end;

{ r's frame is on the heap (a closure made in it uses it): the function
  that finalizes it when its last reference goes, frame.fini.<r>. It
  releases the managed variables (as r does on its way out when its frame
  is not on the heap) and the frame of r's parent, if that is on the heap
  too (r's frame holds a reference to it, its static link). }
function emitFrameFini(r: PDecl): PIRFunc;
var
  frameT: PIRType;
  f: PIRFunc;
  params, vars: TPtrList;
  frame, ignored: TV;
  i: longint;
  v: PDecl;
begin
  frameT := frameType(r);
  listInit(params);
  listAdd(params, irPtrT);
  f := irCreateFunction('frame.fini.' + copy(mangle(r), 5, 255), 'internal', irVoidT,
                        params);
  listFree(params);
  irSetArgName(f, 0, 'frame');
  curFn := f;
  resetAddrAligns;
  curSP := nil;
  irBeginBody(f);
  irSetDebugLoc(nil);
  irSetInsertPoint(irNewBlock('entry'));
  frame := irArg(f, 0);
  listInit(vars);
  managedFrameVars(r, vars);
  for i := 0 to vars.count - 1 do
  begin
    v := PDecl(listGet(vars, i));
    finalizeValue(irStructGEP(frameT, frame, v^.cgSlot, ''), v^.varType);
  end;
  listFree(vars);
  if parentFrameOnHeap(r) then
    ignored := call1(frameRuntime('__pas_frame_release'),
                     irLoad(irPtrT, irStructGEP(frameT, frame, 0, ''), 'link'), '');
  irRetVoid;
  emitFrameFini := f;
end;

procedure emitAsyncs;
var
  i: longint;
begin
  for i := 0 to asyncsToEmit.count - 1 do
    emitAsyncParts(PDecl(listGet(asyncsToEmit, i)));
  listClear(asyncsToEmit);
end;

procedure emitRoutineBody(r: PDecl);
var
  frameT: PIRType;
  frameFini: PIRFunc;
  i, argNo: longint;
  p, v, res: PDecl;
  arg, slot, ret, excFrame, ignoredV: TV;
  f: PIRFunc;
  entry, raised, retB, beforeBody, bodyB: PIRBlock;
  savedLoc: PMeta;
  managedAddrs, managedTypes: TPtrList; { of PTVBox / PType }
  managedResult, resultByPointer: boolean;
  locals: TPtrList; { of PDecl: the local variables and the result }
  owned: TPtrList;  { of PDecl: the owned variables }
  ownedTry: TTryFinally;

  { an alloca for x unless it is in the frame record }
  procedure addLocal(x: PDecl; isParam: boolean);
  var
    t: PIRType;
    name: string;
  begin
    if x^.cgInFrame then
      exit;
    if isByRef(x) then
      t := irPtrT
    else
      t := memType(x^.varType);
    name := nm(x^.ident.name);
    if isParam then
      name := name + '.addr';
    x^.cgLocal := irBox(irAlloca(t, name));
  end;

  { releases the value parameters', local variables' and temporaries'
    references (a frame on the heap: its variables when its last reference
    goes) }
  procedure finalizeAll;
  var
    k: longint;
  begin
    if frameFini = nil then
      for k := 0 to managedAddrs.count - 1 do
        finalizeValue(PTVBox(listGet(managedAddrs, k))^, PType(listGet(managedTypes, k)));
    for k := 0 to routineTemps.count - 1 do
      clearTemp(PTVBox(listGet(routineTemps, k))^);
  end;

  procedure releaseFrame;
  var
    ignored: TV;
  begin
    if frameFini <> nil then
      ignored := call1(frameRuntime('__pas_frame_release'), curFrame, '');
  end;

begin
  if (r^.asyncType <> nil) or (r^.iterType <> nil) then { (its task's parts, after it) }
    listAdd(asyncsToEmit, r);
  { a frame on the heap: the function that finalizes it, first }
  frameFini := nil;
  if (frameType(r) <> nil) and r^.heapFrame then
    frameFini := emitFrameFini(r);
  listInit(managedAddrs);
  listInit(managedTypes);
  f := fnOf(r);
  curFn := f;
  resetAddrAligns;
  curRoutine := r;
  curSP := PSPInfo(r^.cgDbg);
  irBeginBody(f);
  irSetDebugLoc(nil);
  entry := irNewBlock('entry');
  irSetInsertPoint(entry);
  setDebugLoc(bodyLoc(r));
  { the frame record, if any, then an alloca for each other variable }
  { on the heap if a closure made here uses it (counted: see
    emitFrameFini), zeroed }
  frameT := frameType(r);
  if frameFini <> nil then
    curFrame := call2(frameRuntime('__pas_frame_new'), i64c(frameT^.size),
                      irFuncRef(frameFini), 'frame')
  else if frameT <> nil then
    curFrame := irAlloca(frameT, 'frame')
  else
    curFrame := irNull;
  { execution continues here after an exception (see inFrame) }
  pinTemps := r^.hasTry or hasNonLocalLabels(r^.body);
  if (frameT <> nil) and pinTemps then
    pinInMemory(curFrame);
  listInit(locals); { start from zero }
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if (v^.kind = dkVar) and ((v^.init = nil) or v^.isInitVar) and (v^.absTarget = nil) then
      listAdd(locals, v);
  end;
  if r^.isFunction then
    listAdd(locals, r^.resultVar);
  if hasSelf(r) then
    addLocal(r^.selfVar, true);
  for i := 0 to r^.params.count - 1 do
    addLocal(PDecl(listGet(r^.params, i)), true);
  for i := 0 to locals.count - 1 do
    addLocal(PDecl(listGet(locals, i)), false);
  if dbgOn then
    declareFrameVariables(r);
  resultByPointer := r^.isFunction and isByRef(r^.resultVar);
  for i := 0 to locals.count - 1 do
  begin
    v := PDecl(listGet(locals, i));
    if (v <> r^.resultVar) or not resultByPointer then
      emitZeroInit(varSlot(v), v^.varType);
  end;
  if r^.cgLinkInFrame then
    irStore(irArg(f, 0), irStructGEP(frameT, curFrame, 0, ''));
  if (frameFini <> nil) and parentFrameOnHeap(r) then { the frame holds its parent's }
    ignoredV := call1(frameRuntime('__pas_frame_addref'), irArg(f, 0), '');
  if needsLink(r) then
    argNo := 1
  else
    argNo := 0;
  if resultByPointer then { the caller's temporary }
  begin
    irStore(irArg(f, argNo), varSlot(r^.resultVar));
    argNo := argNo + 1;
    emitZeroInit(varAddress(r^.resultVar), r^.resultVar^.varType);
  end;
  if hasSelf(r) then
  begin
    irStore(irArg(f, argNo), varSlot(r^.selfVar));
    argNo := argNo + 1;
  end;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    arg := irArg(f, i + argNo);
    slot := varSlot(p);
    if isByRef(p) then
      irStore(arg, slot)
    else if p^.varType^.kind = tyShortString then
      { value semantics: the characters in use, cut to fit }
      rtv3('__pas_str_assign', slot, i32c(p^.varType^.maxLength), arg)
    else if isStructured(p^.varType) then
      copyAggregate(slot, arg, p^.varType) { value semantics }
    else
      storeScalar(slot, arg, p^.varType);
    if (p^.varType^.kind = tyOpenArray) and not (p^.varKind in [vkVarParam, vkConstParam]) then
      copyOpenArray(slot, p^.varType);
    { the variables holding COM interfaces: value parameters (one more
      reference each), then local variables (nil at first) }
    if not isByRef(p) and not (p^.varKind in [vkConstParam, vkVarParam]) and
       isManaged(p^.varType) then
    begin
      addRefValue(slot, p^.varType);
      listAdd(managedAddrs, irBox(slot));
      listAdd(managedTypes, p^.varType);
    end;
  end;
  { initialized variables (objfpc) get their initial values on each call }
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if (v^.kind = dkVar) and v^.isInitVar then
      irStore(constantFor(v^.init, v^.initValue, v^.varType, memType(v^.varType)),
              varAddress(v));
  end;
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if (v^.kind = dkVar) and ((v^.init = nil) or v^.isInitVar) and
       (v^.absTarget = nil) and isManaged(v^.varType) then
    begin
      listAdd(managedAddrs, irBox(varAddress(v)));
      listAdd(managedTypes, v^.varType);
    end;
  end;
  managedResult := r^.isFunction and (r^.resultVar <> nil) and
                   isManaged(r^.resultVar^.varType);

  initFileVars(r^.body^.decls, false);
  if r^.cgJmpSlot >= 0 then
    emitSetJmpDispatch(r^.body, irStructGEP(frameT, curFrame, r^.cgJmpSlot, ''));
  { an exception leaving the routine releases them too (and the result):
    the body runs protected, in a block of its own. Whether it needs to is
    only known once it is emitted (can anything it calls raise?), so the
    block before it is finished afterwards. }
  raised := nil;
  beforeBody := nil;
  if (managedAddrs.count > 0) or managedResult or r^.usesManaged or
     (frameFini <> nil) then
  begin
    beforeBody := irInsertBlock;
    bodyB := irNewBlock('managed.body');
    irSetInsertPoint(bodyB);
  end;
  listClear(routineTemps);
  exitBlock := irNewBlock('exit');
  { the owned variables' objects are freed however the body ends, the last
    declared first: the body is the protected part of a try-finally }
  listInit(owned);
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    { (those declared among the statements: with their sequence) }
    if (v^.kind = dkVar) and v^.isOwned and not v^.isInlineVar then
      listAdd(owned, v);
  end;
  if (r^.asyncType = nil) and (r^.iterType = nil) then
    initRangeLocals(r);
  if owned.count = 0 then
    visitStmt(r^.body^.body)
  else
  begin
    tryFinallyBegin(ownedTry);
    visitStmt(r^.body^.body);
    tryFinallyMiddle(ownedTry);
    emitFreeOwned(owned, ownedTry.state);
    tryFinallyEnd(ownedTry);
  end;
  endRangeLocals;
  listFree(owned);
  irBr(exitBlock);
  if beforeBody <> nil then
  begin
    savedLoc := irDebugLoc;
    irSetInsertPoint(beforeBody);
    setDebugLoc(bodyLoc(r));
    { only when the landing has something to release: with nothing, it
      would only raise again, as the exception does without it (and the
      frame costs a setjmp on every call) }
    if ((managedAddrs.count > 0) or managedResult or (routineTemps.count > 0) or
        (frameFini <> nil)) and irMayRaise then
    begin
      if not pinTemps then { what the landing releases }
      begin
        if frameT <> nil then
          pinInMemory(curFrame);
        for i := 0 to routineTemps.count - 1 do
          pinInMemory(PTVBox(listGet(routineTemps, i))^);
      end;
      raised := irNewBlock('managed.raise');
      excFrame := emitProtectIn(raised, bodyB, '');
    end
    else
      irBr(bodyB);
    irSetDebugLoc(savedLoc);
  end;
  irSetInsertPoint(exitBlock);
  if raised <> nil then
    excCall1('__pas_exc_pop', excFrame);
  finalizeAll;
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if (v^.kind = dkVar) and (v^.absTarget = nil) then
      emitFileLifetime(varAddress(v), v^.varType, false, irNull);
  end;
  if raised <> nil then
  begin
    retB := irInsertBlock;
    irSetInsertPoint(raised);
    finalizeAll;
    if managedResult then
      finalizeValue(varAddress(r^.resultVar), r^.resultVar^.varType);
    releaseFrame;
    excCall1('__pas_reraise', excFrame);
    irUnreachable;
    irSetInsertPoint(retB);
  end;
  listClear(routineTemps);
  if r^.isFunction and not resultByPointer then
  begin
    res := r^.resultVar;
    slot := varSlot(res);
    ret := loadScalar(slot, res^.varType, 'result');
    releaseFrame;
    irRet(ret);
  end
  else
  begin
    releaseFrame;
    irRetVoid;
  end;
  if not irMayRaise then
    irSetNoRaise(f);
  pinTemps := true;
end;

{ --- Separate units (S11) --- }

{ The external symbols of unit u's interface variables and routines:
  pas.<unit>$<name>; imported: u was compiled separately (declared only). }
procedure collectExports(u: PDecl; imported: boolean);
var
  i, j: longint;
  d, m: PDecl;
  c: PType;
  prefix: string;
  decls: TPtrList;
begin
  decls := withNested(u^.iface^.decls);
  for i := 0 to decls.count - 1 do
  begin
    d := PDecl(listGet(decls, i));
    c := definedClass(d);
    if c <> nil then
    begin
      { a class: its VMT and its methods }
      mapPut(exportedClasses, c^.id, c);
      prefix := 'pas.' + nm(u^.ident.name) + '$' + lowerStr(nm(c^.declName));
      d^.cgExport := intern(prefix + '$vmt');
      d^.cgImported := imported;
      for j := 0 to c^.classMembers.count - 1 do
      begin
        m := PDecl(listGet(c^.classMembers, j));
        { (a generic method: each module compiles the specializations it
          uses) }
        if (m^.kind = dkVar) or ((m^.kind = dkRoutine) and not m^.isAbstract and
                                 (m^.memberKey = nil)) then
        begin { a class variable or a method }
          m^.cgExport := intern(prefix + '.' + nm(m^.ident.name) + overloadSuffix(m));
          m^.cgImported := imported;
        end;
      end;
      continue;
    end;
    c := advancedRecord(d);
    if c <> nil then { an advanced record: its class variables and methods }
    begin
      prefix := 'pas.' + nm(u^.ident.name) + '$' + lowerStr(nm(c^.declName));
      for j := 0 to c^.classMembers.count - 1 do
      begin
        m := PDecl(listGet(c^.classMembers, j));
        if (m^.kind = dkRoutine) or (m^.kind = dkVar) then
        begin
          m^.cgExport := intern(prefix + '.' + nm(m^.ident.name) + overloadSuffix(m));
          m^.cgImported := imported;
        end;
      end;
      continue;
    end;
    if definedInterface(d) <> nil then { an interface: its iid }
    begin
      d^.cgExport := intern('pas.' + nm(u^.ident.name) + '$' + nm(d^.ident.name) + '$iid');
      d^.cgImported := imported;
      continue;
    end;
    if ((d^.kind = dkVar) and not d^.varExternal) or
       ((d^.kind = dkRoutine) and not d^.isExternal and not d^.isGeneric) then
    begin
      d^.cgExport := intern('pas.' + nm(u^.ident.name) + '$' + nm(d^.ident.name) +
                            overloadSuffix(d));
      d^.cgImported := imported;
    end;
  end;
end;

function initName(u: PDecl): string;
begin
  initName := 'pas.' + nm(u^.ident.name) + '$$init';
end;

{ The start of a program or unit (decls, then more: its declarations): the
  class constructors of the classes and advanced records it declares
  (nested ones too), in declaration order; then its class destructors are
  registered with the runtime, which runs them at exit (last registered
  first: the program's, then the units' in reverse order, each one's in
  declaration order, as in Free Pascal). }
procedure emitClassInits(const decls, more: TPtrList);
var
  dtors, all: TPtrList;
  i, k: longint;
  d, r: PDecl;
  c: PType;
  ignored: TV;
begin
  listInit(dtors);
  for k := 0 to 1 do
  begin
    if k = 0 then
      all := withNested(decls)
    else
      all := withNested(more);
    for i := 0 to all.count - 1 do
    begin
      d := PDecl(listGet(all, i));
      c := definedClass(d);
      if c = nil then
        c := advancedRecord(d);
      if (c = nil) or d^.cgImported then
        continue;
      r := c^.classCtor;
      if (r <> nil) and (r^.cgFunc <> nil) then
        ignored := call1(fnOf(r), irNull, '');
      r := c^.classDtor;
      if (r <> nil) and (r^.cgFunc <> nil) then
        listAdd(dtors, r);
    end;
  end;
  for i := dtors.count - 1 downto 0 do
    ignored := call1(classRuntime('__pas_at_exit', 'void', 'ptr'),
                     irFuncRef(fnOf(PDecl(listGet(dtors, i)))), '');
end;

{ --- The program --- }

procedure emitMainBody(prog: PDecl; const units: TPtrList; mainFn: PIRFunc);
var
  blk: PBlock;
  i: longint;
  u: PDecl;
  ignored, tp: TV;
  entry: PIRBlock;
  noParams, noDecls: TPtrList;
  initFn: PIRFunc;
begin
  blk := prog^.progBlock;
  curFn := mainFn;
  resetAddrAligns;
  curRoutine := nil;
  curFrame := irNull;
  curSP := nil;
  if dbgOn then
  begin
    listInit(noParams);
    curSP := createSubprogram(mainFn, 'main', unitFile, lineOf(prog^.loc), nil, noParams, nil,
                              nil);
  end;
  irBeginBody(mainFn);
  irSetDebugLoc(nil);
  entry := irNewBlock('entry');
  irSetInsertPoint(entry);
  setDebugLoc(prog^.loc);
  ignored := call2(rtFn('__pas_init'), irArg(mainFn, 0), irArg(mainFn, 1), '');
  if mode <> lmISO then
  begin
    tp := irGlobal('__pas_tp_mode', 'external', irI32, irNull, false, false, true, 0);
    irStore(i32c(1), tp);
  end;
  if separate then
  begin
    { separately compiled units initialize themselves, in uses order }
    for i := 0 to units.count - 1 do
    begin
      u := PDecl(listGet(units, i));
      initFn := irFindFunction(initName(u));
      if initFn = nil then
      begin
        listInit(noParams);
        initFn := irDeclareFunction(initName(u), irVoidT, noParams, '', '');
      end;
      ignored := call0(initFn, '');
    end;
  end
  else
  begin
    { units: their files, then their initialization parts, in uses order }
    for i := 0 to units.count - 1 do
    begin
      u := PDecl(listGet(units, i));
      initFileVars(u^.iface^.decls, false);
      initFileVars(u^.impl^.decls, false);
    end;
    for i := 0 to units.count - 1 do
    begin
      u := PDecl(listGet(units, i));
      emitClassInits(u^.iface^.decls, u^.impl^.decls);
      if u^.impl^.body <> nil then
        visitStmt(u^.impl^.body);
    end;
  end;
  initFileVars(blk^.decls, true);
  if hasMainJmpBuf then
    emitSetJmpDispatch(blk, mainJmpBuf);
  listInit(noDecls);
  emitClassInits(blk^.decls, noDecls);
  exitBlock := irNewBlock('exit');
  visitStmt(blk^.body);
  irBr(exitBlock);
  irSetInsertPoint(exitBlock);
  ignored := call0(rtFn('__pas_fini'), '');
  irRet(i32c(0));
end;

procedure startModule(m: TLangMode; rangeChecksOn, overflowChecksOn: boolean;
                      const sourceName, triple, dataLayout: string);
var
  fields: TPtrList;
begin
  irSetFastMath(optFastMath);
  irSetTarget(optCPU, optFeatures);
  irSetSizeLevel(optSizeLevel);
  mode := m;
  optRange := rangeChecksOn;
  optOverflow := overflowChecksOn;
  curRoutine := nil;
  pinFn := nil;
  pinTemps := true;
  mapInit(dynInfos);
  mapInit(typeInfos);
  listInit(asyncsToEmit);
  listInit(rangeLocals);
  paSkipPlain := false;
  nextStart := 0;
  listInit(dynOpElems);
  listInit(dynOpFns);
  mapInit(lowered);
  mapInit(stringConsts);
  mapInit(enumNameTables);
  mapInit(addrAligns);
  addrAlignGen := 0;
  mapInit(ansiConsts);
  mapInit(errMsgs);
  mapInit(fileStrs);
  mapInit(locRecs);
  mapInit(classLayouts);
  mapInit(vmts);
  mapInit(iids);
  listInit(vmtsToDefine);
  listInit(allClasses);
  mapInit(exportedClasses);
  listInit(liveTemps);
  listInit(routineTemps);
  listInit(stringTemps);
  listInit(deferredInits);
  listInit(cleanupClasses);
  listInit(cleanupFns);
  listInit(routinesToEmit);
  listInit(thunksToEmit);
  listInit(loopBlocks);
  hasMainJmpBuf := false;
  cgTriple := triple;
  irInit(sourceName, triple, dataLayout);
  listInit(fields);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  routinePairT := irStructLit(fields);
  listFree(fields);
  { generous for every supported libc (glibc: 200-312 bytes, macOS: 192) }
  jmpBufT := irArray(64, irI64);
  { PasExcFrame (runtime/include/pascalrt.h): prev, object, where, address,
    jmp_buf }
  listInit(fields);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  listAdd(fields, jmpBufT);
  excFrameT := irStructLit(fields);
  listFree(fields);
  listInit(fields);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  ownedExcT := irStructLit(fields);
  listFree(fields);
  listInit(cleanups);
  listInit(handledFrames);
  declareRuntime;
end;

procedure startDebugInfo(debugInfo, optimized: boolean; const sourceName: string);
begin
  dbgOn := debugInfo;
  dbgOptimized := optimized;
  curSP := nil;
  if dbgOn then
    initDebugInfo(sourceName);
end;

procedure generateUnitModule(u: PDecl; const deps: TPtrList; m: TLangMode;
                             rangeChecksOn, overflowChecksOn: boolean;
                             const sourceName, triple, dataLayout: string;
                             debugInfo, optimized, zeroInit, keepUnused: boolean;
                             var outText: TBuf);
var
  i: longint;
  d: PDecl;
  noParams: TPtrList;
  initFn: PIRFunc;
  entry: PIRBlock;
begin
  separate := true;
  optZeroInit := zeroInit;
  progDecl := nil;
  startModule(m, rangeChecksOn, overflowChecksOn, sourceName, triple, dataLayout);
  listInit(noParams);
  initFn := irCreateFunction(initName(u), '', irVoidT, noParams);
  startDebugInfo(debugInfo, optimized, sourceName);
  for i := 0 to deps.count - 1 do
    collectExports(PDecl(listGet(deps, i)), true);
  collectExports(u, false);
  for i := 0 to deps.count - 1 do
    emitGlobals(PDecl(listGet(deps, i))^.iface);
  emitGlobals(u^.iface);
  emitGlobals(u^.impl);
  for i := 0 to deps.count - 1 do
  begin
    d := PDecl(listGet(deps, i));
    declareRoutines(d^.iface);
  end;
  declareRoutines(u^.iface);
  declareRoutines(u^.impl);
  finishStaticInits;
  defineVMTs;
  for i := 0 to routinesToEmit.count - 1 do
    emitRoutineBody(PDecl(listGet(routinesToEmit, i)));
  emitAsyncs;
  emitCleanups;

  { the initialization part: files, then the statements }
  curFn := initFn;
  resetAddrAligns;
  curRoutine := nil;
  curFrame := irNull;
  curSP := nil;
  if dbgOn then
    curSP := createSubprogram(initFn, nm(u^.ident.spelling), unitFile, lineOf(u^.loc), nil,
                              noParams, nil, nil);
  irBeginBody(initFn);
  irSetDebugLoc(nil);
  entry := irNewBlock('entry');
  irSetInsertPoint(entry);
  setDebugLoc(u^.loc);
  initFileVars(u^.iface^.decls, false);
  initFileVars(u^.impl^.decls, false);
  emitClassInits(u^.iface^.decls, u^.impl^.decls);
  exitBlock := irNewBlock('exit');
  if u^.impl^.body <> nil then
    visitStmt(u^.impl^.body);
  irBr(exitBlock);
  irSetInsertPoint(exitBlock);
  irRetVoid;
  emitDynOps;
  emitCThunks;
  if dbgOn then
    finalizeDebugInfo;
  if not keepUnused then
    irRemoveUnreachable;
  irWriteModule(outText);
end;

procedure generateModule(prog: PDecl; const units: TPtrList; m: TLangMode;
                         rangeChecksOn, overflowChecksOn: boolean;
                         const sourceName, triple, dataLayout: string;
                         debugInfo, optimized, separateUnits, zeroInit,
                         keepUnused: boolean;
                         var outText: TBuf);
var
  i: longint;
  u: PDecl;
  fields: TPtrList;
  mainFn: PIRFunc;
begin
  separate := separateUnits;
  optZeroInit := zeroInit;
  progDecl := prog;
  startModule(m, rangeChecksOn, overflowChecksOn, sourceName, triple, dataLayout);
  listInit(fields);
  listAdd(fields, irI32);
  listAdd(fields, irPtrT);
  mainFn := irCreateFunction('main', '', irI32, fields);
  listFree(fields);
  irSetArgName(mainFn, 0, 'argc');
  irSetArgName(mainFn, 1, 'argv');
  startDebugInfo(debugInfo, optimized, sourceName);
  if separate then
    for i := 0 to units.count - 1 do
      collectExports(PDecl(listGet(units, i)), true);

  for i := 0 to units.count - 1 do
  begin
    u := PDecl(listGet(units, i));
    emitGlobals(u^.iface);
    if not separate then
      emitGlobals(u^.impl);
  end;
  emitGlobals(prog^.progBlock);
  if hasNonLocalLabels(prog^.progBlock) then
  begin
    { needed by gotos in routine bodies }
    mainJmpBuf := irGlobal('pas.jmpbuf', 'internal', jmpBufT, irZero(jmpBufT), false,
                           false, false, 16);
    hasMainJmpBuf := true;
  end;
  for i := 0 to units.count - 1 do
  begin
    u := PDecl(listGet(units, i));
    declareRoutines(u^.iface);
    if not separate then
      declareRoutines(u^.impl);
  end;
  declareRoutines(prog^.progBlock);
  finishStaticInits;
  defineVMTs;
  for i := 0 to routinesToEmit.count - 1 do
    emitRoutineBody(PDecl(listGet(routinesToEmit, i)));
  emitAsyncs;
  emitCleanups;
  emitMainBody(prog, units, mainFn);
  emitDynOps;
  emitCThunks;
  if dbgOn then
    finalizeDebugInfo;
  if not keepUnused then
    irRemoveUnreachable;
  irWriteModule(outText);
end;
