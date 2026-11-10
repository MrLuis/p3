program strpass;
function Shout(s: string; const t: string): string;
begin
  s := s + '!';
  Shout := s + t;
end;
var a: string; b: string[8];
begin
  a := 'x';
  a := a + 'y';
  b := Shout(a, 'z');
  writeln(a, b);
end.
