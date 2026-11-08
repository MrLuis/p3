{ Class references, class methods and class variables (--mode=objfpc):
  class of T, TClass, virtual constructors called through class
  references, virtual class methods (through classes, class references and
  objects), Self in class methods, class variables shared by descendants,
  ClassType, ClassName, ClassParent, InheritsFrom, exceptions created
  through class references. Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/classrefs.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/classrefs.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/classrefs.expected %t.g.out
}
program classrefs;

uses sysutils;

type
  TShape = class
  private
    class var Count: integer;
  public
    Name: string;
    constructor Create(const n: string); virtual;
    destructor Destroy; override;
    class function Kind: string; virtual;
    class function Sides: integer; virtual; abstract;
    class procedure Bump;
    class function Make(const n: string): TShape;
    class function Describe: string;
    procedure Show;
  end;
  TShapeClass = class of TShape;

  TCircle = class(TShape)
    constructor Create(const n: string); override;
    class function Kind: string; override;
    class function Sides: integer; override;
  end;

  TPolygon = class(TShape)
  public
    class var Made: integer;
    class function Sides: integer; override;
  end;

  TSquare = class(TPolygon)
    class function Kind: string; override;
    class function Sides: integer; override;
  end;

  TBroken = class(TShape)
    constructor Create(const n: string); override;
    class function Sides: integer; override;
  end;

  EShape = class(Exception);
  EShapeClass = class of EShape;
  ESevere = class(EShape);

constructor TShape.Create(const n: string);
begin
  Name := n;
  Bump;
  writeln('  TShape.Create ', n, ' as ', ClassName);
end;

destructor TShape.Destroy;
begin
  writeln('  destroy ', Name, ' (', ClassName, ')');
  inherited Destroy;
end;

class function TShape.Kind: string;
begin
  Kind := 'shape';
end;

class procedure TShape.Bump;
begin
  Count := Count + 1;
end;

{ A constructor called through Self creates an object of Self's class. }
class function TShape.Make(const n: string): TShape;
begin
  Make := Create(n);
end;

class function TShape.Describe: string;
begin
  Describe := Self.ClassName + ': ' + Kind + ', ' + chr(ord('0') + Sides) +
              ' sides';
end;

procedure TShape.Show;
begin
  writeln('  ', Name, ' is a ', Kind, ' (', ClassName, ', ', Sides, ' sides)');
end;

constructor TCircle.Create(const n: string);
begin
  inherited Create(n + '!');
  writeln('  TCircle.Create');
end;

class function TCircle.Kind: string;
begin
  Kind := 'circle/' + inherited Kind;
end;

class function TCircle.Sides: integer;
begin
  Sides := 0;
end;

class function TPolygon.Sides: integer;
begin
  Made := Made + 1;
  Sides := 5;
end;

class function TSquare.Kind: string;
begin
  Kind := 'square';
end;

class function TSquare.Sides: integer;
begin
  Sides := inherited Sides - 1;
end;

constructor TBroken.Create(const n: string);
begin
  inherited Create(n);
  raise EShape.Create('cannot make ' + n);
end;

class function TBroken.Sides: integer;
begin
  Sides := 1;
end;

{ A class reference as a parameter. }
function Build(c: TShapeClass; const n: string): TShape;
begin
  writeln('build a ', c.ClassName, ' (', c.Kind, ')');
  Build := c.Create(n);
end;

procedure Fail(e: EShapeClass; const msg: string);
begin
  raise e.Create(msg);
end;

var
  all: array[0..2] of TShapeClass;
  c: TShapeClass;
  cls: TClass;
  s, t: TShape;
  i: integer;

begin
  all[0] := TCircle;
  all[1] := TSquare;
  all[2] := TPolygon;
  for i := 0 to 2 do
  begin
    s := Build(all[i], 's' + chr(ord('0') + i));
    s.Show;
    writeln('  ', s.ClassType = all[i], ' ', s.ClassType.ClassName, ' ',
            s.InheritsFrom(TShape), ' ', all[i].InheritsFrom(TPolygon), ' ',
            s is TPolygon);
    s.Free;
  end;

  writeln(TSquare.Describe);
  writeln(TCircle.Describe);
  t := TSquare.Make('made');
  t.Show;
  writeln('counts ', TShape.Count, ' ', TSquare.Count, ' ', t.Count,
          ', polygons asked ', TPolygon.Made, ' ', TSquare.Made);
  TSquare.Made := 40;
  writeln('made ', TPolygon.Made);
  t.Free;

  cls := TSquare;
  writeln(cls.ClassName, ' < ', cls.ClassParent.ClassName, ' < ',
          cls.ClassParent.ClassParent.ClassName, ' < ',
          cls.ClassParent.ClassParent.ClassParent.ClassName);
  writeln(cls = TSquare, ' ', cls <> TCircle, ' ',
          TShapeClass(cls).Kind, ' ', cls.InheritsFrom(TShape));
  c := nil;
  writeln('nil: ', c = nil);
  writeln(TObject.ClassName, ' ', TSquare.InstanceSize = TPolygon.InstanceSize,
          ' ', TShape.InstanceSize > TObject.InstanceSize);

  c := TBroken;
  try
    s := c.Create('broken');
    writeln('not reached');
  except
    on E: EShape do writeln('caught ', E.ClassName, ': ', E.Message);
  end;
  try
    Fail(ESevere, 'severe');
  except
    on E: EShape do writeln('caught ', E.ClassName, ': ', E.Message);
  end;
  writeln('count ', TShape.Count);
end.
