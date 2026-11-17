{ Class constructors and destructors (objfpc), as in Free Pascal: a class
  or advanced record may declare one `class constructor` and one `class
  destructor`, static class procedures without parameters that cannot be
  called. The program runs the class constructors of the types it declares
  (nested ones too) in declaration order before its body, and their class
  destructors at exit (also after halt), in declaration order. A class
  constructor may share its name with a constructor (Create). Up to the
  class destructors, output is identical to Free Pascal 3.2.2's (fpc
  -Mobjfpc), whose own output file is already closed when they run.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: not %t > %t.out
  RUN: diff %t.out %S/classctor.expected
}
program classctor;
{$modeswitch advancedrecords}
type
  TCounter = class
    class var Count: integer;
    class var Log: string;
    class constructor Create;
    class destructor Destroy;
    constructor Create;
  end;
  TDerived = class(TCounter)
    class constructor Init;
  end;
  TPoint = record
    x, y: integer;
    class var Origin: integer;
    class constructor Setup;
    class destructor Teardown;
    function Sum: integer;
  end;
  TOuter = class
  type
    TInner = class
      class constructor Create;
      class destructor Destroy;
    end;
  public
    class constructor Create;
  end;

class constructor TCounter.Create;
begin
  Count := 10;
  Log := Log + 'C';
  writeln('TCounter class constructor');
end;

class destructor TCounter.Destroy;
begin
  writeln('TCounter class destructor: ', Count, ' ', Log);
end;

constructor TCounter.Create;
begin
  Count := Count + 1;
  writeln('TCounter.Create (an instance): ', Count);
end;

class constructor TDerived.Init;
begin
  Log := Log + 'D';
  writeln('TDerived class constructor, Count = ', Count);
end;

class constructor TPoint.Setup;
begin
  Origin := 100;
  writeln('TPoint class constructor');
end;

class destructor TPoint.Teardown;
begin
  writeln('TPoint class destructor: ', Origin);
end;

function TPoint.Sum: integer;
begin
  Sum := x + y + Origin;
end;

class constructor TOuter.Create;
begin
  writeln('TOuter class constructor');
end;

class constructor TOuter.TInner.Create;
begin
  writeln('TInner class constructor');
end;

class destructor TOuter.TInner.Destroy;
begin
  writeln('TInner class destructor');
end;

var
  c: TCounter;
  p: TPoint;
begin
  writeln('main: ', TCounter.Count, ' ', TCounter.Log);
  c := TCounter.Create;
  c.Free;
  c := TDerived.Create;
  c.Free;
  p.x := 1;
  p.y := 2;
  writeln(p.Sum);
  TPoint.Origin := 7;
  halt(1);
  writeln('not reached')
end.
