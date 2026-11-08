program classsyntax;
type
  TA = class of TObject;
  TB = class(TObject, IFoo) end;
  TC = class
    strict private X: integer;
  end;
  TD = class
    procedure P; overload;
  end;
  TE = class
    property Items[I: integer]: integer read X;
  end;
  TF = class
    property Y: integer read X default 1;
  end;
  TG = class
    class procedure P;
  end;
  TH = class
    123;
  end;
  TI = class
    constructor Create: integer;
  end;
constructor Make;
begin
end;
procedure TA.P;
begin
  inherited;
end;
begin
end.
