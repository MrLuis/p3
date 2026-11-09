program syntax;
type
  TPlain = record
    x: integer;
    procedure M;
  end;
{$modeswitch advancedrecords}
  TA = record
    class function F: integer;
    procedure G; static;
  end;
  TB = record
  protected
    x: integer;
  end;
  TD = record
    procedure M;
    x: integer;
  end;
  TE = record
    destructor Done;
  end;
  TC = class
    class operator +(a, b: TC): TC;
  end;
begin
end.
