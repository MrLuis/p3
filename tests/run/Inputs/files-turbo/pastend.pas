program pastend;
var f: file of integer; i: integer;
begin
  assign(f, paramstr(1));
  rewrite(f);
  write(f, 5);
  seek(f, 0);
  read(f, i);
  read(f, i);
end.
