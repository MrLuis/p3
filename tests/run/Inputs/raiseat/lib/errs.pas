unit errs;
{ A library's error helper: raises E at an address its caller gives. }
interface
var
  Sites: array[1..3] of Integer;
procedure RaiseAt(E: TObject; Addr, Frame: Pointer);
function Site(P: Pointer): Integer;
implementation
procedure RaiseAt(E: TObject; Addr, Frame: Pointer);
begin
  raise E at Addr, Frame;
end;
function Site(P: Pointer): Integer;
var
  K: Integer;
begin
  Site := 0;
  for K := 1 to 3 do
    if P = Pointer(@Sites[K]) then
      Site := K;
end;
begin
  try
    RaiseAt(Exception.Create('in unit'), @Sites[3], nil);
  except
    on E: Exception do writeln(E.Message, ' at ', Site(ExceptAddr));
  end;
end.
