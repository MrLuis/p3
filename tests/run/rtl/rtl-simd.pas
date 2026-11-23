{ The unit library's 16-byte scans against byte-by-byte references, on
  random texts whose interesting bytes fall at every offset of the blocks:
  PosEx and StringReplace (substring search), CompareStr, CompareText,
  UpperCase, LowerCase, TStringList.Text (line splitting). objfpc and tp
  (the units compile in both), -O0 and -O2.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | FileCheck %s
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | FileCheck %s
  RUN: %p3 --mode=tp -O2 %s -o %t.tp
  CHECK: search: 0 wrong
  CHECK: compare: 0 wrong
  RUN: %t.tp | FileCheck --check-prefix=TP %s
  CHECK: case: 0 wrong
  CHECK: lines: 0 wrong
  TP: case: 0 wrong
}
program rtlsimd;
{$H+}
uses sysutils, strutils
  {$IFDEF P3_OBJFPC}, classes{$ENDIF};

var
  Seed: QWord;

function Next(N: longint): longint;
begin
  {$Q-}{$R-}
  Seed := Seed * 6364136223846793005 + 1442695040888963407;
  Next := longint((Seed shr 33) mod QWord(N));
  {$Q+}{$R+}
end;

{ a text of n bytes from a small alphabet (so that needles match) }
function RandomText(n: longint; const alphabet: string): string;
var
  r: string;
  i: longint;
begin
  r := '';
  for i := 1 to n do
    r := r + alphabet[1 + Next(length(alphabet))];
  RandomText := r;
end;

function RefPos(const sub, s: string; start: longint): longint;
var
  i, j: longint;
  ok: boolean;
begin
  RefPos := 0;
  for i := start to length(s) - length(sub) + 1 do
  begin
    ok := true;
    for j := 1 to length(sub) do
      if s[i + j - 1] <> sub[j] then
        ok := false;
    if ok then
    begin
      RefPos := i;
      exit;
    end;
  end;
end;

function RefReplace(const s, sub, into: string): string;
var
  r: string;
  i, p: longint;
begin
  r := '';
  i := 1;
  p := RefPos(sub, s, i);
  while p > 0 do
  begin
    r := r + copy(s, i, p - i) + into;
    i := p + length(sub);
    p := RefPos(sub, s, i);
  end;
  RefReplace := r + copy(s, i, length(s));
end;

function Sign(x: longint): longint;
begin
  if x < 0 then Sign := -1 else if x > 0 then Sign := 1 else Sign := 0;
end;

function RefCompare(const a, b: string; ignoreCase: boolean): longint;
var
  i, n: longint;
  ca, cb: char;
begin
  n := length(a);
  if length(b) < n then
    n := length(b);
  for i := 1 to n do
  begin
    ca := a[i];
    cb := b[i];
    if ignoreCase then
    begin
      if (ca >= 'A') and (ca <= 'Z') then ca := chr(ord(ca) + 32);
      if (cb >= 'A') and (cb <= 'Z') then cb := chr(ord(cb) + 32);
    end;
    if ca <> cb then
    begin
      RefCompare := ord(ca) - ord(cb);
      exit;
    end;
  end;
  RefCompare := length(a) - length(b);
end;

function RefCase(const s: string; upper: boolean): string;
var
  r: string;
  i: longint;
begin
  r := s;
  for i := 1 to length(r) do
    if upper and (r[i] >= 'a') and (r[i] <= 'z') then
      r[i] := chr(ord(r[i]) - 32)
    else if not upper and (r[i] >= 'A') and (r[i] <= 'Z') then
      r[i] := chr(ord(r[i]) + 32);
  RefCase := r;
end;

{$IFDEF P3_OBJFPC}
{ the lines of s, as TStrings.SetTextStr splits them (LF, CR or CR LF;
  no line after a last break), joined with '|' }
function RefLines(const s: string): string;
var
  r, line: string;
  i: longint;
begin
  r := '';
  line := '';
  i := 1;
  while i <= length(s) do
  begin
    if (s[i] = #13) or (s[i] = #10) then
    begin
      r := r + line + '|';
      line := '';
      if (s[i] = #13) and (i < length(s)) and (s[i + 1] = #10) then
        i := i + 1;
    end
    else
      line := line + s[i];
    i := i + 1;
  end;
  if line <> '' then
    r := r + line + '|';
  RefLines := r;
end;

{$ENDIF}

var
  i, n, start, bad: longint;
  {$IFDEF P3_OBJFPC}
  list: TStringList;
  joined: string;
  {$ENDIF}
  s, sub, t, u: string;
begin
  Seed := 11;
  bad := 0;
  for i := 1 to 4000 do
  begin
    n := Next(120);
    s := RandomText(n, 'abcab' + chr(200));
    sub := RandomText(1 + Next(6), 'abc');
    start := 1 + Next(n + 1);
    if PosEx(sub, s, start) <> RefPos(sub, s, start) then
      bad := bad + 1;
    if StringReplace(s, sub, '#', [rfReplaceAll]) <> RefReplace(s, sub, '#') then
      bad := bad + 1;
    if Pos(sub, s) <> RefPos(sub, s, 1) then
      bad := bad + 1;
  end;
  writeln('search: ', bad, ' wrong');

  bad := 0;
  for i := 1 to 4000 do
  begin
    n := Next(90);
    s := RandomText(n, 'aAbBz');
    t := s;
    if (n > 0) and (Next(4) > 0) then
      t[1 + Next(n)] := RandomText(1, 'aAbBz09')[1];
    if Next(5) = 0 then
      t := t + 'x';
    if Sign(CompareStr(s, t)) <> Sign(RefCompare(s, t, false)) then
      bad := bad + 1;
    if Sign(CompareText(s, t)) <> Sign(RefCompare(s, t, true)) then
      bad := bad + 1;
    if SameText(s, t) <> (RefCompare(s, t, true) = 0) then
      bad := bad + 1;
  end;
  writeln('compare: ', bad, ' wrong');

  bad := 0;
  for i := 1 to 4000 do
  begin
    n := Next(100);
    s := RandomText(n, 'aZ09 _@[`{' + chr(225));
    t := copy(s, 1, n); { (its own copy: s must stay as it is) }
    u := UpperCase(s);
    if u <> RefCase(t, true) then
      bad := bad + 1;
    if LowerCase(s) <> RefCase(t, false) then
      bad := bad + 1;
    if LowerCase(u) <> RefCase(t, false) then
      bad := bad + 1;
    if s <> t then
      bad := bad + 1;
  end;
  { a literal (shared, read-only): copied before it is changed }
  u := UpperCase('a literal of more than thirty-two bytes, lower case');
  if u <> 'A LITERAL OF MORE THAN THIRTY-TWO BYTES, LOWER CASE' then
    bad := bad + 1;
  u := LowerCase('A LITERAL OF MORE THAN THIRTY-TWO BYTES, UPPER CASE');
  if u <> 'a literal of more than thirty-two bytes, upper case' then
    bad := bad + 1;
  writeln('case: ', bad, ' wrong');

  {$IFDEF P3_OBJFPC}
  bad := 0;
  list := TStringList.Create;
  for i := 1 to 3000 do
  begin
    s := RandomText(Next(100), 'abcd' + #10 + #13);
    list.Text := s;
    joined := '';
    for n := 0 to list.Count - 1 do
      joined := joined + list[n] + '|';
    if joined <> RefLines(s) then
      bad := bad + 1;
  end;
  list.Free;
  writeln('lines: ', bad, ' wrong');
  {$ENDIF}
end.
