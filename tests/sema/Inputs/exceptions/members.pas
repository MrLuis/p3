program members;
uses sysutils;
var
  e: Exception;
begin
  e := Exception.Create('m');
  writeln(e.FMessage);
  e := Exception.Create;
  e.Message := 'ok';
end.
