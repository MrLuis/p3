unit bad;
{$modeswitch advancedrecords}
interface
type
  [Serializable]
  TBad = partial record
    Id: Integer;
    Obj: TObject;
  end;
implementation
end.
