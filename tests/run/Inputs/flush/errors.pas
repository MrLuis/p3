program errors;
var
  f: text;
begin
  assign(f, paramstr(2));
  if paramstr(1) = 'closed' then
    flush(f)
  else if paramstr(1) = 'reading' then
  begin
    rewrite(f);
    writeln(f, 'x');
    close(f);
    reset(f);
    flush(f);
  end
  else if paramstr(1) = 'input' then
    flush(input)
  else
  begin
    rewrite(f);
    flush(f);
    close(f);
    writeln('fine');
  end;
end.
