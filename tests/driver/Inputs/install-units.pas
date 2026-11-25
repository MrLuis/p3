program installunits;
uses sysutils, classes;
var
  L: TStringList;
begin
  L := TStringList.Create;
  L.Add('moved');
  L.Add('installed');
  L.Sort;
  writeln(UpperCase(L[0]), ' ', L[1]);
  L.Free;
end.
