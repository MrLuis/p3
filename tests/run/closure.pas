{ Closures (objfpc): a `reference to` procedural type holds a lambda that
  may outlive the routine it is made in. The variables of that routine it
  uses are shared with it, in a frame on the heap that is freed when the
  routine has returned and no closure holds it any more. Values of the
  type are counted like ansistrings: in variables, fields, arrays, records,
  parameters and results.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/closure.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/closure.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/closure.expected
}
{$H+}
program closure;

type
  TIntFn = reference to function(x: Integer): Integer;
  TMaker = reference to function(x: Integer): TIntFn;
  TCounter = reference to function: Integer;
  TStrFn = reference to function: string;
  TProc = reference to procedure;
  TFnList = array of TIntFn;
  TProcs = array of TProc;
  TPlainVisit = procedure(const s: string);
  TPlain = procedure;
  generic TMap<T> = reference to function(const x: T): T;

  TAcc = class
    FTotal: Integer;
    FName: string;
    function Adder: TProc;
    function Show: TStrFn;
  end;

  THolder = record
    F: TIntFn;
    Tag: string;
  end;

  TBox = class
    Fn: TIntFn;
  end;

  generic TCell<T> = class
  type
    TGet = reference to function: T;
  var
    FValue: T;
    function Getter: TGet;
  end;

  TR = record
    a: Integer;
  end;

  EFail = class
    Msg: string;
    constructor Create(const AMsg: string);
  end;

constructor EFail.Create(const AMsg: string);
begin
  Msg := AMsg;
end;

function IntToStr(n: Integer): string;
begin
  if n < 0 then
    exit('-' + IntToStr(-n));
  if n >= 10 then
    exit(IntToStr(n div 10) + Chr(Ord('0') + n mod 10));
  IntToStr := Chr(Ord('0') + n);
end;

function TAcc.Adder: TProc;
begin
  Result := procedure => FTotal := FTotal + 1;
end;

function TAcc.Show: TStrFn;
begin
  Result := function => FName + '=' + IntToStr(FTotal);
end;

function TCell.Getter: TGet;
begin
  Result := function => FValue;
end;

function MakeAdder(n: Integer): TIntFn;
begin
  Result := function(x) => x + n;
end;

{ two closures sharing one variable }
procedure MakeCounter(start: Integer; out step: TProc; out get: TCounter);
var
  count: Integer;
begin
  count := start;
  step := procedure => count := count + 1;
  get := function: Integer => count;
end;

function Twice(f: TIntFn; x: Integer): Integer;
begin
  Twice := f(f(x));
end;

function Square(x: Integer): Integer;
begin
  Square := x * x;
end;

function Greeter(const name: string): TProc;
var
  greeting: string;
begin
  greeting := 'Hello, ' + name;
  Greeter := procedure => Writeln(greeting, '!');
end;

{ a closure making closures: uses the outer routine's variable }
function Curry(base: Integer): TMaker;
begin
  Result := function(x) => function(y: Integer): Integer => base + x * y;
end;

{ nested routines: the closure uses its grandparent's variables }
function Outer(k: Integer): TIntFn;
var
  scale: Integer;

  function Inner: TIntFn;
  begin
    Result := function(x) => x * scale + k;
  end;

begin
  scale := 3;
  Result := Inner();
  scale := 4; { seen by the closure: it shares the variable }
end;

{ a string captured, changed after the closure is made }
function Tagged(const prefix: string): TStrFn;
var
  s: string;
begin
  s := prefix;
  Result := function => s + '!';
  s := s + '?';
end;

{ an exception leaving the routine: its frame is released }
function Fails(n: Integer): TIntFn;
var
  msg: string;
begin
  msg := 'n=' + IntToStr(n);
  Result := function(x) => x + Length(msg);
  if n < 0 then
    raise EFail.Create(msg);
end;

procedure Each(const names: array of string; Visit: TPlainVisit);
var
  i: Integer;
begin
  for i := 0 to High(names) do
    Visit(names[i]);
end;

{ closures made in a lambda, from a copy of its parameter }
function Collect(const names: array of string): TProcs;
var
  list: TProcs;
begin
  list := nil;
  Each(names, procedure(name) =>
    begin
      var s := name;
      SetLength(list, Length(list) + 1);
      list[High(list)] := procedure => Writeln('hi ', s);
    end);
  Collect := list;
end;

function Factorial(n: Integer): Integer;
var
  fact: TIntFn;
begin
  fact := function(k) => if k <= 1 then 1 else k * fact(k - 1);
  Factorial := fact(n);
  fact := nil; { the closure holds the frame that holds it: break the cycle }
end;

procedure CallPlain(p: TPlain);
begin
  p();
end;

{ a lambda in a with statement (also an ordinary lambda) }
procedure WithLambda;
var
  r: TR;
begin
  r.a := 5;
  with r do
    CallPlain(procedure => Writeln('with ', r.a));
  CallPlain(procedure => with r do Writeln('with ', a + 1));
end;

var
  add5, sq, f: TIntFn;
  c1, c2: TCounter;
  p: TProc;
  fs: TFnList;
  ps: TProcs;
  i: Integer;
  acc: TAcc;
  sh: TStrFn;
  h, h2: THolder;
  box: TBox;
  cell: specialize TCell<string>;
  g: TStrFn;
  m: TMaker;
  up: specialize TMap<string>;
  total: Integer;
begin
  add5 := MakeAdder(5);
  Writeln(add5(10));
  Writeln(Twice(add5, 1));
  Writeln(Twice(MakeAdder(100), 1));
  sq := @Square;
  Writeln(sq(7));
  MakeCounter(10, p, c1);
  p();
  p();
  Writeln(c1());
  c2 := c1;
  p();
  Writeln(c2());
  p := Greeter('world');
  p();
  SetLength(fs, 3);
  for i := 0 to 2 do
    fs[i] := MakeAdder(i * 10);
  for i := 0 to 2 do
    Write(fs[i](1), ' ');
  Writeln;
  Writeln(Twice(function(x) => x * 3, 2));
  add5 := nil;
  Writeln(Assigned(add5), ' ', Assigned(sq));

  acc := TAcc.Create;
  acc.FName := 'acc';
  p := acc.Adder;
  sh := acc.Show;
  p();
  p();
  p();
  Writeln(sh());
  acc.Free;

  h.F := Curry(10)(2);
  h.Tag := 'curried';
  h2 := h;
  h.F := nil;
  Writeln(h2.Tag, ' ', h2.F(5));
  m := Curry(1);
  f := m(3);
  m := nil;
  Writeln(f(4));

  box := TBox.Create;
  box.Fn := Outer(100);
  Writeln(box.Fn(2));
  box.Free;

  cell := specialize TCell<string>.Create;
  cell.FValue := 'cell';
  g := cell.Getter;
  cell.FValue := 'cell2';
  Writeln(g());
  cell.Free;

  Writeln(Tagged('x')());

  f := Fails(3);
  Writeln(f(1));
  try
    f := Fails(-1);
  except
    on E: EFail do
      Writeln('caught ', E.Msg);
  end;
  Writeln(f(1));
  for i := 1 to 3 do
    f := if i mod 2 = 0 then Outer(i) else Fails(i);
  Writeln(f(0));

  ps := Collect(['ann', 'bob']);
  for i := 0 to High(ps) do
    ps[i]();
  Writeln(Factorial(5));

  var dbl: TIntFn := function(x) => x * 2;
  Writeln(dbl(21));
  up := function(x) => x + x;
  Writeln(up('ab'));
  total := 0;
  f := function(x) => x + total; { a global: no frame }
  total := 5;
  Writeln(f(1));
  WithLambda;
end.
