{ Tuples (objfpc): the types (T1, T2) and (Name: T1, Name: T2), the values
  (a, b), and (a, b) := e / var (a, b) := e, which give the elements of a
  tuple to variables.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/tuple.expected
  RUN: %p3 --mode=objfpc -O2 -fno-range-checks %s -o %t.fast
  RUN: %t.fast | diff - %S/tuple.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/tuple.expected
}
{$H+}
program tuple;

type
  TPair = (Integer, Integer);
  TNamed = (Name: string, Age: Integer);
  TColor = (Red, Green, Blue);          { still an enumeration }
  TIntArray = array of Integer;
  TNode = record
    Key: (Integer, string);
    Next: ^TNode;
  end;
  TBox = class
    FPos: (X: Integer, Y: Integer);
    FName: string;
    function Pos: (X: Integer, Y: Integer);
    procedure SetBoth(const P: (Integer, Integer));
    property Name: string read FName write FName;
  end;

var
  gp: TPair;
  gn: TNamed;
  count: Integer;

function MinMax(const A: TIntArray): (Min: Integer, Max: Integer);
var
  i: Integer;
begin
  Result := (A[0], A[0]);
  for i := 1 to High(A) do
  begin
    if A[i] < Result.Min then Result.Min := A[i];
    if A[i] > Result.Max then Result.Max := A[i];
  end;
end;

function DivMod2(a, b: Integer): (Integer, Integer);
begin
  DivMod2 := (a div b, a mod b);
end;

function Person(const n: string; a: Integer): TNamed;
begin
  Result := (n + '!', a);
end;

function Next: Integer;
begin
  count := count + 1;
  Next := count;
end;

function Stats(const A: TIntArray): (Count: Integer, Mean: Real, Text: string);
var
  i, sum: Integer;
begin
  sum := 0;
  for i := 0 to High(A) do
    sum := sum + A[i];
  exit((Length(A), sum / Length(A), 'n=' + chr(ord('0') + Length(A))));
end;

function Sum(p: TPair): Integer;
begin
  p.Item1 := p.Item1 + p.Item2;   { a copy }
  Sum := p.Item1;
end;

procedure Swap(var p: (Integer, Integer));
begin
  (p.Item1, p.Item2) := (p.Item2, p.Item1);
end;

function Nested: ((Integer, Integer), string);
begin
  Result := ((1, 2), 'nested');
end;

function Fib(n: Integer): Integer;
var
  a, b: Integer;
begin
  a := 0;
  b := 1;
  while n > 0 do
  begin
    (a, b) := (b, a + b);
    n := n - 1;
  end;
  Fib := a;
end;

function TBox.Pos: (X: Integer, Y: Integer);
begin
  Result := FPos;
end;

procedure TBox.SetBoth(const P: (Integer, Integer));
begin
  FPos := P;
end;

procedure Strings;
var
  i: Integer;
  s, t: string;
  n: TNamed;
begin
  s := 'left';
  t := 'right';
  for i := 1 to 3 do
  begin
    (s, t) := (t + '.', s);
    n := (s + t, i);
  end;
  Writeln(s, ' ', t, ' ', n.Name, ' ', n.Age);
  var (who, age) := Person(s, 7);
  Writeln(who, ' ', age);
  var both := (who, t);
  both.Item2 := both.Item1 + both.Item2;
  Writeln(both.Item2);
end;

procedure Main;
var
  values: TIntArray;
  x, y: Integer;
  r: Real;
  c: Char;
  p: TPair;
  q: (A: Integer, B: Integer);
  node: TNode;
  arr: array[1..2] of (Integer, Char);
  box: TBox;
  b: Byte;
begin
  SetLength(values, 5);
  values[0] := 3; values[1] := 9; values[2] := -4; values[3] := 7; values[4] := 0;

  var (lo, hi) := MinMax(values);
  Writeln(lo, ' to ', hi);
  var m := MinMax(values);
  Writeln(m.Min, ' to ', m.Max);
  Writeln(MinMax(values).Max);

  (x, y) := DivMod2(17, 5);
  Writeln(x, ' ', y);
  (x, y) := (y, x);
  Writeln(x, ' ', y);

  { the elements are converted as in an assignment }
  (r, c) := (x, 'z');
  Writeln(r:0:1, ' ', c);
  (b, r) := DivMod2(100, 7);
  Writeln(b, ' ', r:0:1);

  p := (1, 2);
  q := p;              { names do not matter }
  p := (q.B, q.A);
  Writeln(p.Item1, ' ', p.Item2, ' ', q.A, ' ', q.B);
  Writeln(Sum(p), ' ', p.Item1);
  Writeln(Sum((40, 2)));
  Swap(p);
  Writeln(p.Item1, ' ', p.Item2);
  p := (p.Item2, p.Item1);
  Writeln(p.Item1, ' ', p.Item2);

  var st := Stats(values);
  Writeln(st.Count, ' ', st.Mean:0:2, ' ', st.Text);
  var (n, mean, text) := Stats(values);
  Writeln(n, ' ', mean:0:2, ' ', text);

  { evaluated once, left to right }
  count := 0;
  (x, y) := (Next, Next);
  Writeln(x, ' ', y, ' ', count);

  node.Key := (5, 'five');
  node.Next := nil;
  Writeln(node.Key.Item1, ' ', node.Key.Item2);
  (node.Key.Item1, node.Key.Item2) := (6, 'six');
  Writeln(node.Key.Item1, ' ', node.Key.Item2);

  arr[1] := (1, 'a');
  arr[2] := (2, 'b');
  (arr[1], arr[2]) := (arr[2], arr[1]);
  Writeln(arr[1].Item1, arr[1].Item2, ' ', arr[2].Item1, arr[2].Item2);

  var nest := Nested;
  Writeln(nest.Item1.Item2, ' ', nest.Item2);
  var (inner, word) := Nested;
  Writeln(inner.Item1, ' ', word);
  var mixed := (1, 2.5, 'x', 'text', True, Green);
  Writeln(mixed.Item1, ' ', mixed.Item2:0:1, ' ', mixed.Item3, ' ',
          mixed.Item4, ' ', mixed.Item5, ' ', ord(mixed.Item6));
  Writeln((10, 20).Item2);

  box := TBox.Create;
  box.SetBoth((3, 4));
  (x, y) := box.Pos;
  Writeln(x, ' ', y, ' ', box.Pos.Y);
  (box.FPos.X, box.Name) := (9, 'box');
  Writeln(box.FPos.X, ' ', box.Name);
  box.Free;

  gp := (7, 8);
  gn := ('global', gp.Item1 + gp.Item2);
  Writeln(gn.Name, ' ', gn.Age);
  Writeln(Fib(30));
  if true then
    (x, y) := (1, 2)
  else
    (x, y) := (3, 4);
  Writeln(x, y);
end;

begin
  Main;
  Strings;
  var (a, b) := (1, 'main');
  (gp.Item1, gn.Name) := (a + 1, b + '.');
  Writeln(gp.Item1, ' ', gn.Name);
end.
