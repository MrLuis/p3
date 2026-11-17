program e1;
type
  TFoo = class
    generic function Id<T>(const X: T): T;
    generic function Bad<T>(const X: T): T; virtual;
    generic function Never<T>(const X: T): T;
    generic function Early<T>(const X: T): T;
    generic function Dup<T>(const X: T): T;
    generic function Dup<U>(const X: U): U;
    generic function Two<T, T>(const X: T): T;
    generic function Shape<T>(const X: T): T;
    generic function NoArg<T>: T;
    procedure UseEarly;
  end;

procedure TFoo.UseEarly;
begin
  Writeln(Early(1));
end;

generic function TFoo.Id<T>(const X: T): T;
begin
  Result := X;
end;

generic function TFoo.Bad<T>(const X: T): T;
begin
  Result := X;
end;

generic function TFoo.Early<T>(const X: T): T;
begin
  Result := X;
end;

generic function TFoo.Shape<T>(var X: T): T;
begin
  Result := X;
end;

generic function TFoo.Other<T>(const X: T): T;
begin
  Result := X;
end;

generic function TFoo.NoArg<T>: T;
begin
end;

generic function TFoo.Id<T, U>(const X: T): T;
begin
  Result := X;
end;

var
  f: TFoo;
  i: Integer;
begin
  f := TFoo.Create;
  i := f.Id;
  i := f.specialize Id<Integer, Integer>(1);
  i := f.NoArg;
  i := f.specialize NoArg<Integer>;
  i := f.Id(1, 2);
  i := TFoo.Id(1);
  i := f.specialize Id<string>(1);
end.
