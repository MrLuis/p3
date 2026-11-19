{ Object lifetimes, the usual way: Create, then try ... finally Free so that
  an exception cannot leak the object. bench/lifetime_owned.pas does the
  same with p3's owned variables. }
program lifetime;
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
  a, b: TCounter;
begin
  a := TCounter.Create(i);
  try
    b := TCounter.Create(i * 3);
    try
      a.Bump;
      b.Bump;
      Result := a.value + b.value;
    finally
      b.Free;
    end;
  finally
    a.Free;
  end;
end;

{ A factory: the caller frees what it returns. }
function Make(i: longint): TCounter;
begin
  Result := TCounter.Create(i);
  try
    Result.Bump;
  except
    Result.Free;
    raise;
  end;
end;

function Factory(i: longint): longint;
var
  c: TCounter;
begin
  c := Make(i);
  try
    c.Bump;
    Result := c.value + c.bumps;
  finally
    c.Free;
  end;
end;

{ One object per iteration of an inner loop. }
function PerIteration(i: longint): longint;
var
  j: longint;
  t: TCounter;
begin
  Result := 0;
  for j := 1 to 4 do
  begin
    t := TCounter.Create(i + j);
    try
      t.Bump;
      Result := Result + t.value;
    finally
      t.Free;
    end;
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
