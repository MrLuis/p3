program unsafe_verified;

type
  PInt = ^integer;
  PDouble = ^double;

procedure Check;
var
  x: integer;
  pd: PDouble;
begin
  pd := @x;                    { error: --safe=verified forces $T+ }
  unsafe
    pd := @x;                  { ok: @x is untyped inside unsafe }
  end;
  pd := unsafe(@x);            { ok: unsafe(expr) yields untyped pointer }
  pd := @x;                    { error: verified $T+ restored }
end;

begin
end.
