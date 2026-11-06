program pt;
uses shapes;
type
  TKind = (kA, kB);
  TRec = packed record
    x, y: longint;
    case k: TKind of
      kA: (s: string[8]);
      kB: (p: ^TRec)
  end;
  TFn = function(const a: string; var b: longint): boolean;
const
  tab: array[1..2] of TRec = ((x: 1; y: 2), (x: 3; y: 4));
procedure ext(p: pchar); cdecl; external 'c' name 'puts';
begin
  writeln();
  r.f(1);
end.
