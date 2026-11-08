program errors;
type
  TRec = record x: integer; end;
var
  i: integer;
  r: TRec;

procedure P;
begin
  raise;
end;

begin
  raise 42;
  raise r;
  raise nil;
  raise;
  try
    i := 1;
  except
    on E: TRec do writeln(E.x);
    on X: Missing do ;
    on E: Exception do E := nil;
  end;
  try
  finally
    exit;
  end;
  for i := 1 to 2 do
    try
    finally
      break;
      continue;
      while true do break;
    end;
  try
  except
    on E: EDivByZero do writeln(E.Message);
  end;
  writeln(E.Message);
end.
