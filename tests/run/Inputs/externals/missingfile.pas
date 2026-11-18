program missingfile;
function missing2: longint; cdecl; external 'libp3_no_such.so.1' name 'missing2';
begin
  writeln(missing2);
end.
