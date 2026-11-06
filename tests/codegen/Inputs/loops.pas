program l;
var i: integer; c: char;
begin
  for i := 1 to 10 do c := 'x';
  case i of
    1: c := 'a';
    2, 3: c := 'b'
  end
end.
