unit utwo;
interface
uses ubase;
type
  T1 = class
    class constructor Create;
    class destructor Destroy;
  end;
implementation
type
  T2 = class
    class constructor Create;
    class destructor Destroy;
  end;

class constructor T1.Create;
begin
  writeln('T1 class constructor ', TBase.Ready);
end;

class destructor T1.Destroy;
begin
  writeln('T1 class destructor');
end;

class constructor T2.Create;
begin
  writeln('T2 class constructor');
end;

class destructor T2.Destroy;
begin
  writeln('T2 class destructor');
end;

initialization
  writeln('utwo initialization');
end.
