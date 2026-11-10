unit shared;
interface
var
  value: longint;
  view: array[0..3] of byte absolute value;
procedure SetLow(b: byte);
implementation
var
  hidden: longint;
  low: byte absolute hidden;
procedure SetLow(b: byte);
begin
  low := b;
  value := hidden;
end;
end.
