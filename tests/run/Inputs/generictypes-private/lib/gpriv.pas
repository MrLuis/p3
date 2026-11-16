unit gpriv;
interface
type
  generic TCell<T> = class
    Value: T;
    procedure Put(const v: T);
  end;
function Puts: Integer;
implementation
var
  count: Integer;   { the unit's own: the methods' bodies use it }
procedure TCell.Put(const v: T);
begin
  count := count + 1;
  Value := v;
end;
function Puts: Integer;
begin
  Puts := count;
end;
end.
