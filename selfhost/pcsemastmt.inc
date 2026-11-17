{ Included by pcsema.pas: built-in routines, statements and expressions
  (the second half of src/sema/Sema.cpp). }

{ --- Built-in procedure calls --- }

function isWritable(t: PType): boolean;
begin
  isWritable := isIntegerLike(t) or (t^.kind = tyReal) or
                isCharLike(t) or (getBase(t)^.kind = tyBoolean) or
                isStringType(t) or (t^.kind in [tyShortString, tyAnsiString]);
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

{ Is t an untyped file (Turbo/FPC file)? }
function isUntypedFile(t: PType): boolean;
begin
  isUntypedFile := (t <> nil) and (t^.kind = tyFile) and not t^.isText and
                   (t^.fileElement = nil);
end;

{ write/read of an untyped file: an error (they use blockwrite and
  blockread). True if reported. }
function untypedFileIO(s: PNode; fileTy: PType; isWrite: boolean): boolean;
begin
  untypedFileIO := false;
  if not isUntypedFile(fileTy) then
    exit;
  ms('');
  maCallee(s^.callee);
  if isWrite then
    ma(' cannot be used on an untyped file; use ''blockwrite''')
  else
    ma(' cannot be used on an untyped file; use ''blockread''');
  errR(s^.callee.loc, identRange(s^.callee));
  untypedFileIO := true;
end;

{ A value written to a text file, or put in a template string, with its
  field width and precision if it has them (an nkFormatted). }
procedure checkWritten(arg: PNode; template: boolean);
var
  t, wt: PType;
  k: longint;
  value, width, precision, w: PNode;
begin
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
  { Turbo/FPC: an enumeration's value is written as its name, a pchar as
    the C string it points to }
  if not isError(t) and not isWritable(t) and
     (isISO or not ((getBase(t)^.kind = tyEnum) or isPChar(t))) then
  begin
    if template then
    begin
      ms('a value of type ');
      maT(t);
      ma(' cannot be put in a template string');
    end
    else
    begin
      ms('cannot write a value of type ');
      maT(t);
    end;
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

procedure checkWrite(s: PNode; isWriteln: boolean);
var
  fileTy, t: PType;
  i, first: longint;
  arg: PNode;
begin
  fileTy := checkFileArg(s);
  if untypedFileIO(s, fileTy, true) then
    exit;
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
    checkWritten(PNode(listGet(s^.args, i)), false);
end;

procedure checkRead(s: PNode; isReadln: boolean);
var
  fileTy, t: PType;
  i, first: longint;
  arg: PNode;
begin
  fileTy := checkFileArg(s);
  if untypedFileIO(s, fileTy, false) then
    exit;
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
       (t^.kind <> tyReal) and not (t^.kind in [tyShortString, tyAnsiString]) then
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
{ An integer argument of built-in callee (what: its role). }
procedure checkIntegerArg(e: PNode; const callee: TIdent; const what: string);
var
  t: PType;
begin
  t := check(e);
  if not isError(t) and not isIntegerLike(t) then
  begin
    ms('the ' + what + ' of ');
    maCallee(callee);
    ma(' must be an integer, not ');
    maT(t);
    errR(e^.b, nodeRange(e));
  end;
end;

{ A file variable argument of callee: the file type, or nil after a
  diagnostic. notText: text files are not allowed. }
function checkFileVarArg(e: PNode; const callee: TIdent; notText, untyped: boolean): PType;
var
  t: PType;
begin
  checkFileVarArg := nil;
  t := check(e);
  if isError(t) then
    exit;
  if (t^.kind <> tyFile) or not isVariableReference(e) then
  begin
    ms('');
    maCallee(callee);
    if untyped then
      ma(' expects an untyped file variable, not a value of type ')
    else
      ma(' expects a file variable, not a value of type ');
    maT(t);
    errR(e^.b, nodeRange(e));
    exit;
  end;
  if (untyped and not isUntypedFile(t)) or (notText and t^.isText) then
  begin
    ms('');
    maCallee(callee);
    if untyped then
      ma(' expects an untyped file, not ')
    else
      ma(' expects a typed or untyped file, not ');
    maT(t);
    errR(e^.b, nodeRange(e));
    exit;
  end;
  checkFileVarArg := t;
end;

{ reset, rewrite (an untyped file's: with an optional record size), get,
  put, page, close, closefile, flush. }
procedure checkFileProc(s: PNode; k: TBuiltinKind);
var
  t: PType;
  a: PNode;
  isPage, sized: boolean;
begin
  isPage := k = bkPage;
  if isPage and (s^.args.count = 0) then
    exit; { page(output) }
  sized := (k = bkReset) or (k = bkRewrite);
  if (s^.args.count <> 1) and not (sized and (s^.args.count = 2)) then
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
  else if (isPage or (k = bkFlush)) and not isTextFile(t) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' requires a text file');
    errR(a^.b, nodeRange(a));
  end
  else if ((k = bkGet) or (k = bkPut)) and isUntypedFile(t) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' cannot be used on an untyped file');
    errR(a^.b, nodeRange(a));
  end
  else if (s^.args.count = 2) and not isUntypedFile(t) then
  begin
    a := PNode(listGet(s^.args, 1));
    ms('only an untyped file has a record size');
    errR(a^.b, nodeRange(a));
  end
  else if s^.args.count = 2 then
    checkIntegerArg(PNode(listGet(s^.args, 1)), s^.callee, 'record size');
end;

{ assign(f, name) / assignfile: f any file variable, name a string, a char
  or a pchar. }
procedure checkAssignFile(s: PNode);
var
  t: PType;
  a: PNode;
begin
  if s^.args.count <> 2 then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects a file variable and a name');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  t := checkFileVarArg(PNode(listGet(s^.args, 0)), s^.callee, false, false);
  a := PNode(listGet(s^.args, 1));
  t := check(a);
  if not isError(t) and not isStringish(t) and (t <> pcharT) then
  begin
    ms('the file name of ');
    maCallee(s^.callee);
    ma(' must be a string, not ');
    maT(t);
    errR(a^.b, nodeRange(a));
  end;
end;

{ blockread(f, buf, count [, done]) / blockwrite: f an untyped file, buf any
  variable, count an integer, done an integer variable. }
procedure checkBlockIO(s: PNode);
var
  t: PType;
  a: PNode;
  ok: boolean;
begin
  if (s^.args.count <> 3) and (s^.args.count <> 4) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects 3 or 4 arguments (file, buffer, count [, count done])');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  t := checkFileVarArg(PNode(listGet(s^.args, 0)), s^.callee, true, true);
  a := PNode(listGet(s^.args, 1));
  if not isError(checkAllowingUntyped(a)) and not isVariableReference(a) then
  begin
    ms('the buffer of ');
    maCallee(s^.callee);
    ma(' must be a variable');
    errR(a^.b, nodeRange(a));
  end
  else if isBitPackedComponent(a) then
    diagBitFieldAddress(a)
  else if nameEquals(s^.callee.name, 'blockread') then
    ok := checkModifiable(a);
  checkIntegerArg(PNode(listGet(s^.args, 2)), s^.callee, 'count');
  if s^.args.count = 4 then
  begin
    a := PNode(listGet(s^.args, 3));
    t := check(a);
    if isError(t) then
      exit;
    if not isVariableReference(a) then
    begin
      ms('the count done by ');
      maCallee(s^.callee);
      ma(' goes in a variable');
      errR(a^.b, nodeRange(a));
    end
    else if not isIntegerLike(t) then
    begin
      ms('the count done by ');
      maCallee(s^.callee);
      ma(' goes in an integer variable, not ');
      maT(t);
      errR(a^.b, nodeRange(a));
    end
    else
      ok := checkModifiable(a);
  end;
end;

{ seek(f, n): f a typed or untyped file, n an integer. }
procedure checkSeek(s: PNode);
var
  t: PType;
begin
  if s^.args.count <> 2 then
  begin
    ms('''seek'' expects a file variable and a record number');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  t := checkFileVarArg(PNode(listGet(s^.args, 0)), s^.callee, true, false);
  checkIntegerArg(PNode(listGet(s^.args, 1)), s^.callee, 'record number');
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
{ setlength(s, n): s a string variable, n an integer. }
{ setlength(a, n, ...): a a dynamic array variable, one length for each
  level of arrays of arrays that is set. }
procedure checkDynSetLength(s: PNode);
var
  a0, ai: PNode;
  t, n: PType;
  i: longint;
begin
  a0 := PNode(listGet(s^.args, 0));
  t := a0^.typ;
  if not isVariableReference(a0) then
  begin
    ms('expected a dynamic array variable');
    errR(a0^.b, nodeRange(a0));
  end
  else
    checkModifiable(a0);
  if s^.args.count < 2 then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects a length');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  for i := 1 to s^.args.count - 1 do
  begin
    ai := PNode(listGet(s^.args, i));
    n := check(ai);
    if not isError(n) and not isIntegerLike(n) then
    begin
      ms('expected an integer');
      errR(ai^.b, nodeRange(ai));
    end;
    if t^.kind <> tyDynArray then
    begin
      ms('more lengths than levels of dynamic arrays');
      errR(ai^.b, nodeRange(ai));
      exit;
    end;
    t := t^.dynElement;
  end;
end;

procedure checkSetLength(s: PNode);
var
  a0, a1: PNode;
  t, n: PType;
begin
  if (s^.args.count > 0) and (check(PNode(listGet(s^.args, 0)))^.kind = tyDynArray) then
  begin
    checkDynSetLength(s);
    exit;
  end;
  if s^.args.count <> 2 then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects 2 arguments');
    errR(s^.callee.loc, nodeRange(s));
    exit;
  end;
  a0 := PNode(listGet(s^.args, 0));
  a1 := PNode(listGet(s^.args, 1));
  t := check(a0);
  n := check(a1);
  if not isError(t) then
  begin
    if not (t^.kind in [tyShortString, tyAnsiString]) or not isVariableReference(a0) then
    begin
      ms('expected a string variable');
      errR(a0^.b, nodeRange(a0));
    end
    else
      checkModifiable(a0);
  end;
  if not isError(n) and not isIntegerLike(n) then
  begin
    ms('expected an integer');
    errR(a1^.b, nodeRange(a1));
  end;
end;

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
      if not (t^.kind in [tyShortString, tyAnsiString]) or not isVariableReference(a) then
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

{ inc(x [, n]) / dec(x [, n]): x an ordinal variable (Turbo/FPC: or a
  pointer, moved by n of what it points to), n an integer. }
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
  if not isError(t) and
     (not (isOrdinal(t) or (t^.kind = tyPointer)) or not isVariableReference(a0)) then
  begin
    ms('');
    maCallee(s^.callee);
    ma(' expects an ordinal or pointer variable');
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
  for i := 0 to want - 1 do { (variables: untyped ones too) }
  begin
    a[i] := PNode(listGet(s^.args, i));
    if ((k = bkFillChar) and (i = 0)) or ((k = bkMove) and (i < 2)) then
      t[i] := checkAllowingUntyped(a[i])
    else
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
        if isStringish(argType(e, 0)) or
           (argType(e, 0)^.kind in [tyArrayOfConst, tyOpenArray, tyDynArray]) then
          checkStringFunction := tyIntegerT
        else
          checkStringFunction := bad(argNode(e, 0), 'a string');
      end;
    bkUpCase:
      begin
        if not arity(1) then
          exit;
        if getBase(argType(e, 0))^.kind = tyWideChar then
          checkStringFunction := tyWideCharT { only 'a'..'z' change }
        else if getBase(argType(e, 0))^.kind = tyChar then
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
        if (e^.args.count > 0) and (argType(e, 0)^.kind = tyDynArray) then
        begin
          { copy(a), copy(a, start), copy(a, start, count) }
          if e^.args.count > 3 then
          begin
            ms('''copy'' expects at most 3 arguments');
            errR(e^.callee.loc, nodeRange(e));
            exit;
          end;
          for i := 1 to e^.args.count - 1 do
            if not isIntegerLike(argType(e, i)) then
              exit(bad(argNode(e, i), 'integer index and count'));
          exit(argType(e, 0));
        end;
        if not arity(3) then
          exit;
        if not isStringish(argType(e, 0)) then
          exit(bad(argNode(e, 0), 'a string'));
        if not isIntegerLike(argType(e, 1)) or not isIntegerLike(argType(e, 2)) then
          exit(bad(argNode(e, 1), 'integer index and count'));
        if argType(e, 0)^.kind = tyAnsiString then
          checkStringFunction := ansiStringType
        else
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
      for i := 0 to e^.args.count - 1 do
        if argType(e, i)^.kind = tyAnsiString then
          exit(ansiStringType);
      checkStringFunction := shortStringType(255);
    end;
  end;
end;

{ slice(a, n), for an open array parameter only. }
function checkSlice(e: PNode): PType;
var
  allowed: boolean;
  t0, t1, elem: PType;
  a0, a1: PNode;
begin
  checkSlice := errorType;
  allowed := allowSlice;
  allowSlice := false;
  if e^.args.count <> 2 then
  begin
    ms('');
    maCallee(e^.callee);
    ma(' expects 2 arguments');
    errR(e^.callee.loc, nodeRange(e));
    exit;
  end;
  a0 := argNode(e, 0);
  a1 := argNode(e, 1);
  t0 := argType(e, 0);
  t1 := argType(e, 1);
  if isError(t0) or isError(t1) then
    exit;
  if not allowed then
  begin
    ms('');
    maCallee(e^.callee);
    ma(' can only be passed to an open array parameter');
    errR(e^.callee.loc, nodeRange(e));
    exit;
  end;
  elem := nil;
  if t0^.kind = tyArray then
    elem := t0^.elementType
  else if t0^.kind = tyOpenArray then
    elem := t0^.openElement;
  if elem = nil then
  begin
    ms('');
    maCallee(e^.callee);
    ma(' expects an array, not ');
    maT(t0);
    errR(a0^.b, nodeRange(a0));
    exit;
  end;
  if not isIntegerLike(t1) then
  begin
    ms('expected an integer');
    errR(a1^.b, nodeRange(a1));
    exit;
  end;
  checkSlice := openArrayType(elem);
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
  if bk = bkSlice then
    exit(checkSlice(e));
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
  if (bk = bkFilePos) or (bk = bkFileSize) then
  begin
    if checkFileVarArg(arg, e^.callee, true, false) = nil then
      exit;
    exit(tyInt64T); { (in records) }
  end;
  if bk = bkGetFileHandle then
  begin
    { any file variable, text too; a THandle (longint, as in FPC on Unix) }
    if checkFileVarArg(arg, e^.callee, false, false) = nil then
      exit;
    exit(tyIntegerT);
  end;
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
function checkComparand(e: PNode): PType; forward;

function checkTypecast(e: PNode; td: PDecl): PType;
var
  t, at: PType;
  i: longint;
  op: PDecl;
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
  if t = tmethodT then
    at := checkComparand(argNode(e, 0))
  else
    at := checkAllowingUntyped(argNode(e, 0));
  if isError(t) or isError(at) then
    exit;
  if (t = tmethodT) and isMethodPointer(at) then { its code and object }
  begin
    if isVariableReference(argNode(e, 0)) then
      e^.methodCast := 2
    else
      e^.methodCast := 1;
    exit(t);
  end;
  if isMethodPointer(t) and (at = tmethodT) then
    exit(t);
  if at^.kind = tyUntyped then { Turbo/FPC: the untyped parameter seen as a t }
  begin
    e^.isVarCast := true;
    exit(t);
  end;
  if (t^.kind = tyAnsiString) and (isStringish(at) or (at^.kind = tyPointer)) then
    exit(t); { an untyped pointer is taken to be an ansistring }
  if (t^.kind = tyShortString) and isStringish(at) then
    exit(t);
  if (isOrdinal(t) and isOrdinal(at)) or
     ((t^.kind = tyPointer) and (at^.kind in [tyPointer, tyNil, tyClass, tyAnsiString, tyArrayOfConst,
                                              tyDynArray])) or
     ((t^.kind = tyDynArray) and (at^.kind in [tyDynArray, tyPointer, tyNil])) or
     ((t^.kind = tyClass) and (at^.kind in [tyClass, tyPointer, tyNil])) or
     ((t^.kind = tyClassRef) and (at^.kind in [tyClassRef, tyNil])) or
     ((t^.kind = tyInterface) and
      ((at^.kind in [tyInterface, tyNil]) or
       ((at^.kind = tyClass) and (findInterface(at, t) <> nil)))) then
    exit(t);
  op := findConversion(at, t, true);
  if op <> nil then
  begin
    setConversion(argNode(e, 0), op, t); { a record's Explicit or := operator }
    exit(t);
  end;
  ms('invalid typecast from ');
  maT(at);
  ma(' to ');
  maT(t);
  ma(' (only between ordinal types and between pointer types)');
  errR(e^.b, nodeRange(e));
end;

{ A type name or an expression as the argument of sizeof/low/high: its
  type (also stored on the argument). }
function typeNamedBy(base: PNode): PDecl; forward;

function checkTypeOrValue(arg: PNode): PType;
var
  d: PDecl;
  t: PType;
begin
  if arg^.kind = nkField then { TOuter.TInner }
  begin
    d := typeNamedBy(arg);
    if d <> nil then
    begin
      t := d^.declType;
      if t = nil then
        t := errorType;
      arg^.typ := t;
      exit(t);
    end;
  end;
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

{ --- Tasks (objfpc): async routines, await, spawn, scope, detached --- }

var
  { nesting of scope blocks around the code being checked (spawn needs one) }
  scopeDepth: longint;

{ Is the code being checked in an async routine (or a lambda in one)? }
function inAsyncRoutine: boolean;
var
  r: PDecl;
begin
  r := curRoutine;
  while (r <> nil) and r^.isLambda and (r^.asyncType = nil) do
    r := r^.parent;
  inAsyncRoutine := (r <> nil) and (r^.asyncType <> nil);
end;

{ Is e (checked) the call of an async routine? }
function isAsyncCall(e: PNode): boolean;
var
  d: PDecl;
begin
  d := nil;
  case e^.kind of
    nkCall: d := e^.callDecl;
    nkNameRef: d := e^.refDecl;
    nkField: d := e^.fieldDecl;
    nkIndirectCall:
      if e^.calleeExpr^.kind = nkField then
        d := e^.calleeExpr^.fieldDecl;
  else
    ;
  end;
  isAsyncCall := (d <> nil) and (d^.kind = dkRoutine) and (d^.asyncType <> nil);
end;

procedure warnDanglingTask(const callee: TIdent; rng: TRange);
begin
  ms('dangling task: ''');
  maN(callee.spelling);
  ma(''' is never awaited; write ''await ');
  maN(callee.spelling);
  ma(''' to wait for it, keep it in a variable to await later, or ''detached ');
  maN(callee.spelling);
  ma(''' to let it run on its own');
  diagReportBuf(sevWarning, callee.loc, msg, rng, noRange);
end;

procedure errNoTaskValue(a: PNode);
begin
  ms('this task has no value (its routine is ''async'' without a type)');
  errR(a^.b, nodeRange(a));
end;

{ await e: e a task; its result (needValue: one is needed). }
function checkAwait(e: PNode; needValue: boolean): PType;
var
  t: PType;
begin
  checkAwait := errorType;
  t := check(e^.awaitOperand);
  if isError(t) then
    exit;
  if t^.kind <> tyTask then
  begin
    ms('''await'' waits for a task (the call of an async routine), not a value of type ');
    maT(t);
    errR(e^.b, nodeRange(e));
    exit;
  end;
  if not inAsyncRoutine then
  begin
    ms('''await'' is allowed only in an async routine; elsewhere, Wait(t) waits for a task');
    errR(e^.b, nodeRange(e));
  end;
  if t^.taskResult = nil then
  begin
    if needValue then
    begin
      errNoTaskValue(e);
      exit;
    end;
    exit(t);
  end;
  checkAwait := t^.taskResult;
end;

{ Wait(t) (also as a statement: needValue false). }
function checkWait(const args: TPtrList; const callee: TIdent; rng: TRange;
                   needValue: boolean): PType;
var
  t: PType;
  a: PNode;
  i: longint;
begin
  checkWait := errorType;
  if args.count <> 1 then
  begin
    ms('');
    maCallee(callee);
    ma(' expects 1 argument');
    errR(callee.loc, rng);
    for i := 0 to args.count - 1 do
      check(PNode(listGet(args, i)));
    exit;
  end;
  a := PNode(listGet(args, 0));
  t := check(a);
  if isError(t) then
    exit;
  if t^.kind <> tyTask then
  begin
    ms('''Wait'' waits for a task (the call of an async routine), not a value of type ');
    maT(t);
    errR(a^.b, nodeRange(a));
    exit;
  end;
  if t^.taskResult = nil then
  begin
    if needValue then
    begin
      errNoTaskValue(a);
      exit;
    end;
    exit(t);
  end;
  checkWait := t^.taskResult;
end;

{ The tasks of WhenAll / WhenAny: an array of tasks (static, dynamic or
  open); their task type, or nil after a diagnostic. }
function checkTaskArray(a: PNode; const callee: TIdent): PType;
var
  t, elem: PType;
begin
  checkTaskArray := nil;
  t := check(a);
  if isError(t) then
    exit;
  elem := nil;
  case t^.kind of
    tyArray: elem := t^.elementType;
    tyDynArray: elem := t^.dynElement;
    tyOpenArray: elem := t^.openElement;
  else
    ;
  end;
  if (elem = nil) or (elem^.kind <> tyTask) then
  begin
    ms('');
    maCallee(callee);
    ma(' takes an array of tasks, not a value of type ');
    maT(t);
    errR(a^.b, nodeRange(a));
    exit;
  end;
  checkTaskArray := elem;
end;

{ The hidden async procedure of spawn begin ... end: declared and checked
  where it is spawned (it uses the variables declared so far). }
procedure completeSpawnBlock(r: PDecl);
var
  savedDepth: longint;
begin
  r^.lambdaDone := true;
  resolveSignature(r);
  savedDepth := scopeDepth;
  scopeDepth := 0; { (its task has scopes of its own) }
  declareAndCheckLambda(r);
  scopeDepth := savedDepth;
end;

function visitSpawn(e: PNode): PType;
var
  c: PNode;
  t: PType;
begin
  visitSpawn := errorType;
  c := e^.spawnCall;
  if (c^.kind = nkCall) and (c^.callDecl <> nil) and (c^.callDecl^.kind = dkRoutine) and
     c^.callDecl^.isSpawnBlock and not c^.callDecl^.lambdaDone then
    completeSpawnBlock(c^.callDecl);
  t := check(c);
  if isError(t) then
    exit;
  if (t^.kind <> tyTask) or not isAsyncCall(c) then
  begin
    if e^.spawnPool then
      ms('''Pool.Run''')
    else
      ms('''spawn''');
    ma(' runs the call of an async routine, not a value of type ');
    maT(t);
    errR(c^.b, nodeRange(c));
    exit;
  end;
  if not e^.spawnPool and (scopeDepth = 0) then
  begin
    ms('''spawn'' is allowed only inside a ''scope ... end'' block, which ' +
       'waits for the tasks it starts; Pool.Run starts one on its own');
    errR(e^.b, nodeRange(e));
  end;
  visitSpawn := t;
end;

{ Built-ins whose arguments are not ordinary values (type names,
  procedural variables) or that take none. false for the others. }
function checkSpecialBuiltin(e: PNode; bk: TBuiltinKind; var res: PType): boolean;
var
  t, lt, rt: PType;
  tys, noNames: TPtrList;
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
    bkWait:
      res := checkWait(e^.args, e^.callee, nodeRange(e), true);
    bkWhenAll, bkWhenAny:
      begin
        if not arity(1) then
          exit;
        t := checkTaskArray(argNode(e, 0), e^.callee);
        if t = nil then
          exit;
        if bk = bkWhenAny then
          res := taskType(tyIntegerT)
        else if t^.taskResult <> nil then
          res := taskType(dynArrayType(t^.taskResult))
        else
          res := taskType(nil);
      end;
    bkAddChecked, bkSubChecked, bkMulChecked: { (a op b, ok) in the type of a op b }
      begin
        if not arity(2) then
          exit;
        lt := check(argNode(e, 0));
        rt := check(argNode(e, 1));
        if isError(lt) or isError(rt) then
          exit;
        if not isIntegerLike(lt) or not isIntegerLike(rt) then
        begin
          ms('');
          maCallee(e^.callee);
          ma(' expects integer arguments, not ');
          maT(lt);
          ma(' and ');
          maT(rt);
          errR(e^.callee.loc, nodeRange(e));
          exit;
        end;
        t := integerOperandType(argNode(e, 0), lt, argNode(e, 1), rt);
        listInit(tys);
        listAdd(tys, t);
        listAdd(tys, tyBooleanT);
        listInit(noNames);
        res := getTupleType(tys, noNames);
        exit;
      end;
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
        if (t^.kind = tyDynArray) and (a0^.kind = nkNameRef) and (lookup(a0^.ref.name) <> nil) and
           (lookup(a0^.ref.name)^.kind = dkType) then
        begin
          ms('');
          maCallee(e^.callee);
          ma(' needs a dynamic array variable, not a type');
          errR(a0^.b, nodeRange(a0));
          exit;
        end;
        if t^.kind in [tyShortString, tyArrayOfConst, tyOpenArray, tyDynArray] then
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
           (t^.kind <> tyInterface) and (t^.kind <> tyRoutine) and
           (t^.kind <> tyDynArray) then
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
    bkExceptAddr: { FPC's CodePointer }
      if arity(0) then
        res := tyUntypedPointerT;
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

{ --- Array properties (objfpc) --- }

function findPending(e: PNode): PPendingIndex;
var
  i: longint;
  pi: PPendingIndex;
begin
  findPending := nil;
  for i := 0 to pendingIndexes.count - 1 do
  begin
    pi := PPendingIndex(listGet(pendingIndexes, i));
    if pi^.node = e then
      exit(pi);
  end;
end;

{ Records e (the base of an nkIndex being checked) as pending: an array
  property short of indexes (replacing an older record of e). }
procedure addPending(e: PNode; prop: PDecl; root: PNode; const args: TPtrList);
var
  pi: PPendingIndex;
begin
  pi := findPending(e);
  if pi = nil then
  begin
    pi := PPendingIndex(arenaAlloc(sizeof(TPendingIndex)));
    listAdd(pendingIndexes, pi);
  end;
  pi^.node := e;
  pi^.prop := prop;
  pi^.root := root;
  pi^.args := args;
end;

procedure removePending(pi: PPendingIndex);
var
  i: longint;
begin
  for i := 0 to pendingIndexes.count - 1 do
    if listGet(pendingIndexes, i) = pointer(pi) then
    begin
      listSet(pendingIndexes, i, listGet(pendingIndexes, pendingIndexes.count - 1));
      listPop(pendingIndexes);
      exit;
    end;
end;

{ The name an array property access e is made by (obj.P / P), or for a
  default property, its name at the indexed object. }
function propertyName(e: PNode): TIdent;
var
  root: PNode;
  id: TIdent;
begin
  root := e^.idxRoot;
  if (root^.kind = nkField) and (root^.fieldDecl = e^.idxProp) then
    exit(root^.fieldName);
  if (root^.kind = nkNameRef) and (root^.refDecl = e^.idxProp) then
    exit(root^.ref);
  id := e^.idxProp^.ident;
  id.loc := root^.b;
  propertyName := id;
end;

{ An array property used without its indexes (outside indexBase). }
function checkArrayPropertyUse(p: PDecl; e: PNode; const id: TIdent): boolean;
var
  noArgs: TPtrList;
begin
  checkArrayPropertyUse := true;
  if p^.propParams.count = 0 then
    exit;
  if (e <> nil) and (e = indexBase) then
  begin
    listInit(noArgs);
    addPending(e, p, e, noArgs);
    exit;
  end;
  ms('array property ''');
  maN(id.spelling);
  if p^.propParams.count = 1 then
    ma(''' needs an index')
  else
    ma(''' needs ' + intToStr(p^.propParams.count) + ' indexes');
  errR(id.loc, identRange(id));
  noteDeclaredHere(p);
  checkArrayPropertyUse := false;
end;

{ A property used as a value: it needs a read accessor. }
function checkPropertyRead(p: PDecl; e: PNode; const id: TIdent): PType;
begin
  if (p^.propParams.count > 0) and (e^.kind <> nkIndex) and (findPending(e) <> nil) then
    exit(p^.propType); { checked with its indexes }
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

{ The type helper d is a member of (a property: by its getter), or nil. }
function helperOfMember(d: PDecl): PType;
begin
  helperOfMember := nil;
  if (d <> nil) and (d^.kind = dkProperty) then
    d := d^.readDecl;
  if (d <> nil) and (d^.kind = dkRoutine) and
     (d^.ownerClass <> nil) and (d^.ownerClass^.classHelperFor <> nil) then
    helperOfMember := d^.ownerClass;
end;

{ Is d a member of a type helper (a property: by its getter)? }
function isHelperMember(d: PDecl): boolean;
begin
  isHelperMember := helperOfMember(d) <> nil;
end;

{ The type of a property assigned to; it needs a write accessor. }
function checkPropertyWrite(p: PDecl; const id: TIdent): PType;
var
  acc: PDecl;
begin
  acc := p^.writeDecl;
  if acc = nil then
    acc := p^.readDecl;
  if isHelperMember(acc) then
  begin
    ms('writing a property of a ' + helperKindName(helperOfMember(acc)) +
       ' is not supported yet');
    errR(id.loc, identRange(id));
    exit(errorType);
  end;
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
    assignedProperty := target^.fieldDecl
  else if target^.kind = nkIndex then
    assignedProperty := target^.idxProp;
end;

{ TFoo in TFoo.Create (objfpc): a class named before '.'; nil otherwise. }
function classNamedBy(base: PNode): PType;
var
  d: PDecl;
begin
  classNamedBy := nil;
  if mode <> lmObjFPC then
    exit;
  d := typeNamedBy(base);
  if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) and
     (d^.declType^.kind = tyClass) and (d^.declType^.classHelperFor <> nil) then
    exit; { (not a type: its methods are used on values) }
  if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) and
     (d^.declType^.kind = tyClass) then
  begin
    if base^.kind = nkNameRef then
      base^.refDecl := d;
    base^.typ := d^.declType;
    classNamedBy := d^.declType;
  end
  else if (d <> nil) and (d^.kind = dkType) and (memberTableOf(d^.declType) <> nil) then
  begin
    { an advanced record's name (TR.Create): its member table; the base's
      type is the record }
    if base^.kind = nkNameRef then
      base^.refDecl := d;
    base^.typ := d^.declType;
    classNamedBy := memberTableOf(d^.declType);
  end;
end;

{ The type that base names (TFoo, or a nested TOuter.TInner), or nil. }
function typeNamedBy(base: PNode): PDecl;
var
  d: PDecl;
  outer, owner: PType;
begin
  typeNamedBy := nil;
  if base^.kind = nkNameRef then
  begin
    d := lookup(base^.ref.name);
    if (d <> nil) and (d^.kind = dkType) then
      typeNamedBy := d;
    exit;
  end;
  if (base^.kind <> nkField) or (mode <> lmObjFPC) then
    exit;
  outer := classNamedBy(base^.base);
  if outer = nil then
    exit;
  owner := nil;
  d := findMember(outer, base^.fieldName.name, owner);
  if (d = nil) or (d^.kind <> dkType) then
    exit;
  if base^.fieldDecl <> d then
  begin
    checkMemberAccess(d, owner, base^.fieldName);
    base^.fieldDecl := d;
    if d^.declType <> nil then
      base^.typ := d^.declType
    else
      base^.typ := errorType;
  end;
  typeNamedBy := d;
end;

{ Does e name a type (TFoo, TOuter.TInner), as resolved? }
function namesType(e: PNode): boolean;
begin
  if e^.kind = nkNameRef then
    exit((e^.refDecl <> nil) and (e^.refDecl^.kind = dkType));
  namesType := (e^.kind = nkField) and (e^.fieldDecl <> nil) and
               (e^.fieldDecl^.kind = dkType);
end;

{ The type helper active for values of type t (the last declared in
  scope; a string constant: the ansistring or shortstring one), or nil. }
function helperFor(t: PType): PType;
var
  d: PDecl;
  h: PType;
begin
  helperFor := nil;
  if (t = nil) or isError(t) or (mode <> lmObjFPC) then
    exit;
  if t^.kind = tyStringLit then
  begin
    h := helperFor(ansiStringType);
    if h = nil then
      h := helperFor(shortStringType(255));
    exit(h);
  end;
  d := lookup(helperKey(t));
  if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) and
     (d^.declType^.kind = tyClass) then
    helperFor := d^.declType;
end;

{ The helper of the type base names (AnsiString.Join), or nil. }
function helperNamedBy(base: PNode): PType;
var
  d: PDecl;
  t, h: PType;
begin
  helperNamedBy := nil;
  if mode <> lmObjFPC then
    exit;
  d := typeNamedBy(base);
  if d = nil then
    exit;
  t := d^.declType;
  if (t = nil) or (t^.kind = tyClass) or (memberTableOf(t) <> nil) then
    exit;
  h := helperFor(t);
  if h = nil then
    exit;
  if base^.kind = nkNameRef then
    base^.refDecl := d;
  base^.typ := t;
  helperNamedBy := h;
end;

{ TColor.Red: the value of the enumeration type e's base names (any
  enumeration's; the only way to a scoped one's, $scopedenums on). nil if
  the base does not name an enumeration type (or names one with a helper,
  and not one of its values), or after a diagnostic (reported). }
function enumValueNamedBy(e: PNode; var reported: boolean): PDecl;
var
  td, v: PDecl;
  et: PType;
  i: longint;
begin
  enumValueNamedBy := nil;
  td := typeNamedBy(e^.base);
  if td = nil then
    exit;
  et := td^.declType;
  if (et = nil) or (et^.kind <> tyEnum) then
    exit;
  for i := 0 to et^.enumValues.count - 1 do
  begin
    v := PDecl(listGet(et^.enumValues, i));
    if v^.ident.name = e^.fieldName.name then
    begin
      if e^.base^.kind = nkNameRef then
      begin
        e^.base^.refDecl := td;
        warnHints(td, e^.base^.ref);
      end;
      e^.base^.typ := et;
      exit(v);
    end;
  end;
  if helperFor(et) <> nil then
    exit; { a member of its helper }
  ms('no value named ''');
  maN(e^.fieldName.spelling);
  ma(''' in enumeration ');
  maT(et);
  errR(e^.fieldName.loc, identRange(e^.fieldName));
  reported := true;
end;

{ Resolves r.f / obj.member / TFoo.Create; nil after a diagnostic. }
function resolveMember(e: PNode): PDecl;
var
  gs, types: TPtrList;
  t: PType;
  i: longint;
  viaClass, bt, owner, h: PType;
  d, td: PDecl;
  viaRef, ok, reported: boolean;
  what: string;
begin
  resolveMember := nil;
  viaClass := classNamedBy(e^.base);
  if (viaClass <> nil) and (e^.base^.kind = nkNameRef) then { TFoo.Create: a use of TFoo }
    warnHints(e^.base^.refDecl, e^.base^.ref);
  if viaClass = nil then
  begin
    reported := false;
    d := enumValueNamedBy(e, reported); { TColor.Red }
    if d <> nil then
      exit(d);
    if reported then
      exit;
    h := helperNamedBy(e^.base);
    if h <> nil then { AnsiString.Join }
    begin
      owner := nil;
      d := findMember(h, e^.fieldName.name, owner);
      if d = nil then
      begin
        ms('no member named ''');
        maN(e^.fieldName.spelling);
        ma(''' in ' + helperKindName(h) + ' ');
        maT(h);
        errR(e^.fieldName.loc, identRange(e^.fieldName));
        exit;
      end;
      checkMemberAccess(d, owner, e^.fieldName);
      if (d^.kind <> dkConst) and not ((d^.kind = dkRoutine) and d^.isStatic) and
         (d^.kind <> dkVar) then
      begin
        ms('only static class methods, class variables and constants of a ' +
           helperKindName(h) + ' can be used through a type name; ''');
        maN(e^.fieldName.spelling);
        ma(''' is a ' + kindName(d));
        errR(e^.fieldName.loc, identRange(e^.fieldName));
        exit;
      end;
      e^.fieldDecl := d;
      exit(d);
    end;
    td := typeNamedBy(e^.base); { TH.Join }
    if (td <> nil) and (td^.declType <> nil) and (td^.declType^.kind = tyClass) and
       (td^.declType^.classHelperFor <> nil) then
    begin
      ms('the members of ' + helperKindName(td^.declType) + ' ');
      maT(td^.declType);
      ma(' are used through ');
      maT(td^.declType^.classHelperFor);
      errR(e^.base^.b, nodeRange(e^.base));
      exit;
    end;
  end;
  if viaClass <> nil then
    bt := viaClass
  else
    bt := check(e^.base);
  if isError(bt) then
    exit;
  if (viaClass = nil) and not (bt^.kind in [tyClass, tyClassRef, tyInterface]) then
  begin
    h := helperFor(bt);
    if h <> nil then { a type helper's member }
    begin
      owner := nil;
      d := findMember(h, e^.fieldName.name, owner);
      if d <> nil then
      begin
        checkMemberAccess(d, owner, e^.fieldName);
        e^.fieldDecl := d;
        { Self is by reference: a temporary for a value }
        e^.helperValue := not isHelperVariable(e^.base) or
                          (constParamRoot(e^.base) <> nil);
        exit(d);
      end;
    end;
  end;
  if memberTableOf(bt) <> nil then { an advanced record's member }
    bt := memberTableOf(bt);
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
  if bt^.kind = tyInterface then { s.M, s: an interface }
  begin
    d := findInterfaceMethod(bt, e^.fieldName.name);
    if d = nil then
    begin
      ms('no method named ''');
      maN(e^.fieldName.spelling);
      ma(''' in interface ');
      maT(bt);
      errR(e^.fieldName.loc, identRange(e^.fieldName));
      exit;
    end;
    e^.fieldDecl := d;
    exit(d);
  end;
  viaRef := bt^.kind = tyClassRef; { c.M, c: class of T }
  if viaRef then
    bt := bt^.refClass;
  if bt^.kind <> tyClass then
  begin
    ms('''.'' applied to a value of type ');
    maT(bt);
    ma(', which is not a record');
    errR(e^.fieldName.loc, nodeRange(e^.base));
    exit;
  end;
  owner := nil;
  d := nil;
  if e^.fieldTypeArgs.count = 0 then
    d := findMember(bt, e^.fieldName.name, owner);
  if bt^.classRecord <> nil then
    what := 'record'
  else
    what := 'class';
  if d = nil then { a generic method: specialized here, or by the call (inferred) }
  begin
    if e^.fieldTypeArgs.count > 0 then
    begin
      d := findMember(bt, genericKey(e^.fieldName.name, e^.fieldTypeArgs.count), owner);
      if d = nil then
      begin
        ms('no generic method ''');
        maN(e^.fieldName.spelling);
        ma(''' with ' + intToStr(e^.fieldTypeArgs.count) + ' type parameter');
        if e^.fieldTypeArgs.count <> 1 then
          ma('s');
        ma(' in ' + what + ' ');
        maT(bt);
        errR(e^.fieldName.loc, identRange(e^.fieldName));
        exit;
      end;
    end
    else
    begin
      genericMethodsNamed(bt, e^.fieldName.name, gs);
      if gs.count > 0 then
      begin
        d := PDecl(listGet(gs, 0));
        d := findMember(bt, genericKey(e^.fieldName.name, d^.typeParams.count), owner);
      end;
    end;
  end;
  if d = nil then
  begin
    ms('no member named ''');
    maN(e^.fieldName.spelling);
    ma(''' in ' + what + ' ');
    maT(bt);
    maDidYouMeanMember(bt, e^.fieldName.name);
    errR(e^.fieldName.loc, identRange(e^.fieldName));
    exit;
  end;
  checkMemberAccess(d, owner, e^.fieldName);
  if (viaClass <> nil) or viaRef then
  begin
    { through a class: constructors, class methods, class variables and
      class properties }
    ok := (d^.kind = dkVar) or (d^.kind = dkConst) or (d^.kind = dkType);
    if d^.kind = dkRoutine then
      ok := ((d^.methodKind = mtConstructor) or d^.isClassMethod) and
            (d^.methodKind <> mtOperator);
    if d^.kind = dkProperty then
      ok := d^.propIsClass;
    if not ok and not ((d^.kind = dkRoutine) and (d^.memberKey <> nil) and
                       d^.isClassMethod) then
    begin
      ms('only constructors, class methods, class variables and class ' +
         'properties can be used through ');
      if viaClass <> nil then
      begin
        ma('the ' + what + ' name ');
        maT(bt);
      end
      else
        ma('a class reference');
      ma('; ''');
      maN(e^.fieldName.spelling);
      ma(''' is a ' + kindName(d));
      errR(e^.fieldName.loc, identRange(e^.fieldName));
      exit;
    end;
  end;
  { obj.specialize M<T>: the specialization }
  if (d^.kind = dkRoutine) and (d^.memberKey <> nil) and (e^.fieldTypeArgs.count > 0) then
  begin
    listInit(types);
    for i := 0 to e^.fieldTypeArgs.count - 1 do
    begin
      t := resolveType(PTypeSpec(listGet(e^.fieldTypeArgs, i)), nil);
      if isError(t) then
        exit;
      listAdd(types, t);
    end;
    d := instantiateMethod(e^.fieldName, d, types);
    if d = nil then
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
  if r^.asyncType <> nil then
    methodResultType := r^.asyncType { its task }
  else if r^.isFunction then
    methodResultType := r^.resultType
  else if (r^.methodKind = mtConstructor) and namesType(e^.base) then
    methodResultType := e^.base^.typ { TFoo.Create, TOuter.TInner.Create }
  else if (r^.methodKind = mtConstructor) and (e^.base^.typ <> nil) and
          (e^.base^.typ^.kind = tyClassRef) then
    methodResultType := e^.base^.typ^.refClass; { c.Create, c: class of T }
end;

{ Is callee obj.M / TFoo.Create / inherited M -- a method call rather than
  a call through a procedural value? }
function isMethodCallee(callee: PNode): boolean;
var
  gs: TPtrList;
  bt, owner, h: PType;
  d: PDecl;
begin
  if callee^.kind = nkInherited then
    exit(true);
  if (callee^.kind <> nkField) or (mode <> lmObjFPC) then
    exit(false);
  if classNamedBy(callee^.base) <> nil then
    exit(true);
  h := helperNamedBy(callee^.base);
  if h <> nil then
  begin
    owner := nil;
    d := findMember(h, callee^.fieldName.name, owner);
    exit((d <> nil) and (d^.kind = dkRoutine));
  end;
  bt := check(callee^.base);
  if not (bt^.kind in [tyClass, tyClassRef, tyInterface]) then
  begin
    h := helperFor(bt);
    if h <> nil then
    begin
      owner := nil;
      d := findMember(h, callee^.fieldName.name, owner);
      if (d <> nil) and (d^.kind = dkRoutine) then
        exit(true);
    end;
  end;
  if bt^.kind = tyInterface then
    exit(findInterfaceMethod(bt, callee^.fieldName.name) <> nil);
  if memberTableOf(bt) <> nil then { an advanced record's method }
    bt := memberTableOf(bt);
  if bt^.kind = tyClassRef then
    bt := bt^.refClass;
  if bt^.kind <> tyClass then
    exit(false);
  owner := nil;
  d := findMember(bt, callee^.fieldName.name, owner);
  genericMethodsNamed(bt, callee^.fieldName.name, gs);
  isMethodCallee := ((d <> nil) and (d^.kind = dkRoutine)) or (gs.count > 0);
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
  i: longint;
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
  if encl <> curRoutine then
  begin
    { from a routine nested in the method: the call takes the method's
      Self, and a bare inherited its parameters }
    if encl^.selfVar <> nil then
      encl^.selfVar^.captured := true;
    for i := 0 to encl^.params.count - 1 do
      PDecl(listGet(encl^.params, i))^.captured := true;
  end;
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
function checkInheritedCall(e: PNode; var args: TPtrList; callRange: TRange): PType;
var
  r, p, same: PDecl;
  cands: TPtrList;
  nothing: boolean;
  name, pn: TIdent;
  i: longint;
  n: PNode;
  rg: TRange;
  implicit: TPtrList;
begin
  checkInheritedCall := nil;
  r := resolveInherited(e, nothing);
  if r <> nil then { objfpc: overloads; a bare inherited means the same heading }
  begin
    listInit(cands);
    collectOverloads(r, cands);
    if (cands.count > 1) and not identValid(e^.inhName) and (args.count = 0) then
    begin
      same := nil;
      for i := 0 to cands.count - 1 do
        if (same = nil) and sameSignature(PDecl(listGet(cands, i)), e^.inhEnclosing) then
          same := PDecl(listGet(cands, i));
      if same <> nil then
        r := same;
    end
    else if cands.count > 1 then
    begin
      if identValid(e^.inhName) then
        r := resolveOverload(r, args, e^.inhName, callRange)
      else
        r := resolveOverload(r, args, r^.ident, callRange);
      if r = nil then
        exit(errorType);
    end;
    e^.inhMethod := r;
  end;
  if r = nil then
  begin
    for i := 0 to args.count - 1 do
      check(argValue(PNode(listGet(args, i))));
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
  begin
    orderArgs(r^.params, args, name);
    checkCallArgs(r^.params, r, name, args, callRange);
  end;
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
  if not acceptsArgCount(r^.params, 0) then
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
{ p.Free or p.Destroy of an owned variable p (already resolved): an error,
  since its object is freed again when the routine ends. }
function diagOwnedFree(e: PNode): boolean;
var
  ov: PDecl;
begin
  diagOwnedFree := false;
  ov := ownedVarOf(e^.base);
  if ov = nil then
    exit;
  if not nameEquals(e^.fieldName.name, 'free') and
     not nameEquals(e^.fieldName.name, 'destroy') then
    exit;
  ms('''');
  maN(e^.fieldName.spelling);
  ma(''' cannot be called on owned variable ''');
  maN(ov^.ident.spelling);
  ma(''', which is freed when its routine ends; assign nil to free its ' +
     'object now');
  errR(e^.fieldName.loc, nodeRange(e));
  diagOwnedFree := true;
end;

function checkMethodCall(e: PNode): PType;
var
  gs: TPtrList;
  f, d: PDecl;
  callee: PNode;
  t: PType;
  i: longint;
begin
  callee := e^.calleeExpr;
  if callee^.kind = nkInherited then
    exit(checkInheritedCall(callee, e^.args, nodeRange(e)));
  d := resolveMember(callee);
  if (d <> nil) and diagOwnedFree(callee) then
    exit(errorType);
  if (d <> nil) and (d^.kind = dkRoutine) and (d^.memberKey <> nil) then
  begin { obj.M(args): the types from the args }
    genericMethodsNamed(d^.ownerClass, callee^.fieldName.name, gs);
    d := inferFor(callee^.fieldName, gs, e^.args, nil);
    if d = nil then
    begin
      for i := 0 to e^.args.count - 1 do
        check(PNode(listGet(e^.args, i)));
      exit(errorType);
    end;
    callee^.fieldDecl := d;
  end
  else if (d <> nil) and (d^.kind = dkRoutine) then { objfpc: overloads }
  begin
    d := resolveOverload(d, e^.args, callee^.fieldName, nodeRange(e));
    if d = nil then
      exit(errorType);
    callee^.fieldDecl := d;
    warnHints(d, callee^.fieldName);
  end;
  if (d = nil) or (d^.kind <> dkRoutine) then
  begin
    if (d <> nil) and (d^.kind = dkType) then
    begin
      ms('typecasts through a qualified type name are not supported yet; ' +
         'declare a type alias for ''');
      maN(callee^.fieldName.spelling);
      ma('''');
      errR(callee^.fieldName.loc, identRange(callee^.fieldName));
    end
    else if d <> nil then
    begin
      ms('''');
      maN(callee^.fieldName.spelling);
      ma(''' is a ' + kindName(d) + ', not a method');
      errR(callee^.fieldName.loc, identRange(callee^.fieldName));
    end;
    for i := 0 to e^.args.count - 1 do
      check(PNode(listGet(e^.args, i)));
    exit(errorType);
  end;
  f := d;
  orderArgs(f^.params, e^.args, callee^.fieldName);
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
  comObject: boolean;
begin
  checkIsAs := errorType;
  l := check(e^.lhs);
  { the left operand may be a COM interface (its object) }
  comObject := (l^.kind = tyInterface) and l^.intfCOM;
  op := binaryOpSpelling(e^.bop);
  c := nil;
  rhs := e^.rhs;
  td := nil;
  if rhs^.kind = nkNameRef then
  begin
    d := lookup(rhs^.ref.name);
    if (d <> nil) and (d^.kind = dkType) then
      td := d;
  end
  else if rhs^.kind = nkField then { TOuter.TInner }
    td := typeNamedBy(rhs);
  if td <> nil then
  begin
    t := td^.declType;
    if t = nil then
      t := errorType;
    if rhs^.kind = nkNameRef then
      rhs^.refDecl := td;
    rhs^.typ := t;
    if isError(t) then
      exit;
    if t^.kind = tyClass then
      c := t;
    { obj is IFoo / obj as IFoo: does obj's class implement the interface? }
    if t^.kind = tyInterface then
    begin
      if isError(l) then
        exit;
      if (l^.kind <> tyClass) and not comObject then
      begin
        ms('invalid operands to ''' + op + ''' (');
        maT(l);
        ma(' and ');
        maT(t);
        ma('): the left operand must be an object or a COM interface');
        errRR(e^.opLoc, nodeRange(e^.lhs), nodeRange(rhs));
        exit;
      end;
      if e^.bop = boIs then
        exit(tyBooleanT);
      exit(t);
    end;
  end
  else if isError(check(rhs)) then
    exit;
  if c = nil then
  begin
    ms('the right operand of ''' + op + ''' must be the name of a class or ' +
       'interface type');
    errR(rhs^.b, nodeRange(rhs));
    exit;
  end;
  if isError(l) then
    exit;
  if comObject then { its object's class is not known }
  begin
    if e^.bop = boIs then
      exit(tyBooleanT);
    exit(c);
  end;
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
  declared, savedScope: PScope; { declared: of the variables declared in it }
begin
  listAdd(containers, seq);
  declared := nil;
  savedScope := cur;
  for i := 0 to body.count - 1 do
  begin
    if diagShouldStop then
      break;
    directSequence := seq;
    seqScope := @declared;
    visitStmt(PNode(listGet(body, i)));
  end;
  directSequence := nil;
  seqScope := nil;
  if declared <> nil then
    cur := savedScope;
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

{ The procedural type of the field or property d, or nil. }
function proceduralMember(d: PDecl): PType;
var
  t: PType;
begin
  t := nil;
  if d^.kind = dkField then
    t := d^.fieldType
  else if d^.kind = dkProperty then
    t := d^.propType;
  if (t <> nil) and (t^.kind <> tyRoutine) then
    t := nil;
  proceduralMember := t;
end;

{ A name used as an expression. }
function nameRefNode(const id: TIdent): PNode;
var
  n: PNode;
begin
  n := newNode(nkNameRef, id.loc, id.loc + length(nameStr(id.spelling)));
  n^.ref := id;
  nameRefNode := n;
end;

procedure visitProcCall(s: PNode);
var
  ref: PNode;
  d: PDecl;
  t, rt: PType;
  a0: PNode;
  i: longint;
  w: PWithContext;
  inferred: boolean;
begin
  w := nil;
  if s^.typeArgs.count = 0 then
    d := scopeLookup(cur, s^.callee.name, w)
  else
    d := specializeGenericW(s^.callee, s^.typeArgs, w);
  noteLambdaUse(d, w, s^.callee);
  inferred := false; { f(args) for a generic f: its types inferred }
  if (d = nil) and (s^.typeArgs.count = 0) then
    d := inferSpecializationW(s^.callee, s^.args, nil, inferred, w);
  if d = nil then
  begin
    if (s^.typeArgs.count = 0) and not inferred then
    begin
      diagUndeclared(s^.callee);
      exit;
    end;
    for i := 0 to s^.args.count - 1 do
      check(argValue(PNode(listGet(s^.args, i))));
    exit;
  end;
  if d^.kind = dkRoutine then { objfpc: overloads }
  begin
    d := resolveOverload(d, s^.args, s^.callee, nodeRange(s));
    if d = nil then
      exit;
  end;
  s^.callDecl := d;
  warnHints(d, s^.callee);
  if hasMembers(w) then { a method of an object or advanced record }
  begin
    s^.callWith := w;
    checkScopeAccess(d, w, s^.callee);
  end;

  if (d^.kind = dkBuiltin) and (d^.builtin = bkWait) then
  begin
    checkWait(s^.args, s^.callee, nodeRange(s), false);
    exit;
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
      bkReset, bkRewrite, bkGet, bkPut, bkPage, bkClose, bkCloseFile, bkFlush:
        checkFileProc(s, d^.builtin);
      bkAssign, bkAssignFile: checkAssignFile(s);
      bkBlockRead, bkBlockWrite: checkBlockIO(s);
      bkSeek: checkSeek(s);
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
        end
        else if (finallyLoopDepth >= 0) and (loopDepth = finallyLoopDepth) then
        begin
          ms('');
          maCallee(s^.callee);
          ma(' cannot leave a ''finally'' part');
          errR(s^.callee.loc, identRange(s^.callee));
        end
        else
          ownedBreak(d^.builtin = bkContinue);
      bkExit:
        begin
          if finallyLoopDepth >= 0 then
          begin
            ms('');
            maCallee(s^.callee);
            ma(' is not allowed in a ''finally'' part');
            errR(s^.callee.loc, identRange(s^.callee));
          end;
          if s^.args.count = 0 then
          begin
            ownedDead;
            exit;
          end;
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
            ownedDead;
            exit;
          end;
          a0 := PNode(listGet(s^.args, 0));
          if (a0^.kind = nkTuple) or (a0^.kind = nkCall) then
            t := checkValueFor(curRoutine^.resultType, a0)
          else
            t := check(a0);
          if ownedVarOf(a0) <> nil then
            diagOwnedEscape(ownedVarOf(a0), a0, true);
          checkLambdaEscape(a0, false);
          ownedDead;
          rt := curRoutine^.resultType;
          curRoutine^.resultAssigned := true;
          if not isError(t) and not isError(rt) and not isAssignable(rt, t) and
             not convertImplicitly(a0, rt) then
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
      bkSetLength: checkSetLength(s);
      bkPack, bkUnpack: checkPackUnpack(s, d^.builtin = bkPack);
      bkGetMem, bkFreeMem, bkReallocMem, bkFillChar, bkMove, bkHalt:
        checkMemoryProc(s, d^.builtin);
    else
      ;
    end;
    exit;
  end;
  if (d^.kind = dkRoutine) and (d^.asyncType <> nil) then
  begin
    orderArgs(d^.params, s^.args, s^.callee);
    checkCallArgs(d^.params, d, s^.callee, s^.args, nodeRange(s));
    warnDanglingTask(s^.callee, nodeRange(s));
    exit;
  end;
  if (d^.kind = dkRoutine) and (not d^.isFunction or not isISO) then
  begin
    { Turbo/FPC: a function result may be discarded }
    orderArgs(d^.params, s^.args, s^.callee);
    checkCallArgs(d^.params, d, s^.callee, s^.args, nodeRange(s));
    exit;
  end;
  if (d^.kind = dkVar) and (d^.varType <> nil) and (d^.varType^.kind = tyRoutine) and
     (d^.varType^.routineResult = nil) then
  begin
    d^.isUsed := true; { (called: used) }
    orderArgs(d^.varType^.routineParams, s^.args, s^.callee);
    checkCallArgs(d^.varType^.routineParams, d, s^.callee, s^.args, nodeRange(s));
    exit;
  end;
  if proceduralMember(d) <> nil then
  begin
    { FOnEvent(x) in a method or a with statement: through the member }
    ref := nameRefNode(s^.callee);
    if isError(check(ref)) then
      exit;
    s^.callValue := ref;
    orderArgs(proceduralMember(d)^.routineParams, s^.args, s^.callee);
    checkCallArgs(proceduralMember(d)^.routineParams, nil, s^.callee, s^.args, nodeRange(s));
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
  d, enc, x: PDecl;
begin
  checkNameTarget := errorType;
  w := nil;
  d := scopeLookup(cur, target^.ref.name, w);
  { (a function's name: its result, noted below) }
  if d = nil then
    noteLambdaUse(d, w, target^.ref)
  else if (d^.kind <> dkRoutine) or not d^.isFunction then
    noteLambdaUse(d, w, target^.ref);
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
      x := d; { (overloads) }
      while x <> nil do
      begin
        if enc = x then
        begin
          if x <> curRoutine then { from a nested routine }
            x^.resultVar^.captured := true;
          noteLambdaUse(x^.resultVar, nil, target^.ref);
          target^.refDecl := x^.resultVar;
          x^.resultAssigned := true;
          exit(x^.resultType);
        end;
        x := x^.nextOverload;
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
  warnHints(d, target^.ref);
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
  begin
    if not checkArrayPropertyUse(d, nil, target^.ref) then
      exit(errorType);
    exit(checkPropertyWrite(d, target^.ref));
  end;
  ms('cannot assign to ' + kindName(d) + ' ''');
  maN(target^.ref.spelling);
  ma('''');
  errR(target^.ref.loc, identRange(target^.ref));
  noteDeclaredHere(d);
end;

{ A procedural variable named as the value of a procedural target is
  copied, not called. }
{ [a, b, c] given to a dynamic array of type t. }
function checkDynArrayCtor(s: PNode; t: PType): PType;
var
  i: longint;
  el: PSetElement;
  elemT, at: PType;
begin
  elemT := t^.dynElement;
  for i := 0 to s^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(s^.setElems, i));
    if el^.hi <> nil then
    begin
      ms('a range cannot be an element of a dynamic array');
      errR(el^.hi^.b, nodeRange(el^.hi));
      continue;
    end;
    at := checkValueFor(elemT, el^.lo);
    if isError(at) then
      continue;
    if not isAssignable(elemT, at) and not isPCharLiteral(elemT, el^.lo) and
       not convertImplicitly(el^.lo, elemT) then
    begin
      ms('a value of type ');
      maT(at);
      ma(' cannot be an element of a dynamic array of ');
      maT(elemT);
      errR(el^.lo^.b, nodeRange(el^.lo));
    end;
  end;
  s^.typ := t;
  checkDynArrayCtor := t;
end;

function checkTuple(e: PNode; want: PType): PType; forward;
function checkCond(e: PNode; want: PType): PType; forward;

function checkValueFor(targetTy: PType; value: PNode): PType;
var
  v: PDecl;
  saved: PType;
  t: PType;
begin
  { a call: the type wanted of its result can give a generic routine a
    type argument (inferSpecialization) }
  if value^.kind = nkCall then
  begin
    wantedCall := value;
    wantedType := targetTy;
  end;
  { if c then a else b: both branches are values for the type wanted }
  if (value^.kind = nkCondExpr) and (value^.typ = nil) then
  begin
    t := checkCond(value, targetTy);
    value^.typ := t;
    if isManaged(t) then
      markUsesManaged;
    exit(t);
  end;
  { a tuple written out takes the element types of the tuple wanted }
  if (value^.kind = nkTuple) and (targetTy^.kind = tyRecord) then
    if targetTy^.isTupleRecord and
       (targetTy^.allFields.count = value^.tupElems.count) then
    begin
      t := checkTuple(value, targetTy);
      value^.typ := t;
      if isManaged(t) then
        markUsesManaged;
      exit(t);
    end;
  if (targetTy^.kind = tyDynArray) and (value^.kind = nkSetCtor) then
    exit(checkDynArrayCtor(value, targetTy));
  if (targetTy^.kind = tyPointer) and targetTy^.untyped and (value^.kind = nkAddrOf) then
  begin
    addrOfWantsCode := true;
    checkValueFor := check(value);
    addrOfWantsCode := false;
    exit;
  end;
  if (targetTy^.kind = tyRoutine) and (value^.kind = nkAddrOf) then
  begin { @p picks an overload by the target's heading }
    saved := addrOfTarget;
    addrOfTarget := targetTy;
    checkValueFor := check(value);
    addrOfTarget := saved;
    exit;
  end;
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
  orderArgs(t^.routineParams, e^.args, name);
  checkCallArgs(t^.routineParams, nil, name, e^.args, nodeRange(e));
  checkIndirectCallee := t;
end;

{ (a, b): a tuple value. want: the tuple type it is a value for (with as
  many elements), whose element types its elements then take; where they
  cannot, and without want, each element has the type a variable declared
  with its value would have. }
function checkTuple(e: PNode; want: PType): PType;
var
  types, noNames: TPtrList;
  i: longint;
  ok: boolean;
  x: PNode;
  t, ft, res: PType;
  v: PDecl;
begin
  listInit(types);
  ok := true;
  for i := 0 to e^.tupElems.count - 1 do
  begin
    x := PNode(listGet(e^.tupElems, i));
    if want <> nil then
      t := checkValueFor(PDecl(listGet(want^.allFields, i))^.fieldType, x)
    else
      t := check(x);
    if isError(t) then
      ok := false;
    listAdd(types, t);
  end;
  if not ok then
    exit(errorType);
  res := want;
  i := 0;
  while (res <> nil) and (i < types.count) do
  begin
    ft := PDecl(listGet(want^.allFields, i))^.fieldType;
    x := PNode(listGet(e^.tupElems, i));
    if isAssignable(ft, PType(listGet(types, i))) or convertImplicitly(x, ft) then
      checkWideToChar(ft, x)
    else
      res := nil;
    i := i + 1;
  end;
  if res = nil then
  begin
    for i := 0 to types.count - 1 do
    begin
      t := PType(listGet(types, i));
      x := PNode(listGet(e^.tupElems, i));
      if (t^.kind = tyStringLit) or (isCharLiteral(x) and not keepCharLiterals) then
      begin
        if e^.tupLong then
          t := ansiStringType
        else
          t := shortStringType(255);
      end
      else if (t^.kind in [tyNil, tyUntyped, tyOpenArray, tyArrayOfConst]) or
              containsFile(t) or ((t^.kind = tySet) and isEmptySet(t)) then
      begin
        ms('cannot tell the type of element ' + intToStr(i + 1) +
           ' of the tuple from ');
        if t^.kind = tyNil then
          ma('nil')
        else if t^.kind = tySet then
          ma('an empty set')
        else
        begin
          ma('a value of type ');
          maT(t);
        end;
        ma('; assign the tuple to a variable of a tuple type');
        errR(x^.b, nodeRange(x));
        exit(errorType);
      end;
      listSet(types, i, t);
    end;
    listInit(noNames);
    res := getTupleType(types, noNames);
  end;
  if e^.tupTemp <> nil then
    e^.tupTemp^.varType := res
  else
  begin
    v := makeHiddenVar(res, e^.b);
    if v = nil then
    begin
      ms('a tuple can only be written in the statements of a routine or program');
      errR(e^.b, nodeRange(e));
      exit(errorType);
    end;
    e^.tupTemp := v;
  end;
  checkTuple := res;
end;

procedure finishAssign(s: PNode; targetTy, valueTy: PType); forward;

{ if c then a else b, a value. want: the type it is a value for, which both
  branches then take; where they cannot, and without want, its type is the
  common type of the branches. It becomes an if statement that assigns the
  branch chosen to a hidden variable. }
function checkCond(e: PNode; want: PType): PType;
var
  a, b, r, str, ta, tb: PType;
  tmp: PDecl;
  thenS, elseS, low: PNode;

  function ref(loc: TLoc): PNode;
  var
    id: TIdent;
    n: PNode;
  begin
    id := tmp^.ident;
    id.loc := loc;
    n := nameRefNode(id);
    n^.refDecl := tmp;
    n^.typ := r;
    ref := n;
  end;

  function assign(v: PNode; vt: PType): PNode;
  var
    s: PNode;
  begin
    s := newNode(nkAssign, v^.b, v^.e);
    s^.target := ref(v^.b);
    s^.assignLoc := v^.b;
    s^.value := v;
    s^.assignCompound := false;
    finishAssign(s, r, vt);
    assign := s;
  end;

begin
  checkCond := errorType;
  checkCondition(e^.ceCond, 'if');
  if want <> nil then
  begin
    a := checkValueFor(want, e^.ceThen);
    b := checkValueFor(want, e^.ceElse);
  end
  else
  begin
    a := check(e^.ceThen);
    b := check(e^.ceElse);
  end;
  if isError(a) or isError(b) then
    exit;
  r := nil;
  if want <> nil then
    if not isError(want) and isAssignable(want, a) and isAssignable(want, b) then
      r := want;
  if r = nil then
  begin
    if e^.ceLong then
      str := ansiStringType
    else
      str := shortStringType(255);
    ta := a;
    if a^.kind = tyStringLit then
      ta := str;
    tb := b;
    if b^.kind = tyStringLit then
      tb := str;
    if isInferable(ta) and isInferable(tb) then
      r := commonType(ta, tb, str)
    else if isInferable(ta) and isAssignable(ta, tb) then
      r := ta { (nil, [] in the other branch) }
    else if isInferable(tb) and isAssignable(tb, ta) then
      r := tb;
    if r = nil then
    begin
      ms('the branches of this conditional expression have no type in common: ');
      maT(a);
      ma(' and ');
      maT(b);
      errRR(e^.b, nodeRange(e^.ceThen), nodeRange(e^.ceElse));
      exit;
    end;
  end;
  tmp := makeHiddenVar(r, e^.b);
  if tmp = nil then
  begin
    ms('a conditional expression can only be written in the statements of ' +
       'a routine or program');
    errR(e^.b, nodeRange(e));
    exit;
  end;
  thenS := assign(e^.ceThen, a);
  elseS := assign(e^.ceElse, b);
  low := newNode(nkIf, e^.b, e^.e);
  low^.cond := e^.ceCond;
  low^.thenStmt := thenS;
  low^.elseStmt := elseS;
  e^.ceLowered := low;
  e^.ceResult := ref(e^.b);
  checkCond := r;
end;

procedure visitAssign(s: PNode);
var
  target, savedTarget, root, sum: PNode;
  targetTy, valueTy, lt: PType;
  p, d: PDecl;
begin
  target := s^.target;
  { a lambda's body, $lambda := e, where no result type is known: the type
    of e (a quoted constant: a string) }
  if curRoutine <> nil then
    if curRoutine^.isLambda and curRoutine^.isFunction and
       (curRoutine^.resultType = nil) and (target^.kind = nkNameRef) then
      if target^.ref.name = curRoutine^.ident.name then
      begin
        lt := check(s^.value);
        if not isError(lt) and ((lt^.kind = tyStringLit) or isCharLiteral(s^.value)) then
          lt := ansiStringType;
        if not isError(lt) and not isInferable(lt) then
        begin
          ms('cannot tell the result type of this lambda from ');
          if lt^.kind = tyNil then
            ma('nil')
          else
          begin
            ma('a value of type ');
            maT(lt);
          end;
          ma('; give it one: function(...): T => ...');
          errR(s^.value^.b, nodeRange(s^.value));
          lt := errorType;
        end;
        curRoutine^.resultType := lt;
        curRoutine^.resultVar^.varType := lt;
      end;
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
    { r.f := v / Result.f := v: assigns the result (a record) in part }
    root := target;
    while root^.kind in [nkField, nkIndex] do
      root := root^.base;
    if (root^.kind = nkNameRef) and (root^.refDecl <> nil) and
       (root^.refDecl^.kind = dkVar) and (root^.refDecl^.varKind = vkResult) then
      root^.refDecl^.owner^.resultAssigned := true;
    p := assignedProperty(target);
    if (p <> nil) and not isError(targetTy) then
    begin
      if target^.kind = nkIndex then
        targetTy := checkPropertyWrite(p, propertyName(target))
      else
        targetTy := checkPropertyWrite(p, target^.fieldName);
      target^.typ := targetTy;
    end
    else if not isError(targetTy) and not isVariableReference(target) and
            not isBitPackedComponent(target) then
    begin
      ms('expression is not assignable');
      errR(target^.b, nodeRange(target));
      targetTy := errorType;
    end;
  end;
  { x += e (Free Pascal's C operators): value is x + (e), x read again }
  sum := nil;
  if s^.assignCompound then
    sum := s^.value;
  if (sum <> nil) and not isError(targetTy) and
     (assignedProperty(target) <> nil) then
  begin
    ms('''' + binaryOpSpelling(sum^.bop) +
       '='' cannot assign to a property; use '':=''');
    errR(s^.assignLoc, nodeRange(target));
    targetTy := errorType;
  end;
  if (sum <> nil) and isError(targetTy) then { reported once: x is not read again }
  begin
    check(sum^.rhs);
    exit;
  end;
  if targetTy^.kind = tyUntyped then
  begin
    ms('cannot assign to untyped parameter ''');
    maN(target^.ref.spelling);
    ma('''; assign to a typecast such as longint(');
    maN(target^.ref.spelling);
    ma(')');
    errR(target^.b, nodeRange(target));
    if sum <> nil then
      check(sum^.rhs)
    else
      check(s^.value);
    exit;
  end;
  if (sum <> nil) and (target^.kind = nkNameRef) then
  begin
    p := target^.refDecl;
    if (p <> nil) and (p^.kind = dkVar) and (p^.varKind = vkResult) and
       (sum^.lhs^.kind = nkNameRef) then
    begin
      d := lookup(target^.ref.name);
      if (d <> nil) and (d^.kind = dkRoutine) then
      begin
        compoundResultRef := sum^.lhs;
        compoundResultVar := p;
      end;
    end;
  end;
  valueTy := checkValueFor(targetTy, s^.value);
  compoundResultRef := nil;
  finishAssign(s, targetTy, valueTy);
end;

{ The rest of the checks of target := value, both already checked. }
procedure finishAssign(s: PNode; targetTy, valueTy: PType);
var
  target, savedOwnedTarget: PNode;
  ownedV, ov, tv: PDecl;
  local, modifiable: boolean;
begin
  target := s^.target;
  checkIsolatedCopy(s^.value, 'assigned');
  if (target^.kind = nkNameRef) and (target^.refDecl <> nil) and
     (target^.refDecl^.kind = dkVar) and (target^.refDecl^.race = rqShared) and
     (target^.refWith = nil) and (spawnOutside(target^.refDecl^.owner) <> nil) then
  begin
    ms('a spawned task cannot assign shared variable ''');
    maN(target^.ref.spelling);
    ma(''', which other tasks use; change what it holds through its methods');
    errR(target^.ref.loc, nodeRange(target));
  end;
  { An owned variable's object stays its own unless moved: only a local
    variable can refer to it too. Assigning to the variable gives it an
    object again. }
  ownedV := ownedVarOf(target);
  ov := ownedVarOf(s^.value);
  if (ov <> nil) and not isError(valueTy) then
  begin
    tv := nil;
    if (target^.kind = nkNameRef) and (target^.refDecl <> nil) and
       (target^.refDecl^.kind = dkVar) then
      tv := target^.refDecl;
    local := (tv <> nil) and (target^.refWith = nil) and not tv^.isOwned and
             (curRoutine <> nil) and (tv^.owner = curRoutine) and
             ((tv^.varKind = vkVariable) or (tv^.varKind = vkValueParam)) and
             ((tv^.init = nil) or tv^.isInitVar) and not tv^.isClassVar;
    if not local then
      diagOwnedEscape(ov, s^.value, (tv <> nil) and (tv^.varKind = vkResult));
  end;
  if ownedV <> nil then
    ownedAssign(ownedV);
  if s^.value^.kind = nkAddrOf then { a lambda: where it is kept }
  begin
    tv := nil;
    if (target^.kind = nkNameRef) and (target^.refDecl <> nil) then
      if target^.refDecl^.kind = dkVar then
        tv := target^.refDecl;
    checkLambdaEscape(s^.value,
                      (tv <> nil) and (target^.refWith = nil) and
                      (curRoutine <> nil) and (tv^.owner = curRoutine) and
                      (tv^.varKind = vkVariable) and
                      ((tv^.init = nil) or tv^.isInitVar) and not tv^.isClassVar);
  end;
  if isError(targetTy) or isError(valueTy) then
    exit;
  savedOwnedTarget := ownedTarget;
  ownedTarget := target;
  modifiable := checkModifiable(target);
  ownedTarget := savedOwnedTarget;
  if not modifiable then
    exit;
  if containsFile(targetTy) then
  begin
    ms('files cannot be assigned');
    errR(s^.assignLoc, nodeRange(target));
    exit;
  end;
  checkWideToChar(targetTy, s^.value);
  if not isAssignable(targetTy, valueTy) and
     not convertImplicitly(s^.value, targetTy) then
  begin
    ms('assigning to ');
    maT(targetTy);
    ma(' from incompatible type ');
    maT(valueTy);
    errRR(s^.assignLoc, nodeRange(target), nodeRange(s^.value));
  end;
end;

{ The type of a variable declared without one, from its value's type t. }
{ var x := [a, b, c]: the dynamic array type of the value, an array written
  out. Nil if init is not one: [], a list with a range, or one whose
  elements are all enumeration values, characters or booleans (a set, as
  ever). The error type after a diagnostic. }
function inferArrayLiteral(init: PNode; longStr: boolean; const name: TIdent): PType;
var
  i: longint;
  el: PSetElement;
  str, elem, t, joined: PType;
  ok, setLike: boolean;
begin
  inferArrayLiteral := nil;
  if (init^.kind <> nkSetCtor) or (init^.setElems.count = 0) then
    exit;
  for i := 0 to init^.setElems.count - 1 do
    if PSetElement(listGet(init^.setElems, i))^.hi <> nil then
      exit;
  if longStr then
    str := ansiStringType
  else
    str := shortStringType(255);
  elem := nil;
  ok := true;
  setLike := true;
  for i := 0 to init^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(init^.setElems, i));
    t := check(el^.lo);
    if isError(t) then
    begin
      ok := false;
      continue;
    end;
    if not (isOrdinal(t) and (getBase(t)^.kind <> tyInteger)) then
      setLike := false;
    if t^.kind = tyStringLit then
      t := str;
    if not isInferable(t) then
    begin
      ms('cannot tell the element type of the array ''');
      maN(name.spelling);
      ma(''' from ');
      if t^.kind = tyNil then
        ma('nil')
      else
      begin
        ma('a value of type ');
        maT(t);
      end;
      ma('; give the variable a type');
      errR(el^.lo^.b, nodeRange(el^.lo));
      ok := false;
      continue;
    end;
    if elem <> nil then
      joined := commonType(elem, t, str)
    else
      joined := t;
    if joined = nil then
    begin
      ms('the elements of the array ''');
      maN(name.spelling);
      ma(''' have no type in common: ');
      maT(elem);
      ma(' and ');
      maT(t);
      ma('; give the variable a type');
      errR(el^.lo^.b, nodeRange(el^.lo));
      ok := false;
      continue;
    end;
    elem := joined;
  end;
  if not ok then
    exit(errorType);
  if setLike then
    exit;
  inferArrayLiteral := dynArrayType(elem);
end;

function inferVarType(s: PNode; v: PDecl; t: PType): PType;
begin
  if isError(t) then
    exit(t);
  if (t^.kind = tyStringLit) or isCharLiteral(s^.vdInit) then
  begin
    if s^.vdLong then
      exit(ansiStringType);
    exit(shortStringType(255));
  end;
  if (t^.kind in [tyNil, tyUntyped, tyOpenArray, tyArrayOfConst, tyFile]) or
     ((t^.kind = tySet) and isEmptySet(t)) then
  begin
    ms('cannot tell the type of ''');
    maN(v^.ident.spelling);
    ma(''' from ');
    if t^.kind = tyNil then
      ma('nil')
    else if t^.kind = tySet then
      ma('an empty set')
    else
    begin
      ma('a value of type ');
      maT(t);
    end;
    ma('; give the variable a type');
    errR(s^.vdInit^.b, nodeRange(s^.vdInit));
    exit(errorType);
  end;
  inferVarType := t;
end;

{ var x: T;  var x: T := e;  var x := e;  -- among the statements of a
  sequence, to whose end the variables are visible. }
procedure visitVarDecl(s: PNode);
var
  seq: pointer;
  declared: PPScope;
  init, target, assign: PNode;
  first, v: PDecl;
  t, valueTy: PType;
  i: longint;
begin
  seq := directSequence;
  declared := seqScope;
  directSequence := nil;
  init := s^.vdInit;
  first := PDecl(listGet(s^.vdVars, 0));
  if (seq = nil) or (declared = nil) then
  begin
    ms('a variable can only be declared in a statement sequence (between ' +
       '''begin'' and ''end'')');
    errR(s^.b, nodeRange(s));
    if init <> nil then
      check(init);
    exit;
  end;
  { the value is checked first: it cannot use the variable }
  valueTy := nil;
  if first^.varSpec <> nil then
  begin
    t := resolveType(first^.varSpec, nil);
    if init <> nil then
      valueTy := checkValueFor(t, init);
  end
  else
  begin
    t := inferArrayLiteral(init, s^.vdLong, first^.ident);
    if t <> nil then { var a := [1, 2, 3]: a dynamic array }
    begin
      if isError(t) then
        valueTy := t
      else
        valueTy := checkValueFor(t, init);
    end
    else
    begin
      valueTy := check(init);
      t := inferVarType(s, first, valueTy);
    end;
  end;
  if declared^ = nil then
  begin
    declared^ := newScope(cur);
    cur := declared^;
  end;
  for i := 0 to s^.vdVars.count - 1 do
  begin
    v := PDecl(listGet(s^.vdVars, i));
    v^.varType := t;
    v^.owner := curRoutine;
    checkOwnedDecl(v);
    declare(v);
    if inlineVars <> nil then
      listAdd(inlineVars^, v);
    if v^.isOwned then
      ownedDeclare(v);
  end;
  if init = nil then
    exit;
  target := nameRefNode(first^.ident);
  target^.refDecl := first;
  target^.typ := t;
  first^.isUsed := true; { (as an assignment does) }
  assign := newNode(nkAssign, first^.ident.loc, init^.e);
  assign^.target := target;
  assign^.assignLoc := s^.vdAssignLoc;
  assign^.value := init;
  s^.vdAssign := assign;
  finishAssign(assign, t, valueTy);
  if s^.vdConst then { from here on }
    first^.isReadOnly := true;
end;

{ (a, b) := e;  var (a, b) := e;  -- e, a tuple, is kept in a hidden
  variable (a tuple written out is built in one), then each target is
  assigned its element, with the checks of an assignment. }
procedure visitDestructure(s: PNode);
var
  seq: pointer;
  declared: PPScope;
  value, target, ref, elem, a: PNode;
  isVar, isTuple: boolean;
  n, i: longint;
  vt, t: PType;
  v, temp, f: PDecl;
  tempId, fieldId: TIdent;

  procedure fail;
  var
    k: longint;
  begin
    if isVar then
      exit;
    for k := 0 to s^.dsTargets.count - 1 do
      check(PNode(listGet(s^.dsTargets, k)));
  end;

begin
  seq := directSequence;
  declared := seqScope;
  directSequence := nil;
  value := s^.dsValue;
  isVar := s^.dsVars.count > 0;
  if isVar then
    n := s^.dsVars.count
  else
    n := s^.dsTargets.count;
  if isVar and ((seq = nil) or (declared = nil)) then
  begin
    ms('a variable can only be declared in a statement sequence (between ' +
       '''begin'' and ''end'')');
    errR(s^.b, nodeRange(s));
    check(value);
    exit;
  end;
  { (a, b) := ('x', 1): the targets have types, so 'x' stays a char, which
    a char and a string can take }
  keepCharLiterals := not isVar;
  vt := check(value);
  keepCharLiterals := false;
  isTuple := (vt^.kind = tyRecord) and vt^.isTupleRecord;
  if not isError(vt) and (not isTuple or (vt^.allFields.count <> n)) then
  begin
    ms(intToStr(n) + ' variables cannot take the elements of ');
    if isTuple then
      ma('the tuple ')
    else
      ma('a value of type ');
    maT(vt);
    errR(value^.b, nodeRange(value));
    vt := errorType;
  end;
  if isVar then { declared also after an error, with the error type }
  begin
    if declared^ = nil then
    begin
      declared^ := newScope(cur);
      cur := declared^;
    end;
    listInit(s^.dsTargets);
    for i := 0 to n - 1 do
    begin
      v := PDecl(listGet(s^.dsVars, i));
      if isError(vt) then
        t := vt
      else
        t := PDecl(listGet(vt^.allFields, i))^.fieldType;
      v^.varType := t;
      v^.owner := curRoutine;
      declare(v);
      if inlineVars <> nil then
        listAdd(inlineVars^, v);
      listAdd(s^.dsTargets, nameRefNode(v^.ident));
    end;
  end;
  if isError(vt) then
  begin
    fail;
    exit;
  end;
  { the variable the elements are read from }
  if value^.kind = nkTuple then
    temp := value^.tupTemp
  else
  begin
    temp := makeHiddenVar(vt, value^.b);
    if temp = nil then { (not among the statements of a block: unreachable) }
    begin
      fail;
      exit;
    end;
    ref := nameRefNode(temp^.ident);
    ref^.refDecl := temp;
    ref^.typ := vt;
    a := newNode(nkAssign, value^.b, value^.e);
    a^.target := ref;
    a^.assignLoc := s^.dsAssignLoc;
    a^.value := value;
    a^.assignCompound := false;
    s^.dsInit := a;
  end;
  for i := 0 to n - 1 do
  begin
    f := PDecl(listGet(vt^.allFields, i));
    target := PNode(listGet(s^.dsTargets, i));
    tempId := temp^.ident;
    tempId.loc := value^.b;
    ref := nameRefNode(tempId);
    ref^.refDecl := temp;
    ref^.typ := vt;
    fieldId := f^.ident;
    fieldId.loc := value^.b;
    elem := newNode(nkField, value^.b, value^.e);
    elem^.base := ref;
    elem^.index := nil;
    elem^.fieldName := fieldId;
    elem^.fieldDecl := f;
    elem^.idxProp := nil;
    elem^.idxRoot := nil;
    elem^.constRef := nil;
    elem^.helperValue := false;
    elem^.typ := f^.fieldType;
    a := newNode(nkAssign, target^.b, value^.e);
    a^.target := target;
    a^.assignLoc := s^.dsAssignLoc;
    a^.value := elem;
    a^.assignCompound := false;
    visitAssign(a);
    listAdd(s^.dsAssigns, a);
  end;
end;

{ for var x [: T] ...: declares x in a scope of the loop, which becomes
  the current one. t is its type if it has none written (nil: not known
  yet). }
procedure declareLoopVar(v: PDecl; t: PType);
begin
  if v^.varSpec <> nil then
    t := resolveType(v^.varSpec, nil);
  v^.varType := t;
  v^.owner := curRoutine;
  cur := newScope(cur);
  declare(v);
  if inlineVars <> nil then
    listAdd(inlineVars^, v);
end;

procedure visitFor(s: PNode);
var
  varRef, bound: PNode;
  varTy, t: PType;
  usable: boolean;
  d: PDecl;
  k: longint;
  before: TOwnedState;
  loop: TOwnedLoop;
  outerScope: PScope;
begin
  directSequence := nil;
  varRef := s^.forVar;
  varTy := errorType;
  usable := false;
  outerScope := cur;
  if s^.forInline <> nil then { (its values cannot use it) }
  begin
    if s^.forInline^.varSpec <> nil then
      declareLoopVar(s^.forInline, nil)
    else
      declareLoopVar(s^.forInline, check(s^.forInit));
  end;
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
  ownedGet(before);
  ownedLoopBegin(loop);
  loopDepth := loopDepth + 1;
  visitStmt(s^.forBody);
  loopDepth := loopDepth - 1;
  ownedLoopEnd(loop, before);
  if usable then
    listPop(activeFors);
  cur := outerScope;
end;

{ A hidden variable of a for-in loop (code generation binds it), named in
  scope sc only: its name is no Pascal identifier. }
function forInVar(const name: string; t: PType; loc: TLoc; sc: PScope): PDecl;
var
  id: TIdent;
  v: PDecl;
begin
  id.name := intern(name);
  id.spelling := id.name;
  id.loc := loc;
  v := newDecl(dkVar, id, loc);
  v^.varKind := vkVariable;
  v^.varType := t;
  v^.owner := curRoutine;
  v^.isUsed := true;
  scopeInsert(sc, id.name, v);
  forInVar := v;
end;

function forInRef(v: PDecl; loc: TLoc): PNode;
var
  id: TIdent;
begin
  id := v^.ident;
  id.loc := loc;
  forInRef := nameRefNode(id);
end;

{ base.name (name as written in Free Pascal, lower: its canonical form),
  at loc. }
function forInMember(base: PNode; const spelling, lower: string; loc: TLoc): PNode;
var
  n: PNode;
begin
  n := newNode(nkField, loc, loc + length(spelling));
  n^.base := base;
  n^.fieldName.name := intern(lower);
  n^.fieldName.spelling := intern(spelling);
  n^.fieldName.loc := loc;
  forInMember := n;
end;

{ The member name of a class, advanced record or interface, or nil. }
function findEnumMember(t: PType; const name: string): PDecl;
var
  owner: PType;
begin
  findEnumMember := nil;
  if memberTableOf(t) <> nil then
    t := memberTableOf(t);
  if t^.kind = tyClass then
    findEnumMember := findMember(t, intern(name), owner)
  else if t^.kind = tyInterface then
    findEnumMember := findInterfaceMethod(t, intern(name));
end;

{ for inVar in inColl do inBody (Turbo/FPC), as in Free Pascal: the
  variable takes the elements of an array or string, the members of a set
  in order, the values of an ordinal type, or the values of an enumerator.
  Variable := element is an ordinary assignment (inStep). }
procedure visitForIn(s: PNode);
var
  varRef, c: PNode;
  varTy, ct, elem: PType;
  usable, isType: boolean;
  d, elemV: PDecl;
  cLoc: TLoc;
  kind: longint;
  inScope, saved, outerScope: PScope;
  before: TOwnedState;
  loop: TOwnedLoop;
  untyped: PDecl;

  function makeStep(value: PNode): PNode;
  var
    step: PNode;
    et: PType;
  begin
    if (untyped <> nil) and (untyped^.varType = nil) then
    begin
      cur := inScope;
      et := check(value);
      cur := saved;
      untyped^.varType := et;
      varRef^.typ := et;
    end;
    step := newNode(nkAssign, varRef^.ref.loc, c^.e);
    step^.target := nameRefNode(varRef^.ref);
    step^.assignLoc := s^.inLoc;
    step^.value := value;
    cur := inScope;
    visitAssign(step);
    cur := saved;
    makeStep := step;
  end;

  { through inColl.GetEnumerator: a function without arguments whose result
    has a function MoveNext: boolean and a property Current; false after
    an error }
  function checkEnumerator: boolean;
  var
    get, next, current, collV, enumV: PDecl;
    et: PType;
    getE, nextE, step: PNode;
    ok: boolean;
  begin
    checkEnumerator := false;
    get := findEnumMember(ct, 'getenumerator');
    if (get <> nil) and (get^.kind <> dkRoutine) then
      get := nil;
    if (get = nil) or not get^.isFunction or not acceptsArgCount(get^.params, 0) or
       (get^.resultType = nil) then
    begin
      ms('''for ... in'' cannot enumerate a value of type ');
      maT(ct);
      ma(': it has no function GetEnumerator without arguments');
      errR(cLoc, nodeRange(c));
      exit;
    end;
    et := get^.resultType;
    if isError(et) then
      exit;
    next := findEnumMember(et, 'movenext');
    if (next <> nil) and (next^.kind <> dkRoutine) then
      next := nil;
    if (next = nil) or not next^.isFunction or (next^.resultType = nil) or
       (getBase(next^.resultType)^.kind <> tyBoolean) or
       not acceptsArgCount(next^.params, 0) then
    begin
      ms('enumerator ');
      maT(et);
      ma(' has no function MoveNext: boolean without arguments');
      errR(cLoc, nodeRange(c));
      noteDeclaredHere(get);
      exit;
    end;
    current := findEnumMember(et, 'current');
    if (current <> nil) and (current^.kind <> dkProperty) then
      current := nil;
    if (current = nil) or (current^.propParams.count > 0) then
    begin
      ms('enumerator ');
      maT(et);
      ma(' has no property Current');
      errR(cLoc, nodeRange(c));
      noteDeclaredHere(get);
      exit;
    end;

    collV := forInVar('in.coll', ct, s^.inLoc, inScope);
    enumV := forInVar('in.enum', et, s^.inLoc, inScope);
    cur := inScope;
    getE := forInMember(forInRef(collV, cLoc), 'GetEnumerator', 'getenumerator', cLoc);
    nextE := forInMember(forInRef(enumV, cLoc), 'MoveNext', 'movenext', cLoc);
    ok := not isError(check(getE));
    if ok then
      ok := not isError(check(nextE));
    cur := saved;
    if not ok then
      exit;
    step := nil;
    if usable then
      step := makeStep(forInMember(forInRef(enumV, cLoc), 'Current', 'current', cLoc));
    s^.inKind := fikEnumerator;
    s^.inCollVar := collV;
    s^.inEnumVar := enumV;
    s^.inGetEnum := getE;
    s^.inMoveNext := nextE;
    s^.inStep := step;
    if et^.kind = tyClass then
      markHasTry; { freed in a finally part }
    checkEnumerator := true;
  end;

begin
  directSequence := nil;
  varRef := s^.inVar;
  varTy := errorType;
  usable := false;
  outerScope := cur;
  { for var x in ...: without a type, x has the type of the elements, known
    when the first one is assigned to it (makeStep) }
  untyped := nil;
  if s^.inInline <> nil then
  begin
    if s^.inInline^.varSpec = nil then
      untyped := s^.inInline;
    checkTypeOrValue(s^.inColl); { (it cannot use x) }
    declareLoopVar(s^.inInline, nil);
  end;
  d := nil;
  if untyped = nil then
    d := lookup(varRef^.ref.name);
  if untyped <> nil then
  begin
    varRef^.refDecl := untyped;
    untyped^.isUsed := true;
    usable := true;
  end
  else if d <> nil then
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
      usable := not checkNotForControlVar(varRef);
    end;
  end
  else
    diagUndeclared(varRef^.ref);
  varRef^.typ := varTy;

  c := s^.inColl;
  cLoc := c^.b;
  ct := checkTypeOrValue(c);
  isType := ((c^.kind = nkNameRef) and (c^.refDecl <> nil) and
             (c^.refDecl^.kind = dkType)) or
            ((c^.kind = nkField) and (c^.fieldDecl <> nil) and
             (c^.fieldDecl^.kind = dkType));
  kind := fikInvalid;
  elem := nil;
  if isError(ct) then
  else if isType then
  begin
    if isOrdinal(ct) then
    begin
      kind := fikOrdinals;
      elem := ct;
    end
    else
    begin
      ms('''for ... in'' over a type needs an ordinal type, not ');
      maT(ct);
      errR(cLoc, nodeRange(c));
    end;
  end
  else if ct^.kind = tyArray then
  begin
    kind := fikArray;
    elem := ct^.elementType;
  end
  else if ct^.kind = tyOpenArray then
  begin
    kind := fikOpenArray;
    elem := ct^.openElement;
  end
  else if ct^.kind = tyDynArray then
  begin
    kind := fikDynArray;
    elem := ct^.dynElement;
    markUsesManaged; { the array is held during the loop }
  end
  else if ct^.kind = tyAnsiString then
  begin
    kind := fikAnsiString;
    elem := tyCharT;
    markUsesManaged; { the string is held during the loop }
  end
  else if ct^.kind = tyShortString then
  begin
    kind := fikShortString;
    elem := tyCharT;
  end
  else if ct^.kind = tyStringLit then
  begin
    kind := fikStringConst;
    elem := tyCharT;
  end
  else if ct^.kind = tySet then
  begin
    kind := fikSet;
    elem := ct^.setElement; { nil for []: no elements }
  end
  else if (ct^.kind = tyClass) or (ct^.kind = tyInterface) or
          (memberTableOf(ct) <> nil) then
    kind := fikEnumerator;
  if (kind = fikInvalid) and not isError(ct) and not isType then
  begin
    ms('''for ... in'' cannot enumerate a value of type ');
    maT(ct);
    ma(' (an array, string, set, ordinal type, or an object or record with ');
    ma('a GetEnumerator method)');
    errR(cLoc, nodeRange(c));
  end;

  saved := cur;
  inScope := newScope(saved);
  if kind = fikEnumerator then
    checkEnumerator
  else if kind <> fikInvalid then
  begin
    elemV := nil;
    if elem <> nil then
      elemV := forInVar('in.elem', elem, s^.inLoc, inScope);
    s^.inKind := kind;
    s^.inElemType := elem;
    s^.inElemVar := elemV;
    s^.inStep := nil;
    if usable and (elem <> nil) then
      s^.inStep := makeStep(forInRef(elemV, cLoc));
  end;
  if (untyped <> nil) and (untyped^.varType = nil) then
  begin
    if kind <> fikInvalid then
    begin
      ms('cannot tell the type of ''');
      maN(varRef^.ref.spelling);
      ma(''' from an empty set; give the variable a type');
      errR(varRef^.ref.loc, identRange(varRef^.ref));
    end;
    untyped^.varType := errorType;
    usable := false;
  end;

  if usable then
    listAdd(activeFors, s);
  ownedGet(before);
  ownedLoopBegin(loop);
  loopDepth := loopDepth + 1;
  visitStmt(s^.inBody);
  loopDepth := loopDepth - 1;
  ownedLoopEnd(loop, before);
  if usable then
    listPop(activeFors);
  cur := outerScope;
end;

procedure visitCase(s: PNode);
var
  selTy, labelTy: PType;
  seen: TPtrList;
  i, j, k: longint;
  arm: PCaseArm;
  l, prev: PCaseLabel;
  dup: int64;
  before, after, st: TOwnedState;
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
  ownedGet(before);
  ownedClear(after);
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
        if (caseKey(l^.loValue, labelTy) > caseKey(prev^.hiValue, labelTy)) or
           (caseKey(l^.hiValue, labelTy) < caseKey(prev^.loValue, labelTy)) then
          continue;
        if caseKey(l^.loValue, labelTy) > caseKey(prev^.loValue, labelTy) then
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
    ownedSet(before);
    visitStmt(arm^.body);
    ownedGet(st);
    ownedJoinInto(after, st);
  end;
  listFree(seen);
  ownedSet(before); { (no else part: no arm may run) }
  visitSequence(@s^.elseBody, s^.elseBody);
  ownedJoin(after);
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
    w^.routine := curRoutine;
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
    if memberTableOf(t) <> nil then { its methods, properties too }
      cur := newClassScope(cur, memberTableOf(t), w)
    else
      cur := newWithScope(cur, t, w);
    if helperFor(t) <> nil then { its helper's members (first) }
    begin
      w^.helper := helperFor(t);
      cur := newClassScope(cur, helperFor(t), w);
    end;
  end;
  visitStmt(s^.withBody);
  cur := saved;
end;

{ on [E:] TFoo do body: E is a variable of type TFoo visible in body. }
procedure checkHandler(h: PExceptHandler);
var
  c: PType;
  v: PDecl;
  saved: PScope;
begin
  c := lookupClass(h^.hClassName);
  h^.hClass := c;
  saved := cur;
  cur := newScope(saved);
  if identValid(h^.hVarName) then
  begin
    v := newDecl(dkVar, h^.hVarName, h^.hVarName.loc);
    v^.varKind := vkVariable;
    if c <> nil then
      v^.varType := c
    else
      v^.varType := errorType;
    v^.owner := curRoutine;
    v^.isUsed := true;
    h^.hVar := v;
    scopeInsert(cur, v^.ident.name, v);
  end;
  directSequence := nil;
  visitStmt(h^.hBody);
  cur := saved;
end;

procedure visitTry(s: PNode);
var
  i, saved, tick: longint;
  bit: int64;
  protected_: TOwnedTry;
  bodyEnd, st: TOwnedState;
begin
  markHasTry;
  directSequence := nil;
  ownedTryBegin(protected_);
  visitSequence(s, s^.tryBody);
  ownedTryEnd(protected_);
  ownedGet(bodyEnd);
  if s^.isFinally then
  begin
    { the finally part runs after any of the body's states; what it leaves
      alone is as the body left it }
    tick := 0;
    if flow <> nil then
    begin
      flow^.s := protected_.seen;
      tick := flow^.tick;
    end;
    saved := finallyLoopDepth;
    finallyLoopDepth := loopDepth;
    visitSequence(s, s^.partBody);
    finallyLoopDepth := saved;
    if flow <> nil then
    begin
      st := flow^.s;
      if not ownedLive(bodyEnd) or not ownedLive(st) then
        ownedDead
      else
      begin
        for i := 0 to flow^.n - 1 do
          if flow^.changed[i] > tick then
          begin
            bit := int64(1) shl i;
            bodyEnd.valid := (bodyEnd.valid and not bit) or (st.valid and bit);
            bodyEnd.moved := (bodyEnd.moved and not bit) or (st.moved and bit);
            bodyEnd.entry[i] := st.entry[i];
          end;
        flow^.s := bodyEnd;
      end;
    end;
    exit;
  end;
  handlerDepth := handlerDepth + 1;
  for i := 0 to s^.handlers.count - 1 do
  begin
    if diagShouldStop then
      break;
    if flow <> nil then
      flow^.s := protected_.seen;
    checkHandler(PExceptHandler(listGet(s^.handlers, i)));
    ownedGet(st);
    ownedJoinInto(bodyEnd, st);
  end;
  if flow <> nil then
    flow^.s := protected_.seen;
  visitSequence(s, s^.partBody);
  if (s^.handlers.count = 0) or (s^.tryElseLoc <> noLoc) then
  begin
    ownedGet(st);
    ownedJoinInto(bodyEnd, st);
  end;
  ownedSet(bodyEnd);
  handlerDepth := handlerDepth - 1;
end;

procedure visitRaise(s: PNode);
var
  t: PType;
  p: PNode;
  i: longint;
begin
  directSequence := nil;
  if s^.raiseObj = nil then
  begin
    if handlerDepth = 0 then
    begin
      ms('''raise'' without an object (a re-raise) is only allowed in an ' +
         'exception handler');
      errR(s^.b, nodeRange(s));
    end;
    ownedDead;
    exit;
  end;
  t := check(s^.raiseObj);
  if not isError(t) and (t^.kind <> tyClass) then
  begin
    ms('''raise'' needs an object of a class type, not ');
    maT(t);
    errR(s^.raiseObj^.b, nodeRange(s^.raiseObj));
  end;
  { raise ... at address, frame: pointers (FPC's CodePointer and Pointer) }
  for i := 0 to 1 do
  begin
    if i = 0 then
      p := s^.raiseAddr
    else
      p := s^.raiseFrame;
    if p = nil then
      continue;
    t := checkValueFor(tyUntypedPointerT, p);
    if not isError(t) and not isAssignable(tyUntypedPointerT, t) then
    begin
      if i = 0 then
        ms('the address')
      else
        ms('the frame');
      ma(' of a raised exception must be a pointer, not ');
      maT(t);
      errR(p^.b, nodeRange(p));
    end;
  end;
  ownedDead;
end;

procedure visitIf(s: PNode);
var
  before, thenEnd: TOwnedState;
begin
  directSequence := nil;
  checkCondition(s^.cond, 'if');
  ownedGet(before);
  visitStmt(s^.thenStmt);
  ownedGet(thenEnd);
  ownedSet(before);
  if s^.elseStmt <> nil then
    visitStmt(s^.elseStmt);
  ownedJoin(thenEnd);
end;

procedure visitWhile(s: PNode);
var
  before: TOwnedState;
  loop: TOwnedLoop;
begin
  directSequence := nil;
  ownedLoopBegin(loop);
  checkCondition(s^.cond, 'while');
  ownedGet(before);
  loopDepth := loopDepth + 1;
  visitStmt(s^.thenStmt);
  loopDepth := loopDepth - 1;
  ownedLoopEnd(loop, before);
end;

procedure visitRepeat(s: PNode);
var
  before: TOwnedState;
  loop: TOwnedLoop;
begin
  ownedLoopBegin(loop);
  loopDepth := loopDepth + 1;
  visitSequence(s, s^.stmts);
  loopDepth := loopDepth - 1;
  if flow <> nil then { continue goes to the condition }
  begin
    ownedJoinInto(flow^.s, loop.continues);
    ownedClear(loop.continues);
  end;
  checkCondition(s^.untilCond, 'until');
  ownedClear(before);
  ownedLoopEnd(loop, before);
end;

procedure visitExprStmt(s: PNode);
var
  e: PNode;
begin
  directSequence := nil;
  e := s^.exprStmtExpr;
  if e^.kind = nkAwait then
    e^.typ := checkAwait(e, false)
  else
    e^.typ := check(e);
  if (e^.kind = nkSpawn) and e^.spawnPool and not isError(e^.typ) then
  begin
    ms('dangling task: the task Pool.Run starts is never awaited; keep it in a ' +
       'variable to await later');
    diagReportBuf(sevWarning, e^.b, msg, nodeRange(e), noRange);
  end;
end;

procedure visitScope(s: PNode);
var
  t: PType;
  k: PNode;
begin
  directSequence := nil;
  k := s^.scopeToken;
  if k <> nil then
  begin
    t := check(k);
    if not isError(t) and (t^.kind <> tyToken) and (t^.kind <> tyNil) then
    begin
      ms('the token of a scope must be a TCancelToken, not a value of type ');
      maT(t);
      errR(k^.b, nodeRange(k));
    end;
  end;
  markHasTry; { its end waits however the block ends }
  scopeDepth := scopeDepth + 1;
  visitSequence(s, s^.scopeBody);
  scopeDepth := scopeDepth - 1;
end;

procedure visitDetached(s: PNode);
var
  c, h: PNode;
  t, formal: PType;
  eParam, p: PDecl;
  params: TPtrList;
begin
  directSequence := nil;
  c := s^.detCall;
  t := check(c);
  if not isError(t) and ((t^.kind <> tyTask) or not isAsyncCall(c)) then
  begin
    ms('''detached'' runs the call of an async routine, not a value of type ');
    maT(t);
    errR(c^.b, nodeRange(c));
  end;
  h := s^.detHandler;
  if h <> nil then
  begin
    { procedure(E: Exception): a routine (or procedural value) }
    eParam := newDecl(dkVar, memberIdent('E'), noLoc);
    eParam^.varKind := vkValueParam;
    eParam^.varType := exceptionType;
    listInit(params);
    listAdd(params, eParam);
    formal := newRoutineType(params, nil, false);
    p := newDecl(dkVar, memberIdent('handler'), noLoc);
    p^.varKind := vkValueParam;
    p^.varType := formal;
    checkRoutineArg(h, p, formal);
    checkLambdaEscape(h, false);
    if h^.typ = nil then
      h^.typ := formal;
  end;
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
          if (rt <> nil) and (rt^.kind = tyTask) and isAsyncCall(s^.calleeExpr) and
             (s^.calleeExpr^.calleeExpr^.kind = nkField) then
            warnDanglingTask(s^.calleeExpr^.calleeExpr^.fieldName,
                             nodeRange(s^.calleeExpr));
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
    nkIf: visitIf(s);
    nkWhile: visitWhile(s);
    nkRepeat: visitRepeat(s);
    nkFor: visitFor(s);
    nkForIn: visitForIn(s);
    nkCase: visitCase(s);
    nkWith: visitWith(s);
    nkTry: visitTry(s);
    nkRaise: visitRaise(s);
    nkVarDecl: visitVarDecl(s);
    nkDestructure: visitDestructure(s);
    nkExprStmt: visitExprStmt(s);
    nkScope: visitScope(s);
    nkDetached: visitDetached(s);
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
  noArgs: TPtrList;
begin
  if e = compoundResultRef then
  begin
    e^.refDecl := compoundResultVar;
    exit(compoundResultVar^.varType);
  end;
  visitNameRef := errorType;
  w := nil;
  d := scopeLookup(cur, e^.ref.name, w);
  noteLambdaUse(d, w, e^.ref);
  if d = nil then
  begin
    diagUndeclared(e^.ref);
    exit;
  end;
  if d^.kind = dkRoutine then { objfpc: overloads }
  begin
    listInit(noArgs);
    d := resolveOverload(d, noArgs, e^.ref, identRange(e^.ref));
    if d = nil then
      exit;
  end;
  e^.refDecl := d;
  e^.refWith := w;
  d^.isUsed := true;
  warnHints(d, e^.ref);
  checkScopeAccess(d, w, e^.ref);
  case d^.kind of
    dkProperty:
      if checkArrayPropertyUse(d, e, e^.ref) then
        visitNameRef := checkPropertyRead(d, e, e^.ref);
    dkVar:
      begin
        t := d^.varType;
        if d^.isOwned then
          ownedUse(d, identRange(e^.ref));
        if (t <> nil) and (t^.kind = tyRoutine) then
        begin
          { a functional parameter without arguments is a call }
          if (t^.routineResult <> nil) and acceptsArgCount(t^.routineParams, 0) then
            exit(t^.routineResult);
          if t^.routineOfObject then { nothing else it could be: the value }
            exit(t);
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
        if (constructedThroughSelf(d, w) <> nil) and acceptsArgCount(d^.params, 0) then
          exit(constructedThroughSelf(d, w));
        if (d^.asyncType <> nil) and acceptsArgCount(d^.params, 0) then
          exit(d^.asyncType); { its task }
        if not d^.isFunction then
        begin
          ms('procedure ''');
          maN(e^.ref.spelling);
          ma(''' does not return a value');
          errR(e^.ref.loc, identRange(e^.ref));
          exit;
        end;
        if not acceptsArgCount(d^.params, 0) then
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
        if d^.builtin = bkExceptAddr then
          exit(tyUntypedPointerT);
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
    dkType:
      { objfpc: a class name as a value is that class (a class reference) }
      if (d^.declType <> nil) and (d^.declType^.kind = tyClass) and
         (mode = lmObjFPC) then
        visitNameRef := classRefOf(d^.declType)
      else
      begin
        ms('''');
        maN(e^.ref.spelling);
        ma(''' is a ' + kindName(d) + ', not a value');
        errR(e^.ref.loc, identRange(e^.ref));
        noteDeclaredHere(d);
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
  if memberTableOf(t) <> nil then
    exit(checkRecordUnary(e)); { an advanced record's operator }
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
    { -cardinal and -qword can be negative (FPC: int64) }
    if (e^.uop = uoMinus) and ((p = tyCardinalT) or isQWord(p)) then
      exit(tyInt64T);
    exit(p);
  end;
  ms('invalid operand to ''' + unaryOpSpelling(e^.uop) + ''' (');
  maT(t);
  ma(')');
  errR(e^.b, nodeRange(e^.operand));
end;

{ An operand of = or <>: a method pointer variable is compared, not called. }
function checkComparand(e: PNode): PType;
var
  v: PDecl;
begin
  if e^.kind = nkNameRef then
  begin
    v := lookup(e^.ref.name);
    if (v <> nil) and (v^.kind = dkVar) and (v^.varType <> nil) and isMethodPointer(v^.varType) then
    begin
      e^.refDecl := v;
      e^.typ := v^.varType;
      v^.isUsed := true;
      exit(v^.varType);
    end;
  end;
  checkComparand := check(e);
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
  if e^.bop in [boEq, boNe] then
  begin
    l := checkComparand(e^.lhs);
    r := checkComparand(e^.rhs);
  end
  else
  begin
    l := check(e^.lhs);
    r := check(e^.rhs);
  end;
  if isError(l) or isError(r) then
    exit;
  if (e^.bop in [boEq, boNe]) and (isMethodPointer(l) or isMethodPointer(r)) and
     ((l^.kind = tyNil) or (r^.kind = tyNil) or isAssignable(l, r)) then
    exit(tyBooleanT);
  if (memberTableOf(l) <> nil) or (memberTableOf(r) <> nil) then
    exit(checkRecordBinary(e)); { an advanced record's operator }
  op := e^.bop;
  { Turbo/FPC strings: + concatenates, comparisons work for any lengths }
  if not isISO and isStringish(l) and isStringish(r) then
  begin
    if (op = boAdd) and not (isNumeric(l) or isNumeric(r)) then
    begin
      if (l^.kind = tyAnsiString) or (r^.kind = tyAnsiString) then
        exit(ansiStringType);
      exit(shortStringType(255));
    end;
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
        { the operand with the larger representation (its values hold the
          other's); the left one of equals, the other one of [] }
        if isEmptySet(l) then
          exit(r)
        else if isEmptySet(r) then
          exit(l)
        else if setStorageBits(r) > setStorageBits(l) then
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
  if (op in [boEq, boNe]) and
     ((l^.kind in [tyClass, tyClassRef, tyInterface, tyDynArray]) or
      (r^.kind in [tyClass, tyClassRef, tyInterface, tyDynArray])) and
     areCompatible(l, r) then
    exit(tyBooleanT); { objects and classes (objfpc) }

  if not isISO and ((l^.kind = tyPointer) or (r^.kind = tyPointer)) then
  begin
    { Turbo/FPC pointer arithmetic: p + n, n + p and p - n move p by n of
      what it points to (bytes, for an untyped pointer); p - q counts them;
      pointers are ordered by address }
    if (op in [boAdd, boSub]) and (l^.kind = tyPointer) and isIntegerLike(r) then
      exit(l);
    if (op = boAdd) and isIntegerLike(l) and (r^.kind = tyPointer) then
      exit(r);
    if (l^.kind = tyPointer) and (r^.kind = tyPointer) and areCompatible(l, r) then
    begin
      if op = boSub then
        exit(tyInt64T);
      if op in [boLt, boLe, boGt, boGe] then
        exit(tyBooleanT);
    end;
  end;

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
        begin
          if (getBase(l) <> getBase(r)) and isCharLike(l) then { char, widechar }
            e^.operandType := tyWideCharT;
          exit(tyBooleanT);
        end;
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
  ref: PNode;
  inferred, ok: boolean;
begin
  visitCall := errorType;
  { a lambda that copies what it uses: its factory, declared now }
  if (e^.callDecl <> nil) and (e^.callDecl^.kind = dkRoutine) then
    if e^.callDecl^.isCopyFactory then
    begin
      if not e^.callDecl^.lambdaDone then
      begin
        if wantedCall = e then
          ok := completeCopyFactory(e^.callDecl, e, wantedType)
        else
          ok := completeCopyFactory(e^.callDecl, e, nil);
        if not ok then
          exit;
      end;
      if e^.callDecl^.lambdaBad then
        exit;
    end;
  w := nil;
  if e^.typeArgs.count = 0 then
    d := scopeLookup(cur, e^.callee.name, w)
  else
    d := specializeGenericW(e^.callee, e^.typeArgs, w);
  noteLambdaUse(d, w, e^.callee);
  inferred := false; { f(args) for a generic f: its types inferred }
  if (d = nil) and (e^.typeArgs.count = 0) then
  begin
    if wantedCall = e then
      d := inferSpecializationW(e^.callee, e^.args, wantedType, inferred, w)
    else
      d := inferSpecializationW(e^.callee, e^.args, nil, inferred, w);
  end;
  if d = nil then
  begin
    if (e^.typeArgs.count = 0) and not inferred then
      diagUndeclared(e^.callee);
    for i := 0 to e^.args.count - 1 do
      check(argValue(argNode(e, i)));
    exit;
  end;
  if d^.kind = dkRoutine then { objfpc: overloads }
  begin
    d := resolveOverload(d, e^.args, e^.callee, nodeRange(e));
    if d = nil then
      exit;
  end;
  e^.callDecl := d;
  warnHints(d, e^.callee);
  if hasMembers(w) then { a method of an object or advanced record }
  begin
    e^.callWith := w;
    checkScopeAccess(d, w, e^.callee);
  end;
  if proceduralMember(d) <> nil then
  begin
    { FCalc(x) in a method or a with statement: through the member }
    ref := nameRefNode(e^.callee);
    if isError(check(ref)) then
      exit;
    e^.callValue := ref;
    orderArgs(proceduralMember(d)^.routineParams, e^.args, e^.callee);
    checkCallArgs(proceduralMember(d)^.routineParams, nil, e^.callee, e^.args, nodeRange(e));
    if proceduralMember(d)^.routineResult <> nil then
      exit(proceduralMember(d)^.routineResult);
    ms('procedure ''');
    maN(e^.callee.spelling);
    ma(''' does not return a value');
    errR(e^.callee.loc, identRange(e^.callee));
    exit;
  end;
  if d^.kind = dkRoutine then
  begin
    if d^.asyncType <> nil then { its task }
    begin
      orderArgs(d^.params, e^.args, e^.callee);
      checkCallArgs(d^.params, d, e^.callee, e^.args, nodeRange(e));
      exit(d^.asyncType);
    end;
    if d^.isFunction then
    begin
      orderArgs(d^.params, e^.args, e^.callee);
      checkCallArgs(d^.params, d, e^.callee, e^.args, nodeRange(e));
      exit(d^.resultType);
    end;
    if constructedThroughSelf(d, w) <> nil then
    begin
      orderArgs(d^.params, e^.args, e^.callee);
      checkCallArgs(d^.params, d, e^.callee, e^.args, nodeRange(e));
      exit(constructedThroughSelf(d, w));
    end;
    for i := 0 to e^.args.count - 1 do
      check(argValue(argNode(e, i)));
    ms('procedure ');
    maCallee(e^.callee);
    ma(' does not return a value');
    errR(e^.callee.loc, identRange(e^.callee));
    exit;
  end;
  if (d^.kind = dkVar) and (d^.varType <> nil) and (d^.varType^.kind = tyRoutine) and
     (d^.varType^.routineResult <> nil) then
  begin
    d^.isUsed := true; { (called: used) }
    orderArgs(d^.varType^.routineParams, e^.args, e^.callee);
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

{ One more index for the array property of pending pi (for e): while
  indexes are missing, e itself is pending (if more follow); with all of
  them, they are checked like arguments and e is the property access. }
function checkPropertyIndex(e: PNode; prop: PDecl; root: PNode;
                            const soFar: TPtrList): PType;
var
  args: TPtrList;
  i, n: longint;
  id: TIdent;
  ignored: PType;
begin
  listInit(args);
  for i := 0 to soFar.count - 1 do
    listAdd(args, listGet(soFar, i));
  listAdd(args, e^.index);
  n := prop^.propParams.count;
  if args.count < n then
  begin
    if e = indexBase then
    begin
      addPending(e, prop, root, args);
      exit(prop^.propType);
    end;
    e^.idxProp := prop;
    e^.idxRoot := root;
    e^.idxArgs := args;
    id := propertyName(e);
    ms('array property ''');
    maN(id.spelling);
    ma(''' needs ' + intToStr(n) + ' indexes, got ' + intToStr(args.count));
    errR(id.loc, nodeRange(e));
    noteDeclaredHere(prop);
    for i := 0 to args.count - 1 do
      ignored := check(PNode(listGet(args, i)));
    exit(errorType);
  end;
  e^.idxProp := prop;
  e^.idxRoot := root;
  e^.idxArgs := args;
  id := propertyName(e);
  checkCallArgs(prop^.propParams, prop, id, args, nodeRange(e));
  if e = curAssignTarget then
    exit(prop^.propType); { checked by the assignment }
  checkPropertyIndex := checkPropertyRead(prop, e, id);
end;

function visitIndex(e: PNode): PType;
var
  bt, it, idx, owner: PType;
  c: TConstant;
  bd: TOrdinalBounds;
  savedBase, root: PNode;
  pi: PPendingIndex;
  prop: PDecl;
  args: TPtrList;
  id: TIdent;
begin
  visitIndex := errorType;
  savedBase := indexBase;
  indexBase := e^.base;
  bt := check(e^.base);
  indexBase := savedBase;
  if isError(bt) then
    exit;
  if mode = lmObjFPC then
  begin
    { an array property: obj.P[i], P[i], obj[i] (the default property) }
    pi := findPending(e^.base);
    if pi <> nil then
    begin
      prop := pi^.prop;
      root := pi^.root;
      args := pi^.args;
      removePending(pi);
      exit(checkPropertyIndex(e, prop, root, args));
    end;
    if bt^.kind = tyClass then
    begin
      prop := defaultProperty(bt);
      if prop = nil then
      begin
        ms('class ');
        maT(bt);
        ma(' has no default property');
        errR(e^.index^.b, nodeRange(e^.base));
        exit;
      end;
      owner := nil;
      findMember(bt, prop^.ident.name, owner);
      id := prop^.ident;
      id.loc := e^.base^.b;
      checkMemberAccess(prop, owner, id);
      listInit(args);
      exit(checkPropertyIndex(e, prop, e^.base, args));
    end;
  end;
  it := check(e^.index);
  if isError(it) then
    exit;
  if bt^.kind = tyDynArray then
  begin
    { a[0..high(a)] }
    if not isIntegerLike(it) then
    begin
      ms('array index must be an integer');
      errR(e^.index^.b, nodeRange(e^.index));
      exit;
    end;
    exit(bt^.dynElement);
  end;
  if bt^.kind = tyOpenArray then
  begin
    { a[0..high(a)] }
    if not isIntegerLike(it) then
    begin
      ms('array index must be an integer');
      errR(e^.index^.b, nodeRange(e^.index));
      exit;
    end;
    exit(bt^.openElement);
  end;
  if bt^.kind = tyArrayOfConst then
  begin
    { a[0..high(a)] }
    if not isIntegerLike(it) then
    begin
      ms('array index must be an integer');
      errR(e^.index^.b, nodeRange(e^.index));
      exit;
    end;
    exit(bt^.aocElement);
  end;
  if bt^.kind = tyAnsiString then
  begin
    { s[1..length(s)] }
    if not isIntegerLike(it) then
    begin
      ms('string index must be an integer');
      errR(e^.index^.b, nodeRange(e^.index));
      exit;
    end;
    exit(tyCharT);
  end;
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
  if (bt^.kind = tyPointer) and not isISO and not bt^.untyped and (bt^.pointee <> nil) then
  begin
    { Turbo/FPC: p[i] is (p + i)^ }
    if not isIntegerLike(it) then
    begin
      ms('pointer index must be an integer');
      errR(e^.index^.b, nodeRange(e^.index));
      exit;
    end;
    exit(bt^.pointee);
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

{ @obj.M, @TFoo.M, or @M in a method: a method pointer. nil if the operand
  is not a method. }
function checkMethodAddr(e: PNode): PType;
var
  op: PNode;
  r, d, x, pick: PDecl;
  w: PWithContext;
  name: TIdent;
  viaClass, wantsCode: boolean;
  xr, t, res, named, owner: PType;
  why: string;
begin
  checkMethodAddr := nil;
  wantsCode := addrOfWantsCode;
  addrOfWantsCode := false;
  if mode <> lmObjFPC then
    exit;
  op := e^.inner;
  r := nil;
  viaClass := false;
  if op^.kind = nkField then
  begin
    if not isMethodCallee(op) then
      exit;
    named := classNamedBy(op^.base);
    viaClass := named <> nil;
    d := nil;
    owner := nil;
    if named <> nil then
      d := findMember(named, op^.fieldName.name, owner);
    if (d <> nil) and (d^.kind = dkRoutine) and (not d^.isClassMethod or wantsCode) then
    begin
      { @TFoo.M: its code }
      why := '';
      if d^.methodKind in [mtConstructor, mtDestructor] then
        why := 'constructors and destructors have no code address here'
      else if d^.objMethod <> obNone then
        why := 'the methods of TObject have no code address'
      else if d^.isAbstract then
        why := 'abstract methods have no code';
      if why <> '' then
      begin
        ms('''@');
        maN(op^.fieldName.spelling);
        ma(''': ' + why);
        errR(op^.fieldName.loc, identRange(op^.fieldName));
        exit(errorType);
      end;
      checkMemberAccess(d, owner, op^.fieldName);
      op^.fieldDecl := d;
      d^.isUsed := true;
      warnHints(d, op^.fieldName);
      e^.addrCode := d;
      exit(tyUntypedPointerT);
    end;
    d := resolveMember(op);
    if d = nil then
      exit(errorType);
    if d^.kind = dkRoutine then
      r := d;
    name := op^.fieldName;
    if (r <> nil) and (op^.base^.typ <> nil) and (op^.base^.typ^.kind = tyInterface) then
    begin
      ms('''@');
      maN(name.spelling);
      ma(''': interface methods cannot be method pointers');
      errR(name.loc, identRange(name));
      exit(errorType);
    end;
  end
  else if op^.kind = nkNameRef then
  begin
    w := nil;
    d := scopeLookup(cur, op^.ref.name, w);
    if (d <> nil) and (d^.kind = dkRoutine) and (d^.ownerClass <> nil) and
       not d^.isMethodLambda then
    begin
      r := d;
      op^.refWith := w;
      name := op^.ref;
      checkScopeAccess(r, w, name);
    end;
  end;
  if r = nil then
    exit;
  if r^.overloadIndex <> 0 then
  begin
    pick := nil;
    x := r;
    while (addrOfTarget <> nil) and (x <> nil) do
    begin
      xr := nil;
      if x^.isFunction then
        xr := x^.resultType;
      if (pick = nil) and addrOfTarget^.routineOfObject and
         congruent(addrOfTarget^.routineParams, addrOfTarget^.routineResult, x^.params, xr) then
        pick := x;
      x := x^.nextOverload;
    end;
    if pick = nil then
    begin
      ms('''@');
      maN(name.spelling);
      ma(''': ''');
      maN(name.spelling);
      ma(''' is overloaded; assign it to a method pointer of the heading wanted');
      errR(name.loc, identRange(name));
      exit(errorType);
    end;
    r := pick;
  end;
  why := '';
  if r^.methodKind in [mtConstructor, mtDestructor] then
    why := 'constructors and destructors cannot be method pointers'
  else if r^.objMethod <> obNone then
    why := 'the methods of TObject cannot be method pointers';
  if why <> '' then
  begin
    ms('''@');
    maN(name.spelling);
    ma(''': ' + why);
    errR(name.loc, identRange(name));
    exit(errorType);
  end;
  if r^.isStatic then { a routine without Self: a procedural value }
  begin
    if op^.kind = nkField then
      op^.fieldDecl := r
    else
      op^.refDecl := r;
    r^.isUsed := true;
    warnHints(r, name);
    e^.addrCode := r;
    res := nil;
    if r^.isFunction then
      res := r^.resultType;
    t := newRoutineType(r^.params, res, isCDecl(r));
    if addrOfTarget <> nil then
      t^.routineReference := addrOfTarget^.routineReference;
    exit(t);
  end;
  if op^.kind = nkField then
    op^.fieldDecl := r
  else
    op^.refDecl := r;
  r^.isUsed := true;
  warnHints(r, name);
  e^.addrMethod := r;
  res := nil;
  if r^.isFunction then
    res := r^.resultType;
  t := newRoutineType(r^.params, res, isCDecl(r));
  t^.routineOfObject := true;
  checkMethodAddr := t;
end;

{ move p: the object of the owned variable p of this routine, which gives
  it up. }
function visitMove(e: PNode): PType;
var
  n: PNode;
  w: PWithContext;
  d: PDecl;
begin
  visitMove := errorType;
  n := e^.inner;
  w := nil;
  d := scopeLookup(cur, n^.ref.name, w);
  noteLambdaUse(d, w, n^.ref);
  if d = nil then
  begin
    diagUndeclared(n^.ref);
    exit;
  end;
  if (d^.kind <> dkVar) or not (d^.isOwned or (d^.race = rqIsolated)) or (w <> nil) then
  begin
    ms('''move'' needs an owned or isolated variable, and ''');
    maN(n^.ref.spelling);
    ma(''' is not one');
    errR(n^.ref.loc, identRange(n^.ref));
    noteDeclaredHere(d);
    exit;
  end;
  n^.refDecl := d;
  n^.typ := d^.varType;
  d^.isUsed := true;
  if not d^.isOwned then { isolated: its data, the variable left empty }
    exit(d^.varType);
  if d^.owner <> curRoutine then
  begin
    ms('owned variable ''');
    maN(n^.ref.spelling);
    ma(''' can only be moved by the routine it belongs to');
    errR(n^.ref.loc, identRange(n^.ref));
    noteDeclaredHere(d);
    exit;
  end;
  ownedUse(d, identRange(n^.ref));
  ownedMove(d, e^.b);
  visitMove := d^.varType;
end;

function visitAddrOf(e: PNode): PType;
var
  r, x, pick: PDecl;
  t, xr: PType;
  op: PNode;
  why: string;
begin
  completeLambda(e, addrOfTarget);
  if e^.addrLambda <> nil then
    if e^.addrLambda^.lambdaBad then
      exit(errorType);
  t := checkMethodAddr(e);
  if t <> nil then
    exit(t);
  visitAddrOf := errorType;
  op := e^.inner;
  { @routine (Turbo/FPC): a procedural value }
  if op^.kind = nkNameRef then
  begin
    r := lookup(op^.ref.name);
    if (r <> nil) and (r^.kind = dkRoutine) then
    begin
      if r^.overloadIndex <> 0 then { objfpc: the overload the target wants }
      begin
        pick := nil;
        x := r;
        while (addrOfTarget <> nil) and (x <> nil) do
        begin
          xr := nil;
          if x^.isFunction then
            xr := x^.resultType;
          if (pick = nil) and (isCDecl(x) = addrOfTarget^.routineCDecl) and
             congruent(addrOfTarget^.routineParams,
                       addrOfTarget^.routineResult, x^.params, xr) then
            pick := x;
          x := x^.nextOverload;
        end;
        if pick = nil then
        begin
          ms('''@');
          maN(op^.ref.spelling);
          ma(''': ''');
          maN(op^.ref.spelling);
          ma(''' is overloaded; assign it to a procedural variable of the ' +
             'heading wanted');
          errR(op^.b, nodeRange(op));
          exit;
        end;
        r := pick;
      end;
      op^.refDecl := r;
      if r^.methodKind <> mtNone then
      begin
        ms('''@');
        maN(op^.ref.spelling);
        ma(''': methods cannot be procedural values');
        errR(op^.b, nodeRange(op));
        exit;
      end;
      why := notProceduralValue(r, false);
      if why <> '' then
      begin
        ms('''@');
        maN(op^.ref.spelling);
        ma(''': ' + why);
        errR(op^.b, nodeRange(op));
        exit;
      end;
      { a closure wanted: a lambda (made one by completeLambda) or a
        routine declared at program or unit level (no frame to keep) }
      if r^.isFunction then
        t := newRoutineType(r^.params, r^.resultType, isCDecl(r))
      else
        t := newRoutineType(r^.params, nil, isCDecl(r));
      if addrOfTarget <> nil then
        t^.routineReference := addrOfTarget^.routineReference;
      { a method pointer: a lambda made one by completeLambda }
      t^.routineOfObject := r^.isMethodLambda;
      op^.typ := t;
      exit(t);
    end;
  end;
  t := checkAllowingUntyped(op); { (@x of an untyped x too) }
  if isError(t) then
    exit;
  if isBitPackedComponent(op) then
  begin
    diagBitFieldAddress(op);
    exit;
  end;
  if not isVariableReference(op) then
  begin
    ms('''@'' requires a variable');
    errR(op^.b, nodeRange(op));
    exit;
  end;
  if ownedVarOf(op) <> nil then
  begin
    ms('cannot take the address of owned variable ''');
    maN(ownedVarOf(op)^.ident.spelling);
    ma('''');
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
  if (t^.kind = tyFile) and (t^.isText or (t^.fileElement <> nil)) then
    exit(t^.fileElement); { the buffer variable f^ }
  if t^.kind = tyFile then
  begin
    ms('an untyped file has no buffer variable');
    errR(e^.inner^.b, nodeRange(e^.inner));
    exit;
  end;
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
  noArgs: TPtrList;
  id: TIdent;
  ref: PNode;
begin
  visitField := errorType;
  d := resolveMember(e);
  if d = nil then
    exit;
  if diagOwnedFree(e) then
    exit;
  if d^.kind = dkConst then { TFoo.C, obj.C }
  begin
    warnHints(d, e^.fieldName);
    id := d^.ident;
    id.loc := e^.fieldName.loc;
    ref := nameRefNode(id);
    ref^.refDecl := d;
    if d^.constType <> nil then
      ref^.typ := d^.constType
    else
      ref^.typ := errorType;
    d^.isUsed := true;
    e^.constRef := ref;
    exit(ref^.typ);
  end;
  if d^.kind = dkType then
  begin
    ms('''');
    maN(e^.fieldName.spelling);
    ma(''' is a type, not a value');
    errR(e^.fieldName.loc, identRange(e^.fieldName));
    noteDeclaredHere(d);
    exit;
  end;
  if d^.kind <> dkRoutine then { (a method: once its overload is known) }
    warnHints(d, e^.fieldName);
  if d^.kind = dkField then
  begin
    if d^.fieldType <> nil then
      visitField := d^.fieldType;
    exit;
  end;
  if d^.kind = dkProperty then
  begin
    if not checkArrayPropertyUse(d, e, e^.fieldName) then
      exit;
    exit(checkPropertyRead(d, e, e^.fieldName));
  end;
  if d^.kind = dkVar then { a class variable }
  begin
    if d^.varType <> nil then
      visitField := d^.varType;
    exit;
  end;
  { a method used as a value: a call without arguments }
  if d^.memberKey <> nil then
  begin
    ms('cannot infer the type parameters of generic method ''');
    maN(e^.fieldName.spelling);
    ma(''' without arguments; give them: specialize ');
    maN(e^.fieldName.spelling);
    ma('<...>');
    errR(e^.fieldName.loc, identRange(e^.fieldName));
    noteDeclaredHere(d);
    exit;
  end;
  listInit(noArgs);
  d := resolveOverload(d, noArgs, e^.fieldName, identRange(e^.fieldName));
  if d = nil then
    exit;
  e^.fieldDecl := d;
  warnHints(d, e^.fieldName);
  if not acceptsArgCount(d^.params, 0) then
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
  i: longint;
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
        end
        else if not isISO then
        begin
          { beyond int64: qword ($FFFFFFFFFFFFFFFF and the like: the bits
            of an int64, as in FPC) }
          if e^.intBased then
            exit(tyInt64T);
          exit(tyQWordT);
        end;
        ms('integer literal exceeds maxint (2147483647)');
        errR(e^.b, nodeRange(e));
        visitExpr := tyIntegerT;
      end;
    nkRealLit: visitExpr := tyRealT;
    nkStringLit:
      if e^.strWide <> 0 then
        visitExpr := tyWideCharT
      else if e^.strValue^.len = 1 then
        visitExpr := tyCharT
      else
        visitExpr := stringLiteralType(e^.strValue^.len);
    nkNameRef: visitExpr := visitNameRef(e);
    nkParen: visitExpr := check(e^.inner);
    nkMove: visitExpr := visitMove(e);
    nkUnary: visitExpr := visitUnary(e);
    nkBinary: visitExpr := visitBinary(e);
    nkCall: visitExpr := visitCall(e);
    nkTuple: visitExpr := checkTuple(e, nil);
    nkCondExpr: visitExpr := checkCond(e, nil);
    nkAwait: visitExpr := checkAwait(e, true);
    nkSpawn: visitExpr := visitSpawn(e);
    nkTemplate:
      begin
        { an ansistring; each value is one write can write, with the field
          width and precision it takes there }
        for i := 0 to e^.tmplParts.count - 1 do
          if i mod 2 = 0 then
            check(PNode(listGet(e^.tmplParts, i)))
          else
            checkWritten(PNode(listGet(e^.tmplParts, i)), true);
        visitExpr := ansiStringType;
      end;
    nkNamedArg:
      begin
        { where the parameters have no names to give (a standard routine,
          a typecast) }
        check(e^.naValue);
        ms('a named argument is only allowed in a call to a declared routine');
        errR(e^.naName.loc, identRange(e^.naName));
        visitExpr := errorType;
      end;
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
