{ Lambdas (objfpc): function(params) => expression and
  procedure(params) => statement are values of a procedural type, written
  where they are used. Parameter types left out are those of the type
  wanted; a function's result type is the type wanted, or the type of its
  body. A lambda can use the variables of the routine it is written in,
  while that routine runs.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/lambda.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/lambda.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/lambda.expected
}
{$H+}
program lambda;

type
  TFn = function(x: Integer): Integer;
  TBin = function(a, b: Integer): Integer;
  TProc = procedure(const s: string);
  TPred = function(const s: string): Boolean;
  TMake = function: string;
  TSwap = procedure(var a, b: Integer);
  TKey = function(const p: (string, Integer)): Integer;
  TPoint = record
    X, Y: Integer;
  end;
  TMeasure = function(const p: TPoint): Real;

  generic TList<T> = class
  type
    TTest = function(const Item: T): Boolean;
    TVisit = procedure(const Item: T);
  var
    FItems: array of T;
    procedure Add(const Item: T);
    function Count(Test: TTest): Integer;
    procedure Each(Visit: TVisit);
  end;

  TCounter = class
    FStep, FTotal: Integer;
    procedure Run(const Items: array of Integer);
    function Scaled(const Items: array of Integer): Integer;
  end;

function Apply(f: TFn; v: Integer): Integer;
begin
  Apply := f(v);
end;

function Fold(const items: array of Integer; f: TBin; start: Integer): Integer;
var
  i: Integer;
begin
  Result := start;
  for i := 0 to High(items) do
    Result := f(Result, items[i]);
end;

procedure Each(const items: array of string; p: TProc);
var
  i: Integer;
begin
  for i := 0 to High(items) do
    p(items[i]);
end;

function CountIf(const items: array of string; p: TPred): Integer;
var
  i: Integer;
begin
  Result := 0;
  for i := 0 to High(items) do
    if p(items[i]) then
      Result := Result + 1;
end;

function Twice(f: TFn): Integer;
begin
  Twice := f(f(1));
end;

procedure TList.Add(const Item: T);
begin
  SetLength(FItems, Length(FItems) + 1);
  FItems[High(FItems)] := Item;
end;

function TList.Count(Test: TTest): Integer;
var
  i: Integer;
begin
  Result := 0;
  for i := 0 to High(FItems) do
    if Test(FItems[i]) then
      Result := Result + 1;
end;

procedure TList.Each(Visit: TVisit);
var
  i: Integer;
begin
  for i := 0 to High(FItems) do
    Visit(FItems[i]);
end;

procedure TCounter.Run(const Items: array of Integer);
begin
  { a method's lambda uses the object's fields }
  FTotal := Fold(Items, function(acc, x) => acc + x * FStep, FTotal);
end;

function TCounter.Scaled(const Items: array of Integer): Integer;
var
  factor: Integer;
begin
  factor := 10;
  Scaled := Fold(Items, function(acc, x) => acc + x * factor + Self.FStep, 0);
end;

procedure Locals(k: Integer);
var
  total, i: Integer;
  names: array of string;
begin
  { parameter types from the type wanted; the routine's variables used }
  Writeln(Apply(function(x) => x + k, 1));
  Writeln(Apply(function(x: Integer): Integer => x * k, 4));
  { a procedure lambda changes a variable of the routine }
  total := 0;
  Each(['a', 'bb', 'ccc'], procedure(s) => total := total + Length(s));
  Writeln(total);
  Writeln(CountIf(['a', 'bb', 'ccc'], function(s) => Length(s) >= k - 3));
  { kept in a local variable: its types written, the result from the body }
  var add := function(a, b: Integer) => a + b;
  var shout := function(const s: string) => s + '!';
  var half := function(n: Integer) => n / 2;
  Writeln(add(2, 3), ' ', Fold([1, 2, 3, 4], add, 0), ' ', shout('hey'), ' ',
          half(5):0:1);
  Writeln(Fold([1, 2, 3, 4], function(acc, x) => acc * x, 1));
  { lambdas in a loop, in a lambda, and with a statement for a body }
  for i := 1 to 3 do
    Write(Apply(function(x) => x * i, 10), ' ');
  Writeln;
  Writeln(Apply(function(x) => Apply(function(y) => y + x + k, 100), 20));
  Writeln(Twice(function(x) => x + k));
  SetLength(names, 0);
  Each(['x', 'y', 'z'], procedure(s) =>
    begin
      SetLength(names, Length(names) + 1);
      names[High(names)] := s + s;
    end);
  Writeln(Length(names), ' ', names[2]);
  { in a conditional expression, with one }
  var pick: TFn := if k > 3 then function(x) => x + 1 else function(x) => x - 1;
  Writeln(pick(10), ' ', Apply(function(n) => if n > 0 then n else -n, -9));
end;

var
  sq: TFn;
  make: TMake;
  swap: TSwap;
  key: TKey;
  dist: TMeasure;
  a, b: Integer;
  words: specialize TList<string>;
  nums: specialize TList<Integer>;
  counter: TCounter;
  pt: TPoint;
begin
  Locals(5);
  { at program level: global variables are no hindrance to keeping it }
  sq := function(x) => x * x;
  make := function => 'made';
  swap := procedure(var a, b) => (a, b) := (b, a);
  key := function(p) => p.Item2 + Length(p.Item1);
  dist := function(p) => Sqrt(p.X * p.X + p.Y * p.Y);
  a := 1;
  b := 2;
  swap(a, b);
  pt.X := 3;
  pt.Y := 4;
  Writeln(sq(7), ' ', Apply(sq, 3), ' ', make(), ' ', a, b, ' ',
          key(('abc', 4)), ' ', dist(pt):0:1);
  Each(['x', 'y'], procedure(s) => Writeln('item ', s));

  { with generic types: the parameter type is the type argument }
  words := specialize TList<string>.Create;
  words.Add('pear');
  words.Add('fig');
  words.Add('banana');
  a := 4;
  Writeln(words.Count(function(w) => Length(w) > a), ' ',
          words.Count(function(w) => w[1] = 'f'));
  nums := specialize TList<Integer>.Create;
  nums.Add(3);
  nums.Add(4);
  b := 0;
  nums.Each(procedure(n) => b := b + n * n);
  Writeln(b);
  counter := TCounter.Create;
  counter.FStep := 2;
  counter.Run([1, 2, 3]);
  counter.Run([10]);
  Writeln(counter.FTotal, ' ', counter.Scaled([1, 2]));
  words.Free;
  nums.Free;
  counter.Free;
end.
