program absolutes;
{$H+}
var
  g: longint;
  gv: longint absolute g;

procedure Local;
var
  f: text;
  fv: text absolute f;
  s: ansistring;
  sv: ansistring absolute s;
  n: longint;
  nv: longint absolute n;
begin
  rewrite(fv);
  sv := 'x';
  nv := 1;
  writeln(fv, n, s);
end;

procedure Captures;
var
  n, own: longint;
  nv: longint absolute n;
  procedure Inner;
  begin
    nv := 1;
  end;
begin
  own := 2;
  Inner;
  writeln(own);
end;

begin
  gv := 1;
  Local;
  Captures;
end.
