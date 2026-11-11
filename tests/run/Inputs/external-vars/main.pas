program main;
uses cvars;
function close(fd: longint): longint; cdecl; external 'c';
begin
  writeln(Bump, ' ', OptIndex);
  writeln(close(99), ' ', errno);
end.
