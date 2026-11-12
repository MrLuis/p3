program more;
type
  TMixed = 'a'..#$100;
var
  w: widechar;
  x: TMixed;
begin
  writeln(upcase(widechar('q')), ' ', upcase(#$263A) = #$263A, ' ', ord(upcase(w)));
  w := #$100;
  x := w;
  writeln(ord(x), ' ', ord(low(TMixed)), ' ', ord(high(TMixed)));
  w := #$101;
  x := w;
end.
