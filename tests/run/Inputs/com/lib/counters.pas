unit counters;

interface

type
  ICounter = interface ['{C0000000-0000-0000-0000-000000000001}']
    function Next: integer;
    function Name: string;
  end;

  TCounter = class(TInterfacedObject, ICounter)
  private
    FName: string;
    FValue: integer;
  public
    Partner: ICounter;
    constructor Create(const n: string);
    destructor Destroy; override;
    function Next: integer; virtual;
    function Name: string;
  end;

function NewCounter(const n: string): ICounter;

var
  Last: ICounter;

implementation

constructor TCounter.Create(const n: string);
begin
  FName := n;
  writeln('+ ', n);
end;

destructor TCounter.Destroy;
begin
  writeln('- ', FName);
  inherited Destroy;
end;

function TCounter.Next: integer;
begin
  FValue := FValue + 1;
  Next := FValue;
end;

function TCounter.Name: string;
begin
  Name := FName;
end;

function NewCounter(const n: string): ICounter;
var
  c: ICounter;
begin
  c := TCounter.Create(n);
  Last := c;
  NewCounter := c;
end;

end.
