program dynarrays;
{$H+}
type
  TI = array of longint;
  TS = array of string;
var
  a: TI;
  s: TS;
  m: array of TI;
begin
  setlength(a, 2);
  a[1] := length(a);
  setlength(s, 1);
  setlength(m, 2, 3);
  a := nil;
end.
