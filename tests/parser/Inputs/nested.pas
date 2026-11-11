program nested;
{$modeswitch advancedrecords}
type
  TOuter = class
  private
    const Cap = 4;
    type
      TInner = record
        v: integer;
        function Twice: integer;
      end;
  public
    function Make: TInner;
  end;
var
  x: TOuter.TInner;
function TOuter.TInner.Twice: integer; begin result := v * 2 end;
function TOuter.Make: TInner; begin result.v := Cap end;
begin
  case 4 of TOuter.Cap: x.v := 1; end;
end.
