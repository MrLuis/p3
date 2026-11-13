program errors;
type
  T = class
  private
    FI: integer;
    class var FV: integer;
    class var FS: string;
    function GetI: integer;
    class function GetC: integer;
    class function GetS: integer; static;
    class procedure SetS(v: integer); static;
  public
    class property A: integer read FI;
    class property B: integer read GetI;
    class property C: integer read GetC;
    class property D: integer read FV write GetS;
    class property E: string read FS;
    class property W: integer write SetS;
    property X: integer read FV;
    class property Q[i: integer]: integer read GetS;
  end;
function T.GetI: integer; begin result := 0 end;
class function T.GetC: integer; begin result := 0 end;
class function T.GetS: integer; begin result := 0 end;
class procedure T.SetS(v: integer); begin end;
begin
  T.E := 'x';
  writeln(T.W);
  T.X;
end.
