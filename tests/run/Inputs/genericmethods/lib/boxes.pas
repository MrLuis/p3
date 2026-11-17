unit boxes;
{$H+}
interface
type
  TBox = class
  private
    FName: string;
  public
    constructor Create(const N: string);
    generic function Wrap<T>(const X: T): string;
    function Plain: string;
  end;
  generic TCell<T> = class
    Value: T;
    generic function Convert<R>(const F: R): R;
  end;
implementation
constructor TBox.Create(const N: string);
begin
  FName := N;
end;
function TBox.Plain: string;
begin
  Result := FName;
end;
generic function TBox.Wrap<T>(const X: T): string;
begin
  Result := '[' + FName + ' ' + Plain + ']';
  Writeln(X);
end;
generic function TCell.Convert<R>(const F: R): R;
begin
  Result := F;
end;
end.
