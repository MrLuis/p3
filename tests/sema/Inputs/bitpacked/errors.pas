program errors;
type
  TBits = bitpacked array[0..7] of boolean;
  TRec = bitpacked record
    a: 0..7;
    b: byte;
    s: string[3];
    r: real;
    p: pointer;
    n: record x: integer; end;
    bits: TBits;
  end;
  TOK = bitpacked array[0..3] of integer;
var
  x: TBits;
  r: TRec;
  q: ^boolean;
  pv: pointer;
  i: integer;
  v: byte absolute r.b;
  c: TBits = (true, false, true, false, true, false, true, false);
procedure Take(var b: boolean); begin end;
procedure Open(const a: array of boolean); begin end;
procedure Untyped(var u); begin end;
begin
  Take(x[1]);
  q := @x[2];
  pv := @r.b;
  inc(r.a);
  Open(x);
  Untyped(r.b);
  read(r.a);
  x[0] := true;
  r.bits[3] := x[0];
  pv := @r.s;
  i := r.a;
end.
