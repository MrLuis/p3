program declared;
{ in objfpc too, a declaration of the word keeps it a name }
var
  await, spawn, scope: Integer;
function async(x: Integer): Integer;
begin
  async := x + 1;
end;
begin
  await := async(1);
  spawn := await * 2;
  scope := spawn;
  Writeln(scope);
end.
