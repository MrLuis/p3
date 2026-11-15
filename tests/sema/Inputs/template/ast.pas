program ast;
{$H+}
function Tag(const parts: array of string; const values: array of const): string;
begin
  Tag := parts[0];
end;
var
  s: string; total: real; n: longint;
begin
  s := `Total:
        ${total:0:2} for
        ${n} items`;
  s := `Total: ${total:0:2}`;
  s := Tag`a ${n} b`;
  s := `
    plain
    text
    `;
end.
