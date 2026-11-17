program main;

{$H+}

uses tasks;

function Both: async Integer;
begin
  Result := (await Twice(3)) + Length(await Greet('bob'));
  await Note('in a task');
end;

var
  b: TBox;
begin
  Writeln(Wait(Both));
  Writeln(Wait(Greet('ann')));
  b := TBox.Create;
  b.V := 40;
  Writeln(Wait(b.Get(2)), ' ', Wait(TBox.Make('x')));
  b.Free;
  Wait(Note('done'));
end.
