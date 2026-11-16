program main;
uses gpriv;
var
  c: specialize TCell<Integer>;
begin
  c := specialize TCell<Integer>.Create;
  c.Put(41);
  c.Put(c.Value + 1);
  Writeln(c.Value, ' ', Puts);
  c.Free;
end.
