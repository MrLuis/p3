program d;
var s: 1..10; i: integer;
begin
  i := 11;
  {$R-}
  s := i; writeln('unchecked ', s);
  {$R+}
  s := i; writeln('not reached')
end.
