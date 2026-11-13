{ Class properties (objfpc), as in Free Pascal: `class property P: T read
  R write W;` in a class or an advanced record, whose accessors are class
  variables or static class methods (array properties and `index` too). A
  class property is used through the type name, a class reference, an
  object, a descendant, Self, and unqualified in static, class and
  instance methods. Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/classprop.expected
}
program classprop;
{$modeswitch advancedrecords}
type
  TApp = class
  private
    class var FBuild: integer;
    class var FTitle: string;
    class var FNames: array[0..2] of string;
    class function GetName(i: integer): string; static;
    class procedure SetName(i: integer; const v: string); static;
    class function GetTwice: integer; static;
    class procedure SetBuild(v: integer); static;
    class function GetFlag(i: integer): boolean; static;
  public
    class property Build: integer read FBuild write SetBuild;
    class property Twice: integer read GetTwice;
    class property Title: string read FTitle write FTitle;
    class property Names[i: integer]: string read GetName write SetName;
    class property Odd: boolean index 1 read GetFlag;
    class property Even: boolean index 0 read GetFlag;
    class procedure ShowStatic; static;
    class procedure ShowClass;
    procedure Show;
  end;
  TAppClass = class of TApp;
  TSub = class(TApp)
  end;
  TCounter = record
    class var FCount: integer;
    class function GetDouble: integer; static;
    class property Count: integer read FCount write FCount;
    class property Double: integer read GetDouble;
    procedure Bump;
  end;

class function TApp.GetName(i: integer): string;
begin
  GetName := FNames[i];
end;

class procedure TApp.SetName(i: integer; const v: string);
begin
  FNames[i] := v;
end;

class function TApp.GetTwice: integer;
begin
  GetTwice := FBuild * 2;
end;

class procedure TApp.SetBuild(v: integer);
begin
  writeln('SetBuild ', v);
  FBuild := v;
end;

class function TApp.GetFlag(i: integer): boolean;
begin
  GetFlag := FBuild mod 2 = i;
end;

class procedure TApp.ShowStatic;
begin
  writeln('static: ', Build, ' ', Twice, ' ', Title);
end;

class procedure TApp.ShowClass;
begin
  writeln('class: ', Build, ' ', Self.Twice);
end;

procedure TApp.Show;
begin
  writeln('instance: ', Build, ' ', Self.Title, ' ', Names[1]);
end;

class function TCounter.GetDouble: integer;
begin
  GetDouble := FCount * 2;
end;

procedure TCounter.Bump;
begin
  Count := Count + 1;
end;

var
  a: TApp;
  c: TAppClass;
  r: TCounter;
begin
  TApp.Build := 5;
  writeln(TApp.Build, ' ', TApp.Twice, ' ', TApp.Odd, ' ', TApp.Even);
  TApp.Title := 'pcc';
  TApp.Title := TApp.Title + ' compiler';
  writeln(TApp.Title, ' ', length(TApp.Title));
  TApp.Names[1] := 'one';
  writeln(TApp.Names[1], ' [', TApp.Names[0], ']');
  a := TApp.Create;
  a.Build := 7;
  writeln(a.Build, ' ', TSub.Build, ' ', TSub.Twice);
  c := TSub;
  writeln(c.Twice, ' ', c.Title);
  TApp.ShowStatic;
  TSub.ShowClass;
  a.Show;
  a.Free;
  TCounter.Count := 3;
  r.Bump;
  writeln(TCounter.Count, ' ', TCounter.Double, ' ', r.Count, ' ', r.Double);
end.
