program ok;
function c_labs(x: int64): int64; cdecl; external 'c' name 'labs';
begin
  writeln(c_labs(-7));
end.
