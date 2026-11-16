program ast;
type
  generic TBox<T> = class
    Value: T;
    function Get: T;
  end;
  TIntBox = specialize TBox<Integer>;
function TBox.Get: T;
begin
  Result := Value;
end;
var
  b: TIntBox;
  s: specialize TBox<string>;
begin
  b := TIntBox.Create;
  s := specialize TBox<string>.Create;
end.
