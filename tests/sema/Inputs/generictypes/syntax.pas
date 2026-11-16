program syntax;
type
  generic TNoParams = record
    x: Integer;
  end;
  generic TEmpty<> = record
    x: Integer;
  end;
  generic TBound<T: TObject> = record
    x: T;
  end;
var
  a: specialize TBound;
  b: specialize TBound<Integer;
begin
end.
