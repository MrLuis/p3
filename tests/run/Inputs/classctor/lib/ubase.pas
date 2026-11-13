unit ubase;
interface
type
  TBase = class
    class var Ready: boolean;
    class constructor Create;
    class destructor Destroy;
  end;
implementation
class constructor TBase.Create;
begin
  Ready := true;
  writeln('TBase class constructor');
end;

class destructor TBase.Destroy;
begin
  writeln('TBase class destructor');
end;

initialization
  writeln('ubase initialization ', TBase.Ready);
end.
