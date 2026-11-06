{ read / readln of integers and chars from standard input.
  RUN: %pcc %s -o %t
  RUN: %t < %S/Inputs/io.input > %t.out
  RUN: diff %S/io.expected %t.out
}
program IO(input, output);
var n, sum, i: integer; c: char;
begin
  read(n);
  sum := 0;
  { integers skip blanks and line ends }
  read(i); sum := sum + i;
  read(i); sum := sum + i;
  read(i); sum := sum + i;
  readln;
  writeln('n=', n, ' sum=', sum);
  { a line end reads as a space }
  read(c); write('[', c, ']');
  read(c); write('[', c, ']');
  read(c); write('[', c, ']');
  readln; { skips the rest of the next line: we are already past the first }
  read(c); writeln('[', c, ']');
  readln(n);
  writeln(n * 2)
end.
