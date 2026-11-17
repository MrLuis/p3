unit arrays;
{$H+}
{$modeswitch typehelpers}
interface
type
  generic TFunc<T, R> = reference to function(const X: T): R;
  generic TArray<T> = array of T;
  generic TArrayHelper<T> = type helper for array of T
    function Count: Integer;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TArray<R>;
  end;
implementation
function TArrayHelper.Count: Integer;
begin
  Result := Length(Self);
end;
generic function TArrayHelper.Map<R>(F: specialize TFunc<T, R>): specialize TArray<R>;
var
  I: Integer;
begin
  SetLength(Result, Length(Self));
  for I := 0 to High(Self) do
    Result[I] := F(Self[I]);
end;
end.
