program rec;
{$modeswitch advancedrecords}
type
  TR = record
    class constructor Init(n: integer);
    class constructor Again;
  end;
class constructor TR.Init(n: integer); begin end;
begin
end.
