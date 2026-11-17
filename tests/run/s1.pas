{ S1 (Turbo/FPC): typecasts, memory built-ins, low/high, const
  parameters, exit(value), procedural types, structured typed constants,
  nested includes, external C functions. The expected output was produced
  by Free Pascal 3.2.2 (fpc -Mobjfpc -Sh- -Cr -Co -FiInputs/s1).
  RUN: %p3 --mode=tp -I %S/Inputs/s1 %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/s1.expected %t.out
  RUN: %p3 --mode=tp -O2 -I %S/Inputs/s1 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/s1.expected %t.O2.out
}
program s1;

{$I s1defs.inc}

type
  TCells = array[0..$FFFFFF] of longint;
  PCells = ^TCells;
  TVec = record
    data: PCells;
    count: longint;
  end;
  TPoint = record
    x, y: longint;
  end;
  TProc = procedure(const s: string; var n: longint);

function c_strcmp(a, b: pchar): longint; cdecl; external 'c' name 'strcmp';
function c_labs(x: int64): int64; cdecl; external 'c' name 'labs';

function addFn(a, b: longint): longint;
begin
  addFn := a + b;
end;

function subFn(a, b: longint): longint;
begin
  subFn := a - b;
end;

function mulFn(a, b: longint): longint;
begin
  mulFn := a * b;
end;

procedure countChars(const s: string; var n: longint);
begin
  n := n + length(s);
end;

{ const record and string parameters are read-only and not copied }
function dist2(const p, q: TPoint): longint;
begin
  dist2 := sqr(p.x - q.x) + sqr(p.y - q.y);
end;

function firstWord(const s: string): string;
var
  i: longint;
begin
  for i := 1 to length(s) do
    if s[i] = ' ' then
      exit(copy(s, 1, i - 1));
  firstWord := s;
end;

function classify(x: longint): string;
begin
  if x < 0 then
    exit('negative');
  if x = 0 then
    exit('zero');
  classify := 'positive';
end;

{ a typed constant inside a routine keeps its value between calls }
function nextId: longint;
const
  counter: longint = 100;
begin
  counter := counter + 1;
  nextId := counter;
end;

procedure push(var v: TVec; x: longint);
begin
  reallocmem(v.data, (v.count + 1) * sizeof(longint));
  {$R-}
  v.data^[v.count] := x;
  {$R+}
  v.count := v.count + 1;
end;

var
  fns: array[TOp] of TBinFn;
  op: TOp;
  v: TVec;
  i, n: longint;
  total: int64;
  p, q: TPoint;
  proc: TProc;
  buf: array[0..9] of char;
  s: string;
  small: string[10];
  e: TEntry;
begin
  { typecasts }
  i := -1;
  writeln(byte(i), ' ', shortint(200), ' ', word(70000), ' ', cardinal(i), ' ',
          longint(cardinal(4294967295)), ' ', ord(TOp(2)), ' ', char(97));
  { low / high }
  writeln(low(char) = #0, ' ', ord(high(char)), ' ', low(boolean), ' ',
          high(TOp) = opMul, ' ', low(buf), ' ', high(buf), ' ', high(small),
          ' ', low(shortint), ' ', high(word));
  { structured typed constants from a nested include }
  for op := low(TOp) to high(TOp) do
    with entries[op] do
      writeln(name, ' ', ord(op), ' ', code[1] + code[2]);
  { procedural values in an array, called through the element }
  fns[opAdd] := @addFn;
  fns[opSub] := @subFn;
  fns[opMul] := @mulFn;
  for op := low(TOp) to high(TOp) do
    write(fns[op](7, 3), ' ');
  writeln(assigned(fns[opAdd]));
  proc := @countChars;
  n := 0;
  proc('abc', n);
  proc('de', n);
  writeln('chars ', n);
  { memory: a growing vector, fillchar and overlapping move }
  v.data := nil;
  v.count := 0;
  for i := 1 to 1000 do
    push(v, i * i);
  total := 0;
  {$R-}
  for i := 0 to v.count - 1 do
    total := total + v.data^[i];
  {$R+}
  writeln(v.count, ' ', total);
  reallocmem(v.data, 0);
  writeln(v.data = nil);
  fillchar(buf, sizeof(buf), ord('-'));
  for i := 0 to 4 do
    buf[i] := chr(ord('a') + i);
  move(buf[0], buf[2], 5);
  for i := 0 to 9 do
    write(buf[i]);
  writeln;
  { const parameters and exit(value) }
  p.x := 1; p.y := 2;
  q.x := 4; q.y := 6;
  writeln(dist2(p, q), ' ', firstWord('hello brave world'), ' ', firstWord('single'));
  writeln(classify(-5), ' ', classify(0), ' ', classify(9));
  writeln(nextId, ' ', nextId, ' ', nextId);
  { external C functions }
  s := 'apple' + #0;
  writeln(c_strcmp(@s[1], 'apple') = 0, ' ', c_strcmp('abc', 'abd') < 0, ' ',
          c_labs(-5000000000));
  e := entries[opSub];
  writeln(e.name, ' ', sizeof(e.code));
end.
