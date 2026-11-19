program astr_take;
type
  TB = class
    function F: ansistring; virtual;
  end;
  TD = class(TB)
    function F: ansistring; override;
  end;
function TB.F: ansistring;
begin
  Result := 'base'
end;
function TD.F: ansistring;
begin
  Result := inherited F
end;
var
  D: TD;
begin
  D := TD.Create;
  writeln(D.F);
  D.Free
end.
