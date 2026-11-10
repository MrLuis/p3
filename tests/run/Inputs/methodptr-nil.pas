program methodptrnil;
type
  TN = procedure of object;
var
  n: TN;
begin
  n := nil;
  n;
end.
