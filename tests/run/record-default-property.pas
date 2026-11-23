{ An advanced record's default array property: r[i] reads and writes
  through it, on a variable, a field, a function result and in chains.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/record-default-property.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/record-default-property.expected
}
program recorddefaultproperty;
{$H+}
{$modeswitch advancedrecords}

type
  PNode = ^TNode;
  TNode = record
    Name: string;
    Kids: array of PNode;
  end;

  TKey = record
    Name: string;
    Index: Integer;
    class operator :=(const S: string): TKey;
    class operator :=(I: Integer): TKey;
  end;

  { a handle on a node: v[0], v['b'], v[0][1] }
  TView = record
  private
    N: PNode;
    function Get(const K: TKey): TView;
    procedure Put(const K: TKey; const V: TView);
  public
    function Name: string;
    property Items[const K: TKey]: TView read Get write Put; default;
  end;

  TGrid = record
    Cells: array[0..2, 0..2] of Integer;
    function GetCell(R, C: Integer): Integer;
    procedure SetCell(R, C: Integer; V: Integer);
    property Cell[R, C: Integer]: Integer read GetCell write SetCell; default;
  end;

  THolder = record
    G: TGrid;
  end;

class operator TKey.:=(const S: string): TKey;
begin
  Result.Name := S;
  Result.Index := -1;
end;

class operator TKey.:=(I: Integer): TKey;
begin
  Result.Name := '';
  Result.Index := I;
end;

function TView.Get(const K: TKey): TView;
var
  I: Integer;
begin
  Result.N := nil;
  if K.Index >= 0 then
    Result.N := N^.Kids[K.Index]
  else
    for I := 0 to High(N^.Kids) do
      if N^.Kids[I]^.Name = K.Name then
        Result.N := N^.Kids[I];
end;

procedure TView.Put(const K: TKey; const V: TView);
begin
  N^.Kids[K.Index] := V.N;
end;

function TView.Name: string;
begin
  if N = nil then
    Result := '(none)'
  else
    Result := N^.Name;
end;

function TGrid.GetCell(R, C: Integer): Integer;
begin
  Result := Cells[R, C];
end;

procedure TGrid.SetCell(R, C: Integer; V: Integer);
begin
  Cells[R, C] := V;
end;

function View(P: PNode): TView;
begin
  Result.N := P;
end;

function NewNode(const Name: string): PNode;
var
  P: PNode;
begin
  New(P);
  P^.Name := Name;
  Result := P;
end;

var
  Root, A, B, C: PNode;
  V: TView;
  G: TGrid;
  H: THolder;
begin
  Root := NewNode('root');
  A := NewNode('a');
  B := NewNode('b');
  C := NewNode('c');
  SetLength(Root^.Kids, 2);
  Root^.Kids[0] := A;
  Root^.Kids[1] := B;
  SetLength(A^.Kids, 1);
  A^.Kids[0] := C;
  V := View(Root);
  WriteLn(V[0].Name, ' ', V['b'].Name, ' ', V['x'].Name);
  WriteLn(V[0][0].Name, ' ', V['a'][0].Name, ' ', View(Root)[0][0].Name);
  V[1] := View(C);
  WriteLn(V[1].Name);
  G[1, 2] := 7;
  G[2, 2] := G[1, 2] * 2;
  WriteLn(G[1, 2], ' ', G[2, 2]);
  H.G[0, 0] := 5;
  WriteLn(H.G[0, 0]);
end.
