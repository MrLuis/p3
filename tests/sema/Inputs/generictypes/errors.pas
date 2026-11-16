program errors;
{$H+}{$modeswitch advancedrecords}
type
  generic TBox<T> = class
    Value: T;
    procedure Put(const v: T);
    function Get: T;
    procedure Missing;
  end;
  generic TTwice<T> = record
    v: T;
  end;
  generic TTwice<T> = record
    v: T;
  end;
  generic TDup<T, T> = record
    v: T;
  end;
  generic TSum<T> = record
    a, b: T;
    function Total: T;
  end;
  TRec = record
    x: Integer;
  end;

procedure TBox.Put(const v: T);
begin
  Value := v;
end;

function TBox.Get: T;
begin
  Result := Value + 1;
end;

procedure TBox.Nope;
begin
end;

function TSum.Total: T;
begin
  Result := a + b;
end;

procedure Local;
type
  generic TInner<T> = record
    v: T;
  end;
begin
end;

var
  a: TBox;
  b: specialize TBox<Integer, Integer>;
  c: specialize TNope<Integer>;
  d: specialize TRec<Integer>;
  e: specialize TBox<Integer>;
  f: specialize TBox<string>;
  g: specialize TSum<TRec>;
  h: specialize TBox<Unknown>;
  i: specialize TSum<Integer>;
  j: specialize TSum<Real>;
begin
  e := specialize TBox<Integer>.Create;
  f := e;
  i := j;
  e.Put('text');
  a := TBox.Create;
end.
