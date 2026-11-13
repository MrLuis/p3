program errors;
type
  T = class
    class constructor A;
    class constructor B;
    class destructor D(x: integer);
    procedure A;
  end;
  T2 = class
    class constructor Init;
    class destructor Done; virtual;
  end;
  T3 = class
    class constructor Init;
  end;
class constructor T.A; begin end;
class destructor T.D(x: integer); begin end;
procedure T.A; begin end;
class constructor T2.Wrong; begin end;
class destructor T2.Done; begin Self := nil end;
class procedure T3.Init; begin end;
begin
  T2.Init
end.
