{ Nested table try statements (optimization2 item 8d3): an inner finally
  re-raises into an outer catch, and an inner finally runs on the normal
  path.
  RUN: %p3 --mode=objfpc --ftable-exceptions %s -o %t
  RUN: %t | diff - %S/table-nested.expected
}
program table_nested;
begin
  try
    try
      raise Exception.Create('x')
    finally
      writeln('f')
    end
  except
    writeln('c')
  end;
  try
    try
      writeln('body')
    finally
      writeln('inner')
    end
  except
    writeln('wrong')
  end
end.
