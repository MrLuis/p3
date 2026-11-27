{$T+}
{$pointermath off}
{$nilpointers on}
program unsafe_good;

type
  PInt = ^integer;
  PDouble = ^double;

var
  arr: array[0..3] of integer;
  p: PInt;
  q: PInt?;
  u: pointer;
  pd: PDouble;
  x: integer;

procedure RunUnsafe;
begin
  arr[0] := 10;
  arr[1] := 20;
  arr[2] := 30;
  arr[3] := 40;
  p := @arr[0];
  q := @arr[2];
  unsafe
    p := p + 1;
    Inc(p);
    Writeln(p^, ' ', q^);
    u := @arr[0];
    pd := @arr[0];
    if pd <> nil then
      x := unsafe(PInt(u))[3];
  end;
  unsafe(PInt(@x))^ := 77;
  Writeln(x, ' ', unsafe(q)^);
end;

function unsafe(k: integer): integer;
begin
  unsafe := k * 2;
end;

begin
  RunUnsafe;
  Writeln(unsafe(6));
end.
