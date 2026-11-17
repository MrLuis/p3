program tp;
{ outside objfpc the words are plain names }
var
  async, await, spawn, scope, detached: Integer;
procedure Pool;
begin
end;
begin
  async := 1;
  await := async + 1;
  spawn := await;
  scope := spawn;
  detached := scope;
  Pool;
  Writeln(detached);
  Wait(1);
end.
