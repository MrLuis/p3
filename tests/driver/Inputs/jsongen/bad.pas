program bad;
{$H+}
{$modeswitch advancedrecords}
uses json;
type
  [Json]
  TBad = partial record
    Ok: Integer;
    P: Pointer;
    Fixed: array[1..3] of Integer;
    [Skip] Hidden: Pointer;
  end;
begin
end.
