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
  tmp := createTemp(memType(t));
  if isStructured(t) then
  begin
    copyAggregate(tmp, emitAddress(e), t);
    exit(tmp);
  end;
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
    rtv3('__pas_write_pstr', 'ptr, ptr, i32', f, s, width);
    exit;
  end;
  if isStringType(t) then
  begin
    s := emitAddress(value);
    rtv4('__pas_write_str', 'ptr, ptr, i64, i32', f, s, i64c(getStringLength(t)), width);
    exit;
  end;
  v := visitExpr(value);
  if t^.kind = tyReal then
    rtv4('__pas_write_real', 'ptr, double, i32, i32', f, v, width, prec)
  else if isCharBase(t) then
    rtv3('__pas_write_char', 'ptr, i8, i32', f, v, width)
  else if isBoolBase(t) then
  begin
    b := irCast('zext', v, irI32);
    rtv3('__pas_write_bool', 'ptr, i32, i32', f, b, width);
  end
  else
    rtv3('__pas_write_int', 'ptr, i64, i32', f, toI64(v, t), width);
end;

procedure emitWrite(s: PNode; isWriteln: boolean);
var
  ft: PType;
  f, a: TV;
  first, i: longint;
  arg: PNode;
begin
  f := fileTarget(s^.args, false, ft, first);
  if (ft <> nil) and not ft^.isText then
  begin
    for i := first to s^.args.count - 1 do
    begin
      arg := PNode(listGet(s^.args, i));
      a := addressForValue(arg, ft^.fileElement);
      rtv2('__pas_write_elem', 'ptr, ptr', f, a);
    end;
    exit;
  end;
  for i := first to s^.args.count - 1 do
    emitWriteArg(f, PNode(listGet(s^.args, i)));
  if isWriteln then
    rtv1('__pas_writeln', 'ptr', f);
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
    rtv3('__pas_read_str', 'ptr, ptr, i32', f, addr, i32c(t^.maxLength));
    exit;
  end;
  if t^.kind = tyReal then
  begin
    irStore(rt1(irDoubleT, '__pas_read_real', 'ptr', f), addr);
    exit;
  end;
  if isCharBase(t) then
  begin
    v := rt1(irI8, '__pas_read_char', 'ptr', f);
    emitRangeCheck(v, getBase(t), t, arg^.b);
    irStore(v, addr);
    exit;
  end;
  { integers: read as i32 (or i64 if needed), then checked against the
    target type }
  wide := (intBits(t) = 64) or not isSignedOrdinal(t);
  if wide then
  begin
    v := rt1(irI64, '__pas_read_int64', 'ptr', f);
    minV := -9223372036854775807 - 1;
    maxV := 9223372036854775807;
  end
  else
  begin
    v := rt1(irI32, '__pas_read_int', 'ptr', f);
    minV := -2147483648;
    maxV := 2147483647;
  end;
  x := irCast('sext', v, irI64);
  tb := getOrdinalBounds(t);
  if rangeChecks(arg^.b) and ((tb.lo > minV) or (tb.hi < maxV)) then
  begin
    bad := outsideRange(x, tb.lo, tb.hi);
    emitRuntimeCheck(bad, 'value out of range', arg^.b);
  end;
  irStore(irCast('trunc', x, regType(t)), addr);
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
        rtv2('__pas_read_elem', 'ptr, ptr', f, emitAddress(arg));
        continue;
      end;
      tmp := createTemp(memType(elem));
      rtv2('__pas_read_elem', 'ptr, ptr', f, tmp);
      v := convertForAssign(loadScalar(tmp, elem), elem, arg^.typ, arg^.b);
      addr := emitAddress(arg);
      storeScalar(addr, v, arg^.typ);
    end;
    exit;
  end;
  for i := first to s^.args.count - 1 do
    emitReadArg(f, PNode(listGet(s^.args, i)));
  if isReadln then
    rtv1('__pas_readln', 'ptr', f);
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
  offset := irBin('sub', i, i64c(ub.lo));
  elem := irArrayGEP(memType(ua), base, offset);
  packedAddr := emitAddress(packed_);
  bytes := count * sizeOf_(ua^.elementType);
  if isPack then
    emitMemCpy(packedAddr, elem, i64c(bytes))
  else
    emitMemCpy(elem, packedAddr, i64c(bytes));
end;

{ --- goto --- }

function labelBlock(l: PDecl): string;
begin
  if l^.cgLabel = nil then
    l^.cgLabel := intern(irNewLabel('label.' + nameStr(l^.ident.name)));
  labelBlock := nameStr(l^.cgLabel);
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
  start: string;
  sw: TBuf;
  i: longint;
  d: PDecl;
begin
  r := rt1(irI32, '_setjmp', 'ptr', buf);
  start := irNewLabel('body');
  bufInit(sw);
  bufAdd(sw, 'switch i32 ' + r.v + ', label %' + start + ' [');
  for i := 0 to blk^.decls.count - 1 do
  begin
    d := PDecl(listGet(blk^.decls, i));
    if (d^.kind = dkLabel) and d^.labelNonLocal then
      bufAdd(sw, ' i32 ' + intToStr(d^.labelIndex) + ', label %' + labelBlock(d));
  end;
  bufAdd(sw, ' ]');
  irEmitTermBuf(sw);
  bufFree(sw);
  irStartBlock(start);
end;

{ --- Statements --- }

procedure pushLoop(const continueL, breakL: string);
begin
  listAdd(loopLabels, intern(continueL));
  listAdd(loopLabels, intern(breakL));
end;

procedure popLoop;
var
  ignored: pointer;
begin
  ignored := listPop(loopLabels);
  ignored := listPop(loopLabels);
end;

procedure visitAssign(s: PNode);
var
  target, value: PNode;
  tt: PType;
  dst, src, pair, v: TV;
begin
  target := s^.target;
  value := s^.value;
  tt := target^.typ;
  if tt^.kind = tyShortString then
  begin
    emitStringAssign(emitAddress(target), tt, value);
    exit;
  end;
  if isStructured(tt) then
  begin
    dst := emitAddress(target);
    src := emitAddress(value);
    copyAggregate(dst, src, tt);
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
  storeScalar(emitAddress(target), v, tt);
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
    storeScalar(addr, v, rt);
  end;
end;

procedure emitIncDec(s: PNode; isInc: boolean);
var
  varE, stepE: PNode;
  t: PType;
  addr, old, step, nv, wide, neg, bad: TV;
  loc: TLoc;
  bd: TOrdinalBounds;
  check: boolean;
begin
  varE := PNode(listGet(s^.args, 0));
  t := varE^.typ;
  addr := emitAddress(varE);
  old := loadScalar(addr, t);
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
      nv := emitCheckedArith('add', true, old, step, loc)
    else
      nv := emitCheckedArith('sub', true, old, step, loc);
    emitRangeCheck(nv, getBase(t), t, loc);
    storeScalar(addr, nv, t);
    exit;
  end;
  { computed in i64 so the range check sees the true result }
  if isInc then
    wide := irBin('add', toI64(old, t), step)
  else
  begin
    neg := irBin('sub', i64c(0), step);
    wide := irBin('add', toI64(old, t), neg);
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
      emitRuntimeCheck(bad, 'integer overflow in ''' + nameStr(s^.callee.name) + '''', loc)
    else
      emitRuntimeCheck(bad, 'value out of range', loc);
  end;
  storeScalar(addr, irCast('trunc', wide, regType(t)), t);
end;

procedure visitProcCall(s: PNode);
var
  d: PDecl;
  k: TBuiltinKind;
  ignored, f, a0, a1, a2, mem, sz, old, dst, cnt, neg, v, code, p: TV;
  arg, countE: PNode;
  target: string;
  isFill: boolean;
  pointee: PType;
begin
  d := s^.callDecl;
  if d^.kind = dkRoutine then
  begin
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
          bkReset: rtv1('__pas_reset', 'ptr', f);
          bkRewrite: rtv1('__pas_rewrite', 'ptr', f);
          bkGet: rtv1('__pas_get', 'ptr', f);
          bkPut: rtv1('__pas_put', 'ptr', f);
        else
          rtv1('__pas_page', 'ptr', f);
        end;
      end;
    bkPack, bkUnpack: emitPackUnpack(s, k = bkPack);
    bkBreak, bkContinue, bkExit:
      begin
        if s^.args.count > 0 then
          emitSetResult(PNode(listGet(s^.args, 0)));
        if k = bkExit then
          target := exitLabel
        else if k = bkContinue then
          target := nameStr(PName(listGet(loopLabels, loopLabels.count - 2)))
        else
          target := nameStr(PName(listGet(loopLabels, loopLabels.count - 1)));
        irBr(target);
        irStartBlock(irNewLabel('after.jump')); { unreachable code follows }
      end;
    bkDelete:
      begin
        a0 := emitAddress(PNode(listGet(s^.args, 0)));
        a1 := emitI32(PNode(listGet(s^.args, 1)));
        a2 := emitI32(PNode(listGet(s^.args, 2)));
        rtv3('__pas_str_delete', 'ptr, i32, i32', a0, a1, a2);
      end;
    bkInsert:
      begin
        arg := PNode(listGet(s^.args, 1));
        a0 := emitPStr(PNode(listGet(s^.args, 0)));
        a1 := emitAddress(arg);
        a2 := emitI32(PNode(listGet(s^.args, 2)));
        rtv4('__pas_str_insert', 'ptr, ptr, i32, i32', a0, a1,
             i32c(arg^.typ^.maxLength), a2);
      end;
    bkInc, bkDec: emitIncDec(s, k = bkInc);
    bkGetMem:
      begin
        arg := PNode(listGet(s^.args, 1));
        sz := toI64(visitExpr(arg), arg^.typ);
        mem := rt1(irPtrT, '__pas_new', 'i64', sz);
        irStore(mem, emitAddress(PNode(listGet(s^.args, 0))));
      end;
    bkFreeMem:
      rtv1('free', 'ptr', visitExpr(PNode(listGet(s^.args, 0))));
    bkReallocMem:
      begin
        a0 := emitAddress(PNode(listGet(s^.args, 0)));
        arg := PNode(listGet(s^.args, 1));
        sz := toI64(visitExpr(arg), arg^.typ);
        old := irLoad(irPtrT, a0);
        mem := rt2(irPtrT, '__pas_realloc', 'ptr, i64', old, sz);
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
        neg := irICmp('slt', cnt, i64c(0));
        cnt := irSelect(neg, i64c(0), cnt);
        if isFill then
        begin
          arg := PNode(listGet(s^.args, 2));
          v := irCast('trunc', toI64(visitExpr(arg), arg^.typ), irI8);
          emitMemSet(dst, v, cnt);
        end
        else
          emitMemMove(dst, emitAddress(PNode(listGet(s^.args, 0))), cnt);
      end;
    bkHalt:
      begin
        if s^.args.count = 0 then
          code := i32c(0)
        else
          code := emitI32(PNode(listGet(s^.args, 0)));
        rtv1('__pas_halt', 'i32', code);
        irUnreachable;
        irStartBlock(irNewLabel('after.halt')); { unreachable code follows }
      end;
    bkNew:
      begin
        arg := PNode(listGet(s^.args, 0));
        pointee := arg^.typ^.pointee;
        mem := rt1(irPtrT, '__pas_new', 'i64', i64c(sizeOf_(pointee)));
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
            emitRuntimeCheck(irICmp('eq', p, irNull), '''dispose'' of a nil pointer',
                             arg^.b);
          emitFileLifetime(p, pointee, false, irNull);
        end;
        rtv1('__pas_dispose', 'ptr', p);
      end;
  else
    ; { Sema allows only procedures here }
  end;
end;

procedure visitIf(s: PNode);
var
  c: TV;
  thenL, endL, elseL: string;
begin
  c := visitExpr(s^.cond);
  thenL := irNewLabel('if.then');
  endL := irNewLabel('if.end');
  elseL := endL;
  if s^.elseStmt <> nil then
    elseL := irNewLabel('if.else');
  irCondBr(c, thenL, elseL);
  irStartBlock(thenL);
  visitStmt(s^.thenStmt);
  irBr(endL);
  if s^.elseStmt <> nil then
  begin
    irStartBlock(elseL);
    visitStmt(s^.elseStmt);
    irBr(endL);
  end;
  irStartBlock(endL);
end;

procedure visitWhile(s: PNode);
var
  condL, bodyL, endL: string;
begin
  condL := irNewLabel('while.cond');
  bodyL := irNewLabel('while.body');
  endL := irNewLabel('while.end');
  irBr(condL);
  irStartBlock(condL);
  irCondBr(visitExpr(s^.cond), bodyL, endL);
  irStartBlock(bodyL);
  pushLoop(condL, endL);
  visitStmt(s^.thenStmt);
  popLoop;
  irBr(condL);
  irStartBlock(endL);
end;

procedure visitRepeat(s: PNode);
var
  bodyL, condL, endL: string;
  i: longint;
begin
  bodyL := irNewLabel('repeat.body');
  condL := irNewLabel('repeat.cond');
  endL := irNewLabel('repeat.end');
  irBr(bodyL);
  irStartBlock(bodyL);
  pushLoop(condL, endL);
  for i := 0 to s^.stmts.count - 1 do
    visitStmt(PNode(listGet(s^.stmts, i)));
  popLoop;
  irBr(condL);
  irStartBlock(condL);
  irCondBr(visitExpr(s^.untilCond), endL, bodyL);
  irStartBlock(endL);
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
  start, final, start64, final64, enter, i, done: TV;
  checkL, loopL, latchL, incL, endL, preL, nextName, op: string;
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
    enter := irICmp('sge', start64, final64)
  else
    enter := irICmp('sle', start64, final64);
  checkL := irNewLabel('for.check');
  loopL := irNewLabel('for.body');
  latchL := irNewLabel('for.latch');
  incL := irNewLabel('for.inc');
  endL := irNewLabel('for.end');
  irCondBr(enter, checkL, endL);

  { ISO 6.8.3.9: if the loop runs, both bounds must be assignment
    compatible with the control variable }
  irStartBlock(checkL);
  emitRangeCheckMsg(start, s^.forInit^.typ, varTy, s^.forInit^.b,
                    '''for'' loop bound out of range');
  emitRangeCheckMsg(final, s^.forFinal^.typ, varTy, s^.forFinal^.b,
                    '''for'' loop bound out of range');
  start := convertOrdinal(start, s^.forInit^.typ, varTy);
  final := convertOrdinal(final, s^.forFinal^.typ, varTy);
  preL := irCurrentBlock;
  irBr(loopL);

  irStartBlock(loopL);
  nextName := irTmp;
  i := irPhi2(start.t, start.v, preL, nextName, incL);
  storeScalar(varAddress(v), i, varTy);
  pushLoop(latchL, endL);
  visitStmt(s^.forBody);
  popLoop;
  irBr(latchL);
  irStartBlock(latchL);
  done := irICmp('eq', i, final);
  irCondBr(done, endL, incL);

  irStartBlock(incL);
  if down then
    op := 'sub'
  else
    op := 'add';
  irBinNamed(nextName, op, i, irConstInt(i.t, 1));
  irBr(loopL);
  irStartBlock(endL);
end;

procedure visitCase(s: PNode);
const
  maxExpandedRange = 256;
var
  sel, lo, hi, inRange: TV;
  signed: boolean;
  defaultL, endL, nextL: string;
  armLabels, largeRanges: TPtrList;
  sw: TBuf;
  i, j: longint;
  arm: PCaseArm;
  cl: PCaseLabel;
  armL: PName;
  v: int64;
  lr: PLargeRange;
begin
  sel := visitExpr(s^.selector);
  signed := isSignedOrdinal(s^.selector^.typ);
  defaultL := irNewLabel('case.default');
  endL := irNewLabel('case.end');
  { ranges (Turbo/FPC) become switch cases when small; large ones are
    tested with comparisons in the default block }
  listInit(armLabels);
  listInit(largeRanges);
  bufInit(sw);
  bufAdd(sw, 'switch ' + sel.t^.text + ' ' + sel.v + ', label %' + defaultL + ' [');
  for i := 0 to s^.arms.count - 1 do
  begin
    arm := PCaseArm(listGet(s^.arms, i));
    armL := intern(irNewLabel('case.arm'));
    listAdd(armLabels, armL);
    for j := 0 to arm^.labels.count - 1 do
    begin
      cl := PCaseLabel(listGet(arm^.labels, j));
      if cl^.hiValue - cl^.loValue >= maxExpandedRange then
      begin
        lr := PLargeRange(arenaAlloc(sizeof(TLargeRange)));
        lr^.lo := cl^.loValue;
        lr^.hi := cl^.hiValue;
        lr^.target := armL;
        listAdd(largeRanges, lr);
        continue;
      end;
      v := cl^.loValue;
      while v <= cl^.hiValue do
      begin
        bufAdd(sw, ' ' + sel.t^.text + ' ' + irConstInt(sel.t, v).v + ', label %' +
                   nameStr(armL));
        v := v + 1;
      end;
    end;
  end;
  bufAdd(sw, ' ]');
  irEmitTermBuf(sw);
  bufFree(sw);

  for i := 0 to s^.arms.count - 1 do
  begin
    arm := PCaseArm(listGet(s^.arms, i));
    irStartBlock(nameStr(PName(listGet(armLabels, i))));
    visitStmt(arm^.body);
    irBr(endL);
  end;

  irStartBlock(defaultL);
  for i := 0 to largeRanges.count - 1 do
  begin
    lr := PLargeRange(listGet(largeRanges, i));
    if signed then
    begin
      lo := irICmp('sge', sel, irConstInt(sel.t, lr^.lo));
      hi := irICmp('sle', sel, irConstInt(sel.t, lr^.hi));
    end
    else
    begin
      lo := irICmp('uge', sel, irConstInt(sel.t, lr^.lo));
      hi := irICmp('ule', sel, irConstInt(sel.t, lr^.hi));
    end;
    inRange := irBin('and', lo, hi);
    nextL := irNewLabel('case.range');
    irCondBr(inRange, nameStr(lr^.target), nextL);
    irStartBlock(nextL);
  end;
  if s^.elseLoc <> noLoc then
  begin
    for i := 0 to s^.elseBody.count - 1 do
      visitStmt(PNode(listGet(s^.elseBody, i)));
    irBr(endL);
  end
  else if mode = lmISO then
    { ISO 7185 6.8.3.5: an error if no label equals the selector }
    emitRuntimeError('no ''case'' label matches the selector value', s^.b)
  else
    irBr(endL); { Turbo/FPC: nothing }
  irStartBlock(endL);
  listFree(armLabels);
  listFree(largeRanges);
end;

procedure visitGoto(s: PNode);
var
  l, owner: PDecl;
  buf: TV;
begin
  l := s^.lblDecl;
  if l^.labelOwner = curRoutine then
    irBr(labelBlock(l))
  else
  begin
    { non-local: unwind to the owner's activation with _longjmp }
    owner := l^.labelOwner;
    if owner <> nil then
      buf := irStructGEP(frameType(owner), framePtrFor(owner), owner^.cgJmpSlot)
    else
      buf := ptrV(mainJmpBuf);
    rtv2('_longjmp', 'ptr, i32', buf, i32c(l^.labelIndex));
    irUnreachable;
  end;
  irStartBlock(irNewLabel('after.goto')); { unreachable code follows }
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
    w^.cgAddr := intern(emitAddress(w^.rec).v);
  end;
  visitStmt(s^.withBody);
end;

procedure visitStmt(s: PNode);
var
  i: longint;
  ignored: TV;
  lbl: string;
begin
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
        lbl := labelBlock(s^.lblDecl);
        irBr(lbl);
        irStartBlock(lbl);
        visitStmt(s^.lblBody);
      end;
    nkGoto: visitGoto(s);
    nkWith: visitWith(s);
  else
    ;
  end;
end;

{ --- Declarations --- }

{ The initializer bytes of a string or char constant for an object of
  type t (memory type ty): length-prefixed for short strings, padded with
  NULs or cut to the size. }
procedure addStringInit(var b: TBuf; p: PCharArray; start, count: int64; ty: PIRType;
                        t: PType);
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
  irAddCString(b, bytes.data, 0, ty^.count);
  bufFree(bytes);
end;

procedure addScalarConst(var b: TBuf; const c: TConstValue; t: PType; ty: PIRType);
var
  x: double;
  ch: char;
begin
  case c.kind of
    ckOrdinal:
      if t^.kind = tyReal then
      begin
        x := c.intVal;
        bufAdd(b, irConstDouble(x).v);
      end
      else if t^.kind = tyShortString then
      begin
        { a char }
        ch := chr(c.intVal and 255);
        addStringInit(b, PCharArray(@ch), 0, 1, ty, t);
      end
      else
        bufAdd(b, irConstInt(ty, c.intVal).v);
    ckReal: bufAdd(b, irConstDouble(c.realVal).v);
    ckString: addStringInit(b, c.strVal^.chars, 0, c.strVal^.len, ty, t);
  else
    bufAdd(b, irZero(ty).v);
  end;
end;

{ The constant for a typed-constant initializer init of type t (memory
  type ty); folded is Sema's value for non-aggregates. }
procedure addConstant(var b: TBuf; init: PNode; const folded: TConstValue; t: PType;
                      ty: PIRType);
var
  i, j: longint;
  el, match: PAggElement;
  f: PDecl;
  fty: PIRType;
begin
  if init^.kind <> nkAggregate then
  begin
    addScalarConst(b, folded, t, ty);
    exit;
  end;
  if t^.kind = tyArray then
  begin
    bufAddChar(b, '[');
    for i := 0 to init^.aggElems.count - 1 do
    begin
      el := PAggElement(listGet(init^.aggElems, i));
      if i > 0 then
        bufAdd(b, ', ');
      bufAdd(b, ty^.elem^.text + ' ');
      addConstant(b, el^.value, el^.folded, t^.elementType, ty^.elem);
    end;
    bufAddChar(b, ']');
    exit;
  end;
  if t^.kind = tyRecord then
  begin
    { fixed fields only (Sema rejects variant parts); missing ones are 0 }
    if t^.fieldList^.fixed.count = 0 then
    begin
      bufAdd(b, '{}');
      exit;
    end;
    bufAdd(b, '{ ');
    for i := 0 to t^.fieldList^.fixed.count - 1 do
    begin
      f := PDecl(listGet(t^.fieldList^.fixed, i));
      fty := PIRType(listGet(ty^.fields, i));
      if i > 0 then
        bufAdd(b, ', ');
      bufAdd(b, fty^.text + ' ');
      match := nil;
      for j := 0 to init^.aggElems.count - 1 do
      begin
        el := PAggElement(listGet(init^.aggElems, j));
        if el^.field.name = f^.ident.name then
          match := el;
      end;
      if match <> nil then
        addConstant(b, match^.value, match^.folded, f^.fieldType, fty)
      else
        bufAdd(b, irZero(fty).v);
    end;
    bufAdd(b, ' }');
    exit;
  end;
  { a scalar in parentheses }
  el := PAggElement(listGet(init^.aggElems, 0));
  addConstant(b, el^.value, folded, t, ty);
end;

{ A static variable: a typed constant, or a variable of the program or of
  a unit (zero-initialized: ISO leaves them undefined; this makes programs
  deterministic at no cost). }
procedure emitStaticVar(v: PDecl);
var
  ty: PIRType;
  name: string;
  init: TBuf;
begin
  ty := memType(v^.varType);
  name := '@' + uniqueSym('pas.' + nameStr(v^.ident.name));
  v^.cgName := intern(name);
  bufInit(init);
  if v^.init <> nil then
    addConstant(init, v^.init, v^.initValue, v^.varType, ty)
  else
    bufAdd(init, irZero(ty).v);
  irGlobalBuf(name, 'internal', ty, init, false, 0);
  bufFree(init);
end;

procedure emitGlobals(blk: PBlock);
var
  i: longint;
  d: PDecl;
begin
  for i := 0 to blk^.decls.count - 1 do
  begin
    d := PDecl(listGet(blk^.decls, i));
    if d^.kind = dkVar then
      emitStaticVar(d);
  end;
end;

function paramIRType(p: PDecl): PIRType;
begin
  if (p^.varKind = vkVarParam) or isStructured(p^.varType) then
    paramIRType := irPtrT
  else
    paramIRType := regType(p^.varType);
end;

{ An external (C) routine: no static link, C types. }
procedure declareExternal(r: PDecl);
var
  sym, decl: string;
  i: longint;
  p: PDecl;
  ret: PIRType;
begin
  if r^.externalName <> nil then
    sym := nameStr(r^.externalName)
  else
    sym := nameStr(r^.ident.spelling);
  ret := retTypeOf(nil);
  if r^.isFunction then
    ret := regType(r^.resultType);
  decl := 'declare ' + ret^.text + ' @"' + sym + '"(';
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    if i > 0 then
      decl := decl + ', ';
    if p^.varKind = vkVarParam then
      decl := decl + 'ptr'
    else
      decl := decl + regType(p^.varType)^.text;
  end;
  irDeclare(sym, decl + ')');
  r^.cgName := intern('@"' + sym + '"');
end;

{ Lays out the frames of the routines declared in blk (recursively) and
  queues their bodies. Frame: [static link], parameters, locals, [result],
  [jmp_buf]. Every routine takes its static link first (null at the outer
  level), so procedural parameters can call any routine the same way. }
procedure declareRoutines(blk: PBlock);
var
  i, j: longint;
  r, v: PDecl;
  slots: TPtrList;
  name: string;

  procedure addSlot(x: PDecl);
  begin
    x^.cgSlot := slots.count;
    if isByRef(x) then
      listAdd(slots, irPtrT)
    else
      listAdd(slots, memType(x^.varType));
  end;

begin
  for i := 0 to blk^.decls.count - 1 do
  begin
    r := PDecl(listGet(blk^.decls, i));
    if (r^.kind <> dkRoutine) or (r^.canonical <> nil) then
      continue; { the forward declaration is the canonical node }
    if r^.isExternal then
    begin
      declareExternal(r);
      continue;
    end;
    listInit(slots);
    if r^.parent <> nil then
      listAdd(slots, irPtrT);
    for j := 0 to r^.params.count - 1 do
      addSlot(PDecl(listGet(r^.params, j)));
    for j := 0 to r^.body^.decls.count - 1 do
    begin
      v := PDecl(listGet(r^.body^.decls, j));
      if v^.kind <> dkVar then
        continue;
      if v^.init <> nil then
        emitStaticVar(v) { static storage, local scope }
      else
        addSlot(v);
    end;
    if r^.isFunction then
      addSlot(r^.resultVar);
    name := uniqueSym(mangle(r));
    r^.cgJmpSlot := -1;
    if hasNonLocalLabels(r^.body) then
    begin
      r^.cgJmpSlot := slots.count;
      listAdd(slots, jmpBufT);
      r^.cgFrame := irStructNamed('frame.' + copy(name, 5, 255) + '.jb', slots);
    end
    else
      r^.cgFrame := irStructNamed('frame.' + copy(name, 5, 255), slots);
    listFree(slots);
    r^.cgName := intern('@' + name);
    listAdd(routinesToEmit, r);
    declareRoutines(r^.body);
  end;
end;

procedure emitRoutineBody(r: PDecl);
var
  header: TBuf;
  frameT: PIRType;
  i: longint;
  p, v, res: PDecl;
  arg, slot: TV;
begin
  curRoutine := r;
  frameT := frameType(r);
  bufInit(header);
  bufAdd(header, 'define internal ' + retTypeOf(nil)^.text);
  if r^.isFunction then
  begin
    bufClear(header);
    bufAdd(header, 'define internal ' + regType(r^.resultType)^.text);
  end;
  bufAdd(header, ' ' + nameStr(r^.cgName) + '(ptr %link');
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    bufAdd(header, ', ' + paramIRType(p)^.text + ' %p' + intToStr(i));
  end;
  bufAddChar(header, ')');
  irBeginFunction(header);
  bufFree(header);

  curFrame := irAlloca(frameT, 'frame');
  { ISO leaves locals undefined; start from zero for determinism }
  emitMemSet(curFrame, i8c(0), i64c(frameT^.size));
  if r^.parent <> nil then
    irStore(ptrV('%link'), irStructGEP(frameT, curFrame, 0));
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    arg := irV(paramIRType(p), '%p' + intToStr(i));
    slot := irStructGEP(frameT, curFrame, p^.cgSlot);
    if isByRef(p) then
      irStore(arg, slot)
    else if isStructured(p^.varType) then
      copyAggregate(slot, arg, p^.varType) { value semantics }
    else
      storeScalar(slot, arg, p^.varType);
  end;

  initFileVars(r^.body^.decls, false);
  if r^.cgJmpSlot >= 0 then
    emitSetJmpDispatch(r^.body, irStructGEP(frameT, curFrame, r^.cgJmpSlot));
  exitLabel := irNewLabel('exit');
  visitStmt(r^.body^.body);
  irBr(exitLabel);
  irStartBlock(exitLabel);
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if v^.kind = dkVar then
      emitFileLifetime(varAddress(v), v^.varType, false, irNull);
  end;
  if r^.isFunction then
  begin
    res := r^.resultVar;
    slot := irStructGEP(frameT, curFrame, res^.cgSlot);
    irRet(loadScalar(slot, res^.varType));
  end
  else
    irRetVoid;
  irEndFunction;
end;

{ --- The program --- }

procedure emitMain(prog: PDecl; const units: TPtrList);
var
  header: TBuf;
  blk: PBlock;
  i: longint;
  u: PDecl;
begin
  blk := prog^.progBlock;
  curRoutine := nil;
  curFrame := irNull;
  bufInit(header);
  bufAdd(header, 'define i32 @main(i32 %argc, ptr %argv)');
  irBeginFunction(header);
  bufFree(header);
  rtv2('__pas_init', 'i32, ptr', irV(irI32, '%argc'), ptrV('%argv'));
  if mode = lmTP then
  begin
    irDeclare('__pas_tp_mode', '@__pas_tp_mode = external global i32');
    irStore(i32c(1), ptrV('@__pas_tp_mode'));
  end;
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
  initFileVars(blk^.decls, true);
  if mainJmpBuf <> '' then
    emitSetJmpDispatch(blk, ptrV(mainJmpBuf));
  exitLabel := irNewLabel('exit');
  visitStmt(blk^.body);
  irBr(exitLabel);
  irStartBlock(exitLabel);
  rtv0('__pas_fini');
  irRet(i32c(0));
  irEndFunction;
end;

procedure generateModule(prog: PDecl; const units: TPtrList; m: TLangMode;
                         rangeChecksOn, overflowChecksOn: boolean;
                         const sourceName: string; var outText: TBuf);
var
  i: longint;
  u: PDecl;
  fields: TPtrList;
begin
  mode := m;
  optRange := rangeChecksOn;
  optOverflow := overflowChecksOn;
  progDecl := prog;
  curRoutine := nil;
  mapInit(lowered);
  mapInit(stringConsts);
  mapInit(symCounts);
  listInit(routinesToEmit);
  listInit(loopLabels);
  mainJmpBuf := '';
  irInit(sourceName);
  listInit(fields);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  routinePairT := irStructLit(fields);
  listFree(fields);
  { generous for every supported libc (glibc: 200-312 bytes, macOS: 192) }
  jmpBufT := irArray(64, irI64);

  for i := 0 to units.count - 1 do
  begin
    u := PDecl(listGet(units, i));
    emitGlobals(u^.iface);
    emitGlobals(u^.impl);
  end;
  emitGlobals(prog^.progBlock);
  if hasNonLocalLabels(prog^.progBlock) then
  begin
    { needed by gotos in routine bodies }
    mainJmpBuf := '@' + uniqueSym('pas.jmpbuf');
    irGlobal(mainJmpBuf, 'internal', jmpBufT, 'zeroinitializer', false, 16);
  end;
  for i := 0 to units.count - 1 do
  begin
    u := PDecl(listGet(units, i));
    declareRoutines(u^.iface);
    declareRoutines(u^.impl);
  end;
  declareRoutines(prog^.progBlock);
  for i := 0 to routinesToEmit.count - 1 do
    emitRoutineBody(PDecl(listGet(routinesToEmit, i)));
  emitMain(prog, units);
  irWriteModule(outText);
end;
