{$pointermath off}
{$T+}
{$nilpointers on}
program unsafe_tp;

type
  PInt = ^integer;
  PDouble = ^double;

procedure ErrorsAndRun(q: PInt?);
var
  arr: array[0..3] of integer;
  p: PInt;
  pd: PDouble;
  u: pointer;
  x: integer;
begin
  x := 1;
  arr[0] := 10;
  arr[1] := 20;
  arr[2] := 30;
  p := @arr[0];

  { Inside unsafe block: pointermath off, typed @, and nilpointers are relaxed }
  unsafe
    p := p + 1;
    Inc(p);
    x := q^;
    u := @x;
    pd := @x;                  { ok: @x is untyped pointer inside unsafe }
    u := unsafe(@x);           { nested unsafe expr }
    x := unsafe(PInt(u))[0];   { inner unsafe(expr) does not break outer block }
    p := p - 1;                { still unsafe after inner unsafe(expr) }
  end;

  { After end: strict checks are restored }
  p := p + 1;                  { error: pointermath off restored }
  x := q^;                     { error: nilpointers restored }
  pd := @x;                    { error: $T+ restored (^integer vs ^double) }

  { unsafe(expr) form: relaxes only within (...) }
  x := unsafe(p + 1)^;
  unsafe(p)^ := 99;
  pd := unsafe(@x);
  x := unsafe(q)^;
  x := p[1];                   { error: pointermath off outside unsafe(expr) }
end;

function UseIdentifier: integer;
var
  unsafe: integer;
begin
  unsafe := 42;
  UseIdentifier := unsafe;
end;

function unsafe(k: integer): integer;
begin
  unsafe := k + 1;
end;

begin
  Writeln(UseIdentifier, ' ', unsafe(4));
end.
