program ansistringread;
{$H+}
var
  s: string;
begin
  readln(s); writeln(length(s), ' ', s);
  readln(s); writeln(length(s), ' ', s[700]);
  readln(s); writeln(length(s), ' ', s);
  s := 'old'; readln(s); writeln(length(s));
end.
