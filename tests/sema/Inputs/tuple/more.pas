program more;
{$H+}
type
  TPair = (Integer, Integer);
  TEnum = (Real, Other);

function Pair: (Integer, Integer);
begin
  Result := (1, 2);
end;

function Bad: (Integer, Integer);
begin
  exit((1, 'x'));
end;

procedure Take(p: TPair);
begin
end;

procedure TakeVar(var p: TPair);
begin
end;

procedure Default(p: TPair = (1, 2));
begin
end;

var
  p: TPair;
  named: (A: Integer, B: Integer);
  x, y: Integer;
  e: TEnum;
  bad3: TPair = (1, 2, 3);
  good: TPair = (1, 2);
  alsogood: (A: Integer, B: string) = (B: 'b'; A: 1);

begin
  Take((1, 2.5));
  TakeVar((1, 2));
  TakeVar(named);
  p := (nope, 2);
  x := p.Item3;
  x := named.Item1;
  e := Real;
  e := (1, 2);
  if true then var (u, v) := Pair;
  if p = p then x := 1;
  writeln(p);
  (x, y) := Pair;
  p := Pair;
  named := p;
  Take(named);
  Take((x, y));
  ((x, y), e) := (p, Other);
end.
