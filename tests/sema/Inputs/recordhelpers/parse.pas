program parse;
{$modeswitch advancedrecords}
type
  TR = record a: longint; end;
  T1 = record helper for TR
    f: longint;
  end;
  T2 = record helper for TR
    constructor Create;
  end;
  T3 = record helper for TR
    const c: longint = 1;
  end;
  T4 = packed record helper for TR end;
  T5 = record helper for TR
    var g: longint;
  end;
{$modeswitch advancedrecords-}
  T6 = record helper for TR end;
  { a field named helper is still a field }
  T7 = record helper: longint; end;
begin
end.
