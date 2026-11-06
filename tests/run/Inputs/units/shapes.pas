unit Shapes;
interface
uses MathUtils;
type TSquare = record side: integer end;
function area(s: TSquare): integer;
procedure describe(s: TSquare);
implementation
function area(s: TSquare): integer;
begin
  area := square(s.side)
end;
procedure describe(s: TSquare);
begin
  writeln('square with side ', s.side, ' has area ', area(s))
end;
begin
  writeln('Shapes ready')
end.
