program missing;
var f: file;
begin
  assign(f, paramstr(1) + '.none');
  reset(f, 1);
end.
