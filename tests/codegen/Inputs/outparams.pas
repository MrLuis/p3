program outparams;
{$H+}
procedure P(n: longint; out s: ansistring; out i: longint);
begin
  s := 'x';
  i := n;
end;
var
  t: ansistring;
  k: longint;
begin
  P(length(t), t, k);
end.
