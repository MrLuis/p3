program consts;
{$H+}
{$packrecords 1}
type
  TR = record a: byte; s: ansistring; end;
  TQ = record a: byte; p: pointer; end;
  TOk = record a: byte; b: int64; c: string[4]; end;
{$packrecords default}
  TOuter = record x: TOk; s: ansistring; end;
const
  R: TR = (a: 1; s: 'x');
  Q: TQ = (a: 1; p: nil);
  Ok: TOk = (a: 1; b: 2; c: 'ab');
  O: TOuter = (s: 'y');
begin
end.
