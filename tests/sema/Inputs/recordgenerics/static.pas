{$modeswitch advancedrecords}
program static;
type
  TRec = record
    generic class function Make<T>(const A: T): T;
  end;
begin
end.
