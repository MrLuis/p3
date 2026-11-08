{ Included by pcsema.pas: built-in routines, statements and expressions
  (the second half of src/sema/Sema.cpp). }

{ --- Built-in procedure calls --- }

function isWritable(t: PType): boolean;
begin
  isWritable := isIntegerLike(t) or (t^.kind = tyReal) or
                (getBase(t)^.kind = tyChar) or (getBase(t)^.kind = tyBoolean) or
                isStringType(t) or (t^.kind = tyShortString);
end;

{ If the first argument of read/write is a file, checks and returns its
  type; otherwise nil. }
function checkFileArg(s: PNode): PType;
var
  first: PNode;
  t: PType;
begin
  checkFileArg := nil;
  if (s^.args.count = 0) or (PNode(listGet(s^.args, 0))^.kind = nkFormatted) then
    exit;
  first := PNode(listGet(s^.args, 0));
  t := check(first);
  if t^.kind <> tyFile then
    exit;
  if not isVariableReference(first) then
  begin
    ms('file argument must be a variable');
    errR(first^.b, nodeRange(first));
    exit;
  end;
  checkFileArg := t;
end;

procedure maCallee(const callee: TIdent);
begin
  ma('''');
  maN(callee.spelling);
  ma('''');
end;

procedure checkWrite(s: PNode; isWriteln: boolean);
var
  fileTy, t, wt: PType;
  i, first, k: longint;
  arg, value, width, precision, w: PNode;
begin
  fileTy := checkFileArg(s);
  first := 0;
  if fileTy <> nil then
    first := 1;
  if not isWriteln and (s^.args.count - first = 0) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' requires at least one argument');
    errR(s^.callee.loc, identRange(s^.callee));
  end;
  if (fileTy <> nil) and not fileTy^.isText then
  begin
    if isWriteln then
    begin
      ms('''writeln'' requires a text file');
      errR(s^.callee.loc, identRange(s^.callee));
    end;
    for i := first to s^.args.count - 1 do
    begin
      arg := PNode(listGet(s^.args, i));
      if arg^.kind = nkFormatted then
      begin
        ms('field widths are only allowed for text files');
        errR(arg^.fmtWidth^.b, nodeRange(arg^.fmtWidth));
        continue;
      end;
      t := check(arg);
      if not isError(t) and not isAssignable(fileTy^.fileElement, t) then
      begin
        ms('cannot write a value of type ');
        maT(t);
        ma(' to a ');
        maT(fileTy);
        errR(arg^.b, nodeRange(arg));
      end;
    end;
    exit;
  end;
  for i := first to s^.args.count - 1 do
  begin
    arg := PNode(listGet(s^.args, i));
    value := arg;
    width := nil;
    precision := nil;
    if arg^.kind = nkFormatted then
    begin
      value := arg^.fmtValue;
      width := arg^.fmtWidth;
      precision := arg^.fmtPrecision;
    end;
    t := check(value);
    if arg^.kind = nkFormatted then
      arg^.typ := t;
    if not isError(t) and not isWritable(t) then
    begin
      ms('cannot write a value of type ');
      maT(t);
      errR(value^.b, nodeRange(value));
    end;
    for k := 1 to 2 do
    begin
      if k = 1 then
        w := width
      else
        w := precision;
      if w = nil then
        continue;
      wt := check(w);
      if not isError(wt) and not isIntegerLike(wt) then
      begin
        if k = 1 then
          ms('field width')
        else
          ms('precision');
        ma(' must be an integer, not ');
        maT(wt);
        errR(w^.b, nodeRange(w));
      end;
    end;
    { ISO 6.9.3.4: only real values take a fraction-digits argument }
    if (precision <> nil) and not isError(t) and (t^.kind <> tyReal) then
    begin
      ms('a precision ('':n'') is only allowed for real values');
      errR(precision^.b, nodeRange(precision));
    end;
  end;
end;

procedure checkRead(s: PNode; isReadln: boolean);
var
  fileTy, t: PType;
  i, first: longint;
  arg: PNode;
begin
  fileTy := checkFileArg(s);
  first := 0;
  if fileTy <> nil then
    first := 1;
  if not isReadln and (s^.args.count - first = 0) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' requires at least one argument');
    errR(s^.callee.loc, identRange(s^.callee));
  end;
  if (fileTy <> nil) and not fileTy^.isText then
  begin
    if isReadln then
    begin
      ms('''readln'' requires a text file');
      errR(s^.callee.loc, identRange(s^.callee));
    end;
    for i := first to s^.args.count - 1 do
    begin
      arg := PNode(listGet(s^.args, i));
      t := check(arg);
      if isError(t) then
        continue;
      if not isVariableReference(arg) then
      begin
        ms('argument to ');
        maCallee(s^.callee);
        ma(' must be a variable');
        errR(arg^.b, nodeRange(arg));
      end
      else if not isAssignable(t, fileTy^.fileElement) then
      begin
        ms('cannot read a component of ');
        maT(fileTy);
        ma(' into a variable of type ');
        maT(t);
        errR(arg^.b, nodeRange(arg));
      end
      else
        checkNotForControlVar(arg);
    end;
    exit;
  end;
  for i := first to s^.args.count - 1 do
  begin
    arg := PNode(listGet(s^.args, i));
    if arg^.kind = nkFormatted then
    begin
      ms('field widths are only allowed in ''write'' and ''writeln''');
      errR(arg^.fmtWidth^.b, nodeRange(arg^.fmtWidth));
      continue;
    end;
    t := check(arg);
    if isError(t) then
      continue;
    if not isVariableReference(arg) then
    begin
      ms('argument to ');
      maCallee(s^.callee);
      ma(' must be a variable');
      errR(arg^.b, nodeRange(arg));
      continue;
    end;
    if not isIntegerLike(t) and (getBase(t)^.kind <> tyChar) and
       (t^.kind <> tyReal) and (t^.kind <> tyShortString) then
    begin
      ms('cannot read a value of type ');
      maT(t);
      ma(' (only integer, real and char)');
      errR(arg^.b, nodeRange(arg));
    end
    else if checkModifiable(arg) then
      checkNotForControlVar(arg);
  end;
end;

{ reset/rewrite/get/put(f) and page[(f)] }
procedure checkFileProc(s: PNode; isPage: boolean);
var
  t: PType;
  a: PNode;
begin
  if isPage and (s^.args.count = 0) then
    exit; { page(output) }
  if s^.args.count <> 1 then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects 1 file argument');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  a := PNode(listGet(s^.args, 0));
  t := check(a);
  if isError(t) then
    exit;
  if (t^.kind <> tyFile) or not isVariableReference(a) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects a file variable, not a value of type ');
    maT(t);
    errR(a^.b, nodeRange(a));
  end
  else if isPage and not isTextFile(t) then
  begin
    ms('''page'' requires a text file');
    errR(a^.b, nodeRange(a));
  end;
end;

{ pack(a, i, z): z := a[i..]   unpack(z, a, i): a[i..] := z }
procedure checkPackUnpack(s: PNode; isPack: boolean);
var
  unpacked, index, packed_: PNode;
  ut, it, pt: PType;
begin
  if s^.args.count <> 3 then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects 3 arguments');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  if isPack then
  begin
    unpacked := PNode(listGet(s^.args, 0));
    index := PNode(listGet(s^.args, 1));
    packed_ := PNode(listGet(s^.args, 2));
  end
  else
  begin
    unpacked := PNode(listGet(s^.args, 1));
    index := PNode(listGet(s^.args, 2));
    packed_ := PNode(listGet(s^.args, 0));
  end;
  ut := check(unpacked);
  it := check(index);
  pt := check(packed_);
  if isError(ut) or isError(it) or isError(pt) then
    exit;
  if (ut^.kind <> tyArray) or ut^.isPackedArray or not isVariableReference(unpacked) then
  begin
    ms('expected an unpacked array variable');
    errR(unpacked^.b, nodeRange(unpacked));
    exit;
  end;
  if (pt^.kind <> tyArray) or not pt^.isPackedArray or not isVariableReference(packed_) then
  begin
    ms('expected a packed array variable');
    errR(packed_^.b, nodeRange(packed_));
    exit;
  end;
  if ut^.elementType <> pt^.elementType then
  begin
    ms('packed and unpacked arrays must have the same component type');
    errR(packed_^.b, nodeRange(packed_));
  end;
  if not areCompatible(it, ut^.indexType) then
  begin
    ms('index of type ');
    maT(it);
    ma(' does not match ');
    maT(ut^.indexType);
    errR(index^.b, nodeRange(index));
  end;
end;

{ delete(var s, index, count) / insert(source, var s, index) }
procedure checkDeleteInsert(s: PNode; isInsert: boolean);
var
  str, a: PNode;
  t: PType;
  i: longint;
begin
  if s^.args.count <> 3 then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects 3 arguments');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  if isInsert then
    str := PNode(listGet(s^.args, 1))
  else
    str := PNode(listGet(s^.args, 0));
  for i := 0 to 2 do
  begin
    a := PNode(listGet(s^.args, i));
    t := check(a);
    if isError(t) then
      continue;
    if a = str then
    begin
      if (t^.kind <> tyShortString) or not isVariableReference(a) then
      begin
        ms('expected a string variable');
        errR(a^.b, nodeRange(a));
      end
      else
        checkModifiable(a);
    end
    else if isInsert and (i = 0) then
    begin
      if not isStringish(t) then
      begin
        ms('expected a string');
        errR(a^.b, nodeRange(a));
      end;
    end
    else if not isIntegerLike(t) then
    begin
      ms('expected an integer');
      errR(a^.b, nodeRange(a));
    end;
  end;
end;

{ inc(x [, n]) / dec(x [, n]) }
procedure checkIncDec(s: PNode);
var
  t, n: PType;
  a0, a1: PNode;
begin
  if (s^.args.count = 0) or (s^.args.count > 2) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects 1 or 2 arguments');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  a0 := PNode(listGet(s^.args, 0));
  t := check(a0);
  if not isError(t) and (not isOrdinal(t) or not isVariableReference(a0)) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects an ordinal variable');
    errR(a0^.b, nodeRange(a0));
  end
  else if not isError(t) and checkModifiable(a0) then
    checkNotForControlVar(a0);
  if s^.args.count = 2 then
  begin
    a1 := PNode(listGet(s^.args, 1));
    n := check(a1);
    if not isError(n) and not isIntegerLike(n) then
    begin
      ms('step must be an integer');
      errR(a1^.b, nodeRange(a1));
    end;
  end;
end;

{ new(p [, tags]) / dispose(p [, tags]): the tags are only checked }
procedure checkNewDispose(s: PNode; isNew: boolean);
var
  arg: PNode;
  t: PType;
  i: longint;
  c: TConstant;
begin
  if s^.args.count = 0 then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects a pointer argument');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  arg := PNode(listGet(s^.args, 0));
  t := check(arg);
  if not isError(t) then
  begin
    if t^.kind <> tyPointer then
    begin
      ms('');
      maCallee(s^.callee);
      if isNew then
        ma(' expects a pointer variable, not a value of type ')
      else
        ma(' expects a pointer , not a value of type ');
      maT(t);
      errR(arg^.b, nodeRange(arg));
    end
    else if isNew and not isVariableReference(arg) then
    begin
      ms('argument to ''new'' must be a variable');
      errR(arg^.b, nodeRange(arg));
    end
    else if isNew then
      checkModifiable(arg);
  end;
  for i := 1 to s^.args.count - 1 do
    evaluateConstant(PNode(listGet(s^.args, i)), 'variant selector', c);
end;

{ getmem(p, n) freemem(p) reallocmem(p, n) fillchar(x, n, v)
  move(src, dst, n) halt[(code)] }
procedure checkMemoryProc(s: PNode; k: TBuiltinKind);
var
  want, i: longint;
  t: array[0..2] of PType;
  a: array[0..2] of PNode;
  ht: PType;

  procedure expectArg(n: longint; ok: boolean; const what: string);
  begin
    if not ok then
    begin
      ms('');
      maCallee(s^.callee);
      ma(' expects ' + what);
      errR(a[n]^.b, nodeRange(a[n]));
    end;
  end;

begin
  if k = bkFreeMem then
    want := 1
  else if (k = bkGetMem) or (k = bkReallocMem) then
    want := 2
  else
    want := 3;
  if k = bkHalt then
  begin
    if s^.args.count > 1 then
    begin
      ms('''halt'' expects at most 1 argument');
      errR(s^.callee.loc, nodeRange(s));
    end
    else if s^.args.count = 1 then
    begin
      ht := check(PNode(listGet(s^.args, 0)));
      if not isError(ht) and not isIntegerLike(ht) then
      begin
        ms('''halt'' expects an integer exit code, not ');
        maT(ht);
        errR(PNode(listGet(s^.args, 0))^.b, nodeRange(PNode(listGet(s^.args, 0))));
      end;
    end;
    exit;
  end;
  if s^.args.count <> want then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects ' + intToStr(want) + ' argument');
    if want <> 1 then
      ma('s');
    errR(s^.callee.loc, nodeRange(s));
    for i := 0 to s^.args.count - 1 do
      check(PNode(listGet(s^.args, i)));
    exit;
  end;
  for i := 0 to want - 1 do
  begin
    a[i] := PNode(listGet(s^.args, i));
    t[i] := check(a[i]);
  end;
  for i := 0 to want - 1 do
    if isError(t[i]) then
      exit;
  case k of
    bkGetMem, bkReallocMem:
      begin
        expectArg(0, (t[0]^.kind = tyPointer) and isVariableReference(a[0]),
                  'a pointer variable');
        expectArg(1, isIntegerLike(t[1]), 'an integer size');
        if (t[0]^.kind = tyPointer) and isVariableReference(a[0]) then
          checkModifiable(a[0]);
      end;
    bkFreeMem:
      expectArg(0, t[0]^.kind = tyPointer, 'a pointer');
    bkFillChar:
      begin
        expectArg(0, isVariableReference(a[0]) and not containsFile(t[0]), 'a variable');
        expectArg(1, isIntegerLike(t[1]), 'an integer count');
        expectArg(2, isOrdinal(t[2]), 'an ordinal fill value');
        if isVariableReference(a[0]) then
          checkModifiable(a[0]);
      end;
  else { move }
    begin
      expectArg(0, isVariableReference(a[0]) and not containsFile(t[0]),
                'a source variable');
      expectArg(1, isVariableReference(a[1]) and not containsFile(t[1]),
                'a destination variable');
      expectArg(2, isIntegerLike(t[2]), 'an integer count');
      if isVariableReference(a[1]) then
        checkModifiable(a[1]);
    end;
  end;
end;

{ --- Built-in function calls --- }

{ argument types of e, already checked }
function argType(e: PNode; i: longint): PType;
begin
  argType := PNode(listGet(e^.args, i))^.typ;
end;

function argNode(e: PNode; i: longint): PNode;
begin
  argNode := PNode(listGet(e^.args, i));
end;

function checkStringFunction(e: PNode; bk: TBuiltinKind): PType;
var
  i: longint;

  function bad(a: PNode; const what: string): PType;
  begin
    ms('');
    maCallee(e^.callee);
    ma(' expects ' + what);
    errR(a^.b, nodeRange(a));
    bad := errorType;
  end;

  function arity(n: longint): boolean;
  begin
    if e^.args.count = n then
      exit(true);
    ms('');
    maCallee(e^.callee);
    ma(' expects ' + intToStr(n) + ' argument');
    if n <> 1 then
      ma('s');
    errR(e^.callee.loc, nodeRange(e));
    arity := false;
  end;

begin
  checkStringFunction := errorType;
  for i := 0 to e^.args.count - 1 do
    if isError(argType(e, i)) then
      exit;
  case bk of
    bkLength:
      begin
        if not arity(1) then
          exit;
        if isStringish(argType(e, 0)) then
          checkStringFunction := tyIntegerT
        else
          checkStringFunction := bad(argNode(e, 0), 'a string');
      end;
    bkUpCase:
      begin
        if not arity(1) then
          exit;
        if getBase(argType(e, 0))^.kind = tyChar then
          checkStringFunction := tyCharT
        else
          checkStringFunction := bad(argNode(e, 0), 'a char');
      end;
    bkPos:
      begin
        if not arity(2) then
          exit;
        for i := 0 to 1 do
          if not isStringish(argType(e, i)) then
            exit(bad(argNode(e, i), 'strings'));
        checkStringFunction := tyIntegerT;
      end;
    bkCopy:
      begin
        if not arity(3) then
          exit;
        if not isStringish(argType(e, 0)) then
          exit(bad(argNode(e, 0), 'a string'));
        if not isIntegerLike(argType(e, 1)) or not isIntegerLike(argType(e, 2)) then
          exit(bad(argNode(e, 1), 'integer index and count'));
        checkStringFunction := shortStringType(255);
      end;
  else { concat }
    begin
      if e^.args.count = 0 then
      begin
        ms('''concat'' expects strings');
        errR(e^.callee.loc, nodeRange(e));
        exit;
      end;
      for i := 0 to e^.args.count - 1 do
        if not isStringish(argType(e, i)) then
          exit(bad(argNode(e, i), 'strings'));
      checkStringFunction := shortStringType(255);
    end;
  end;
end;

function checkBuiltinFunction(e: PNode; bk: TBuiltinKind): PType;
var
  t: PType;
  arg: PNode;
  isEoln: boolean;

  function expect(ok: boolean; const what: string): boolean;
  begin
    if not ok then
    begin
      ms('');
      maCallee(e^.callee);
      ma(' expects ' + what + ' argument, not ');
      maT(t);
      errR(arg^.b, nodeRange(arg));
    end;
    expect := ok;
  end;

begin
  checkBuiltinFunction := errorType;
  if bk in [bkLength, bkCopy, bkPos, bkConcat, bkUpCase] then
    exit(checkStringFunction(e, bk));
  if e^.args.count <> 1 then
  begin
    ms('');
    maCallee(e^.callee);
    ma(' expects 1 argument, got ');
    maI(e^.args.count);
    errR(e^.callee.loc, nodeRange(e));
    exit;
  end;
  t := argType(e, 0);
  if isError(t) then
    exit;
  arg := argNode(e, 0);
  if (bk = bkEof) or (bk = bkEoln) then
  begin
    isEoln := bk = bkEoln;
    if (t^.kind <> tyFile) or not isVariableReference(arg) or
       (isEoln and not isTextFile(t)) then
    begin
      ms('');
      maCallee(e^.callee);
      if isEoln then
        ma(' expects a text file variable, not ')
      else
        ma(' expects a file variable, not ');
      maT(t);
      errR(arg^.b, nodeRange(arg));
      exit;
    end;
    exit(tyBooleanT);
  end;
  case bk of
    bkAbs, bkSqr:
      begin
        if not expect(isNumeric(t), 'a numeric') then
          exit;
        if t^.kind = tyReal then
          checkBuiltinFunction := tyRealT
        else
          checkBuiltinFunction := promoteInteger(t);
      end;
    bkOdd:
      if expect(isIntegerLike(t), 'an integer') then
        checkBuiltinFunction := tyBooleanT;
    bkOrd:
      begin
        if not expect(isOrdinal(t), 'an ordinal') then
          exit;
        if isIntegerLike(t) then
          checkBuiltinFunction := promoteInteger(t)
        else
          checkBuiltinFunction := tyIntegerT;
      end;
    bkChr:
      if expect(isIntegerLike(t), 'an integer') then
        checkBuiltinFunction := tyCharT;
    bkSucc, bkPred:
      if expect(isOrdinal(t), 'an ordinal') then
        checkBuiltinFunction := getBase(t);
    bkSin, bkCos, bkExp, bkLn, bkSqrt, bkArctan:
      if expect(isNumeric(t), 'a numeric') then
        checkBuiltinFunction := tyRealT;
    bkTrunc, bkRound:
      begin
        if not expect(isNumeric(t), 'a real') then
          exit;
        { Turbo/FPC: the result is an int64 }
        if isISO then
          checkBuiltinFunction := tyIntegerT
        else
          checkBuiltinFunction := tyInt64T;
      end;
  else
    ;
  end;
end;

{ T(x) (Turbo/FPC): between ordinal types (integer types wrap, not
  range-checked) and between pointer types. }
function checkTypecast(e: PNode; td: PDecl): PType;
var
  t, at: PType;
  i: longint;
begin
  checkTypecast := errorType;
  t := td^.declType;
  if t = nil then
    t := errorType;
  if e^.args.count <> 1 then
  begin
    ms('a typecast to ');
    maT(t);
    ma(' takes exactly one value');
    errR(e^.callee.loc, nodeRange(e));
    for i := 0 to e^.args.count - 1 do
      check(argNode(e, i));
    exit;
  end;
  at := check(argNode(e, 0));
  if isError(t) or isError(at) then
    exit;
  if (isOrdinal(t) and isOrdinal(at)) or
     ((t^.kind = tyPointer) and (at^.kind in [tyPointer, tyNil, tyClass])) or
     ((t^.kind = tyClass) and (at^.kind in [tyClass, tyPointer, tyNil])) then
    exit(t);
  ms('invalid typecast from ');
  maT(at);
  ma(' to ');
  maT(t);
  ma(' (only between ordinal types and between pointer types)');
  errR(e^.b, nodeRange(e));
end;

{ A type name or an expression as the argument of sizeof/low/high: its
  type (also stored on the argument). }
function checkTypeOrValue(arg: PNode): PType;
var
  d: PDecl;
  t: PType;
begin
  if arg^.kind = nkNameRef then
  begin
    d := lookup(arg^.ref.name);
    if (d <> nil) and (d^.kind = dkType) then
    begin
      arg^.refDecl := d;
      t := d^.declType;
      if t = nil then
        t := errorType;
      arg^.typ := t;
      exit(t);
    end;
  end;
  checkTypeOrValue := check(arg);
end;

{ Built-ins whose arguments are not ordinary values (type names,
  procedural variables) or that take none. false for the others. }
function checkSpecialBuiltin(e: PNode; bk: TBuiltinKind; var res: PType): boolean;
var
  t: PType;
  a0: PNode;
  v: PDecl;

  function arity(n: longint): boolean;
  var
    j: longint;
  begin
    if e^.args.count = n then
      exit(true);
    ms('');
    maCallee(e^.callee);
    ma(' expects ' + intToStr(n) + ' argument');
    if n <> 1 then
      ma('s');
    errR(e^.callee.loc, nodeRange(e));
    for j := 0 to e^.args.count - 1 do
      check(argNode(e, j));
    arity := false;
  end;

begin
  checkSpecialBuiltin := true;
  res := errorType;
  case bk of
    bkSizeOf:
      begin
        if not arity(1) then
          exit;
        a0 := argNode(e, 0);
        t := checkTypeOrValue(a0);
        if isError(t) then
          exit;
        if containsFile(t) or (t^.kind = tyStringLit) or (t^.kind = tyNil) or
           isEmptySet(t) then
        begin
          ms('''sizeof'' cannot be applied to ');
          maT(t);
          errR(a0^.b, nodeRange(a0));
          exit;
        end;
        res := tyInt64T;
      end;
    bkLow, bkHigh:
      begin
        if not arity(1) then
          exit;
        a0 := argNode(e, 0);
        t := checkTypeOrValue(a0);
        if isError(t) then
          exit;
        if t^.kind = tyShortString then
          res := tyIntegerT
        else if t^.kind = tyArray then
          res := t^.indexType
        else if isOrdinal(t) then
          res := t
        else
        begin
          ms('');
          maCallee(e^.callee);
          ma(' expects an ordinal type, an array or a string, not ');
          maT(t);
          errR(a0^.b, nodeRange(a0));
        end;
      end;
    bkAssigned:
      begin
        if not arity(1) then
          exit;
        a0 := argNode(e, 0);
        { a procedural variable is tested, not called }
        v := nil;
        if a0^.kind = nkNameRef then
        begin
          v := lookup(a0^.ref.name);
          if (v <> nil) and ((v^.kind <> dkVar) or (v^.varType = nil) or
                             (v^.varType^.kind <> tyRoutine)) then
            v := nil;
        end;
        if v <> nil then
        begin
          a0^.refDecl := v;
          a0^.typ := v^.varType;
          v^.isUsed := true;
          t := v^.varType;
        end
        else
          t := check(a0);
        if isError(t) then
          exit;
        if (t^.kind <> tyPointer) and (t^.kind <> tyClass) and
           (t^.kind <> tyRoutine) then
        begin
          ms('''assigned'' expects a pointer or a procedural variable, not ');
          maT(t);
          errR(a0^.b, nodeRange(a0));
          exit;
        end;
        res := tyBooleanT;
      end;
    bkParamCount:
      if arity(0) then
        res := tyIntegerT;
    bkParamStr:
      begin
        if not arity(1) then
          exit;
        a0 := argNode(e, 0);
        t := check(a0);
        if not isError(t) and not isIntegerLike(t) then
        begin
          ms('''paramstr'' expects an integer, not ');
          maT(t);
          errR(a0^.b, nodeRange(a0));
          exit;
        end;
        res := shortStringType(255);
      end;
  else
    checkSpecialBuiltin := false;
  end;
end;

{ --- Statements --- }

{ --- Classes (objfpc): members, method calls, inherited --- }

{ "; did you mean 'X'?" for a member name of class c: the closest member of
  c or its ancestors (ties: the alphabetically first). }
procedure maDidYouMeanMember(c: PType; name: PName);
var
  limit, best, dist, i: longint;
  bestName, bestSpelling, cand: PName;
  d: PDecl;
begin
  if name^.len <= 3 then
    limit := 1
  else if name^.len div 3 > 2 then
    limit := name^.len div 3
  else
    limit := 2;
  best := limit + 1;
  bestName := nil;
  bestSpelling := nil;
  while c <> nil do
  begin
    for i := 0 to c^.classMembers.count - 1 do
    begin
      d := PDecl(listGet(c^.classMembers, i));
      cand := d^.ident.name;
      if cand = name then
        continue;
      dist := editDistance(name, cand);
      if (dist < best) or
         ((dist = best) and (dist <= limit) and nameLess(cand, bestName)) then
      begin
        best := dist;
        bestName := cand;
        bestSpelling := d^.ident.spelling;
      end;
    end;
    c := c^.classParent;
  end;
  if bestName <> nil then
  begin
    ma('; did you mean ''');
    maN(bestSpelling);
    ma('''?');
  end;
end;

{ A property used as a value: it needs a read accessor. }
function checkPropertyRead(p: PDecl; e: PNode; const id: TIdent): PType;
begin
  if (e = curAssignTarget) or (p^.readDecl <> nil) or isError(p^.propType) or
     identValid(p^.readName) then
    exit(p^.propType);
  ms('property ''');
  maN(id.spelling);
  ma(''' is write-only');
  errR(id.loc, identRange(id));
  noteDeclaredHere(p);
  checkPropertyRead := errorType;
end;

{ The type of a property assigned to; it needs a write accessor. }
function checkPropertyWrite(p: PDecl; const id: TIdent): PType;
begin
  if (p^.writeDecl <> nil) or isError(p^.propType) or
     ((p^.readDecl = nil) and identValid(p^.writeName)) then
    exit(p^.propType);
  ms('property ''');
  maN(id.spelling);
  ma(''' is read-only');
  errR(id.loc, identRange(id));
  noteDeclaredHere(p);
  checkPropertyWrite := errorType;
end;

{ Is the target of an assignment a property (written by its accessor)? }
function assignedProperty(target: PNode): PDecl;
begin
  assignedProperty := nil;
  if (target^.kind = nkNameRef) and (target^.refDecl <> nil) and
     (target^.refDecl^.kind = dkProperty) then
    assignedProperty := target^.refDecl
  else if (target^.kind = nkField) and (target^.fieldDecl <> nil) and
          (target^.fieldDecl^.kind = dkProperty) then
    assignedProperty := target^.fieldDecl;
end;

{ TFoo in TFoo.Create (objfpc): a class named before '.'; nil otherwise. }
function classNamedBy(base: PNode): PType;
var
  d: PDecl;
begin
  classNamedBy := nil;
  if (base^.kind <> nkNameRef) or (mode <> lmObjFPC) then
    exit;
  d := lookup(base^.ref.name);
  if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) and
     (d^.declType^.kind = tyClass) then
  begin
    base^.refDecl := d;
    base^.typ := d^.declType;
    classNamedBy := d^.declType;
  end;
end;

{ Resolves r.f / obj.member / TFoo.Create; nil after a diagnostic. }
function resolveMember(e: PNode): PDecl;
var
  viaClass, bt, owner: PType;
  d: PDecl;
begin
  resolveMember := nil;
  viaClass := classNamedBy(e^.base);
  if viaClass <> nil then
    bt := viaClass
  else
    bt := check(e^.base);
  if isError(bt) then
    exit;
  if bt^.kind = tyRecord then
  begin
    d := findField(bt, e^.fieldName.name);
    if d = nil then
    begin
      ms('no field named ''');
      maN(e^.fieldName.spelling);
      ma(''' in ');
      maT(bt);
      errR(e^.fieldName.loc, identRange(e^.fieldName));
      exit;
    end;
    e^.fieldDecl := d;
    exit(d);
  end;
  if bt^.kind <> tyClass then
  begin
    ms('''.'' applied to a value of type ');
    maT(bt);
    ma(', which is not a record');
    errR(e^.fieldName.loc, nodeRange(e^.base));
    exit;
  end;
  owner := nil;
  d := findMember(bt, e^.fieldName.name, owner);
  if d = nil then
  begin
    ms('no member named ''');
    maN(e^.fieldName.spelling);
    ma(''' in class ');
    maT(bt);
    maDidYouMeanMember(bt, e^.fieldName.name);
    errR(e^.fieldName.loc, identRange(e^.fieldName));
    exit;
  end;
  checkMemberAccess(d, owner, e^.fieldName);
  if (viaClass <> nil) and
     ((d^.kind <> dkRoutine) or (d^.methodKind <> mtConstructor)) then
  begin
    ms('only a constructor can be called through the class name ');
    maT(bt);
    ma('; ''');
    maN(e^.fieldName.spelling);
    ma(''' is a ' + kindName(d));
    errR(e^.fieldName.loc, identRange(e^.fieldName));
    exit;
  end;
  e^.fieldDecl := d;
  resolveMember := d;
end;

{ The value of calling method r through e (obj.M or TFoo.Create): a
  function's result, or the new object of a constructor called through its
  class; nil if the call has no value. }
function methodResultType(r: PDecl; e: PNode): PType;
begin
  methodResultType := nil;
  if r^.isFunction then
    methodResultType := r^.resultType
  else if (r^.methodKind = mtConstructor) and (e^.base^.kind = nkNameRef) and
          (e^.base^.refDecl <> nil) and (e^.base^.refDecl^.kind = dkType) then
    methodResultType := e^.base^.typ;
end;

{ Is callee obj.M / TFoo.Create / inherited M -- a method call rather than
  a call through a procedural value? }
function isMethodCallee(callee: PNode): boolean;
var
  bt, owner: PType;
  d: PDecl;
begin
  if callee^.kind = nkInherited then
    exit(true);
  if (callee^.kind <> nkField) or (mode <> lmObjFPC) then
    exit(false);
  if classNamedBy(callee^.base) <> nil then
    exit(true);
  bt := check(callee^.base);
  if bt^.kind <> tyClass then
    exit(false);
  owner := nil;
  d := findMember(bt, callee^.fieldName.name, owner);
  isMethodCallee := (d <> nil) and (d^.kind = dkRoutine);
end;

{ inherited [name]: the inherited method, or nil (after a diagnostic, or
  with nothing set for a bare inherited without an inherited method, which
  does nothing). }
function resolveInherited(e: PNode; var nothing: boolean): PDecl;
var
  encl, d: PDecl;
  parentClass, owner: PType;
  name: TIdent;
  l: TLoc;
begin
  resolveInherited := nil;
  nothing := false;
  encl := enclosingMethod;
  if encl = nil then
  begin
    ms('''inherited'' is only allowed in a method');
    errR(e^.b, nodeRange(e));
    exit;
  end;
  e^.inhEnclosing := encl;
  parentClass := encl^.ownerClass^.classParent;
  if identValid(e^.inhName) then
    name := e^.inhName
  else
    name := encl^.ident;
  d := nil;
  owner := nil;
  if parentClass <> nil then
    d := findMember(parentClass, name.name, owner);
  if (d = nil) or (d^.kind <> dkRoutine) then
  begin
    if not identValid(e^.inhName) and (d = nil) then
    begin
      nothing := true;
      exit;
    end;
    if identValid(e^.inhName) then
      l := name.loc
    else
      l := e^.b;
    if d <> nil then
    begin
      ms('''inherited'' needs a method, but ''');
      maN(name.spelling);
      ma(''' in ');
      maT(owner);
      ma(' is a ' + kindName(d));
      errR(l, nodeRange(e));
      noteDeclaredHere(d);
    end
    else
    begin
      ms('no inherited method ''');
      maN(name.spelling);
      ma(''' in ');
      if parentClass <> nil then
        maT(parentClass)
      else
        ma('''TObject''');
      errR(l, nodeRange(e));
    end;
    exit;
  end;
  checkMemberAccess(d, owner, name);
  if d^.isAbstract then
  begin
    ms('cannot call abstract method ''');
    maN(name.spelling);
    ma('''');
    errR(e^.b, nodeRange(e));
    noteDeclaredHere(d);
    exit;
  end;
  e^.inhMethod := d;
  resolveInherited := d;
end;

{ inherited [name] [(args)] called: the result type or nil. }
function checkInheritedCall(e: PNode; const args: TPtrList; callRange: TRange): PType;
var
  r, p: PDecl;
  nothing: boolean;
  name, pn: TIdent;
  i: longint;
  n: PNode;
  rg: TRange;
  implicit: TPtrList;
begin
  checkInheritedCall := nil;
  r := resolveInherited(e, nothing);
  if r = nil then
  begin
    for i := 0 to args.count - 1 do
      check(PNode(listGet(args, i)));
    if not nothing then
      checkInheritedCall := errorType;
    exit;
  end;
  if identValid(e^.inhName) then
    name := e^.inhName
  else
    name := r^.ident;
  if not identValid(e^.inhName) and (args.count = 0) then
  begin
    { the enclosing method's parameters, passed on unchanged }
    listInit(implicit);
    for i := 0 to e^.inhEnclosing^.params.count - 1 do
    begin
      p := PDecl(listGet(e^.inhEnclosing^.params, i));
      pn := p^.ident;
      pn.loc := e^.b;
      rg := identRange(pn);
      n := newNode(nkNameRef, rg.b, rg.e);
      n^.ref := pn;
      n^.refDecl := p;
      if p^.varType <> nil then
        n^.typ := p^.varType
      else
        n^.typ := errorType;
      p^.isUsed := true;
      listAdd(implicit, n);
    end;
    e^.inhArgs := implicit;
    name.loc := e^.b;
    checkCallArgs(r^.params, r, name, e^.inhArgs, callRange);
  end
  else
    checkCallArgs(r^.params, r, name, args, callRange);
  if r^.isFunction then
  begin
    e^.typ := r^.resultType;
    checkInheritedCall := r^.resultType;
  end;
end;

{ inherited name used as a value: a function without arguments. }
function visitInherited(e: PNode): PType;
var
  r: PDecl;
  nothing: boolean;
begin
  visitInherited := errorType;
  if not identValid(e^.inhName) then
  begin
    ms('expected a method name after ''inherited'' in an expression');
    errR(e^.b, nodeRange(e));
    exit;
  end;
  r := resolveInherited(e, nothing);
  if r = nil then
    exit;
  if r^.params.count > 0 then
  begin
    ms('missing arguments in call to ''');
    maN(e^.inhName.spelling);
    ma('''');
    errR(e^.inhName.loc, identRange(e^.inhName));
    noteDeclaredHere(r);
    exit;
  end;
  if not r^.isFunction then
  begin
    ms(kindName(r) + ' ''');
    maN(e^.inhName.spelling);
    ma(''' does not return a value');
    errR(e^.inhName.loc, identRange(e^.inhName));
    exit;
  end;
  visitInherited := r^.resultType;
end;

{ obj.M(args), TFoo.Create(args), inherited M(args) (an nkIndirectCall):
  the result type, nil for a call without a value, or the error type. }
function checkMethodCall(e: PNode): PType;
var
  f, d: PDecl;
  callee: PNode;
  t: PType;
  i: longint;
begin
  callee := e^.calleeExpr;
  if callee^.kind = nkInherited then
    exit(checkInheritedCall(callee, e^.args, nodeRange(e)));
  d := resolveMember(callee);
  if (d = nil) or (d^.kind <> dkRoutine) then
  begin
    for i := 0 to e^.args.count - 1 do
      check(PNode(listGet(e^.args, i)));
    exit(errorType);
  end;
  f := d;
  checkCallArgs(f^.params, f, callee^.fieldName, e^.args, nodeRange(e));
  t := methodResultType(f, callee);
  if t <> nil then
    callee^.typ := t;
  checkMethodCall := t;
end;

{ obj is TFoo / obj as TFoo (objfpc) }
function checkIsAs(e: PNode): PType;
var
  l, t, c: PType;
  rhs: PNode;
  d, td: PDecl;
  op: string;
begin
  checkIsAs := errorType;
  l := check(e^.lhs);
  op := binaryOpSpelling(e^.bop);
  c := nil;
  rhs := e^.rhs;
  td := nil;
  if rhs^.kind = nkNameRef then
  begin
    d := lookup(rhs^.ref.name);
    if (d <> nil) and (d^.kind = dkType) then
      td := d;
  end;
  if td <> nil then
  begin
    t := td^.declType;
    if t = nil then
      t := errorType;
    rhs^.refDecl := td;
    rhs^.typ := t;
    if isError(t) then
      exit;
    if t^.kind = tyClass then
      c := t;
  end
  else if isError(check(rhs)) then
    exit;
  if c = nil then
  begin
    ms('the right operand of ''' + op + ''' must be the name of a class type');
    errR(rhs^.b, nodeRange(rhs));
    exit;
  end;
  if isError(l) then
    exit;
  if l^.kind <> tyClass then
  begin
    ms('invalid operands to ''' + op + ''' (');
    maT(l);
    ma(' and ');
    maT(c);
    ma(')');
    errRR(e^.opLoc, nodeRange(e^.lhs), nodeRange(rhs));
    exit;
  end;
  if not inheritsFrom(c, l) and not inheritsFrom(l, c) then
  begin
    ms('invalid operands to ''' + op + ''': ');
    maT(l);
    ma(' and ');
    maT(c);
    ma(' are unrelated classes');
    errRR(e^.opLoc, nodeRange(e^.lhs), nodeRange(rhs));
    exit;
  end;
  if e^.bop = boIs then
    checkIsAs := tyBooleanT
  else
    checkIsAs := c;
end;

procedure visitSequence(seq: pointer; const body: TPtrList);
var
  i: longint;
begin
  listAdd(containers, seq);
  for i := 0 to body.count - 1 do
  begin
    if diagShouldStop then
      break;
    directSequence := seq;
    visitStmt(PNode(listGet(body, i)));
  end;
  directSequence := nil;
  listPop(containers);
end;

procedure visitLabeled(s: PNode);
var
  seq: pointer;
  l: PDecl;
begin
  seq := directSequence;
  directSequence := nil;
  l := lookup(labelKey(s^.lbl.name));
  if (l <> nil) and (l^.kind <> dkLabel) then
    l := nil;
  if (l = nil) or (l^.labelOwner <> curRoutine) then
  begin
    ms('label ');
    maN(s^.lbl.name);
    ma(' is not declared in this block');
    errR(s^.lbl.loc, identRange(s^.lbl));
  end
  else if l^.labelTarget <> nil then
  begin
    ms('label ');
    maN(s^.lbl.name);
    ma(' labels more than one statement');
    errR(s^.lbl.loc, identRange(s^.lbl));
    ms('previous use is here');
    note(l^.labelTarget^.b);
  end
  else
  begin
    s^.lblDecl := l;
    l^.labelTarget := s;
    l^.labelSequence := seq;
    l^.labelTopLevel := (seq <> nil) and (seq = pointer(routineBody));
  end;
  listAdd(containers, s);
  visitStmt(s^.lblBody);
  listPop(containers);
end;

procedure visitGoto(s: PNode);
var
  l: PDecl;
  g: PPendingGoto;
  i: longint;
begin
  directSequence := nil;
  l := lookup(labelKey(s^.lbl.name));
  if (l <> nil) and (l^.kind <> dkLabel) then
    l := nil;
  if l = nil then
  begin
    ms('use of undeclared label ');
    maN(s^.lbl.name);
    errR(s^.lbl.loc, identRange(s^.lbl));
    exit;
  end;
  s^.lblDecl := l;
  g := PPendingGoto(arenaAlloc(sizeof(TPendingGoto)));
  g^.stmt := s;
  listInit(g^.containers);
  for i := 0 to containers.count - 1 do
    listAdd(g^.containers, listGet(containers, i));
  g^.from := curRoutine;
  listAdd(pendingGotos, g);
end;

procedure visitProcCall(s: PNode);
var
  d: PDecl;
  t, rt: PType;
  a0: PNode;
  i: longint;
  w: PWithContext;
begin
  w := nil;
  d := scopeLookup(cur, s^.callee.name, w);
  if d = nil then
  begin
    diagUndeclared(s^.callee);
    exit;
  end;
  s^.callDecl := d;
  if classOfWith(w) <> nil then
  begin
    s^.callWith := w;
    checkScopeAccess(d, w, s^.callee);
  end;

  if (d^.kind = dkBuiltin) and d^.isProcedure then
  begin
    case d^.builtin of
      bkWrite: checkWrite(s, false);
      bkWriteln: checkWrite(s, true);
      bkRead: checkRead(s, false);
      bkReadln: checkRead(s, true);
      bkNew: checkNewDispose(s, true);
      bkDispose: checkNewDispose(s, false);
      bkReset, bkRewrite, bkGet, bkPut, bkPage:
        checkFileProc(s, d^.builtin = bkPage);
      bkBreak, bkContinue:
        if s^.args.count > 0 then
        begin
          ms('');
          maCallee(s^.callee);
          ma(' takes no arguments');
          errR(s^.callee.loc, nodeRange(s));
        end
        else if loopDepth = 0 then
        begin
          ms('');
          maCallee(s^.callee);
          ma(' is only allowed inside a loop');
          errR(s^.callee.loc, identRange(s^.callee));
        end;
      bkExit:
        begin
          if s^.args.count = 0 then
            exit;
          { exit(value) (Turbo/FPC): sets the function result, then exits }
          if (s^.args.count > 1) or (curRoutine = nil) or not curRoutine^.isFunction then
          begin
            if s^.args.count > 1 then
              ms('''exit'' takes at most one argument')
            else
              ms('''exit'' with a value is only allowed in a function');
            errR(s^.callee.loc, nodeRange(s));
            for i := 0 to s^.args.count - 1 do
              check(PNode(listGet(s^.args, i)));
            exit;
          end;
          a0 := PNode(listGet(s^.args, 0));
          t := check(a0);
          rt := curRoutine^.resultType;
          curRoutine^.resultAssigned := true;
          if not isError(t) and not isError(rt) and not isAssignable(rt, t) then
          begin
            ms('cannot return a value of type ');
            maT(t);
            ma(' from a function of type ');
            maT(rt);
            errR(a0^.b, nodeRange(a0));
          end;
        end;
      bkInc, bkDec: checkIncDec(s);
      bkDelete, bkInsert: checkDeleteInsert(s, d^.builtin = bkInsert);
      bkPack, bkUnpack: checkPackUnpack(s, d^.builtin = bkPack);
      bkGetMem, bkFreeMem, bkReallocMem, bkFillChar, bkMove, bkHalt:
        checkMemoryProc(s, d^.builtin);
    else
      ;
    end;
    exit;
  end;
  if (d^.kind = dkRoutine) and (not d^.isFunction or not isISO) then
  begin
    { Turbo/FPC: a function result may be discarded }
    checkCallArgs(d^.params, d, s^.callee, s^.args, nodeRange(s));
    exit;
  end;
  if (d^.kind = dkVar) and (d^.varType <> nil) and (d^.varType^.kind = tyRoutine) and
     (d^.varType^.routineResult = nil) then
  begin
    checkCallArgs(d^.varType^.routineParams, d, s^.callee, s^.args, nodeRange(s));
    exit;
  end;
  if (d^.kind = dkBuiltin) or (d^.kind = dkRoutine) then
  begin
    ms('function ');
    maCallee(s^.callee);
    ma(' cannot be called as a statement; use its result');
  end
  else
  begin
    ms('');
    maCallee(s^.callee);
    ma(' is a ' + kindName(d) + ', not a procedure');
  end;
  errR(s^.callee.loc, identRange(s^.callee));
  noteDeclaredHere(d);
end;

{ An assignment target that is a plain name: a variable, a field (inside
  with), or the result of an enclosing function. }
function checkNameTarget(target: PNode): PType;
var
  w: PWithContext;
  d, enc: PDecl;
begin
  checkNameTarget := errorType;
  w := nil;
  d := scopeLookup(cur, target^.ref.name, w);
  if d = nil then
  begin
    diagUndeclared(target^.ref);
    exit;
  end;
  checkScopeAccess(d, w, target^.ref);
  if (d^.kind = dkRoutine) and d^.isFunction then
  begin
    enc := curRoutine;
    while enc <> nil do
    begin
      if enc = d then
      begin
        target^.refDecl := d^.resultVar;
        d^.resultAssigned := true;
        exit(d^.resultType);
      end;
      enc := enc^.parent;
    end;
    ms('cannot assign to function ''');
    maN(target^.ref.spelling);
    ma(''' outside its body');
    errR(target^.ref.loc, identRange(target^.ref));
    exit;
  end;
  target^.refDecl := d;
  target^.refWith := w;
  d^.isUsed := true;
  if d^.kind = dkVar then
  begin
    checkNotForControlVar(target);
    if d^.varKind = vkResult then { objfpc Result }
      d^.owner^.resultAssigned := true;
    if d^.varType <> nil then
      checkNameTarget := d^.varType;
    exit;
  end;
  if d^.kind = dkField then
  begin
    if d^.fieldType <> nil then
      checkNameTarget := d^.fieldType;
    exit;
  end;
  if d^.kind = dkProperty then
    exit(checkPropertyWrite(d, target^.ref));
  ms('cannot assign to ' + kindName(d) + ' ''');
  maN(target^.ref.spelling);
  ma('''');
  errR(target^.ref.loc, identRange(target^.ref));
  noteDeclaredHere(d);
end;

{ A procedural variable named as the value of a procedural target is
  copied, not called. }
function checkValueFor(targetTy: PType; value: PNode): PType;
var
  v: PDecl;
begin
  if (targetTy^.kind = tyRoutine) and (value^.kind = nkNameRef) then
  begin
    v := lookup(value^.ref.name);
    if (v <> nil) and (v^.kind = dkVar) and (v^.varType <> nil) and
       (v^.varType^.kind = tyRoutine) then
    begin
      value^.refDecl := v;
      value^.typ := v^.varType;
      v^.isUsed := true;
      exit(v^.varType);
    end;
  end;
  checkValueFor := check(value);
end;

{ The procedural type of a call through a designator, or nil after a
  diagnostic. }
function checkIndirectCallee(e: PNode): PType;
var
  t: PType;
  name: TIdent;
  i: longint;
begin
  checkIndirectCallee := nil;
  t := check(e^.calleeExpr);
  if isError(t) then
    exit;
  if t^.kind <> tyRoutine then
  begin
    ms('called value of type ');
    maT(t);
    ma(' is not a procedure or function');
    errR(e^.calleeExpr^.b, nodeRange(e^.calleeExpr));
    for i := 0 to e^.args.count - 1 do
      check(PNode(listGet(e^.args, i)));
    exit;
  end;
  name.name := nil;
  name.spelling := intern('procedural value');
  name.loc := e^.b;
  checkCallArgs(t^.routineParams, nil, name, e^.args, nodeRange(e));
  checkIndirectCallee := t;
end;

procedure visitAssign(s: PNode);
var
  target, savedTarget: PNode;
  targetTy, valueTy: PType;
  p: PDecl;
begin
  target := s^.target;
  if target^.kind = nkNameRef then
  begin
    targetTy := checkNameTarget(target);
    target^.typ := targetTy;
  end
  else
  begin
    savedTarget := curAssignTarget;
    curAssignTarget := target;
    targetTy := check(target);
    curAssignTarget := savedTarget;
    p := assignedProperty(target);
    if (p <> nil) and not isError(targetTy) then
    begin
      targetTy := checkPropertyWrite(p, target^.fieldName);
      target^.typ := targetTy;
    end
    else if not isError(targetTy) and not isVariableReference(target) then
    begin
      ms('expression is not assignable');
      errR(target^.b, nodeRange(target));
      targetTy := errorType;
    end;
  end;
  valueTy := checkValueFor(targetTy, s^.value);
  if isError(targetTy) or isError(valueTy) then
    exit;
  if not checkModifiable(target) then
    exit;
  if containsFile(targetTy) then
  begin
    ms('files cannot be assigned');
    errR(s^.assignLoc, nodeRange(target));
    exit;
  end;
  if not isAssignable(targetTy, valueTy) then
  begin
    ms('assigning to ');
    maT(targetTy);
    ma(' from incompatible type ');
    maT(valueTy);
    errRR(s^.assignLoc, nodeRange(target), nodeRange(s^.value));
  end;
end;

procedure visitFor(s: PNode);
var
  varRef, bound: PNode;
  varTy, t: PType;
  usable: boolean;
  d: PDecl;
  k: longint;
begin
  directSequence := nil;
  varRef := s^.forVar;
  varTy := errorType;
  usable := false;
  d := lookup(varRef^.ref.name);
  if d <> nil then
  begin
    varRef^.refDecl := d;
    d^.isUsed := true;
    if d^.kind <> dkVar then
    begin
      ms('''for'' loop control variable must be a variable, but ''');
      maN(varRef^.ref.spelling);
      ma(''' is a ' + kindName(d));
      errR(varRef^.ref.loc, identRange(varRef^.ref));
      noteDeclaredHere(d);
    end
    else if (d^.varType <> nil) and not isError(d^.varType) then
    begin
      varTy := d^.varType;
      if not isOrdinal(varTy) then
      begin
        ms('''for'' loop control variable must be of an ordinal type, not ');
        maT(varTy);
        errR(varRef^.ref.loc, identRange(varRef^.ref));
      end
      else if isISO and ((d^.owner <> curRoutine) or (d^.varKind = vkVarParam)) then
      begin
        { ISO 7185 6.8.3.9: declared in the closest enclosing block }
        ms('''for'' loop control variable ''');
        maN(varRef^.ref.spelling);
        ma(''' must be a local variable of the enclosing block');
        errR(varRef^.ref.loc, identRange(varRef^.ref));
        noteDeclaredHere(d);
      end
      else if not checkNotForControlVar(varRef) then
        usable := true;
    end;
  end
  else
    diagUndeclared(varRef^.ref);
  varRef^.typ := varTy;

  for k := 1 to 2 do
  begin
    if k = 1 then
      bound := s^.forInit
    else
      bound := s^.forFinal;
    t := check(bound);
    if usable and not isError(t) and not areCompatible(t, varTy) then
    begin
      if k = 1 then
        ms('''for'' loop initial value of type ')
      else
        ms('''for'' loop final value of type ');
      maT(t);
      ma(' does not match control variable of type ');
      maT(varTy);
      errR(bound^.b, nodeRange(bound));
    end;
  end;
  if usable then
    listAdd(activeFors, s);
  loopDepth := loopDepth + 1;
  visitStmt(s^.forBody);
  loopDepth := loopDepth - 1;
  if usable then
    listPop(activeFors);
end;

procedure visitCase(s: PNode);
var
  selTy, labelTy: PType;
  seen: TPtrList;
  i, j, k: longint;
  arm: PCaseArm;
  l, prev: PCaseLabel;
  dup: int64;
begin
  directSequence := nil;
  selTy := check(s^.selector);
  if not isError(selTy) and not isOrdinal(selTy) then
  begin
    ms('''case'' selector must be of an ordinal type, not ');
    maT(selTy);
    errR(s^.selector^.b, nodeRange(s^.selector));
    selTy := errorType;
  end;
  { labels are checked against the selector's host type }
  if isError(selTy) then
    labelTy := selTy
  else
    labelTy := getBase(selTy);
  { ISO 7185 6.8.3.5: no value in more than one label }
  listInit(seen);
  for i := 0 to s^.arms.count - 1 do
  begin
    arm := PCaseArm(listGet(s^.arms, i));
    for j := 0 to arm^.labels.count - 1 do
    begin
      l := PCaseLabel(listGet(arm^.labels, j));
      if not checkLabel(l, labelTy, 'case label') or isError(selTy) then
        continue;
      for k := 0 to seen.count - 1 do
      begin
        prev := PCaseLabel(listGet(seen, k));
        if (l^.loValue > prev^.hiValue) or (l^.hiValue < prev^.loValue) then
          continue;
        if l^.loValue > prev^.loValue then
          dup := l^.loValue
        else
          dup := prev^.loValue;
        ms('duplicate case label value ');
        maV(selTy, dup);
        errR(l^.lo^.b, caseLabelRange(l));
        ms('previous case label is here');
        note(prev^.lo^.b);
        break;
      end;
      listAdd(seen, l);
    end;
    visitStmt(arm^.body);
  end;
  listFree(seen);
  visitSequence(@s^.elseBody, s^.elseBody);
end;

procedure visitWith(s: PNode);
var
  saved: PScope;
  i: longint;
  w: PWithContext;
  t: PType;
begin
  directSequence := nil;
  { each record opens a scope; they nest left to right }
  saved := cur;
  for i := 0 to s^.withRecords.count - 1 do
  begin
    w := PWithContext(listGet(s^.withRecords, i));
    t := check(w^.rec);
    if isError(t) then
      continue;
    if t^.kind = tyClass then { objfpc: an object }
    begin
      cur := newClassScope(cur, t, w);
      continue;
    end;
    if t^.kind <> tyRecord then
    begin
      ms('''with'' requires a record, not a value of type ');
      maT(t);
      errR(w^.rec^.b, nodeRange(w^.rec));
      continue;
    end;
    if not isVariableReference(w^.rec) then
    begin
      ms('''with'' requires a record variable');
      errR(w^.rec^.b, nodeRange(w^.rec));
      continue;
    end;
    cur := newWithScope(cur, t, w);
  end;
  visitStmt(s^.withBody);
  cur := saved;
end;

procedure visitStmt(s: PNode);
var
  rt: PType;
begin
  case s^.kind of
    nkEmpty: ;
    nkCompound: visitSequence(s, s^.stmts);
    nkLabeled: visitLabeled(s);
    nkGoto: visitGoto(s);
    nkProcCall: visitProcCall(s);
    nkIndirectCallStmt:
      begin
        directSequence := nil;
        if isMethodCallee(s^.calleeExpr^.calleeExpr) then
        begin
          rt := checkMethodCall(s^.calleeExpr);
          if rt <> nil then
            s^.calleeExpr^.typ := rt;
          exit;
        end;
        rt := checkIndirectCallee(s^.calleeExpr);
        if rt = nil then
          s^.calleeExpr^.typ := errorType
        else if rt^.routineResult <> nil then
          s^.calleeExpr^.typ := rt^.routineResult
        else
          s^.calleeExpr^.typ := rt;
      end;
    nkAssign: visitAssign(s);
    nkIf:
      begin
        directSequence := nil;
        checkCondition(s^.cond, 'if');
        visitStmt(s^.thenStmt);
        if s^.elseStmt <> nil then
          visitStmt(s^.elseStmt);
      end;
    nkWhile:
      begin
        directSequence := nil;
        checkCondition(s^.cond, 'while');
        loopDepth := loopDepth + 1;
        visitStmt(s^.thenStmt);
        loopDepth := loopDepth - 1;
      end;
    nkRepeat:
      begin
        loopDepth := loopDepth + 1;
        visitSequence(s, s^.stmts);
        loopDepth := loopDepth - 1;
        checkCondition(s^.untilCond, 'until');
      end;
    nkFor: visitFor(s);
    nkCase: visitCase(s);
    nkWith: visitWith(s);
  else
    ;
  end;
end;

{ --- Expressions (each returns the expression's type) --- }

function visitNameRef(e: PNode): PType;
var
  w: PWithContext;
  d: PDecl;
  t: PType;
begin
  visitNameRef := errorType;
  w := nil;
  d := scopeLookup(cur, e^.ref.name, w);
  if d = nil then
  begin
    diagUndeclared(e^.ref);
    exit;
  end;
  e^.refDecl := d;
  e^.refWith := w;
  d^.isUsed := true;
  checkScopeAccess(d, w, e^.ref);
  case d^.kind of
    dkProperty:
      visitNameRef := checkPropertyRead(d, e, e^.ref);
    dkVar:
      begin
        t := d^.varType;
        if (t <> nil) and (t^.kind = tyRoutine) then
        begin
          { a functional parameter without arguments is a call }
          if (t^.routineResult <> nil) and (t^.routineParams.count = 0) then
            exit(t^.routineResult);
          if t^.routineResult <> nil then
          begin
            ms('missing arguments in call to ''');
            maN(e^.ref.spelling);
            ma('''');
          end
          else
          begin
            ms('procedure ''');
            maN(e^.ref.spelling);
            ma(''' does not return a value');
          end;
          errR(e^.ref.loc, identRange(e^.ref));
          exit;
        end;
        if t <> nil then
          visitNameRef := t;
      end;
    dkField:
      if d^.fieldType <> nil then
        visitNameRef := d^.fieldType;
    dkConst:
      if d^.constType <> nil then
        visitNameRef := d^.constType;
    dkRoutine:
      begin
        if not d^.isFunction then
        begin
          ms('procedure ''');
          maN(e^.ref.spelling);
          ma(''' does not return a value');
          errR(e^.ref.loc, identRange(e^.ref));
          exit;
        end;
        if d^.params.count > 0 then
        begin
          ms('missing arguments in call to ''');
          maN(e^.ref.spelling);
          ma('''');
          errR(e^.ref.loc, identRange(e^.ref));
          noteDeclaredHere(d);
          exit;
        end;
        visitNameRef := d^.resultType; { a call without arguments }
      end;
    dkBuiltin:
      begin
        { eof / eoln without arguments test input }
        if (d^.builtin = bkEof) or (d^.builtin = bkEoln) then
          exit(tyBooleanT);
        if d^.builtin = bkParamCount then
          exit(tyIntegerT);
        if not d^.isProcedure then
        begin
          ms('missing argument in call to ''');
          maN(e^.ref.spelling);
          ma('''');
        end
        else
        begin
          ms('procedure ''');
          maN(e^.ref.spelling);
          ma(''' does not return a value');
        end;
        errR(e^.ref.loc, identRange(e^.ref));
      end;
  else
    begin
      ms('''');
      maN(e^.ref.spelling);
      ma(''' is a ' + kindName(d) + ', not a value');
      errR(e^.ref.loc, identRange(e^.ref));
      noteDeclaredHere(d);
    end;
  end;
end;

function visitUnary(e: PNode): PType;
var
  t, p: PType;
begin
  visitUnary := errorType;
  t := check(e^.operand);
  if isError(t) then
    exit;
  if e^.uop = uoNot then
  begin
    if getBase(t)^.kind = tyBoolean then
      exit(tyBooleanT);
    if not isISO and isIntegerLike(t) then { bitwise not, same type }
      exit(getBase(t));
  end
  else if isNumeric(t) then
  begin
    if t^.kind = tyReal then
      exit(tyRealT);
    p := promoteInteger(t);
    { -cardinal can be negative (FPC: int64) }
    if (e^.uop = uoMinus) and (p = tyCardinalT) then
      exit(tyInt64T);
    exit(p);
  end;
  ms('invalid operand to ''' + unaryOpSpelling(e^.uop) + ''' (');
  maT(t);
  ma(')');
  errR(e^.b, nodeRange(e^.operand));
end;

function visitBinary(e: PNode): PType;
var
  l, r: PType;
  op: TBinaryOp;
  bothIntegers: boolean;

  function integerOp: PType;
  begin
    e^.operandType := integerOperandType(e^.lhs, l, e^.rhs, r);
    integerOp := e^.operandType;
  end;

begin
  visitBinary := errorType;
  if e^.bop in [boIs, boAs] then
    exit(checkIsAs(e));
  l := check(e^.lhs);
  r := check(e^.rhs);
  if isError(l) or isError(r) then
    exit;
  op := e^.bop;
  { Turbo/FPC strings: + concatenates, comparisons work for any lengths }
  if not isISO and isStringish(l) and isStringish(r) then
  begin
    if (op = boAdd) and not (isNumeric(l) or isNumeric(r)) then
      exit(shortStringType(255));
    if isRelational(op) and (op <> boIn) and needsStringCompare(l, r) then
      exit(tyBooleanT);
  end;
  if (l^.kind = tySet) and (r^.kind = tySet) and (op <> boIn) then
  begin
    if not areCompatible(l, r) then
    begin
      ms('incompatible set types ');
      maT(l);
      ma(' and ');
      maT(r);
      errRR(e^.opLoc, nodeRange(e^.lhs), nodeRange(e^.rhs));
      exit;
    end;
    case op of
      boAdd, boSub, boMul:
        if isEmptySet(l) then
          exit(r)
        else
          exit(l);
      boEq, boNe, boLe, boGe:
        exit(tyBooleanT);
    else
      ;
    end;
  end;
  if (op in [boEq, boNe]) and ((l^.kind = tyPointer) or (l^.kind = tyNil)) and
     ((r^.kind = tyPointer) or (r^.kind = tyNil)) and
     (areCompatible(l, r) or ((l^.kind = tyNil) and (r^.kind = tyNil))) then
    exit(tyBooleanT);
  if (op in [boEq, boNe]) and ((l^.kind = tyClass) or (r^.kind = tyClass)) and
     areCompatible(l, r) then
    exit(tyBooleanT); { objects (objfpc) }

  bothIntegers := isIntegerLike(l) and isIntegerLike(r);
  case op of
    boAdd, boSub, boMul:
      begin
        if bothIntegers then
          exit(integerOp);
        if isNumeric(l) and isNumeric(r) then
          exit(tyRealT);
      end;
    boRealDiv:
      if isNumeric(l) and isNumeric(r) then
        exit(tyRealT);
    boIntDiv, boMod:
      if bothIntegers then
        exit(integerOp);
    boAnd, boOr, boXor:
      begin
        if (getBase(l)^.kind = tyBoolean) and (getBase(r)^.kind = tyBoolean) and
           ((op <> boXor) or not isISO) then
          exit(tyBooleanT);
        { Turbo/FPC: bitwise on integers }
        if not isISO and bothIntegers then
          exit(integerOp);
      end;
    boShl, boShr:
      if bothIntegers then { the left operand's (promoted) type }
      begin
        e^.operandType := promoteInteger(l);
        exit(e^.operandType);
      end;
    boEq, boNe, boLt, boLe, boGt, boGe:
      begin
        if bothIntegers then
        begin
          integerOp; { records the operand type }
          exit(tyBooleanT);
        end;
        if isNumeric(l) and isNumeric(r) then
          exit(tyBooleanT);
        if isOrdinal(l) and isOrdinal(r) and areCompatible(l, r) then
          exit(tyBooleanT);
        if isStringType(l) and isStringType(r) then
        begin
          if areCompatible(l, r) then
            exit(tyBooleanT);
          ms('cannot compare strings of different lengths (');
          maI(getStringLength(l));
          ma(' and ');
          maI(getStringLength(r));
          ma(')');
          errRR(e^.opLoc, nodeRange(e^.lhs), nodeRange(e^.rhs));
          exit;
        end;
      end;
    boIn:
      if isOrdinal(l) and (r^.kind = tySet) and
         (isEmptySet(r) or haveSameOrdinalHost(r^.setElement, l)) then
        exit(tyBooleanT);
  end;
  ms('invalid operands to ''' + binaryOpSpelling(op) + ''' (');
  maT(l);
  ma(' and ');
  maT(r);
  ma(')');
  errRR(e^.opLoc, nodeRange(e^.lhs), nodeRange(e^.rhs));
end;

function visitCall(e: PNode): PType;
var
  d: PDecl;
  i: longint;
  res: PType;
  w: PWithContext;
begin
  visitCall := errorType;
  w := nil;
  d := scopeLookup(cur, e^.callee.name, w);
  if d = nil then
  begin
    diagUndeclared(e^.callee);
    for i := 0 to e^.args.count - 1 do
      check(argNode(e, i));
    exit;
  end;
  e^.callDecl := d;
  if classOfWith(w) <> nil then
  begin
    e^.callWith := w;
    checkScopeAccess(d, w, e^.callee);
  end;
  if d^.kind = dkRoutine then
  begin
    if d^.isFunction then
    begin
      checkCallArgs(d^.params, d, e^.callee, e^.args, nodeRange(e));
      exit(d^.resultType);
    end;
    for i := 0 to e^.args.count - 1 do
      check(argNode(e, i));
    ms('procedure ');
    maCallee(e^.callee);
    ma(' does not return a value');
    errR(e^.callee.loc, identRange(e^.callee));
    exit;
  end;
  if (d^.kind = dkVar) and (d^.varType <> nil) and (d^.varType^.kind = tyRoutine) and
     (d^.varType^.routineResult <> nil) then
  begin
    checkCallArgs(d^.varType^.routineParams, d, e^.callee, e^.args, nodeRange(e));
    exit(d^.varType^.routineResult);
  end;
  if (d^.kind = dkType) and not isISO then
    exit(checkTypecast(e, d));
  if d^.kind = dkBuiltin then
    if checkSpecialBuiltin(e, d^.builtin, res) then
      exit(res);
  for i := 0 to e^.args.count - 1 do
    check(argNode(e, i));
  if d^.kind = dkBuiltin then
  begin
    if not d^.isProcedure then
      exit(checkBuiltinFunction(e, d^.builtin));
    ms('procedure ');
    maCallee(e^.callee);
    ma(' does not return a value');
    errR(e^.callee.loc, identRange(e^.callee));
    exit;
  end;
  if (d^.kind = dkVar) and (d^.varType <> nil) and (d^.varType^.kind = tyArray) then
  begin
    ms('');
    maCallee(e^.callee);
    ma(' is an array; use ''['' '']'' to index it');
  end
  else
  begin
    ms('');
    maCallee(e^.callee);
    ma(' is a ' + kindName(d) + ', not a function');
  end;
  errR(e^.callee.loc, identRange(e^.callee));
  noteDeclaredHere(d);
end;

function visitIndex(e: PNode): PType;
var
  bt, it, idx: PType;
  c: TConstant;
  bd: TOrdinalBounds;
begin
  visitIndex := errorType;
  bt := check(e^.base);
  it := check(e^.index);
  if isError(bt) or isError(it) then
    exit;
  if bt^.kind = tyShortString then
  begin
    { s[0] is the length byte, s[1..N] the characters }
    if not isIntegerLike(it) then
    begin
      ms('string index must be an integer');
      errR(e^.index^.b, nodeRange(e^.index));
      exit;
    end;
    if fold(e^.index, c) and ((c.value.intVal < 0) or (c.value.intVal > bt^.maxLength)) then
    begin
      ms('string index ');
      maI(c.value.intVal);
      ma(' is out of bounds 0..');
      maI(bt^.maxLength);
      errR(e^.index^.b, nodeRange(e^.index));
    end;
    exit(tyCharT);
  end;
  if (bt^.kind = tyStringLit) and not isISO then
  begin
    { Turbo/FPC: a string constant can be indexed, 1..N }
    if not isIntegerLike(it) then
    begin
      ms('string index must be an integer');
      errR(e^.index^.b, nodeRange(e^.index));
      exit;
    end;
    if fold(e^.index, c) and ((c.value.intVal < 1) or (c.value.intVal > bt^.strLength)) then
    begin
      ms('string index ');
      maI(c.value.intVal);
      ma(' is out of bounds 1..');
      maI(bt^.strLength);
      errR(e^.index^.b, nodeRange(e^.index));
    end;
    exit(tyCharT);
  end;
  if bt^.kind <> tyArray then
  begin
    ms('subscripted value is not an array (type ');
    maT(bt);
    ma(')');
    errR(e^.index^.b, nodeRange(e^.base));
    exit;
  end;
  idx := bt^.indexType;
  if not areCompatible(it, idx) then
  begin
    ms('array index of type ');
    maT(it);
    ma(' does not match index type ');
    maT(idx);
    errR(e^.index^.b, nodeRange(e^.index));
    exit;
  end;
  { constant indices are checked at compile time }
  if fold(e^.index, c) then
  begin
    bd := getOrdinalBounds(idx);
    if (c.value.intVal < bd.lo) or (c.value.intVal > bd.hi) then
    begin
      ms('array index ');
      maV(idx, c.value.intVal);
      ma(' is out of bounds ');
      maV(idx, bd.lo);
      ma('..');
      maV(idx, bd.hi);
      errR(e^.index^.b, nodeRange(e^.index));
    end;
  end;
  visitIndex := bt^.elementType;
end;

function visitAddrOf(e: PNode): PType;
var
  r: PDecl;
  t: PType;
  op: PNode;
begin
  visitAddrOf := errorType;
  op := e^.inner;
  { @routine (Turbo/FPC): a procedural value }
  if op^.kind = nkNameRef then
  begin
    r := lookup(op^.ref.name);
    if (r <> nil) and (r^.kind = dkRoutine) then
    begin
      op^.refDecl := r;
      if r^.methodKind <> mtNone then
      begin
        ms('''@');
        maN(op^.ref.spelling);
        ma(''': methods cannot be procedural values');
        errR(op^.b, nodeRange(op));
        exit;
      end;
      if (r^.parent <> nil) or r^.isExternal then
      begin
        ms('''@');
        maN(op^.ref.spelling);
        ma(''': only routines declared at program or unit level can be ' +
           'procedural values');
        errR(op^.b, nodeRange(op));
        exit;
      end;
      if r^.isFunction then
        t := newRoutineType(r^.params, r^.resultType)
      else
        t := newRoutineType(r^.params, nil);
      op^.typ := t;
      exit(t);
    end;
  end;
  t := check(op);
  if isError(t) then
    exit;
  if not isVariableReference(op) then
  begin
    ms('''@'' requires a variable');
    errR(op^.b, nodeRange(op));
    exit;
  end;
  visitAddrOf := tyUntypedPointerT;
end;

function visitDeref(e: PNode): PType;
var
  t: PType;
begin
  visitDeref := errorType;
  t := check(e^.inner);
  if isError(t) then
    exit;
  if t^.kind = tyPointer then
  begin
    if t^.untyped then
    begin
      ms('cannot dereference an untyped pointer');
      errR(e^.inner^.b, nodeRange(e^.inner));
      exit;
    end;
    if t^.pointee <> nil then
      visitDeref := t^.pointee;
    exit;
  end;
  if t^.kind = tyFile then
    exit(t^.fileElement); { the buffer variable f^ }
  ms('''^'' applied to a value of type ');
  maT(t);
  ma(', which is not a pointer');
  errR(e^.inner^.b, nodeRange(e^.inner));
end;

function visitSetCtor(e: PNode): PType;
var
  base, t: PType;
  ok: boolean;
  i, k: longint;
  el: PSetElement;
  x: PNode;
  c: TConstant;
begin
  base := nil;
  ok := true;
  for i := 0 to e^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(e^.setElems, i));
    for k := 1 to 2 do
    begin
      if k = 1 then
        x := el^.lo
      else
        x := el^.hi;
      if x = nil then
        continue;
      t := check(x);
      if isError(t) then
      begin
        ok := false;
        continue;
      end;
      if not isOrdinal(t) then
      begin
        ms('set elements must be of an ordinal type, not ');
        maT(t);
        errR(x^.b, nodeRange(x));
        ok := false;
        continue;
      end;
      if base = nil then
        base := getBase(t)
      else if not haveSameOrdinalHost(t, base) then
      begin
        ms('set element of type ');
        maT(t);
        ma(' does not match earlier elements of type ');
        maT(base);
        errR(x^.b, nodeRange(x));
        ok := false;
        continue;
      end;
      if fold(x, c) and ((c.value.intVal < 0) or (c.value.intVal > 255)) then
      begin
        ms('set element ');
        maV(t, c.value.intVal);
        ma(' is outside 0..255');
        errR(x^.b, nodeRange(x));
        ok := false;
      end;
    end;
  end;
  if ok then
    visitSetCtor := setConstructorType(base)
  else
    visitSetCtor := errorType;
end;

function visitField(e: PNode): PType;
var
  d: PDecl;
  t: PType;
begin
  visitField := errorType;
  d := resolveMember(e);
  if d = nil then
    exit;
  if d^.kind = dkField then
  begin
    if d^.fieldType <> nil then
      visitField := d^.fieldType;
    exit;
  end;
  if d^.kind = dkProperty then
    exit(checkPropertyRead(d, e, e^.fieldName));
  { a method used as a value: a call without arguments }
  if d^.params.count > 0 then
  begin
    ms('missing arguments in call to ''');
    maN(e^.fieldName.spelling);
    ma('''');
    errR(e^.fieldName.loc, identRange(e^.fieldName));
    noteDeclaredHere(d);
    exit;
  end;
  t := methodResultType(d, e);
  if t <> nil then
    exit(t);
  ms(kindName(d) + ' ''');
  maN(e^.fieldName.spelling);
  ma(''' does not return a value');
  errR(e^.fieldName.loc, identRange(e^.fieldName));
end;

function visitExpr(e: PNode): PType;
var
  t: PType;
begin
  case e^.kind of
    nkIntegerLit:
      begin
        if e^.intValue >= 0 then
        begin
          if e^.intValue <= pascalMaxInt then
            exit(tyIntegerT);
          { Turbo/FPC: larger literals are int64 constants }
          if not isISO then
            exit(tyInt64T);
        end;
        if isISO then
          ms('integer literal exceeds maxint (2147483647)')
        else
          ms('integer literal exceeds the range of int64');
        errR(e^.b, nodeRange(e));
        visitExpr := tyIntegerT;
      end;
    nkRealLit: visitExpr := tyRealT;
    nkStringLit:
      if e^.strValue^.len = 1 then
        visitExpr := tyCharT
      else
        visitExpr := stringLiteralType(e^.strValue^.len);
    nkNameRef: visitExpr := visitNameRef(e);
    nkParen: visitExpr := check(e^.inner);
    nkUnary: visitExpr := visitUnary(e);
    nkBinary: visitExpr := visitBinary(e);
    nkCall: visitExpr := visitCall(e);
    nkFormatted:
      begin
        check(e^.fmtValue);
        ms('field widths are only allowed in ''write'' and ''writeln''');
        errR(e^.fmtWidth^.b, nodeRange(e^.fmtWidth));
        visitExpr := errorType;
      end;
    nkIndex: visitExpr := visitIndex(e);
    nkField: visitExpr := visitField(e);
    nkNil: visitExpr := tyNilT;
    nkDeref: visitExpr := visitDeref(e);
    nkSetCtor: visitExpr := visitSetCtor(e);
    nkAddrOf: visitExpr := visitAddrOf(e);
    nkAggregate:
      begin
        ms('a parenthesized list is only allowed as the value of a typed constant');
        errR(e^.b, nodeRange(e));
        visitExpr := errorType;
      end;
    nkInherited: visitExpr := visitInherited(e);
    nkIndirectCall:
      begin
        if isMethodCallee(e^.calleeExpr) then
        begin
          t := checkMethodCall(e);
          if t <> nil then
            exit(t);
          ms('procedure does not return a value');
          errR(e^.b, nodeRange(e));
          exit(errorType);
        end;
        t := checkIndirectCallee(e);
        if t = nil then
          exit(errorType);
        if t^.routineResult = nil then
        begin
          ms('procedure does not return a value');
          errR(e^.b, nodeRange(e));
          exit(errorType);
        end;
        visitExpr := t^.routineResult;
      end;
  else
    visitExpr := errorType;
  end;
end;
