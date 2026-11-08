program exceptionsyntax;
begin
  try
    writeln;
  end;
  try
  except
    on : Exception do ;
  end;
  try
  except
    on E: Exception writeln;
  end;
  try
  except
    on E: Exception do writeln;
    writeln
  end;
  try
  finally
    writeln
  writeln;
end.
