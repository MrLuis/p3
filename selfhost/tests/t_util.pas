program t_util;
{ pcutil test: arena, lists, interned names, maps, buffers, intToStr. }

uses pcsys, pcutil;

procedure checkG(x: double; const want: string);
begin
  if formatG(x, 6) = want then
    sysWrite(sysOut, 'ok   %g ' + want + #10)
  else
    sysWrite(sysOut, 'FAIL %g ' + want + ': got ' + formatG(x, 6) + #10);
end;

procedure check(ok: boolean; const what: string);
begin
  if ok then
    sysWrite(sysOut, 'ok   ' + what + #10)
  else
    sysWrite(sysOut, 'FAIL ' + what + #10);
end;

var
  l: TPtrList;
  m: TIntMap;
  b: TBuf;
  n1, n2, n3: PName;
  i, bad: longint;
  p: PCharArray;
  names: array[0..19999] of PName;
  s: string;
  total: int64;
begin
  sysInit;

  { arena: zeroed, distinct, 8-byte granularity }
  p := PCharArray(arenaAlloc(100));
  bad := 0;
  for i := 0 to 99 do
    if p^[i] <> #0 then
      bad := bad + 1;
  check(bad = 0, 'arena memory is zeroed');
  check(arenaAlloc(1) <> arenaAlloc(1), 'arena blocks are distinct');
  check(arenaAlloc(2000000) <> nil, 'large arena block');

  { lists }
  listInit(l);
  for i := 0 to 9999 do
    listAdd(l, arenaAlloc(8));
  check(l.count = 10000, 'list grows to 10000');
  listSet(l, 5, nil);
  check(listGet(l, 5) = nil, 'listSet / listGet');
  check(listPop(l) <> nil, 'listPop');
  check(l.count = 9999, 'listPop shrinks');
  listFree(l);
  check(l.count = 0, 'listFree');

  { names }
  n1 := intern('writeln');
  n2 := intern('writeln');
  n3 := intern('WriteLn');
  check(n1 = n2, 'equal strings intern to one name');
  check(n1 <> n3, 'names are case-sensitive (callers lower-case identifiers)');
  check(nameStr(n1) = 'writeln', 'nameStr');
  check(nameEquals(n3, 'WriteLn') and not nameEquals(n3, 'WriteL'), 'nameEquals');
  check(intern('')^.len = 0, 'the empty name');
  check(n1^.id + 1 = n3^.id, 'ids are given in order');
  for i := 0 to 19999 do
    names[i] := intern('name' + intToStr(i));
  bad := 0;
  for i := 0 to 19999 do
    if (intern('name' + intToStr(i)) <> names[i]) or
       (nameStr(names[i]) <> 'name' + intToStr(i)) then
      bad := bad + 1;
  check(bad = 0, '20000 names survive pool growth');
  sysWrite(sysOut, 'names interned: ' + intToStr(nameCount) + #10);
  s := 'pcc';
  sysWrite(sysOut, 'hash(pcc) = ' + intToStr(hashChars(PCharArray(@s[1]), 0, 3)) + #10);

  { maps }
  mapInit(m);
  check(mapGet(m, 42) = nil, 'empty map');
  for i := 1 to 10000 do
    mapPut(m, int64(i) * 1000003, names[i mod 20000]);
  mapPut(m, 1000003, names[0]);
  bad := 0;
  for i := 2 to 10000 do
    if mapGet(m, int64(i) * 1000003) <> pointer(names[i mod 20000]) then
      bad := bad + 1;
  check((bad = 0) and (mapGet(m, 1000003) = pointer(names[0])), 'map get / overwrite');
  check(m.count = 10000, 'map count');
  check(not mapHas(m, 7) and mapHas(m, 2000006), 'mapHas');
  mapPut(m, -5, n1);
  check(mapGet(m, -5) = pointer(n1), 'negative keys');

  { buffers }
  bufInit(b);
  total := 0;
  for i := 1 to 1000 do
  begin
    bufAdd(b, 'abc');
    bufAddChar(b, ';');
    total := total + 4;
  end;
  check(b.len = total, 'buffer of 4000 chars');
  check((bufCharAt(b, 3) = ';') and (bufCharAt(b, 3999) = ';'), 'bufCharAt');
  bufClear(b);
  bufAddName(b, n3);
  bufAddChar(b, ' ');
  bufAddInt(b, -42);
  bufAddChar(b, #10);
  bufWrite(b, sysOut);
  bufFree(b);

  { text }
  sysWrite(sysOut, intToStr(0) + ' ' + intToStr(7) + ' ' + intToStr(-7) + ' ' +
                   intToStr(high(int64)) + ' ' + intToStr(low(int64)) + #10);
  check(lowerStr('MixedCase_09') = 'mixedcase_09', 'lowerStr');

  { formatG = C printf %g (expected values from Python's '%g') }
  checkG(0.1, '0.1');
  checkG(1e10, '1e+10');
  checkG(123456, '123456');
  checkG(1234567, '1.23457e+06');
  checkG(0.0001, '0.0001');
  checkG(0.00001234, '1.234e-05');
  checkG(-2.5, '-2.5');
  checkG(1e-310, '1e-310');
  checkG(1.7976931348623157e308, '1.79769e+308');
  checkG(5e-324, '4.94066e-324');
  checkG(100, '100');
  checkG(0.5, '0.5');
  checkG(9.9999996, '10');
  checkG(2.5e-5, '2.5e-05');
  check(formatG(1.0, 17) = '1', 'formatG precision 17');
  check(uintToStr(-1) = '18446744073709551615', 'uintToStr(2^64-1)');
  check(uintToStr(low(int64)) = '9223372036854775808', 'uintToStr(2^63)');
  check(uintToStr(42) = '42', 'uintToStr small');
  bufInit(b);
  s := 'a\b' + #9 + '"' + #10 + #0 + #200;
  bufAddEscaped(b, PCharArray(@s[1]), 0, length(s));
  bufAddChar(b, #10);
  bufWrite(b, sysOut);
  bufFree(b);
  sysFlush;
end.
