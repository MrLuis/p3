{$H+}
program unchecked;
{ without the racechecks modeswitch a spawned task may use what it sees,
  and the words are plain names where no type follows them }
uses threading;
type
  TBytes = array of Byte;
function Run: async Integer;
var
  sum: Integer;
begin
  sum := 0;
  scope
    spawn begin sum := sum + 1; end;
    spawn begin sum := sum + 2; end;
  end;
  Result := sum;
end;
var
  shared, isolated: Integer;
begin
  shared := 1;
  isolated := 2;
  Writeln(Wait(Run) + shared + isolated);
end.
