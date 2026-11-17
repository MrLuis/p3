{$modeswitch advancedrecords}
program errors;
type
  TRec = record
    X: Integer;
    generic function Id<T>(const A: T): T;
    generic function Never<T>(const A: T): T;
    generic class function Ok<T>(const A: T): T; static;
  end;

generic function TRec.Id<T>(const A: T): T;
begin
  Result := A;
end;

generic function TRec.Other<T>(const A: T): T;
begin
  Result := A;
end;

generic class function TRec.Ok<T>(const A: T): T;
begin
  Result := A;
end;

begin
end.
