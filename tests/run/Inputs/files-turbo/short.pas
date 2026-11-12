program short;
var f: file; buf: array[0..9] of byte;
begin
  assign(f, paramstr(1));
  rewrite(f, 1);
  blockwrite(f, buf, 3);
  close(f);
  reset(f, 1);
  blockread(f, buf, 5);
end.
