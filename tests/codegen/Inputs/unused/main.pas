program main;
uses tools;
type
  TSquare = class(TShape)
    Side: Integer;
    function Area: Integer; override;
  end;
var
  Callback: function(N: Integer): Integer;
  S: TShape;

function Unused(N: Integer): Integer;
begin
  writeln('never called');
  Unused := N;
end;

{ only called by Unused2, which nothing calls }
procedure UnusedHelper;
begin
  writeln('never called either');
end;

procedure Unused2;
begin
  UnusedHelper;
end;

{ its address is taken }
function Negate(N: Integer): Integer;
begin
  Negate := -N;
end;

function TSquare.Area: Integer;
begin
  Area := Side * Side;
end;

begin
  Callback := @Negate;
  S := TSquare.Create;
  TSquare(S).Side := 3;
  writeln(Twice(4), ' ', Callback(5), ' ', S.Area, ' ', S.Name);
  S.Free;
end.
