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

procedure emitWriteArg(const f: TV; arg: PNode);
var
  value: PNode;
  width, prec, v, s, b: TV;
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
  else if isBoolBase(t) then
  begin
    b := irCast('zext', v, irI32, '');
    rtv3('__pas_write_bool', f, b, width);
  end
  else
    rtv3('__pas_write_int', f, toI64(v, t), width);
end;

procedure emitWrite(s: PNode; isWriteln: boolean);
var
  ft: PType;
  f, a: TV;
  first, i: longint;
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
  for i := first to s^.args.count - 1 do
    emitWriteArg(f, PNode(listGet(s^.args, i)));
  if isWriteln then
    rtv1('__pas_writeln', f);
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
  r := call1(rtFn('_setjmp'), buf, 'jmp');
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
  obj, addr, ignored, pair, v: TV;
  c, t: PType;
  args, noIndexes: TPtrList;
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
  t := p^.propType;
  addr := fieldAddress(obj, c, p^.writeDecl);
  if t^.kind = tyShortString then
  begin
    emitStringAssign(addr, t, value);
    exit;
  end;
  if isStructured(t) then
  begin
    copyValue(addr, emitAddress(value), t);
    exit;
  end;
  if t^.kind = tyRoutine then
  begin
    pair := emitRoutinePair(value, t);
    irStore(pair, addr);
    exit;
  end;
  v := convertForAssign(visitExpr(value), value^.typ, t, value^.b);
  storeValue(addr, v, t);
end;

procedure visitAssign(s: PNode);
var
  target, value: PNode;
  tt: PType;
  dst, src, pair, v: TV;
  p: PDecl;
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
    emitStringAssign(emitAddress(target), tt, value);
    exit;
  end;
  if isStructured(tt) then
  begin
    dst := emitAddress(target);
    src := emitAddress(value);
    copyValue(dst, src, tt);
    exit;
  end;
  if tt^.kind = tyRoutine then
  begin
    { a procedural variable (Turbo/FPC) }
    pair := emitRoutinePair(value, tt);
    irStore(pair, emitAddress(target));
    exit;
  end;
  v := convertForAssign(visitExpr(value), value^.typ, tt, value^.b);
  storeValue(emitAddress(target), v, tt);
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
  else if isStructured(rt) then
  begin
    src := emitAddress(value);
    copyAggregate(addr, src, rt);
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
  if isIntegerLike(t) and (intBits(t) = 64) then
  begin
    { no wider type to check in }
    if isInc then
      nv := emitCheckedArith('add', true, old, step, loc, nm(s^.callee.name))
    else
      nv := emitCheckedArith('sub', true, old, step, loc, nm(s^.callee.name));
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
    check := overflowChecks(loc) or rangeChecks(loc)
  else
    check := rangeChecks(loc);
  if check then
  begin
    bd := getOrdinalBounds(t);
    bad := outsideRange(wide, bd.lo, bd.hi);
    if isIntegerLike(t) then
      emitRuntimeCheck(bad, 'integer overflow in ''' + nm(s^.callee.name) + '''', loc)
    else
      emitRuntimeCheck(bad, 'value out of range', loc);
  end;
  storeScalar(addr, irCast('trunc', wide, regType(t), ''), t);
end;

procedure visitProcCall(s: PNode);
var
  d: PDecl;
  k: TBuiltinKind;
  ignored, f, a0, a1, a2, mem, sz, old, dst, cnt, neg, v, code, p, src: TV;
  arg, countE: PNode;
  isFill: boolean;
  pointee: PType;
begin
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
  case k of
    bkWrite, bkWriteln: emitWrite(s, k = bkWriteln);
    bkRead, bkReadln: emitRead(s, k = bkReadln);
    bkReset, bkRewrite, bkGet, bkPut, bkPage:
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
    bkDelete:
      begin
        a0 := emitAddress(PNode(listGet(s^.args, 0)));
        a1 := emitI32(PNode(listGet(s^.args, 1)));
        a2 := emitI32(PNode(listGet(s^.args, 2)));
        rtv3('__pas_str_delete', a0, a1, a2);
      end;
    bkInsert:
      begin
        arg := PNode(listGet(s^.args, 1));
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
      rtv1('free', visitExpr(PNode(listGet(s^.args, 0))));
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

procedure visitRepeat(s: PNode);
var
  bodyB, condB, endB: PIRBlock;
  i: longint;
begin
  bodyB := irNewBlock('repeat.body');
  condB := irNewBlock('repeat.cond');
  endB := irNewBlock('repeat.end');
  irBr(bodyB);
  irSetInsertPoint(bodyB);
  pushLoop(condB, endB);
  for i := 0 to s^.stmts.count - 1 do
    visitStmt(PNode(listGet(s^.stmts, i)));
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
procedure visitFor(s: PNode);
var
  v: PDecl;
  varTy: PType;
  down: boolean;
  start, final, start64, final64, enter, i, done, next: TV;
  checkB, loopB, latchB, incB, endB, preB: PIRBlock;
  phi: PIRInst;
begin
  v := s^.forVar^.refDecl;
  varTy := v^.varType;
  down := s^.isDownto;
  start := visitExpr(s^.forInit);
  final := visitExpr(s^.forFinal);
  { the bounds may have other integer types than the variable: compare
    them as i64, check them, then convert them to the variable's type }
  start64 := toI64(start, s^.forInit^.typ);
  final64 := toI64(final, s^.forFinal^.typ);
  if down then
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
  pushLoop(latchB, endB);
  visitStmt(s^.forBody);
  popLoop;
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
  v: int64;
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
      if cl^.hiValue - cl^.loValue >= maxExpandedRange then
      begin
        lr := PLargeRange(arenaAlloc(sizeof(TLargeRange)));
        lr^.lo := cl^.loValue;
        lr^.hi := cl^.hiValue;
        lr^.target := armB;
        listAdd(largeRanges, lr);
        continue;
      end;
      v := cl^.loValue;
      while v <= cl^.hiValue do
      begin
        irAddCase(sw, irConstInt(sel.t, v), armB);
        v := v + 1;
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
    for i := 0 to s^.elseBody.count - 1 do
      visitStmt(PNode(listGet(s^.elseBody, i)));
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
    rtv2('_longjmp', buf, i32c(l^.labelIndex));
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

procedure visitStmtList(const l: TPtrList);
var
  i: longint;
begin
  for i := 0 to l.count - 1 do
    visitStmt(PNode(listGet(l, i)));
end;

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
    isv := call2(classRuntime('__pas_class_is', 'i32', 'ptr,ptr'), obj,
                 vmtOf(hd^.hClass), '');
    doB := irNewBlock('handler.do');
    nextB := irNewBlock('handler.next');
    irCondBr(irICmp('ne', isv, i32c(0), ''), doB, nextB, false);
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
procedure emitTryFinally(s: PNode);
var
  raisedB, finallyB, endB, reraiseB: PIRBlock;
  state, f: TV;
  c: PCleanup;
  ignored: pointer;
  sw: PIRInst;
  k: longint;
begin
  raisedB := irNewBlock('finally.raise');
  finallyB := irNewBlock('finally');
  endB := irNewBlock('try.end');
  state := irCreateTemp(irI32, 'finally.state');
  f := emitProtect(raisedB, 'try.body');
  c := newCleanup(clFinally, f);
  c^.state := state;
  c^.finallyB := finallyB;
  listAdd(cleanups, c);
  visitStmtList(s^.tryBody);
  ignored := listPop(cleanups);
  excCall1('__pas_exc_pop', f);
  irStore(i32c(0), state);
  irBr(finallyB);

  irSetInsertPoint(raisedB);
  irStore(i32c(1), state);
  irBr(finallyB);

  irSetInsertPoint(finallyB);
  visitStmtList(s^.partBody);
  reraiseB := irNewBlock('finally.reraise');
  sw := irSwitch(irLoad(irI32, state, 'state'), endB);
  irAddCase(sw, i32c(1), reraiseB);
  for k := 0 to c^.exits.count - 1 do
    irAddCase(sw, i32c(2 + k), PIRBlock(listGet(c^.exits, k)));
  irSetInsertPoint(reraiseB);
  excCall1('__pas_reraise', f);
  irUnreachable;
  irSetInsertPoint(endB);
end;

procedure visitRaise(s: PNode);
var
  obj: TV;
begin
  if s^.raiseObj = nil then
    excCall1('__pas_reraise', PTVBox(listGet(handledFrames, handledFrames.count - 1))^)
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
  i, mark: longint;
  ignored: TV;
  b: PIRBlock;
begin
  { statements carry their source position into the debug information }
  setDebugLoc(s^.b);
  mark := liveTemps.count;
  case s^.kind of
    nkEmpty: ;
    nkCompound:
      for i := 0 to s^.stmts.count - 1 do
        visitStmt(PNode(listGet(s^.stmts, i)));
    nkAssign: visitAssign(s);
    nkProcCall: visitProcCall(s);
    nkIndirectCallStmt: ignored := visitExpr(s^.calleeExpr);
    nkIf: visitIf(s);
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
  else
    ;
  end;
  releaseTemps(mark); { the results held by its expressions }
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
      else if t^.kind = tyShortString then
      begin
        { a char }
        ch := chr(c.intVal and 255);
        scalarConstant := stringInit(PCharArray(@ch), 0, 1, ty, t);
      end
      else
        scalarConstant := irConstInt(ty, c.intVal);
    ckReal: scalarConstant := irConstDouble(c.realVal);
    ckString: scalarConstant := stringInit(c.strVal^.chars, 0, c.strVal^.len, ty, t);
  else
    scalarConstant := irZero(ty);
  end;
end;

{ The constant for a typed-constant initializer init of type t (memory
  type ty); folded is Sema's value for non-aggregates. }
function constantFor(init: PNode; const folded: TConstValue; t: PType; ty: PIRType): TV;
var
  i, j: longint;
  el, match: PAggElement;
  f: PDecl;
  fty: PIRType;
  elems: TPtrList;
begin
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
begin
  ty := memType(v^.varType);
  if v^.cgImported then
    { a variable of a separately compiled unit: declared only }
    v^.cgValue := irBox(irGlobal(nm(v^.cgExport), 'external', ty, irNull, false, false,
                                 true, 0))
  else
  begin
    if v^.init <> nil then
      init := constantFor(v^.init, v^.initValue, v^.varType, ty)
    else
      init := irZero(ty);
    if v^.cgExport <> nil then
      v^.cgValue := irBox(irGlobal(nm(v^.cgExport), '', ty, init, false, false, false, 0))
    else
    begin
      if v^.isClassVar then
        name := 'pas.' + lowerStr(nm(v^.varClass^.declName)) + '.' + nm(v^.ident.name)
      else
        name := 'pas.' + nm(v^.ident.name);
      v^.cgValue := irBox(irGlobal(name, 'internal', ty, init, false, false, false, 0));
    end;
  end;
  { class variables have no debug information }
  if dbgOn and not v^.cgImported and not v^.isClassVar then
    declareGlobalDebugInfo(v, PTVBox(v^.cgValue)^);
end;

procedure emitGlobals(blk: PBlock);
var
  i, j: longint;
  d, m: PDecl;
  c: PType;
begin
  for i := 0 to blk^.decls.count - 1 do
  begin
    d := PDecl(listGet(blk^.decls, i));
    c := definedClass(d);
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
    if d^.kind = dkVar then
      emitStaticVar(d);
  end;
end;

function paramIRType(p: PDecl): PIRType;
begin
  if (p^.varKind = vkVarParam) or isStructured(p^.varType) or
     (p^.varType^.kind = tyUntyped) then
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
         (p^.varType^.kind = tyUntyped) then
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
  listAdd(params, irPtrT);
  if hasSelf(r) then
    listAdd(params, irPtrT);
  for j := 0 to r^.params.count - 1 do
    listAdd(params, paramIRType(PDecl(listGet(r^.params, j))));
end;

function routineRetType(r: PDecl): PIRType;
begin
  if r^.isFunction then
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
  ty := irArray(4 + c^.classVTable.count, irPtrT);
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
    if (m^.kind = dkRoutine) and not m^.isAbstract then
    begin
      listInit(params);
      routineParamTypes(m, params);
      m^.cgFunc := irDeclareFunction(nm(m^.cgExport), routineRetType(m), params, '', '');
      listFree(params);
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
procedure defineVMTs;
var
  i, j: longint;
  c: PType;
  r: PDecl;
  elems: TPtrList;
  g: PTVBox;
begin
  for i := 0 to vmtsToDefine.count - 1 do
  begin
    c := PType(listGet(vmtsToDefine, i));
    listInit(elems);
    listAdd(elems, irBox(vmtOf(c^.classParent)));
    listAdd(elems, irBox(getPStrConstant(c^.declName^.chars, 0, c^.declName^.len)));
    listAdd(elems, irBox(irConstIntToPtr(instanceSize(c))));
    listAdd(elems, irBox(interfaceTable(c)));
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
    g := PTVBox(mapGet(vmts, c^.id));
    irSetGlobalInit(g^, irConstArray(irArray(elems.count, irPtrT), elems));
    listFree(elems);
  end;
  listClear(vmtsToDefine);
end;

procedure declareRoutine(r: PDecl); forward;

{ Lays out the frames of the routines declared in blk (recursively),
  creates their functions and queues their bodies, and declares the
  classes' VMTs. A method is declared where its implementation is. }
{ The C-callable entry of a cdecl routine r at program or unit level (its
  procedural values point to it): cthunk.<sym>, which calls r with a null
  static link; created right after r, its body emitted with the
  routines'. }
procedure declareCThunk(r: PDecl; const sym: string);
var
  params: TPtrList;
  target, t: PIRFunc;
  i: longint;
begin
  if not isCDecl(r) or (r^.methodKind <> mtNone) or (r^.parent <> nil) then
    exit;
  target := fnOf(r);
  listInit(params);
  for i := 1 to target^.params.count - 1 do
    listAdd(params, listGet(target^.params, i));
  t := irCreateFunction('cthunk.' + sym, 'internal', target^.ret, params);
  listFree(params);
  for i := 0 to r^.params.count - 1 do
    irSetArgName(t, i, nm(PDecl(listGet(r^.params, i))^.ident.name));
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
  irArgsAdd(buf, irNull);
  for i := 0 to t^.params.count - 1 do
    irArgsAdd(buf, irArg(t, i));
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
  params: TPtrList;
begin
  for i := 0 to blk^.decls.count - 1 do
  begin
    r := PDecl(listGet(blk^.decls, i));
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
    if (r^.kind <> dkRoutine) or (r^.canonical <> nil) then
      continue; { the forward declaration is the canonical node }
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
      listFree(params);
      declareCThunk(r, nm(r^.cgExport));
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
  slots, params: TPtrList;
  name, dbgName: string;
  f: PIRFunc;
  ret: PIRType;
  loc: TLoc;

  procedure addSlot(x: PDecl);
  begin
    x^.cgSlot := slots.count;
    if isByRef(x) then
      listAdd(slots, irPtrT)
    else
      listAdd(slots, memType(x^.varType));
  end;

begin
  begin
    listInit(slots);
    if r^.parent <> nil then
      listAdd(slots, irPtrT);
    if hasSelf(r) then
      addSlot(r^.selfVar);
    for j := 0 to r^.params.count - 1 do
      addSlot(PDecl(listGet(r^.params, j)));
    for j := 0 to r^.body^.decls.count - 1 do
    begin
      v := PDecl(listGet(r^.body^.decls, j));
      if v^.kind <> dkVar then
        continue;
      if (v^.init <> nil) and not v^.isInitVar then
        emitStaticVar(v) { static storage, local scope }
      else
        addSlot(v);
    end;
    if r^.isFunction then
      addSlot(r^.resultVar);
    name := mangle(r);
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
    irSetArgName(f, 0, 'link');
    argNo := 1;
    if hasSelf(r) then
    begin
      irSetArgName(f, 1, 'self');
      argNo := 2;
    end;
    for j := 0 to r^.params.count - 1 do
    begin
      p := PDecl(listGet(r^.params, j));
      irSetArgName(f, j + argNo, nm(p^.ident.name));
    end;
    if r^.isInline then { the inline directive }
      f^.attrs := 'inlinehint';
    r^.cgFunc := f;
    if r^.cgExport <> nil then
      declareCThunk(r, nm(r^.cgExport))
    else
      declareCThunk(r, name);
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

procedure emitCleanups;
var
  k: longint;
begin
  for k := 0 to cleanupClasses.count - 1 do
    emitCleanup(PType(listGet(cleanupClasses, k)), PIRFunc(listGet(cleanupFns, k)));
  listClear(cleanupClasses);
  listClear(cleanupFns);
end;

procedure emitRoutineBody(r: PDecl);
var
  frameT: PIRType;
  i, argNo: longint;
  p, v, res: PDecl;
  arg, slot, ret, excFrame: TV;
  f: PIRFunc;
  entry, raised, retB: PIRBlock;
  managedAddrs, managedTypes: TPtrList; { of PTVBox / PType }
  managedResult: boolean;

  { releases the value parameters', local variables' and temporaries'
    references }
  procedure finalizeAll;
  var
    k: longint;
  begin
    for k := 0 to managedAddrs.count - 1 do
      finalizeValue(PTVBox(listGet(managedAddrs, k))^, PType(listGet(managedTypes, k)));
    for k := 0 to routineTemps.count - 1 do
      intfCall1('__pas_intf_clear', PTVBox(listGet(routineTemps, k))^);
  end;

begin
  listInit(managedAddrs);
  listInit(managedTypes);
  f := fnOf(r);
  curFn := f;
  curRoutine := r;
  curSP := PSPInfo(r^.cgDbg);
  irBeginBody(f);
  irSetDebugLoc(nil);
  entry := irNewBlock('entry');
  irSetInsertPoint(entry);
  setDebugLoc(bodyLoc(r));
  frameT := frameType(r);
  curFrame := irAlloca(frameT, 'frame');
  { ISO leaves locals undefined; start from zero for determinism }
  emitMemSet(curFrame, i8c(0), i64c(frameT^.size), frameT^.align);
  if dbgOn then
    declareFrameVariables(r);
  if r^.parent <> nil then
    irStore(irArg(f, 0), irStructGEP(frameT, curFrame, 0, ''));
  argNo := 1;
  if hasSelf(r) then
  begin
    irStore(irArg(f, 1), irStructGEP(frameT, curFrame, r^.selfVar^.cgSlot, ''));
    argNo := 2;
  end;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    arg := irArg(f, i + argNo);
    slot := irStructGEP(frameT, curFrame, p^.cgSlot, '');
    if isByRef(p) then
      irStore(arg, slot)
    else if isStructured(p^.varType) then
      copyAggregate(slot, arg, p^.varType) { value semantics }
    else
      storeScalar(slot, arg, p^.varType);
    { the variables holding COM interfaces: value parameters (one more
      reference each), then local variables (nil at first) }
    if not isByRef(p) and (p^.varKind <> vkConstParam) and isManaged(p^.varType) then
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
       isManaged(v^.varType) then
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
  { an exception leaving the routine releases them too (and the result) }
  raised := nil;
  if (managedAddrs.count > 0) or managedResult or r^.usesManaged then
  begin
    raised := irNewBlock('managed.raise');
    excFrame := emitProtect(raised, 'managed.body');
  end;
  listClear(routineTemps);
  exitBlock := irNewBlock('exit');
  visitStmt(r^.body^.body);
  irBr(exitBlock);
  irSetInsertPoint(exitBlock);
  if raised <> nil then
    excCall1('__pas_exc_pop', excFrame);
  finalizeAll;
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if v^.kind = dkVar then
      emitFileLifetime(varAddress(v), v^.varType, false, irNull);
  end;
  if raised <> nil then
  begin
    retB := irInsertBlock;
    irSetInsertPoint(raised);
    finalizeAll;
    if managedResult then
      finalizeValue(varAddress(r^.resultVar), r^.resultVar^.varType);
    excCall1('__pas_reraise', excFrame);
    irUnreachable;
    irSetInsertPoint(retB);
  end;
  listClear(routineTemps);
  if r^.isFunction then
  begin
    res := r^.resultVar;
    slot := irStructGEP(frameT, curFrame, res^.cgSlot, '');
    ret := loadScalar(slot, res^.varType, 'result');
    irRet(ret);
  end
  else
    irRetVoid;
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
begin
  for i := 0 to u^.iface^.decls.count - 1 do
  begin
    d := PDecl(listGet(u^.iface^.decls, i));
    c := definedClass(d);
    if c <> nil then
    begin
      { a class: its VMT and its methods }
      prefix := 'pas.' + nm(u^.ident.name) + '$' + lowerStr(nm(c^.declName));
      d^.cgExport := intern(prefix + '$vmt');
      d^.cgImported := imported;
      for j := 0 to c^.classMembers.count - 1 do
      begin
        m := PDecl(listGet(c^.classMembers, j));
        if (m^.kind = dkVar) or ((m^.kind = dkRoutine) and not m^.isAbstract) then
        begin { a class variable or a method }
          m^.cgExport := intern(prefix + '.' + nm(m^.ident.name) + overloadSuffix(m));
          m^.cgImported := imported;
        end;
      end;
      continue;
    end;
    c := advancedRecord(d);
    if c <> nil then { an advanced record: its methods }
    begin
      prefix := 'pas.' + nm(u^.ident.name) + '$' + lowerStr(nm(c^.declName));
      for j := 0 to c^.classMembers.count - 1 do
      begin
        m := PDecl(listGet(c^.classMembers, j));
        if m^.kind = dkRoutine then
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
    if (d^.kind = dkVar) or ((d^.kind = dkRoutine) and not d^.isExternal) then
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

{ --- The program --- }

procedure emitMainBody(prog: PDecl; const units: TPtrList; mainFn: PIRFunc);
var
  blk: PBlock;
  i: longint;
  u: PDecl;
  ignored, tp: TV;
  entry: PIRBlock;
  noParams: TPtrList;
  initFn: PIRFunc;
begin
  blk := prog^.progBlock;
  curFn := mainFn;
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
      if u^.impl^.body <> nil then
        visitStmt(u^.impl^.body);
    end;
  end;
  initFileVars(blk^.decls, true);
  if hasMainJmpBuf then
    emitSetJmpDispatch(blk, mainJmpBuf);
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
  mode := m;
  optRange := rangeChecksOn;
  optOverflow := overflowChecksOn;
  curRoutine := nil;
  mapInit(lowered);
  mapInit(stringConsts);
  mapInit(classLayouts);
  mapInit(vmts);
  mapInit(iids);
  listInit(vmtsToDefine);
  listInit(liveTemps);
  listInit(routineTemps);
  listInit(cleanupClasses);
  listInit(cleanupFns);
  listInit(routinesToEmit);
  listInit(thunksToEmit);
  listInit(loopBlocks);
  hasMainJmpBuf := false;
  irInit(sourceName, triple, dataLayout);
  listInit(fields);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  routinePairT := irStructLit(fields);
  listFree(fields);
  { generous for every supported libc (glibc: 200-312 bytes, macOS: 192) }
  jmpBufT := irArray(64, irI64);
  { PasExcFrame (runtime/include/pascalrt.h): prev, object, where, jmp_buf }
  listInit(fields);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  listAdd(fields, jmpBufT);
  excFrameT := irStructLit(fields);
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
                             debugInfo, optimized: boolean; var outText: TBuf);
var
  i: longint;
  d: PDecl;
  noParams: TPtrList;
  initFn: PIRFunc;
  entry: PIRBlock;
begin
  separate := true;
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
  defineVMTs;
  for i := 0 to routinesToEmit.count - 1 do
    emitRoutineBody(PDecl(listGet(routinesToEmit, i)));
  emitCleanups;
  emitCThunks;

  { the initialization part: files, then the statements }
  curFn := initFn;
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
  exitBlock := irNewBlock('exit');
  if u^.impl^.body <> nil then
    visitStmt(u^.impl^.body);
  irBr(exitBlock);
  irSetInsertPoint(exitBlock);
  irRetVoid;
  if dbgOn then
    finalizeDebugInfo;
  irWriteModule(outText);
end;

procedure generateModule(prog: PDecl; const units: TPtrList; m: TLangMode;
                         rangeChecksOn, overflowChecksOn: boolean;
                         const sourceName, triple, dataLayout: string;
                         debugInfo, optimized, separateUnits: boolean; var outText: TBuf);
var
  i: longint;
  u: PDecl;
  fields: TPtrList;
  mainFn: PIRFunc;
begin
  separate := separateUnits;
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
  defineVMTs;
  for i := 0 to routinesToEmit.count - 1 do
    emitRoutineBody(PDecl(listGet(routinesToEmit, i)));
  emitCleanups;
  emitCThunks;
  emitMainBody(prog, units, mainFn);
  if dbgOn then
    finalizeDebugInfo;
  irWriteModule(outText);
end;
