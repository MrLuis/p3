{ Records returned by functions (--mode=objfpc): a record larger than two
  registers is written through a hidden result pointer, a small one is
  returned as a value. Checks both sizes, a record with a short string and
  an array, a result used as an argument and with a field selector,
  `x := F(x)`, nested calls, recursion, methods of classes and advanced
  records, operators, a property getter, an exception raised
  after the result was partly written, and a loop reusing the result.
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/record-results.expected %t.out
}
program recordresults;
{$modeswitch advancedrecords}
uses sysutils;

type
  TSmall = record a, b: longint; end;                 { 8 bytes: a value }
  TTwo = record a, b: int64; end;                     { 16 bytes: a value }
  TBig = record a, b, c: int64; end;                  { 24: by pointer }
  TText = record n: longint; s: string[40]; end;
  TMixed = record
    id: longint;
    name: string[20];
    v: array[0..5] of longint;
    x: double;
  end;
  TVec = record
    x, y, z: double;
    class operator + (const p, q: TVec): TVec;
    function Scaled(k: double): TVec;
  end;

  TMaker = class
    base: longint;
    function Make(n: longint): TMixed;
    function Big(n: longint): TBig;
  end;

  EFail = class(Exception);

class operator TVec.+ (const p, q: TVec): TVec;
begin
  Result.x := p.x + q.x; Result.y := p.y + q.y; Result.z := p.z + q.z;
end;

function TVec.Scaled(k: double): TVec;
begin
  Result.x := x * k; Result.y := y * k; Result.z := z * k;
end;

function MakeSmall(n: longint): TSmall;
begin
  Result.a := n; Result.b := n * 2;
end;

function MakeTwo(n: longint): TTwo;
begin
  Result.a := n; Result.b := n * 3;
end;

function MakeBig(n: longint): TBig;
begin
  Result.a := n; Result.b := n + 1; Result.c := n + 2;
end;

function MakeText(n: longint): TText;
begin
  Result.n := n;
  Result.s := 'item ' + IntToStr(n);
end;

function MakeMixed(n: longint): TMixed;
var i: longint;
begin
  FillChar(Result, SizeOf(Result), 0);
  Result.id := n;
  Result.name := 'mixed';
  for i := 0 to 5 do Result.v[i] := n * i;
  Result.x := n / 4;
end;

function Sum(const m: TMixed): longint;
var i: longint;
begin
  Result := m.id;
  for i := 0 to 5 do Result := Result + m.v[i];
end;

function Twice(b: TBig): TBig;
begin
  Result.a := b.a * 2; Result.b := b.b * 2; Result.c := b.c * 2;
end;

{ The result is used while it is being assigned. }
function Rotate(b: TBig): TBig;
begin
  Result.a := b.b; Result.b := b.c; Result.c := b.a;
end;

function Fact(n: longint): TBig;
var r: TBig;
begin
  if n <= 1 then
  begin
    Result.a := 1; Result.b := n; Result.c := 0;
  end
  else
  begin
    r := Fact(n - 1);
    Result.a := r.a * n; Result.b := n; Result.c := r.c + 1;
  end;
end;

{ Raises after writing part of the result. }
function Failing(n: longint): TBig;
begin
  Result.a := n;
  if n > 2 then raise EFail.Create('too big');
  Result.b := n; Result.c := n;
end;

function TMaker.Make(n: longint): TMixed;
begin
  Result := MakeMixed(n + base);
end;

function TMaker.Big(n: longint): TBig;
begin
  Result := MakeBig(n + base);
end;

var
  sm: TSmall; tw: TTwo; bg: TBig; tx: TText; mx: TMixed;
  v, w: TVec; mk: TMaker; i, total: longint; caught: boolean;
begin
  sm := MakeSmall(3); writeln(sm.a, ' ', sm.b);
  tw := MakeTwo(4); writeln(tw.a, ' ', tw.b);
  bg := MakeBig(5); writeln(bg.a, ' ', bg.b, ' ', bg.c);
  tx := MakeText(7); writeln(tx.n, ' ', tx.s);
  mx := MakeMixed(2);
  writeln(mx.id, ' ', mx.name, ' ', mx.v[5], ' ', mx.x:0:2);
  { as an argument, and with a field selector }
  writeln(Sum(MakeMixed(3)), ' ', MakeBig(10).c, ' ', MakeText(8).s);
  { nested }
  bg := Twice(Twice(MakeBig(1)));
  writeln(bg.a, ' ', bg.b, ' ', bg.c);
  { the argument is the destination }
  bg := MakeBig(1);
  bg := Rotate(bg);
  writeln(bg.a, ' ', bg.b, ' ', bg.c);
  bg := Rotate(Rotate(bg));
  writeln(bg.a, ' ', bg.b, ' ', bg.c);
  { with, on a variable the result was stored in }
  bg := MakeBig(20);
  with bg do writeln(a + b + c);
  { recursion }
  bg := Fact(6);
  writeln(bg.a, ' ', bg.b, ' ', bg.c);
  { methods, operators }
  mk := TMaker.Create;
  mk.base := 100;
  mx := mk.Make(1);
  writeln(mx.id, ' ', mx.v[2]);
  bg := mk.Big(5);
  writeln(bg.a, ' ', bg.c);
  mk.Free;
  v.x := 1; v.y := 2; v.z := 3;
  w := v.Scaled(2.0) + v;
  writeln(w.x:0:1, ' ', w.y:0:1, ' ', w.z:0:1);
  w := (v + v).Scaled(0.5);
  writeln(w.x:0:1, ' ', w.y:0:1, ' ', w.z:0:1);
  { a loop reusing the temporary }
  total := 0;
  for i := 1 to 1000 do
  begin
    mx := MakeMixed(i);
    total := total + Sum(MakeMixed(i)) + mx.id;
  end;
  writeln(total);
  { an exception after a partly written result }
  bg := MakeBig(1);
  caught := false;
  try
    bg := Failing(5);
  except
    on E: EFail do caught := true;
  end;
  writeln(caught, ' ', bg.a, ' ', bg.b, ' ', bg.c);
  bg := Failing(2);
  writeln(bg.a, ' ', bg.b, ' ', bg.c);
end.
