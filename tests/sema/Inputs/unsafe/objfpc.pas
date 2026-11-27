{$H+}
{$nullsafe on}
{$nilpointers on}
{$pointermath off}
program unsafe_objfpc;

type
  PInt = ^integer;
  TBox = class
    V: integer;
  end;

procedure Test(p: PInt?; b: TBox?);
var
  arr: array[0..2] of integer;
  np: PInt;
  nb: TBox;
begin
  arr[0] := 5;
  arr[1] := 6;
  np := @arr[0];
  unsafe
    np := np + 1;
    Writeln(unsafe(p)^, ' ', np^);
    nb := b;                   { error: $nullsafe remains enforced for classes }
  end;
  Writeln(b.V);                { error: $nullsafe outside unsafe }
  Writeln(p[0]);               { error: pointermath off restored }
end;

begin
end.
