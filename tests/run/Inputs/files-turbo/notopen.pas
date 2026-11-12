program notopen;
var f: file of integer;
begin
  assign(f, 'x');
  close(f);
end.
