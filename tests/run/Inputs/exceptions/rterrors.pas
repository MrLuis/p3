program rterrors;
{ Unhandled exceptions and other exception errors; argv[1] picks one. }
type
  EParse = class(Exception);
  TPlain = class end;
var
  W: string;
  P: TPlain;

procedure Fail;
begin
  try
    raise EParse.Create('bad token');
  finally
    writeln('finally runs first');
  end;
end;

begin
  W := paramstr(1);
  if W = 'exception' then
    raise Exception.Create('boom');
  if W = 'subclass' then
    Fail;
  if W = 'plain' then
    raise TPlain.Create;
  if W = 'nil' then
    raise P;
  if W = 'reraise' then
    try
      raise EParse.Create('first');
    except
      on E: EParse do
      begin
        writeln('handling ', E.Message);
        raise;
      end;
    end;
  if W = 'unmatched' then
    try
      raise TPlain.Create;
    except
      on E: Exception do writeln('not this');
    end;
  if W = 'divzero' then
    writeln(length(W) div (length(W) - 7));
  if W = 'at' then
    raise EParse.Create('somewhere') at nil;
  if W = 'reraiseat' then
    try
      raise TPlain.Create at nil, nil;
    except
      raise;
    end;
  writeln('no error');
end.
