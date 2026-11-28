program dynarray_align;
type
  TVD = vector[4] of double;
var
  arr: array of TVD;
  acc: TVD;

procedure SumOpen(const a: array of TVD);
var
  i: longint;
begin
  for i := 0 to high(a) do
    acc := acc + a[i];
end;

begin
  SetLength(arr, 4);
  acc[1] := 1.0;
  arr[0] := acc;
  acc := arr[0];
  SumOpen(arr);
  writeln(acc[1]:0:1);
end.
