{ Table exception boundary matrix (optimization2 item 8d4): exit through
  finally, typed handler, and an exception from a nested routine.
  RUN: %p3 --mode=objfpc --ftable-exceptions %s -o %t
  RUN: %t | diff - %S/table-stress.expected
}
program table_stress;
type EC = class(Exception) end;
procedure P;
begin
  try
    writeln('a');
    exit
  finally
    writeln('f')
  end;
  writeln('bad')
end;
procedure Q;
  procedure Inner;
  begin
    raise EC.Create('nested')
  end;
begin
  try
    Inner
  except
    writeln('nested caught')
  end
end;
begin
  P;
  try
    raise EC.Create('x')
  except
    on E: EC do writeln('handler')
  end;
  Q
end.
