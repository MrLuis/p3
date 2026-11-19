{ Table-based exceptions (optimization2 item 8b5): raise enters the
  unwinder and a catch-all handler runs.
  RUN: %p3 --mode=objfpc --ftable-exceptions %s -o %t
  RUN: %t | diff - %S/table-raise.expected
}
program table_raise;
begin
  try
    raise Exception.Create('x');
  except
    writeln('caught')
  end
end.
