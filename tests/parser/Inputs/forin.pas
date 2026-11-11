program forin;
var
  c: char;
  s: string;
begin
  for c in s do
    write(c);
  for c in 'ab' + s do
    ;
end.
