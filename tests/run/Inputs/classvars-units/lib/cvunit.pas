unit cvunit;
{$H+}
{$modeswitch advancedrecords}
{$modeswitch typehelpers}
interface
type
  TCounter = record
  private
    class var FTotal: longint;
  public
    class var Name: string;
    class procedure Add(n: longint); static;
    class function Total: longint; static;
  end;
  TIntHelper = type helper for longint
    class var Calls: longint;
    function Twice: longint;
  end;
implementation
class procedure TCounter.Add(n: longint); begin FTotal := FTotal + n; end;
class function TCounter.Total: longint; begin Total := FTotal; end;
function TIntHelper.Twice: longint; begin Inc(Calls); Twice := Self * 2; end;
end.
