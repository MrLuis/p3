program errors;
var
  f: file;
  g: file of integer;
  t: text;
  i: integer;
  b: byte;
begin
  write(f, 1);
  read(f, i);
  i := f^;
  get(f);
  reset(g, 4);
  blockread(g, i, 1);
  blockwrite(f, 3, 1);
  seek(t, 1);
  i := filepos(t);
  assign(f, 3);
  blockread(f, i, 'x');
  blockread(f, i, 1, b + 1);
  reset(f, 'a');
  close(i);
end.
