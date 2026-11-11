{ for ... in in --mode=tp (no classes): arrays, strings, sets and ordinal
  types. Output is identical to Free Pascal 3.2.2's (fpc -Mtp).
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t | diff - %S/forin-tp.expected
  RUN: %pcc --mode=tp -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/forin-tp.expected
}
program forintp;
type
  TDay = (mon, tue, wed, thu, fri);
  TWork = mon..fri;
  TDays = set of TDay;
var
  a: array[TDay] of integer;
  d: TDay;
  w: TWork;
  ds: TDays;
  s: string;
  s5: string[5];
  c: char;
  n: integer;
begin
  for d := mon to fri do
    a[d] := ord(d) * ord(d);
  for n in a do
    write(n, ' ');
  writeln;
  ds := [fri, tue, wed];
  for d in ds do
    write(ord(d));
  writeln;
  for w in TWork do
    write(ord(w));
  writeln;
  s := 'turbo';
  for c in s do
    write(upcase(c));
  writeln;
  s5 := 'abcdefgh';
  for c in s5 do
    write(c);
  writeln;
  for c in 'x' + 'yz' do
    write(c);
  writeln;
  n := 0;
  for c in char do
    if c in ['0'..'9'] then
      n := n + 1;
  writeln(n, ' digits');
end.
