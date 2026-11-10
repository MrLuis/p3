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

type
  TRisky = class
    constructor Make(n: integer);
  end;
var
  r: TRisky;

constructor TRisky.Make(n: integer);
begin
  if n = 0 then
    raise Exception.Create('no');
end;

begin
  P(1);
  t := TThing.Make;
  r := TRisky.Make(1);
end.
