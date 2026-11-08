program exceptions;
uses sysutils;
type
  EMine = class(Exception);
var
  i: integer;
begin
  try
    i := 1;
    raise EMine.Create('x')
  except
    on E: EMine do writeln(E.Message);
    on Exception do
      raise;
  else
    writeln('else');
    i := 2
  end;
  try
    i := 3;
  except
    writeln('all');
    raise
  end;
  try
  except
  end;
  try
    try
      i := 4;
    finally
      i := 5;
    end;
  except
    on E: EMine do ;
  end;
end.
