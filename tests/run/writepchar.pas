{ write and writeln of a pchar (Turbo/FPC), as in Free Pascal: the
  characters it points to, up to the NUL; nothing for nil (not even
  padding); a field width pads on the left. Any pointer to char is a pchar. Output is identical to
  Free Pascal's.
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t %t.tmp > %t.out
  RUN: diff %t.out %S/writepchar.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast %t.tmp > %t.out
  RUN: diff %t.out %S/writepchar.expected
}
program writepchar;

type
  PCh = ^char;
  TRec = record
    name: pchar;
  end;

var
  line: string[20];
  p, q: pchar;
  c: PCh;
  r: TRec;
  f: text;
  s: string;
  n: longint;

function Tail(x: pchar; skip: longint): pchar;
begin
  Tail := x + skip;
end;

function c_getenv(name: pchar): pchar; cdecl; external 'c' name 'getenv';

begin
  line := 'hello, world';
  line[13] := #0;
  p := @line[1];
  writeln(p);
  write(p, '|', p + 7, '|');
  writeln;
  writeln('[', p:16, ']');
  writeln('[', p:3, ']');
  n := 14;
  writeln('[', (p + 7):n, ']');

  { nil and the empty string }
  q := nil;
  writeln('[', q, ']');
  writeln('[', q:4, ']');
  writeln('[', p + 12, ']');
  writeln('[', (p + 12):4, ']');

  { any pointer to char; fields; function results }
  c := @line[8];
  writeln(c);
  r.name := p;
  writeln(r.name, ' ', Tail(r.name, 4));
  writeln(c_getenv('PCC_NO_SUCH_VARIABLE'), '.');

  { cut at the first NUL }
  line[6] := #0;
  writeln(p, ' ', p + 6);

  { to a text file }
  assign(f, paramstr(1));
  rewrite(f);
  writeln(f, p, ':', (p + 6):8);
  close(f);
  reset(f);
  readln(f, s);
  close(f);
  writeln(s);
end.
