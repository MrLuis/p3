program toi64;
var q: QWord; i: int64;
begin
  q := 9223372036854775807;
  i := q;
  q := q + 1;
  i := q;
  writeln(i);
end.
