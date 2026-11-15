program ast;
type
  TPair = (Integer, Integer);
  TNamed = (Name: string, Age: Integer);
  TColor = (Red, Green);
function F: (Integer, ^Integer);
begin
  F := (1, nil);
end;
var
  p: TPair;
  x, y: Integer;
begin
  p := (1, x + 2);
  (x, y) := p;
  var (a, b) := (x, 'text');
  x := (a, b).Item1;
end.
