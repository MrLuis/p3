program exception;
uses sysutils;
begin
  try
    raise Exception.Create('x');
  except
    on e: Exception do writeln('caught');
  end;
end.
