program template;
{$H+}
type
  TColor = (Red, Green);

function Tag(const parts: array of string; const values: array of const): string;
begin
  Tag := parts[0];
end;

procedure run(n: longint; x: real; c: TColor; const name: string);
var
  s: string;
begin
  s := `n = ${n}, x = ${x:8:2}`;
  s := `${name} is ${c}!`;
  s := `plain text`;
  s := Tag`a ${n} b`;
end;

begin
  run(1, 2.5, Green, 'name');
end.
