{ Dynamic array / string overflow and negative-count guards in runtime/ansi.c:
  SetLength(a, $2000000000000001) on array of int64 overflows size_t * 8 and
  must trap cleanly instead of allocating a tiny wrapped buffer; Copy with
  negative count/start must safely return an empty slice.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: not %t 2>&1 | FileCheck %s
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: not %t.fast 2>&1 | FileCheck %s
  CHECK: Runtime error: out of memory in a dynamic array
}
{$H+}
program ansioverflow;

var
  a, b: array of int64;
  s: string;
  n: int64;
begin
  SetLength(a, 3);
  a[0] := 10;
  a[1] := 20;
  a[2] := 30;
  b := Copy(a, -5, Low(int64));
  if Length(b) <> 0 then
    Halt(1);

  { Try negative SetLength on ansistring -> clears cleanly }
  s := 'abc';
  SetLength(s, -5);
  if Length(s) <> 0 then
    Halt(1);

  { Wrap count * 8 past 2^64 (would wrap to 8 bytes without overflow guard) }
  n := $2000000000000001;
  SetLength(a, n);
  Writeln('unreachable ', Length(a));
end.
