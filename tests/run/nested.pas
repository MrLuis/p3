{ Constants and types declared in classes and advanced records (objfpc), as
  in Free Pascal: used unqualified in the class, its methods, its
  descendants and the classes and records it declares, and as TOuter.Name
  elsewhere (types, constants, constructors, is, case labels, sizeof, for
  ... in); nested records with methods (TOuter.TInner.M), nested classes
  with virtual methods, typed constants, a forward nested class. Output is
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/nested.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/nested.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/nested.expected
}
program nested;
{$H+}
{$modeswitch advancedrecords}
{$macro on}
{$define TGenericStringBuilder:=TAnsiStringBuilder}
{$define SBChar:=AnsiChar}
uses sysutils;

type
  { the shape of Free Pascal's TStringBuilder }
  TGenericStringBuilder = class
  private
    const
      DefaultCapacity = 8;
    type
      TCharEnumerator = record
      private
        FBuilder: TGenericStringBuilder;
        FPosition: SizeInt;
        function GetCurrent: SBChar; inline;
      public
        function MoveNext: Boolean; inline;
        property Current: SBChar read GetCurrent;
      end;
  private
    FData: array of SBChar;
    FLength: SizeInt;
    function GetCapacity: SizeInt;
  public
    constructor Create;
    procedure Append(const s: string);
    function Text: string;
    function GetEnumerator: TCharEnumerator;
    property Capacity: SizeInt read GetCapacity;
  end;

  TTree = class
  public
    const
      Title = 'tree';
      Limit = 3;
      Counter: longint = 100; { a typed constant: static storage }
    type
      TKind = (kNode, kLeaf);
      TNode = class;
      TNodes = array of TNode;
      TNode = class
        Value: longint;
        constructor Create(v: longint);
        function Kind: TKind; virtual;
        function Describe: string; virtual;
      end;
      TLeaf = class(TNode)
        function Kind: TKind; override;
        function Describe: string; override;
      end;
      TStats = record
        Count: longint;
        Kinds: array[TKind] of longint;
        procedure Add(n: TNode);
      end;
    class var Made: longint;
  private
    FNodes: TNodes;
  public
    procedure Add(n: TNode);
    function Stats: TStats;
    destructor Destroy; override;
  end;

  { a descendant sees the nested names of its ancestors }
  TBigTree = class(TTree)
    function Capacity: longint;
    function MakeLeaf(v: longint): TNode;
  end;

  TGrid = record
  public
    const Size = 3;
    type TCells = array[0..Size - 1] of longint;
  var
    Cells: TCells;
    function Sum: longint;
  end;

function TGenericStringBuilder.TCharEnumerator.GetCurrent: SBChar;
begin
  Result := FBuilder.FData[FPosition];
end;

function TGenericStringBuilder.TCharEnumerator.MoveNext: Boolean;
begin
  Inc(FPosition);
  Result := FPosition < FBuilder.FLength;
end;

constructor TGenericStringBuilder.Create;
begin
  SetLength(FData, DefaultCapacity);
end;

function TGenericStringBuilder.GetCapacity: SizeInt;
begin
  Result := Length(FData);
end;

procedure TGenericStringBuilder.Append(const s: string);
var
  i: SizeInt;
begin
  while FLength + Length(s) > Length(FData) do
    SetLength(FData, Length(FData) * 2);
  for i := 1 to Length(s) do
    FData[FLength + i - 1] := s[i];
  Inc(FLength, Length(s));
end;

function TGenericStringBuilder.Text: string;
var
  i: SizeInt;
begin
  Result := '';
  for i := 0 to FLength - 1 do
    Result := Result + FData[i];
end;

function TGenericStringBuilder.GetEnumerator: TCharEnumerator;
begin
  Result.FBuilder := Self;
  Result.FPosition := -1;
end;

constructor TTree.TNode.Create(v: longint);
begin
  Value := v;
  Inc(Made); { the outer class's class variable }
end;

function TTree.TNode.Kind: TKind;
begin
  Result := kNode;
end;

function TTree.TNode.Describe: string;
begin
  Result := 'node ' + IntToStr(Value);
end;

function TTree.TLeaf.Kind: TKind;
begin
  Result := kLeaf;
end;

function TTree.TLeaf.Describe: string;
begin
  Result := 'leaf ' + IntToStr(Value) + ' of ' + Title;
end;

procedure TTree.TStats.Add(n: TNode);
begin
  Inc(Count);
  Inc(Kinds[n.Kind]);
end;

procedure TTree.Add(n: TNode);
begin
  if Length(FNodes) < Limit then
  begin
    SetLength(FNodes, Length(FNodes) + 1);
    FNodes[High(FNodes)] := n;
  end
  else
    n.Free;
end;

function TTree.Stats: TStats;
var
  n: TNode;
  k: TKind;
begin
  Result.Count := 0;
  for k in TKind do
    Result.Kinds[k] := 0;
  for n in FNodes do
    Result.Add(n);
end;

destructor TTree.Destroy;
var
  n: TNode;
begin
  for n in FNodes do
    n.Free;
  inherited;
end;

function TBigTree.Capacity: longint;
begin
  Result := Limit * 10;
end;

function TBigTree.MakeLeaf(v: longint): TNode;
begin
  Result := TLeaf.Create(v);
end;

function TGrid.Sum: longint;
var
  i: longint;
begin
  Result := 0;
  for i := 0 to Size - 1 do
    Result := Result + Cells[i];
end;

var
  b: TGenericStringBuilder;
  c: SBChar;
  e: TGenericStringBuilder.TCharEnumerator;
  t: TBigTree;
  n: TTree.TNode;
  s: TTree.TStats;
  k: TTree.TKind;
  g: TGrid;
  slots: array[0..TTree.Limit - 1] of longint;
  i: longint;
begin
  writeln('string builder');
  b := TGenericStringBuilder.Create;
  b.Append('Hello, ');
  b.Append('nested types');
  writeln(b.Text, ' (', b.Capacity, ')');
  for c in b do
    write(UpCase(c));
  writeln;
  e := b.GetEnumerator;
  while e.MoveNext do
    if e.Current = 'e' then
      write('*');
  writeln;
  b.Free;

  writeln('nested classes');
  t := TBigTree.Create;
  t.Add(TTree.TNode.Create(1));
  t.Add(TTree.TLeaf.Create(2));
  n := t.MakeLeaf(3);
  t.Add(n);
  t.Add(TTree.TNode.Create(4)); { over the limit: freed }
  s := t.Stats;
  writeln(s.Count, ' nodes, ', s.Kinds[Low(TTree.TKind)], ' node, ', s.Kinds[High(TTree.TKind)], ' leaves');
  writeln(n.Describe, ' ', n.ClassName, ' ', n is TTree.TLeaf, ' ', TTree.Made);
  for k in TTree.TKind do
    write(Ord(k));
  writeln;
  writeln(t.Capacity, ' ', TTree.Title, ' ', t.Title, ' ', TTree.Counter);
  TTree.Counter := TTree.Counter + 1;
  writeln(TTree.Counter);
  t.Free;

  writeln('constants and records');
  for i := 0 to High(slots) do
    slots[i] := i * TTree.Limit;
  writeln(slots[High(slots)], ' ', SizeOf(TGrid.TCells), ' ', TGrid.Size);
  g.Cells[0] := 4;
  g.Cells[1] := 5;
  g.Cells[2] := 6;
  writeln(g.Sum);
  case 3 of
    TTree.Limit: writeln('limit');
  else
    writeln('other');
  end;
end.
