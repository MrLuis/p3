{ Table-based finally and non-local exits (optimization2 item 8d1):
  continue and break run the finally part before leaving.
  RUN: %p3 --mode=objfpc --ftable-exceptions %s -o %t
  RUN: %t | diff - %S/table-finally.expected
}
program table_finally;
var i: longint;
begin
  try
    writeln('a')
  finally
    writeln('b')
  end;
  for i := 1 to 3 do
  begin
    try
      if i = 2 then continue;
      write(i)
    finally
      write('f')
    end;
  end;
  for i := 1 to 5 do
  begin
    try
      if i = 3 then break;
      write(i)
    finally
      write('g')
    end;
  end;
  writeln
end.
