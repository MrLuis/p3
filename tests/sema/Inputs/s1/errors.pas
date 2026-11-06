program errors;
type
  PInt = ^longint;
  TRec = record a: longint; end;
  TFn = function(x: longint): longint;
  TBig = array[int64] of byte;
const
  arr: array[1..3] of longint = (1, 2);
  rec: TRec = (b: 1);
var
  r: real;
  p: PInt;
  f: TFn;
  i: longint;
  t: text;
function c_bad(r: TRec): longint; cdecl; external 'c' name 'bad';
procedure modify(const s: string; const r: TRec);
begin
  s := 'x';
  r.a := 1;
  inc(r.a);
end;
procedure proc;
  function inner(x: longint): longint;
  begin
    inner := x;
  end;
begin
  f := @inner;
  exit(1);
end;
begin
  i := longint(r);
  p := PInt(i);
  i := sizeof(t);
  i := low(r);
  writeln(assigned(i));
  i := 10000000000000000000;
  i := p(1);
  modify('a', rec);
end.
