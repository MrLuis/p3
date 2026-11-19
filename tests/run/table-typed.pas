{ Typed on-handlers under table exceptions (optimization2 item 8c).
  RUN: %p3 --mode=objfpc --ftable-exceptions %s -o %t
  RUN: %t | diff - %S/table-typed.expected
}
program table_typed;
type
  EA = class(Exception) end;
  EB = class(EA) end;
begin
  try
    raise EB.Create('boom')
  except
    on X: EA do writeln('EA ', X.Message);
    on X: Exception do writeln('wrong')
  end;
  try
    raise Exception.Create('y')
  except
    on X: EA do writeln('wrong')
  else
    writeln('else')
  end
end.
