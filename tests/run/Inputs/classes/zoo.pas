program zoo;
uses animals;
type
  TDog = class(TAnimal)
  protected
    function Sound: string; override;
  public
    constructor Create;
    destructor Destroy; override;
    function Describe(Prefix: string; Count: integer): string; override;
  end;
  TNode = class;
  TList = class
    Head: TNode;
    procedure Push(V: integer);
    function Sum: integer;
    destructor Destroy; override;
  end;
  TNode = class
    Value: integer;
    Next: TNode;
  end;

constructor TDog.Create;
begin
  inherited Create('dog', 4);
end;
destructor TDog.Destroy;
begin
  writeln('bye ', Name);
  inherited Destroy;
end;
function TDog.Sound: string;
begin
  Result := 'woof (' + inherited Sound + ')';
end;
function TDog.Describe(Prefix: string; Count: integer): string;
begin
  Result := '<' + inherited Describe(Prefix, Count) + '>';
end;

procedure TList.Push(V: integer);
var N: TNode;
begin
  N := TNode.Create;
  N.Value := V;
  N.Next := Head;
  Head := N;
end;
function TList.Sum: integer;
var N: TNode;
begin
  Result := 0;
  N := Head;
  while N <> nil do
  begin
    Result := Result + N.Value;
    N := N.Next;
  end;
end;
destructor TList.Destroy;
var N: TNode;
begin
  while Head <> nil do
  begin
    N := Head;
    Head := Head.Next;
    N.Free;
  end;
  writeln('list freed');
end;

var
  D: TDog;
  A: TAnimal;
  L: TList;
  I: integer;
begin
  D := TDog.Create;
  D.Speak;
  writeln(D.Describe('a ', 2));
  A := D;
  with A do
  begin
    Legs := Legs + 1;
    writeln(Name, ' ', Legs, ' ', ClassName);
    Speak;
  end;
  A := TAnimal.Create('cat', 4);
  A.Speak;
  writeln(A.Describe('the ', 1), ' ', Created);
  A.Destroy;
  D.Destroy;
  L := TList.Create;
  for I := 1 to 10 do L.Push(I);
  writeln(L.Sum);
  L.Free;
end.
