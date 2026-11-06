program d;
const
  A = 10;
  B = A + 1;
  C = 'x';
type
  T1 = array [1..3] of integer;
  T2 = array [1..3] of integer;
  Bad1 = 5..1;
  Bad2 = 1..'z';
  Bad3 = array [integer] of char;
  Bad4 = array [real] of char;
  Rec = record x: integer; x: char end;
  Color = (red, green);
  Fruit = (apple, red);
  Str5 = packed array [1..5] of char;
var
  v1: T1; v2: T2; s5: Str5; i: integer; r: real;
  pt: record x, y: integer end;
begin
  v1 := v2;
  v1[4] := 1;
  v1['a'] := 1;
  i[1] := 2;
  pt.z := 1;
  i.x := 1;
  with i do writeln;
  s5 := 'abc';
  s5 := 'abcde';
  if s5 = 'abcdef' then writeln;
  i := r;
  r := i;
  writeln(red)
end.
