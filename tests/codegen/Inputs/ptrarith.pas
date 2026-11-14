program ptrarith;
type
  PInt = ^longint;
  TRec = record
    a: longint;
    b: int64;
  end;
  PRec = ^TRec;
var
  p, q: PInt;
  r: PRec;
  u: pointer;
  n: longint;
  d: int64;
  less: boolean;
begin
  q := p + n;
  q := p - n;
  d := q - p;
  r := r + 1;
  d := r - r;
  u := u + n;
  d := u - u;
  inc(p);
  dec(r, n);
  n := p[n];
  less := p < q;
  flush(output);
end.
