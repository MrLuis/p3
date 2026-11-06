{ M12 (Turbo/FPC): // comments, $hex %bin &oct, #nn chars, shl/shr/xor,
  bitwise and/or/not, break/continue/exit, inc/dec.
  RUN: %pcc --mode=tp %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/tpa.expected %t.out
  RUN: %pcc --mode=tp -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/tpa.expected %t.O2.out
}
program TPA;
// line comments, based numbers and control characters
var i, n: integer; c: char; b: boolean;

function firstOver(limit: integer): integer;
var k: integer;
begin
  firstOver := -1;
  for k := 1 to 100 do
    if k * k > limit then begin firstOver := k; exit end;
end;

begin
  writeln($FF, ' ', %1010, ' ', &17, ' ', $7FFFFFFF = maxint);
  write('tab'#9'bell-less'#$21); writeln;
  writeln(5 and 3, ' ', 5 or 3, ' ', 5 xor 3, ' ', 1 shl 10, ' ', 1024 shr 3, ' ', not 0);
  b := true xor false; writeln(b);
  n := 0;
  for i := 1 to 10 do begin
    if odd(i) then continue;
    if i > 8 then break;
    n := n + i
  end;
  writeln('even sum <= 8: ', n);
  i := 0;
  repeat inc(i); if i = 3 then continue; if i = 6 then break until false;
  writeln('i = ', i);
  i := 10; inc(i, 5); dec(i); writeln(i);
  c := 'a'; inc(c, 2); writeln(c);
  writeln('firstOver(50) = ', firstOver(50));
  while true do begin
    writeln('leaving');
    exit
  end;
  writeln('not reached')
end.
