{ M7/M8: constants, enumerations, subranges, reals, arrays, records with
  variants, strings, with.  Real formatting follows ISO 7185 6.9.3.4.
  RUN: %pcc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/types.expected %t.out
  RUN: %pcc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/types.expected %t.O2.out
}
program Types(output);
const
  N = 5;
  Pi = 3.14159265358979;
  Greeting = 'hello';
  Neg = -N;
type
  Color = (red, green, blue);
  Small = 1..N;
  Letter = 'a'..'z';
  Vector = array [Small] of integer;
  Matrix = array [1..3, 1..3] of real;
  Name = packed array [1..5] of char;
  Point = record x, y: real end;
  Shape = record
    name: Name;
    case kind: Color of
      red: (radius: real);
      green: (w, h: integer);
      blue: (corner: Point)
  end;
var
  v: Vector;
  m: Matrix;
  i, j, total: integer;
  c: Color;
  s: Shape;
  p, q: Point;
  nm, other: Name;
  l: Letter;
  r: real;

function norm(pt: Point): real;
begin
  norm := sqrt(sqr(pt.x) + sqr(pt.y))
end;

procedure scale(var pt: Point; f: real);
begin
  pt.x := pt.x * f; pt.y := pt.y * f
end;

procedure clobber(vv: Vector);
begin
  vv[1] := -1  { a copy: the caller's array is unchanged }
end;

function sum(vv: Vector): integer;
var k, acc: integer;
begin
  acc := 0;
  for k := 1 to N do acc := acc + vv[k];
  sum := acc
end;

begin
  writeln(N, ' ', Neg, ' ', Greeting, ' ', Pi:8:4);
  for i := 1 to N do v[i] := i * i;
  clobber(v);
  writeln('sum = ', sum(v), ' v[1] = ', v[1]);
  for i := 1 to 3 do
    for j := 1 to 3 do
      m[i, j] := i + j / 10;
  writeln(m[2][3]:5:2, m[3, 1]:5:2);
  for c := red to blue do write(ord(c), ' ');
  writeln(succ(red) = green, ' ', pred(blue) < blue);
  p.x := 3; p.y := 4;
  q := p;
  scale(q, 2);
  writeln('norm(p) = ', norm(p):4:1, ' q = (', q.x:3:1, ', ', q.y:3:1, ')');
  with s do begin
    name := 'boxes';
    kind := green;
    w := 3; h := 4
  end;
  writeln(s.name, ' area = ', s.w * s.h);
  s.kind := blue;
  s.corner := p;
  with s.corner do writeln('corner = ', x:3:1, ' ', y:3:1);
  nm := 'alpha'; other := 'alpha';
  writeln(nm = other, ' ', nm < 'beta ', ' ', nm > 'aardv');
  l := 'q';
  writeln(l, ' ', ord(l) - ord('a'));
  r := 1 / 3;
  writeln(r, ' ', r:10:6, ' ', -r:12);
  writeln(trunc(2.7), ' ', round(2.5), ' ', round(-2.5), ' ', trunc(-2.7));
  writeln(sin(0):4:1, ' ', cos(0):4:1, ' ', exp(1):8:5, ' ', ln(exp(2)):4:1, ' ', arctan(1) * 4:8:5);
  total := 7; r := total;
  writeln(r:4:1, ' ', abs(-2.5):4:1, ' ', sqr(1.5):5:2, ' ', 7 / 2:4:1)
end.
