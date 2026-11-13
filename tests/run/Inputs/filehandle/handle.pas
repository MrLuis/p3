program handle;
function cread(fd: longint; buf: pointer; n: int64): int64; cdecl; external name 'read';
var
  f: file;
  t: text;
  g: file of integer;
  h: THandle;
  buf: packed array[1..7] of char;
begin
  assign(f, ParamStr(0) + '.dat');
  rewrite(f, 1);
  buf := 'program';
  blockwrite(f, buf, 7);
  buf := '.......';
  close(f);
  writeln(GetFileHandle(f));
  reset(f, 1);
  h := GetFileHandle(f);
  writeln(h > 2, ' ', sizeof(THandle));
  writeln(cread(h, @buf, 7), ' ', buf);
  close(f);
  writeln(GetFileHandle(f));
  writeln(GetFileHandle(input), ' ', GetFileHandle(output));
  assign(t, ParamStr(0) + '.txt');
  rewrite(t);
  writeln(GetFileHandle(t) > 2);
  close(t);
  assign(g, ParamStr(0) + '.bin');
  rewrite(g);
  writeln(GetFileHandle(g) > 2);
  close(g)
end.
