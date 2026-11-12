{ Turbo/Free Pascal files: the untyped type `file` and the file routines
  assign, close, reset and rewrite with a record size, blockread and
  blockwrite (with and without the count done), seek, filepos, filesize and
  eof; typed files named with assign are random access (read after rewrite,
  write after reset); named text files. The files go to the path given as
  the first argument. Output is
  identical to Free Pascal 3.2.2's (fpc -Mtp and -Mobjfpc).
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t %t.tmp | diff - %S/files-turbo.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.objfpc
  RUN: %t.objfpc %t.tmp2 | diff - %S/files-turbo.expected
}
program filesturbo;
type
  TRec = record
    a: longint;
    b: word;
  end;
var
  f: file;
  g: file of TRec;
  t: text;
  buf: array[0..15] of byte;
  big: array[0..99] of byte;
  r: TRec;
  i: integer;
  n: longint;
  w: word;
  q: int64;
  s: string;
  base: string;
begin
  base := paramstr(1);

  { untyped files: records of the size reset and rewrite give }
  assign(f, base + '.bin');
  rewrite(f, 1);
  for i := 0 to 15 do
    buf[i] := i * 3;
  blockwrite(f, buf, 10);
  blockwrite(f, buf[10], 6, n);
  writeln(n, ' ', filepos(f), ' ', filesize(f), ' ', eof(f));
  close(f);
  reset(f, 4);
  writeln(filesize(f), ' ', eof(f), ' ', filepos(f));
  blockread(f, buf, 2, n);
  writeln(n, ' ', buf[0], ' ', buf[7], ' ', filepos(f));
  seek(f, 3);
  blockread(f, buf, 5, w);
  writeln(w, ' ', buf[0], ' ', eof(f));
  close(f);
  reset(f); { 128-byte records }
  writeln(filesize(f));
  blockread(f, big, 1, q);
  writeln(q);
  close(f);
  reset(f, 1);
  n := 0;
  while not eof(f) do
  begin
    blockread(f, buf, 3, i);
    n := n + i;
  end;
  writeln('bytes ', n);
  close(f);

  { typed files named with assign: random access }
  assign(g, base + '.dat');
  rewrite(g);
  for i := 1 to 3 do
  begin
    r.a := i * 100;
    r.b := i;
    write(g, r);
  end;
  writeln(filepos(g), ' ', filesize(g), ' ', eof(g));
  seek(g, 1);
  read(g, r);
  writeln(r.a, ' ', r.b, ' ', filepos(g), ' ', eof(g));
  r.a := 7;
  seek(g, 0);
  write(g, r);
  close(g);
  reset(g);
  seek(g, 2);
  r.a := 9;
  write(g, r); { writing after reset }
  seek(g, 0);
  while not eof(g) do
  begin
    read(g, r);
    write(r.a, ' ');
  end;
  writeln;
  close(g);

  { named text files }
  assign(t, base + '.txt');
  rewrite(t);
  writeln(t, 'hello ', 42);
  writeln(t, 'second');
  close(t);
  reset(t);
  while not eof(t) do
  begin
    readln(t, s);
    writeln('[', s, ']');
  end;
  close(t);
end.
