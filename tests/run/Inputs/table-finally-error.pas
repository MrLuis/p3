program table_finally_error;
begin
  try
    raise Exception.Create('first')
  finally
    writeln('f')
  end
end.
