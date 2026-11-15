program errors;
{$H+}
type
  TPair = (Integer, Integer);
  TFiles = (Integer, Text);
  TDup = (A: Integer, a: Real);
  TDup2 = (Integer, Item1: Real);
  TEnum = (Real, Other);
  TRec = record
    x, y: Integer;
  end;

function Pair: (Integer, Integer);
begin
  Result := (1, 2);
end;

procedure Take(p: TPair);
begin
end;

procedure TakeVar(var p: TPair);
begin
end;

const
  K = (1, 2);

var
  p: TPair;
  q: (Integer, string);
  t3: (Integer, Integer, Integer);
  rec: TRec;
  x, y: Integer;
  s: string;
  e: TEnum;

begin
  p := (1, 'a');
  p := (1, 2, 3);
  q := p;
  p := t3;
  rec := (1, 2);
  p := rec;
  x := (1, 2);
  (x, y) := (1, 2, 3);
  (x, y) := x;
  (x, s) := Pair;
  (x, 5) := Pair;
  (x, y) := (nil, 2);
  var (a, b, c) := Pair;
  var (d, d2) := 5;
  var n := (nil, 1);
  var m := ([], 1);
  Take((1, 2.5));
  TakeVar((1, 2));
  p := (nope, 2);
  x := p.Item3;
  e := Real;
  e := (1, 2);
  if true then var (u, v) := Pair;
  if p = p then x := 1;
  writeln(p);
end.
