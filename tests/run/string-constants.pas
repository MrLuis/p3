{ Concatenations of string constants are constants, of any length:
  'a' + 'b', names of string constants, characters (#10, chr) in them; in
  const declarations, typed constants, assignments, write, comparisons,
  parameters and Pos. Before, two literals made a shortstring, cut at 255
  characters, and a const could not concatenate.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/string-constants.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/string-constants.expected
}
program stringconstants;
{$H+}
const
  A = 'abc';
  B = A + 'def';
  Line = 'one' + #10 + 'two';
  Long = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' +
         'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb';
  Longer = Long + Long + '!';
  C = 'x' + 'y' + 'z';
  One = '' + 'q';
  T: string = A + '-' + B;

procedure Show(const S: string);
begin
  writeln(Length(S), ' ', Copy(S, 1, 4), '..', Copy(S, Length(S) - 3, 4));
end;

var
  S: string;
  Short: string[10];
begin
  writeln(B, ' ', C, ' ', One, ' ', T);
  writeln(Line);
  Show(Long);
  Show(Longer);
  S := 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' + 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb';
  Show(S);
  Show('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' + 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' + 'c');
  writeln(Length(A + 'gh'), ' ', S = Long, ' ', B = 'abcdef', ' ', 'ab' + 'c' = A);
  writeln(Pos('cd' + 'e', B), ' ', Pos('b' + 'bb', Long));
  writeln('w' + 'x' + Chr(89) + 'z');
  Short := A + B + 'ghijkl';
  writeln(Short);
  S := S + 'tail' + '!';
  Show(S);
end.
