program classes;
type
  TBase = class;
  TBase = class
  private
    FX: integer;
  protected
    procedure SetX(V: integer); virtual;
  public
    constructor Create(AX: integer);
    destructor Destroy; override;
    function Twice: integer; virtual; abstract;
    property X: integer read FX write SetX;
  published
    Tag: string;
  end;
  TDerived = class(TBase)
    function Twice: integer; override;
  end;
  TEmpty = class(TDerived);
constructor TBase.Create(AX: integer);
begin
  inherited Create;
  X := AX;
end;
destructor TBase.Destroy;
begin
  inherited;
end;
procedure TBase.SetX(V: integer);
begin
  FX := V;
end;
function TDerived.Twice: integer;
begin
  Result := X * 2;
end;
var
  B: TBase;
begin
  B := TDerived.Create(2);
  if (B is TDerived) and ((B as TDerived).Twice > 0) then
    (B as TEmpty).Tag := 'x';
  TDerived(B).X := 3;
  with B do X := X + 1;
  B.Free;
end.
