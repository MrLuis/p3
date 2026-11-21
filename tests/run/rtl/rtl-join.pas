{ Concat chains of three and more are one allocation through
  __pas_astr_join (optimization3.md item 7): the same results as the old
  pairwise concatenation, with empty pieces, constants, a character in
  the middle, aliasing (the left piece is the result's own variable), a
  function's own result and the ISO Concat's arguments. The output is
  the same as Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-join.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-join.expected
}
program rtljoin;
{$H+}
uses SysUtils;

{ the way the chains were built before }
function Pair(const a, b, c: string): string;
begin
  Pair := (a + b) + c;
end;

{ a chain as an expression's operand and a function's own result }
function Thrice(const s: string): string;
begin
  Thrice := s + s + s;
end;

var
  i: longint;
  r, big, ins, empty: string;
  sep: char;

begin
  big := 'The Quick Brown Fox';
  ins := 'inserted';
  empty := '';
  sep := '|';

  writeln('1: ' + big + sep + big);
  writeln('2: ' + big + empty + big);
  writeln('3: ' + empty + empty + empty);
  writeln('4: ' + ins + ins + ins);
  writeln('5: ' + '<' + sep + '>');
  r := big;
  for i := 1 to 3 do
    r := r + r + sep;
  writeln('6: ', length(r), ' ', pos(big + big + big + sep, r));
  writeln('7: ' + IntToStr(3) + ':' + big + '+' + big);
  writeln('8: ' + big + sep + ins + sep + big + sep + empty + sep + big);
  writeln('9: ', length(concat(big, sep, ins)), ' ', concat(big, sep, ins));
  writeln('10: ' + 'A' + 'B' + 'C');
  writeln('11: ' + big + ' ' + big);
  writeln('12: ', length(Thrice(big)), ' ', Thrice(ins));
  writeln('13: ' + big + sep);
  writeln('14: ' + sep + big);
  writeln('15: ' + empty + sep + big);

  { every shape's value, against the pairwise concatenation }
  writeln(Pair(big, big, big) = big + big + big, ' ',
          Pair(empty, ins, empty) = empty + ins + empty, ' ',
          Pair(ins, sep, big) = ins + sep + big, ' ',
          Pair(big, sep, empty) = (big + sep) + empty, ' ',
          Pair(empty, 'x', big) = '' + 'x' + big, ' ',
          Thrice(empty) = empty + empty + empty);
end.
