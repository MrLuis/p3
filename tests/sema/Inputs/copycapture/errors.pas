program errors;
type
  TFn = reference to function: Integer;
  TPlain = function: Integer;
  TProc = reference to procedure;
  TR = record a: Integer; end;
  TThing = class
    K: Integer;
    function Get: TFn;
  end;

function TThing.Get: TFn;
begin
  Get := function[K]: Integer => K;
  Get := function[Self]: Integer => Self.K;
end;

procedure Run(const items: array of Integer; var f: TFn);
var
  q: TProc;
  p: TPlain;
  n: Integer;
  r: TR;
begin
  n := 1;
  p := function[n]: Integer => n;
  f := function[items]: Integer => 1;
  q := procedure[n] => n := 2;
  f := function[nope]: Integer => 1;
  f := function[Run]: Integer => 1;
  f := function[n](x: Integer): Integer => n;
  f := function[r]: Integer => r.a;
end;

begin
end.
