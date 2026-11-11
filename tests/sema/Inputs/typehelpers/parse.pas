program parse;
{$H+}
{$modeswitch typehelpers}
type
  TA = type helper for longint
    x: longint;
  end;
  TB = type helper for longint
    constructor Create;
  end;
  TC = type helper for longint
    class function NotStatic: longint;
  end;
  TD = type helper for longint
    const Typed: longint = 1;
  end;
  TE = type helper for longint
    var y: longint;
  end;
  {$modeswitch typehelpers-}
  TOff = type helper for byte end;
  TOk = type longint;
begin
end.
