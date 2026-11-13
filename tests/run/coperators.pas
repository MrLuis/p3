{ Free Pascal's C operators (tp mode): x += e, x -= e, x *= e and x /= e
  are x := x + (e), x := x - (e), x := x * (e) and x := x / (e), for every
  type the operator works on (integers, reals, strings, sets, chars into
  strings). x is read again, so its indexes and calls are evaluated twice,
  as in Free Pascal; in function f, f += e adds to f's result (also from
  a nested routine). On by default ($coperators off turns them off).
  Output is identical to Free Pascal 3.2.2's (fpc -Mtp).
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t | diff - %S/coperators.expected
}
program coperators;
{$macro on}
{$define CELL := arr[k]}
type
  TRec = record
    n: integer;
    r: real;
  end;
  PInt = ^integer;
var
  i, cnt, k: integer;
  q: int64;
  b: byte;
  r: real;
  s: string;
  st: set of 0..9;
  arr: array[1..5] of integer;
  rec: TRec;
  p: PInt;
  c: char;

function idx: integer;
begin
  cnt := cnt + 1;
  idx := 2
end;

function f: integer;
begin
  f := 10;
  f += 5;
  f *= 2
end;

function outer: integer;
  procedure inner;
  begin
    outer -= 1
  end;
begin
  outer := 100;
  inner;
  inner
end;

procedure bump(var v: integer; by: integer);
begin
  v += by
end;

begin
  i := 1; i += 2; i -= 1; i *= 10; writeln(i);
  i := 2; i *= 3 + 4; writeln(i);          { i * (3 + 4) }
  i := 5; i -= -3; writeln(i);
  i := 10; i -= 2 - 1; writeln(i);         { i - (2 - 1) }
  q := 1; q *= 1000000000000; writeln(q);
  b := 200; b += 50; writeln(b);
  r := 3; r /= 2; r += 1; writeln(r:0:2);
  r := 1; r /= 4 * 2; writeln(r:0:3);      { r / (4 * 2) }
  r := 2; r *= 3; writeln(r:0:1);
  s := 'ab'; s += 'cd'; s += 'e' + 'f'; writeln(s);
  c := 'x'; s := c; s += c; writeln(s);
  st := [1]; st += [2, 3]; st -= [1]; st *= [3, 4]; writeln(3 in st, 2 in st, 1 in st);
  cnt := 0; arr[2] := 7; arr[idx] += 1; writeln(arr[2], ' ', cnt);
  k := 3; arr[k] := 1; arr[k] += arr[k] + 1; writeln(arr[3]);
  k := 4; arr[4] := 6; CELL *= 7; writeln(arr[4]);
  rec.n := 1; rec.n += 1; rec.r := 0.5; rec.r *= 3; writeln(rec.n, ' ', rec.r:0:1);
  with rec do n += 5; writeln(rec.n);
  new(p); p^ := 3; p^ *= 2; writeln(p^); dispose(p);
  writeln(f);
  writeln(outer);
  i := 1; bump(i, 41); writeln(i);
  i:=1;i+=1;i-=-1;writeln(i)
end.
