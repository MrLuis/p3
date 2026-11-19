{ Table exceptions with a body that does not raise (item 8b6).
  RUN: %p3 --mode=objfpc --ftable-exceptions %s -o %t
  RUN: %t | diff - %S/table-no-raise.expected
}
program table_no_raise;
procedure P; begin end;
begin
  try
    P;
    writeln('ok')
  except
    writeln('bad')
  end
end.
