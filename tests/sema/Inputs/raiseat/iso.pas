program iso(output);
{ ExceptAddr is only predeclared with --mode=objfpc; at is no reserved word }
var
  exceptaddr, at: integer;
begin
  exceptaddr := 1;
  at := 2;
  writeln(exceptaddr + at)
end.
