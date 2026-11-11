unit cvars;
interface
var
  OptIndex: longint; external 'c' name 'optind';
  errno: longint; external 'StdCLib' name 'errno';
function Bump: longint;
implementation
function Bump: longint;
begin
  OptIndex := OptIndex + 1;
  Bump := OptIndex;
end;
end.
