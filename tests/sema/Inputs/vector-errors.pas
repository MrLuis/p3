program verrors;
{$H+}
type
  TBAD = vector[4] of real;      { lanes of different shapes }
  TQ = vector[4] of qword;       { qword lanes }
  TN = vector[0] of longint;     { an empty vector }
  TR = vector[4] of record i: longint; end; { not ordinal or real }
var
  a: vector[2] of longint;
  b: vector[3] of char;
  i: longint;
begin
  a[i + 10] := 1;   { an out-of-bounds vector index (not compile-time known) }
  writeln(b['x'], a[3]);
end.
