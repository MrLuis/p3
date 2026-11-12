program resultoff;
{$modeswitch result-}
{$modeswitch out off}
{$modeswitch class}
{$modeswitch exceptions+}
function Plain(x: integer): integer;
var
  result: integer; { its own variable }
begin
  result := x + 1;
  Plain := result * 3;
end;
var
  out: integer; { just a name }
begin
  out := Plain(4);
  writeln(out);
end.
