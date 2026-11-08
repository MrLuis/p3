unit parsing;
{ Raises an exception class of its own; its initialization part runs
  protected statements. }
interface
uses sysutils;
type
  EParseError = class(Exception)
    position: integer;
    constructor Create(const msg: string; pos: integer);
  end;
function ParseDigit(c: char): integer;
implementation
constructor EParseError.Create(const msg: string; pos: integer);
begin
  inherited Create(msg);
  position := pos;
end;
function ParseDigit(c: char): integer;
begin
  if (c < '0') or (c > '9') then
    raise EParseError.Create('not a digit: ' + c, ord(c));
  ParseDigit := ord(c) - ord('0');
end;
initialization
  try
    ParseDigit('x');
  except
    on E: EParseError do
      writeln('unit init caught: ', E.Message);
  end;
end.
