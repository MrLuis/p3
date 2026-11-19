{ Object lifetimes with p3's owned variables (p3 only): the same work as
  bench/lifetime.pas, each object freed when its owned variable's scope is
  left, a factory's result given up with move. }
program lifetime_owned;
{$H+}
type
  TCounter = class
    value, bumps: longint;
    constructor Create(v: longint);
    procedure Bump;
  end;

constructor TCounter.Create(v: longint);
begin
  value := v;
end;

procedure TCounter.Bump;
begin
  value := (value * 3 + 1) mod 10007;
  bumps := bumps + 1;
end;

{ Two objects for one call. }
function Pair(i: longint): longint;
var
  a, b: TCounter owned;
begin
  a := TCounter.Create(i);
  b := TCounter.Create(i * 3);
  a.Bump;
  b.Bump;
  Result := a.value + b.value;
end;

{ A factory: the caller owns what it returns. }
function Make(i: longint): TCounter;
var
  c: TCounter owned;
begin
  c := TCounter.Create(i);
  c.Bump;
  Result := move c;
end;

function Factory(i: longint): longint;
var
  c: TCounter owned;
begin
  c := Make(i);
  c.Bump;
  Result := c.value + c.bumps;
end;

{ One object per iteration of an inner loop. }
function PerIteration(i: longint): longint;
var
  j: longint;
begin
  Result := 0;
  for j := 1 to 4 do
  begin
    var t: TCounter owned := TCounter.Create(i + j);
    t.Bump;
    Result := Result + t.value;
  end;
end;

var
  i: longint;
  total: int64;
begin
  total := 0;
  for i := 1 to 1000000 do
    total := total + Pair(i) + Factory(i) + PerIteration(i);
  writeln(total);
end.
