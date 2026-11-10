program main;
uses withdef;
const
  Base = 1; { the default's Base is the unit's }
var
  o: TC;
begin
  writeln(Add(2), ' ', Add(2, Base));
  writeln(Greet, ' ', Greet('you'));
  o := TC.Create;
  writeln(o.Get);
  o.Free;
end.
