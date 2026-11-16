program syntax;
var
  x: Integer;
  b: Boolean;
begin
  x := if b then 1;
  x := if b 1 else 2;
  x := if b then else 2;
end.
