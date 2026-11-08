program propertysyntax;
type
  TA = class
    strict public X: integer;
  end;
  TB = class
    property P[i: integer: integer read G;
  end;
  TC = class
    property Q: integer read F stored true;
  end;
  TD = class
    property R[]: integer read G;
  end;
begin
end.
