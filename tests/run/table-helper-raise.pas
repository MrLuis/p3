{ An exception raised in an uninstrumented helper reaches the caller's
  table-protected region (item 8b6).
  RUN: %p3 --mode=objfpc --ftable-exceptions %s -o %t
  RUN: %t | diff - %S/table-helper-raise.expected
}
program table_helper_raise;
procedure Boom;
begin
  raise Exception.Create('boom')
end;
begin
  try
    Boom
  except
    writeln('caught')
  end
end.
