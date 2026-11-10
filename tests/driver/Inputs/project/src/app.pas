program app;
{ built by the project in the directory above (mode objfpc, unit paths,
  defines, no unused-variable warnings) }
uses greet;
var
  unused: integer;
begin
{$ifdef GREETING}
  Greet('project');
{$else}
  writeln('no GREETING');
{$endif}
end.
