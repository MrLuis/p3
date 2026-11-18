program missingname;
function missing1: longint; cdecl; external 'p3_no_such_library';
begin
  writeln(missing1);
end.
