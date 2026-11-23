program main;
uses customer;
var
  c: TCustomer;
begin
  c.Id := 7;
  c.Name := 'Ada';
  c.Active := True;
  writeln(c.ToJson);
end.
