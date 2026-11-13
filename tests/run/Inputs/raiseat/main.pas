program main;
uses errs;
begin
  try
    RaiseAt(Exception.Create('in program'), @Sites[2], @Sites[1]);
  except
    on E: Exception do writeln(E.Message, ' at ', Site(ExceptAddr));
  end;
  RaiseAt(Exception.Create('unhandled'), nil, nil);
end.
