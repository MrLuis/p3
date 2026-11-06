{ M12 (Turbo/FPC): short strings -- +, comparisons, indexing, string
  results, length/copy/pos/concat/upcase/delete/insert, read/write.
  RUN: %pcc --mode=tp %s -o %t
  RUN: echo line of input | %t > %t.out
  RUN: diff %S/strs.expected %t.out
  RUN: %pcc --mode=tp -O2 %s -o %t.O2
  RUN: echo line of input | %t.O2 > %t.O2.out
  RUN: diff %S/strs.expected %t.O2.out
}
program Strs;
type Name = string[8];
var s, t: string; short: Name; i: integer; c: char;
    words: array [1..3] of string[10];

function greet(who: string): string;
begin
  greet := 'Hello, ' + who + '!'
end;

function reversed(x: string): string;
var k: integer; r: string;
begin
  r := '';
  for k := length(x) downto 1 do r := r + x[k];
  reversed := r
end;

procedure shout(var x: string);
var k: integer;
begin
  for k := 1 to length(x) do x[k] := upcase(x[k])
end;

begin
  s := 'Pascal';
  t := s + ' & ' + 'LLVM';
  writeln(t, ' (', length(t), ')');
  writeln(greet('world'));
  writeln(reversed(s));
  shout(s); writeln(s);
  short := 'truncated text';
  writeln('[', short, '] ', length(short));
  writeln(copy(t, 3, 4), ' ', pos('LLVM', t), ' ', pos('xyz', t));
  writeln(concat('a', 'b', 'c'), ' ', 'abc' < 'abd', ' ', s = 'PASCAL', ' ', 'b' > 'abc');
  delete(t, 1, 9); writeln(t);
  insert('<', t, 1); insert('>', t, 6); writeln(t);
  c := 'x'; s := c; s := s + c; writeln(s, ' ', ord(s[0]));
  words[1] := 'one'; words[2] := 'two'; words[3] := words[1] + words[2];
  for i := 1 to 3 do write(words[i]:7); writeln;
  writeln('[', s:5, ']', '[', 'abc':2, ']');
  readln(s); writeln('read: ', s, ' len=', length(s))
end.
