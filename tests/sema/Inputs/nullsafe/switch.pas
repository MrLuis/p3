program switch;
{$nullsafe maybe}
type
  TFoo = class
  end;
var
  a: TFoo;
begin
  a := nil; { (not null-safe: no error) }
end.
