{ if / while / repeat / for / case (ISO mode).
  RUN: %pcc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/control.expected %t.out
  RUN: %pcc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/control.expected %t.O2.out
}
program Control(output);
var i, j, n, d: integer; isPrime, a, b: boolean; c: char;
begin
  { if / else if chains }
  for i := 1 to 15 do
    if i mod 15 = 0 then writeln('FizzBuzz')
    else if i mod 3 = 0 then writeln('Fizz')
    else if i mod 5 = 0 then writeln('Buzz')
    else writeln(i);
  { the dangling else binds to the inner if }
  a := false; b := true;
  if a then if b then writeln('wrong') else writeln('wrong too');
  a := true; b := false;
  if a then if b then writeln('wrong') else writeln('inner else');
  { while + trial division }
  for n := 2 to 30 do begin
    isPrime := true;
    d := 2;
    while isPrime and (d * d <= n) do begin
      if n mod d = 0 then isPrime := false;
      d := d + 1
    end;
    if isPrime then write(n, ' ')
  end;
  writeln;
  while false do writeln('never');
  { repeat runs at least once }
  n := 27; i := 0;
  repeat
    if odd(n) then n := 3 * n + 1 else n := n div 2;
    i := i + 1
  until n = 1;
  writeln('collatz(27) steps: ', i);
  repeat writeln('once') until true;
  { for: downto, chars, booleans, empty ranges, bounds evaluated once }
  for c := 'e' downto 'a' do write(c);
  writeln;
  for i := 3 downto 1 do write(i);
  writeln;
  for i := 5 to 1 do writeln('never');
  for i := 1 downto 5 do writeln('never');
  n := 3;
  for i := 1 to n do begin n := n + 1; write(i) end;
  writeln(' n=', n);
  for a := false to true do write(a, ' ');
  writeln;
  { no overflow at the ends of the integer range }
  n := 0;
  for i := maxint - 2 to maxint do n := n + 1;
  writeln('iterations up to maxint: ', n);
  n := 0;
  for i := -maxint + 1 downto -maxint - 1 do n := n + 1;
  writeln('iterations down to minint: ', n);
  { nested loops }
  for i := 1 to 3 do begin
    for j := 1 to 3 do write(i * j:3);
    writeln
  end;
  { case on integers, chars and booleans }
  for i := -1 to 3 do
    case i of
      -1: writeln('minus one');
      0, 2: writeln('even');
      1, 3: writeln('odd')
    end;
  for c := 'a' to 'e' do
    case c of
      'a', 'e': writeln(c, ' vowel');
      'b', 'c', 'd': writeln(c, ' consonant');
    end;
  case odd(7) of true: writeln('seven is odd'); false: writeln('?') end
end.
