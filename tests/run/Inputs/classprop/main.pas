program main;
uses config;
begin
  TConfig.Name := 'app';
  TConfig.Level := 3;
  writeln(TConfig.Caption);
  TConfig.Level := TConfig.Level + 20;
  writeln(TConfig.Level, ' ', TConfig.Caption);
end.
