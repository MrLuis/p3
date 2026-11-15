program syntax;
var
  x: longint;
  s: string;
begin
  s := `empty ${} value`;
  s := `two ${x x} values ${x}`;
  s := `width ${x:} missing`;
  s := `inside ${ `a ${x +} b` } out ${x}`;
  s := Tag`width ${x:3} in a tag`;
  s := `never ${x closed;
end.
