program digits;
uses parsing;
var
  s: string;
  i, sum: integer;
begin
  s := '12a4';
  sum := 0;
  for i := 1 to length(s) do
    try
      sum := sum + ParseDigit(s[i]);
    except
      on E: EParseError do
        writeln(E.ClassName, ' at ', i, ': ', E.Message, ' (', E.position, ')');
    end;
  writeln('sum ', sum);
  ParseDigit('?');
end.
