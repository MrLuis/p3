program ok;
{$modeswitch advancedrecords}
const
  N = 4;
type
  [Gen(N, 'x')]
  TRec = record
    [Key] A: Integer;
    [Skip, Other(1 + 2)] B, C: Integer;
    [Doc] function F: Integer;
  end;
  [Cls]
  TCls = class
    [Col('c')] FX: Integer;
    [Prop] property X: Integer read FX;
  end;
[Fast]
procedure P;
begin
end;
function TRec.F: Integer;
begin
  Result := 0;
end;
begin
end.
