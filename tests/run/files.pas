{ M10: files -- text and typed, internal files, buffer variables, eof/eoln,
  a program parameter bound to the first command-line argument, pack/unpack.
  RUN: rm -f %t.data
  RUN: %pcc %s -o %t
  RUN: %t %t.data > %t.out
  RUN: diff %S/files.expected %t.out
  RUN: diff %S/Inputs/files.data %t.data
}
program Files(input, output, data);
type
  Person = record name: packed array [1..5] of char; age: integer end;
var
  data: text;
  people: file of Person;
  nums: file of integer;
  p: Person;
  c: char;
  i, n, sum, lines: integer;
  a: array [1..6] of char;
  z: packed array [1..3] of char;

procedure copyNumbers(var src: text);
var tmp: file of integer; k: integer;
begin
  rewrite(tmp);
  while not eof(src) do begin
    read(src, k); write(tmp, k);
    if eoln(src) then readln(src)
  end;
  reset(tmp);
  sum := 0;
  while not eof(tmp) do begin read(tmp, k); sum := sum + k end
end;

begin
  { program parameter `data` is bound to the first command-line argument }
  rewrite(data);
  writeln(data, '10 20');
  writeln(data, '30');
  write(data, '40');            { no final newline: an implicit line end }
  reset(data);
  lines := 0;
  while not eof(data) do begin
    while not eoln(data) do begin read(data, c); write(c) end;
    readln(data); lines := lines + 1; write('|')
  end;
  writeln(' lines=', lines);
  reset(data);
  copyNumbers(data);
  writeln('sum=', sum);

  rewrite(people);
  p.name := 'alice'; p.age := 30; write(people, p);
  p.name := 'bob  '; p.age := 25; write(people, p);
  reset(people);
  while not eof(people) do begin
    read(people, p); writeln(p.name, ' ', p.age:3)
  end;

  { the buffer variable with get / put }
  rewrite(nums);
  for i := 1 to 3 do begin nums^ := i * 11; put(nums) end;
  reset(nums);
  n := 0;
  while not eof(nums) do begin n := n + nums^; get(nums) end;
  writeln('n=', n);

  for i := 1 to 6 do a[i] := chr(ord('a') + i - 1);
  pack(a, 2, z);
  writeln(z);
  z := 'XYZ';
  unpack(z, a, 4);
  for i := 1 to 6 do write(a[i]);
  writeln
end.
