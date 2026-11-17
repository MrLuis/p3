{$modeswitch advancedrecords}
program i2;
type
  generic TBox<T> = record
    Value: T;
  end;
  generic TOther<T> = record
    Value: T;
  end;
generic function Same<T>(const X, Y: specialize TBox<T>): Boolean;
begin
  Result := X.Value = Y.Value;
end;
generic function Unbox<T>(const B: specialize TBox<T>): T;
begin
  Result := B.Value;
end;
var
  bi: specialize TBox<Integer>;
  bc: specialize TBox<Char>;
  o: specialize TOther<Integer>;
  i: Integer;
begin
  Writeln(Same(bi, bc));
  i := Unbox(o);
  i := Unbox(5);
end.
