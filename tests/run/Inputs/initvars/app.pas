program app;
uses settings;
begin
  writeln(Verbose, ' ', Level, ' ', Title, ' ', NextId, ' ', NextId);
  Level := Level + 1;
  writeln(Level);
end.
