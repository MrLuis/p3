{ Pointer arithmetic (Turbo/FPC), as in Free Pascal: p + n, n + p and
  p - n move a pointer by n of what it points to (bytes, for an untyped
  pointer); p - q counts them; inc and dec move a pointer variable; p[i]
  is (p + i)^; pointers are ordered by address. Output is identical to
  Free Pascal's.
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t > %t.out
  RUN: diff %t.out %S/ptrarith.expected
  RUN: %pcc --mode=tp -O2 -fno-range-checks %s -o %t.fast
  RUN: %t.fast > %t.out
  RUN: diff %t.out %S/ptrarith.expected
}
program ptrarith;

type
  PInt = ^longint;
  PPInt = ^PInt;
  TRec = record
    a: longint;
    b: int64;
    name: string[10];
  end;
  PRec = ^TRec;
  PByte_ = ^byte;
  THolder = record
    cur: PInt;
  end;

var
  a: array[0..9] of longint;
  r: array[0..3] of TRec;
  line: string[15];
  rows: array[0..2] of PInt;
  p, q: PInt;
  pp: PPInt;
  pr: PRec;
  pc, pe: pchar;
  pb: PByte_;
  u, v: pointer;
  h: THolder;
  i, n: longint;
  w: word;
  total: int64;

function Sum(first, last: PInt): longint;
var
  s: longint;
begin
  s := 0;
  while first < last do
  begin
    s := s + first^;
    inc(first);
  end;
  Sum := s;
end;

procedure Advance(var x: PInt; by: longint);
begin
  inc(x, by);
end;

procedure Fill(const x: PInt; count, value: longint);
var
  k: longint;
begin
  for k := 0 to count - 1 do
    x[k] := value + k;
end;

function CLength(s: pchar): longint;
var
  e: pchar;
begin
  e := s;
  while e^ <> #0 do
    inc(e);
  CLength := e - s;
end;

begin
  for i := 0 to 9 do
    a[i] := i * 10;

  { + and - }
  p := @a[0];
  q := p + 3;
  writeln(q^);
  q := 2 + q;
  writeln(q^);
  q := q - 4;
  writeln(q^);
  n := 7;
  writeln((p + n)^, ' ', (n + p)^, ' ', (p + n - 2)^);
  n := -2;
  q := @a[5];
  writeln((q + n)^, ' ', (q - n)^);
  w := 3;
  writeln((p + w)^);
  q := PInt(@a[1]) + 2;
  writeln(q^);
  writeln((PInt(@a[9]) - 9)^);
  (p + 8)^ := 81;
  (q - 1)^ := 21;
  writeln(a[8], ' ', a[2]);
  writeln(PInt(@a[1] + 8)^); { @x is untyped: 8 bytes }

  { difference }
  writeln(q - p, ' ', p - q);
  writeln(PInt(@a[9]) - PInt(@a[0]));
  total := (q - p) * 100;
  writeln(total);

  { inc and dec }
  p := @a[0];
  inc(p);
  writeln(p^);
  inc(p, 3);
  writeln(p^);
  dec(p, 2);
  writeln(p^);
  dec(p);
  writeln(p^);
  n := 4;
  inc(p, n);
  writeln(p^);
  dec(p, n + 1);
  writeln(p^);
  inc(p, -n + 6);
  writeln(p^);
  h.cur := @a[0];
  inc(h.cur, 6);
  writeln(h.cur^);
  Advance(h.cur, 2);
  writeln(h.cur^);
  Advance(h.cur, -8);
  writeln(h.cur^);

  { indexing }
  p := @a[2];
  writeln(p[0], ' ', p[2], ' ', p[-2]);
  p[1] := 31;
  p[-1] := 11;
  n := 5;
  p[n] := p[n] + 1;
  writeln(a[1], ' ', a[3], ' ', a[7]);
  Fill(@a[0], 4, 100);
  writeln(a[0], ' ', a[1], ' ', a[2], ' ', a[3], ' ', a[4]);
  for i := 0 to 2 do
    rows[i] := @a[i * 3];
  pp := @rows[0];
  writeln(pp[1][2], ' ', pp[2][0], ' ', (pp + 2)^[1]);
  pp[0][1] := 5;
  writeln(a[1]);

  { order }
  p := @a[1];
  q := @a[1];
  writeln(p < q, ' ', p > q, ' ', p <= q, ' ', p >= q);
  inc(q);
  writeln(p < q, ' ', p > q, ' ', p <= q, ' ', p >= q);
  writeln(q < p, ' ', q > p, ' ', q <= p, ' ', q >= p);
  writeln(Sum(@a[4], PInt(@a[4]) + 6), ' ', Sum(@a[9], @a[9]));

  { records: by their size }
  for i := 0 to 3 do
  begin
    r[i].a := i;
    r[i].b := i * 1000;
    r[i].name := 'r';
  end;
  pr := @r[0];
  writeln((pr + 2)^.b, ' ', pr[3].b, ' ', (pr + 1)^.name);
  pr[1].name := 'changed';
  inc(pr, 3);
  writeln(pr^.a, ' ', pr - PRec(@r[0]), ' ', r[1].name);
  dec(pr);
  writeln(pr^.b);

  { characters and bytes }
  line := 'hello, world';
  line[13] := #0;
  pc := @line[1];
  writeln((pc + 1)^, pc[4], (pc + 7)^);
  writeln(CLength(pc), ' ', CLength(pc + 7));
  pe := pc + CLength(pc);
  while pe > pc do
  begin
    dec(pe);
    write(pe^);
  end;
  writeln;
  pc[0] := 'J';
  writeln(line[1], line[2]);
  a[0] := $04030201;
  pb := PByte_(@a[0]);
  writeln(pb^ + (pb + 3)^, ' ', pb[1], ' ', pb[2]);

  { untyped pointers: by bytes }
  a[1] := 77;
  u := @a[0];
  u := u + 4;
  writeln(PInt(u)^);
  v := u - 4;
  writeln(u - v, ' ', v < u);
  inc(u, sizeof(longint) * 2);
  writeln(PInt(u)^);
  dec(u, 4);
  writeln(PInt(u)^);
  inc(v);
  writeln(PByte_(v)^);
end.
