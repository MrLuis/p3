program hello;
{ Greets the names on the command line (or the world), using a unit of the
  project (src/lib/greeting.pas) and one of pcc's library (sysutils). }
uses sysutils, greeting;
var
  i: Integer;
  g: TGreeter;
begin
  g := TGreeter.Create('Hello');
  try
    if ParamCount = 0 then
      writeln(g.Greet('world'))
    else
      for i := 1 to ParamCount do
        writeln(g.Greet(ParamStr(i)));
    writeln(IntToStr(g.Count), ' greeting(s)');
  finally
    g.Free;
  end;
end.
