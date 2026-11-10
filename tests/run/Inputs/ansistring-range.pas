program ansistringrange;
{$H+}
var
  s: string;
  i: longint;
begin
  s := 'a'; i := 2;
  writeln('ok ', s[1]);
  writeln(s[i]);
end.
