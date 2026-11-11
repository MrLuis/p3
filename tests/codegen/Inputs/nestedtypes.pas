program nested;
{$modeswitch advancedrecords}
type
  TOuter = class
  public
    const Limit: longint = 7;
    type
      TInner = record
        v: longint;
        function Twice: longint;
      end;
      TNode = class
        function Name: longint; virtual;
      end;
    class var Count: longint;
  end;
function TOuter.TInner.Twice: longint; begin result := v * 2 end;
function TOuter.TNode.Name: longint; begin result := Count end;
var
  r: TOuter.TInner;
  n: TOuter.TNode;
begin
  n := TOuter.TNode.Create;
  r.v := TOuter.Limit;
  writeln(r.Twice, n.Name);
end.
