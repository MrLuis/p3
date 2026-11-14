unit shapes;

interface

type
  TShape = class
    name: string;
    constructor Create(const n: string);
    destructor Destroy; override;
  end;

{ The caller owns the result. }
function MakeShape(const n: string): TShape;
{ Makes two shapes and frees them; raises if fail. }
procedure UseShapes(fail: boolean);

implementation

constructor TShape.Create(const n: string);
begin
  name := n;
  writeln('create ', name);
end;

destructor TShape.Destroy;
begin
  writeln('destroy ', name);
  inherited Destroy;
end;

function MakeShape(const n: string): TShape;
var
  s: TShape owned;
begin
  s := TShape.Create(n);
  MakeShape := move s;
end;

procedure UseShapes(fail: boolean);
var
  a, b: TShape owned;
begin
  a := MakeShape('circle');
  b := MakeShape('square');
  if fail then
    raise Exception.Create('failed');
  writeln('used');
end;

end.
