program errors;
{$modeswitch advancedrecords}
type
  TOuter = class
  strict private
    const Secret = 1;
  private
    const Cap = 64;
    type TInner = record v: integer; end;
    type TFwd = class;
    type IBad = interface end;
  public
    const Cap = 2;
    type TList = array of integer;
  end;
  TRec = record
    const Max = 3;
  end;
var
  a: TOuter.TNope;
  b: TOuter.Cap;
  c: TInner;
  d: integer;
  e: TRec.TNone;
  o: TOuter;
procedure TOuter.TNope.Run; begin end;
begin
  d := TOuter.Secret;
  d := TOuter.TList;
  d := o.Cap + TRec.Max;
  d := TOuter.Missing;
  o := TOuter.TList(1);
  d := TOuter.Cap(2);
end.
