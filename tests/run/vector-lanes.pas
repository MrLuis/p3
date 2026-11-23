{ vector[N] of T: comparisons lane by lane give a vector of booleans (a
  mask), against a vector or a scalar broadcast to the lanes; and, or,
  xor and not work on boolean and integer lanes; vectors of one shape
  are assignable to each other.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/vector-lanes.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/vector-lanes.expected
  RUN: not %p3 --mode=objfpc -c %S/Inputs/vector-lanes-errors.pas -o %t.o 2>&1 | FileCheck %s
  CHECK: invalid operands to '=' ('vector[16] of byte' and 'vector[8] of byte')
  CHECK: invalid operands to 'and' ('vector[4] of real' and 'vector[4] of real')
  CHECK: invalid operand to 'not' ('vector[4] of real')
}
program vcmp;
type
  TB16 = vector[16] of Byte;
  TM16 = vector[16] of Boolean;
  TR4 = vector[4] of Double;
  TI4 = vector[4] of Integer;
var
  a, b: TB16; m, n: TM16; i: Integer; r: TR4; ri: vector[4] of Boolean; s: TI4; si: vector[4] of Boolean;
procedure Show(const x: TM16);
var j: Integer;
begin
  for j := 1 to 16 do if x[j] then write('1') else write('.');
  writeln;
end;
begin
  for i := 1 to 16 do begin a[i] := i * 15; b[i] := 128; end;
  Show(a = 120);
  Show(a < b);
  Show(a >= 200);
  m := (a = 30) or (a = 60);
  Show(m);
  n := not m;
  Show(n);
  Show((a > 100) and (a < 200));
  Show((a > 100) xor (a < 200));
  Show(m = n);
  for i := 1 to 4 do begin r[i] := i * 0.5; s[i] := i - 3; end;
  ri := r > 1.0;
  si := s < 0;
  for i := 1 to 4 do write(ri[i], ' ', si[i], ' ');
  writeln;
  s := s and 1;
  for i := 1 to 4 do write(s[i], ' ');
  writeln;
end.
