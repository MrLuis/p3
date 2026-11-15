{ Tuples (objfpc): the targets of (a, b) := e are anything assignable (a
  property, an array element, a field through with); a tuple written out
  picks an overload by its own element types; nil, [] and @x take the
  element types of the tuple wanted.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/tuple-targets.expected
}
{$H+}
program tupletargets;
type
  PInt = ^Integer;
  TBox = class
    FV: Integer;
    property V: Integer read FV write FV;
  end;
function Over(p: (Real, Real)): Integer; overload;
begin
  Over := 1;
end;
function Over(s: string): Integer; overload;
begin
  Over := 2;
end;
var
  b: TBox;
  a: array[0..1] of Integer;
  t: (Integer, Integer);
  p: PInt;
  q: (PInt, set of Byte);
begin
  b := TBox.Create;
  (b.V, a[1]) := (5, 6);
  Writeln(b.V, a[1]);
  with t do
  begin
    (Item1, Item2) := (a[1], b.V);
    Writeln(Item1, Item2);
  end;
  Writeln(Over((1.0, 2.5)));
  q := (nil, []);
  q := (@a[0], [1, 2]);
  p := q.Item1;
  Writeln(p = @a[0], 2 in q.Item2);
  b.Free;
end.
