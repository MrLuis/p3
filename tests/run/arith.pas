{ Integer / boolean / char semantics (ISO 7185 6.7).
  RUN: %pcc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/arith.expected %t.out
  RUN: %pcc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/arith.expected %t.O2.out
}
program Arith(output);
var a, b, z: integer; t, f: boolean; c: char;
begin
  a := 17; b := 5; z := 0;
  writeln(a + b, ' ', a - b, ' ', a * b, ' ', a div b, ' ', a mod b);
  { div truncates toward zero; ISO mod is never negative }
  writeln((-17) div 5, ' ', (-17) mod 5, ' ', 17 div (-5));
  { the sign applies to the whole term: -(17 mod 5) }
  writeln(-17 mod 5);
  writeln(1 + 2 * 3, ' ', (1 + 2) * 3, ' ', -2 * 3, ' ', 10 - 4 - 3);
  writeln(maxint, ' ', -maxint);
  writeln(abs(-42), ' ', sqr(12), ' ', odd(7), ' ', odd(-3), ' ', odd(10));
  c := 'A';
  writeln(ord(c), ' ', chr(ord(c) + 2), ' ', succ(c), ' ', pred(c), ' ',
          succ(41), ' ', pred(false = false));
  t := true; f := false;
  writeln(t and f, ' ', t or f, ' ', not t, ' ', t = f, ' ', f < t, ' ',
          'a' < 'b', ' ', 3 >= 3, ' ', 2 <> 2);
  { short-circuit evaluation: the division is never executed }
  writeln((z <> 0) and (a div z > 1));
  writeln((z = 0) or (a div z > 1));
  writeln(ord(true), ' ', ord(false), ' ', succ(false));
  { field widths pad on the left; strings and booleans are truncated }
  writeln('[', 42:5, '][', -7:1, '][', 'x':3, '][', 'hello':3, '][',
          false:2, '][', true:6, ']')
end.
