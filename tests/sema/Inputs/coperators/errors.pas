program errors;
type
  TC = class
  private
    FV: integer;
  public
    property V: integer read FV write FV;
  end;
var
  x: integer;
  c: char;
  b: boolean;
  s: string;
  o: TC;
procedure p(var u);
begin
  u += 1
end;
function g: integer;
begin
  g := 1
end;
begin
  undeclared += 1;
  g += 1;
  x += undeclared2;
  x /= 2;
  c += 'a';
  b += true;
  s -= 'x';
  o.V += 1;
  x += 1
end.
