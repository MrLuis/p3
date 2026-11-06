program r;
var i: integer; c: char;
begin
  case i of
    1..10: writeln;
    5: writeln
  end;
  case c of
    'a'..'c', 'x': writeln;
    'b': writeln;
    'z'..'y': writeln
  end;
  case 'abc' of 'a': writeln end
end.
