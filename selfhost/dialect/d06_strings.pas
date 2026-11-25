{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
program d06_strings;
{ Dialect check L12: string parameters (value, var, const), short-string
  truncation, string results, and the existing string routines. }
type
  TName = string[8];

function shout(const s: string): string;
var
  i: longint;
  r: string;
begin
  r := '';
  for i := 1 to length(s) do
    r := r + upcase(s[i]);
  shout := r + '!';
end;

procedure appendTo(var acc: string; const part: string);
begin
  acc := acc + part;
end;

procedure tryModify(s: string);
begin
  s := 'changed';
  writeln(s);
end;

var
  a: string;
  n: TName;
begin
  a := 'pascal';
  writeln(shout(a));
  appendTo(a, ' compiler');
  writeln(a, ' ', length(a));
  tryModify(a);
  writeln(a);
  n := 'abcdefghijkl';
  writeln(n, ' ', length(n));
  writeln(pos('comp', a), ' ', copy(a, 8, 4));
  delete(a, 1, 7);
  insert('self-hosted ', a, 1);
  writeln(a);
  writeln('abc' < 'abd', ' ', 'abc' = 'ABC', ' ', a[1]);
end.
