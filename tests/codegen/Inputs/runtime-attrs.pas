program runtime_attrs;
type
  TInts = array of integer;
var
  a: TInts;
  s: ansistring;
  i: integer;
begin
  i := 1;
  a := [1, 2];
  s := 'abc';
  s := Copy(s, i, 2)
end.
