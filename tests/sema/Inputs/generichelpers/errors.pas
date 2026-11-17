{$modeswitch typehelpers}
program errors;
type
  generic TAll<T> = type helper for T
    function Me: T;
  end;
  generic TTwo<T, U> = type helper for array of T
    function Me: T;
  end;
  generic TFirst<T> = type helper for array of T
    function Count: Integer;
  end;
  { declared last: the one array types get (TFirst's Count is hidden) }
  generic TSecond<T> = type helper for array of T
    function Which: Integer;
  end;
  TInts = array of Integer;

function TAll.Me: T; begin Result := Self; end;
function TTwo.Me: T; begin Result := Self[0]; end;
function TFirst.Count: Integer; begin Result := Length(Self); end;
function TSecond.Which: Integer; begin Result := 2; end;

var
  i: TInts;
  r: Real;
begin
  Writeln(i.Which);
  Writeln(i.Count);
  Writeln(r.Me);
end.
