program main;
uses utwo, ubase;
type
  TMain = class
    class constructor Create;
    class destructor Destroy;
  end;

class constructor TMain.Create;
begin
  writeln('TMain class constructor');
end;

class destructor TMain.Destroy;
begin
  writeln('TMain class destructor');
end;

begin
  writeln('main');
end.
