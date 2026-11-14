{ flush(f) (Turbo/FPC) writes out a text file's buffered output: another
  reader of the file, and output written without Pascal's buffer, see
  what was written before it. Output is identical to Free Pascal's.
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t %t.tmp > %t.out
  RUN: diff %t.out %S/flush.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast %t.tmp > %t.out
  RUN: diff %t.out %S/flush.expected
}
program flushes;

function c_write(fd: longint; const data; size: int64): int64; cdecl;
  external 'c' name 'write';

var
  f, g: text;
  s: string;
  direct: string[20];

begin
  assign(f, paramstr(1));
  rewrite(f);
  write(f, 'first');
  assign(g, paramstr(1));
  reset(g);
  writeln('before flush: ', eof(g)); { still in f's buffer }
  close(g);
  flush(f);
  reset(g);
  readln(g, s);
  writeln('after flush: ', s);
  close(g);
  writeln(f, ' second');
  flush(f);
  flush(f); { nothing left: no effect }
  reset(g);
  readln(g, s);
  writeln('again: ', s);
  close(g);
  close(f);

  { the standard output, before output that does not go through it }
  write('pascal, ');
  flush(output);
  direct := 'then C' + chr(10);
  c_write(1, direct[1], length(direct));
  writeln('done');
end.
