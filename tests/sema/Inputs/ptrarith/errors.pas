program errors;
type
  PInt = ^longint;
  PReal = ^real;
var
  p, q: PInt;
  pr: PReal;
  u: pointer;
  x: real;
  n: longint;
  f: text;
  d: file of longint;
begin
  p := p + q;
  p := p * 2;
  p := p + x;
  p := 1 - p;
  n := p - pr;
  p := nil + 1;
  n := p div 2;
  if p < pr then;
  if p < nil then;
  n := u[1];
  n := p['a'];
  n := p - 1;
  p := q - p;
  inc(p, x);
  inc(nil);
  flush;
  flush(d);
  flush(n);
  flush(f, f);
end.
