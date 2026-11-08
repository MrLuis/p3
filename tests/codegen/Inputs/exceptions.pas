program exceptions;
type
  TThing = class
    constructor Make;
  end;
var
  t: TThing;

constructor TThing.Make;
begin
end;

procedure P(n: integer);
begin
  try
    try
      if n = 1 then
        exit;
      raise Exception.Create('x');
    finally
      writeln('f');
    end;
  except
    on E: Exception do
      writeln(E.Message);
  end;
end;

begin
  P(1);
  t := TThing.Make;
end.
