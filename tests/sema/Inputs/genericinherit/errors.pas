program e1;
type
  IThing = interface
    function GetX: Integer;
    procedure SetX(V: Integer);
    function GetS: string;
    property X: Integer read GetX write SetX;
    property Bad: Integer read GetS;
    property Missing: Integer read Nope;
    property WrongSet: Integer read GetX write GetX;
    property Items[I: Integer]: Integer read GetX;
    property X: Integer read GetX;
    property NoAccess: Integer;
  end;
  TPlain = class
  end;
  generic TBox<T> = record
    V: T;
  end;
  TBad = class(specialize TBox<Integer>)
  end;
var
  t: IThing;
begin
  t.X := 3;
  Writeln(t.X, t.Nothing);
end.
