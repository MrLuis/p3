program kinds;
{$push}{$interfaces corba}{$pop}
{$push}{$inline off}{$pop}
type
  IComFoo = interface procedure P; end;
{$interfaces corba}
{$push}{$interfaces com}{$pop}
  ICorbaFoo = interface procedure P; end;
function Twice(x: longint): longint; inline;
begin
  Twice := x * 2;
end;
begin
  writeln(Twice(4));
end.
