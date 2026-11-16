{ Generic types (objfpc, as in Free Pascal): generic TName<T> = ... is a
  template for classes, records, arrays and procedural types;
  specialize TName<integer> is the type made of it for those type
  arguments, the same type wherever it is written. The implementations of
  a generic class's methods (TName.Method) are templates too.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/generictypes.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/generictypes.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/generictypes.expected
}
{$H+}
{$modeswitch advancedrecords}
program generictypes;

type
  generic TPair<K, V> = record
    Key: K;
    Value: V;
  end;

  generic TArrayOf<T> = array of T;
  generic TCompare<T> = function(const a, b: T): Integer;
  generic TGrid<T> = array[1..2, 1..2] of T;

  { a record with methods }
  generic TRange<T> = record
    Lo, Hi: T;
    function Contains(const x: T): Boolean;
    procedure Widen(const x: T);
  end;

  { a class: fields, constructor, virtual methods, a property, a class
    method, a class variable }
  generic TList<T> = class
  private
    FItems: array of T;
    FCount: Integer;
    function GetItem(i: Integer): T;
    procedure SetItem(i: Integer; const v: T);
  public
    class var Made: Integer;
    constructor Create(Capacity: Integer = 4);
    destructor Destroy; override;
    procedure Add(const Item: T); virtual;
    function Count: Integer;
    function Last: T;
    procedure Sort(Compare: specialize TCompare<T>);
    function Pairs: specialize TArrayOf<specialize TPair<Integer, T>>;
    class function Describe: string;
    property Items[i: Integer]: T read GetItem write SetItem; default;
  end;

  { a linked list: the type inside itself, by its name }
  generic TNode<T> = class
    Value: T;
    Next: TNode;
    constructor Create(const v: T; n: TNode);
    function Length: Integer;
  end;

  { an interface, and a class that implements a specialization }
  generic ISource<T> = interface
    function Next(out v: T): Boolean;
  end;

  TIntList = specialize TList<Integer>;
  TStrList = specialize TList<string>;
  TIntSource = specialize ISource<Integer>;

  { a descendant of a specialization }
  TLoudList = class(TIntList)
    procedure Add(const Item: Integer); override;
  end;

  TCounter = class(TInterfacedObject, TIntSource)
    FLeft: Integer;
    constructor Create(n: Integer);
    function Next(out v: Integer): Boolean;
  end;

function TRange.Contains(const x: T): Boolean;
begin
  Result := (x >= Lo) and (x <= Hi);
end;

procedure TRange.Widen(const x: T);
begin
  if x < Lo then Lo := x;
  if x > Hi then Hi := x;
end;

constructor TList.Create(Capacity: Integer);
begin
  SetLength(FItems, Capacity);
  Made := Made + 1;
end;

destructor TList.Destroy;
begin
  Writeln('list of ', FCount, ' freed');
  inherited Destroy;
end;

function TList.GetItem(i: Integer): T;
begin
  Result := FItems[i];
end;

procedure TList.SetItem(i: Integer; const v: T);
begin
  FItems[i] := v;
end;

procedure TList.Add(const Item: T);
begin
  if FCount = Length(FItems) then
    SetLength(FItems, FCount * 2 + 1);
  FItems[FCount] := Item;
  FCount := FCount + 1;
end;

function TList.Count: Integer;
begin
  Result := FCount;
end;

function TList.Last: T;
begin
  Result := FItems[FCount - 1];
end;

procedure TList.Sort(Compare: specialize TCompare<T>);
var
  i, j: Integer;
  t: T;
begin
  for i := 1 to FCount - 1 do
  begin
    t := FItems[i];
    j := i - 1;
    while (j >= 0) and (Compare(FItems[j], t) > 0) do
    begin
      FItems[j + 1] := FItems[j];
      j := j - 1;
    end;
    FItems[j + 1] := t;
  end;
end;

function TList.Pairs: specialize TArrayOf<specialize TPair<Integer, T>>;
var
  i: Integer;
begin
  SetLength(Result, FCount);
  for i := 0 to FCount - 1 do
  begin
    Result[i].Key := i;
    Result[i].Value := FItems[i];
  end;
end;

class function TList.Describe: string;
begin
  Result := `a list, ${SizeOf(T)} bytes an item`;
end;

constructor TNode.Create(const v: T; n: TNode);
begin
  Value := v;
  Next := n;
end;

function TNode.Length: Integer;
begin
  if Next = nil then
    Result := 1
  else
    Result := 1 + Next.Length;
end;

procedure TLoudList.Add(const Item: Integer);
begin
  Writeln('adding ', Item);
  inherited Add(Item);
end;

constructor TCounter.Create(n: Integer);
begin
  FLeft := n;
end;

function TCounter.Next(out v: Integer): Boolean;
begin
  v := FLeft;
  FLeft := FLeft - 1;
  Result := v > 0;
end;

function ByValue(const a, b: Integer): Integer;
begin
  Result := a - b;
end;

function ByLength(const a, b: string): Integer;
begin
  Result := Length(a) - Length(b);
end;

function Total(l: specialize TList<Integer>): Integer;
var
  i: Integer;
begin
  Result := 0;
  for i := 0 to l.Count - 1 do
    Result := Result + l[i];
end;

generic function Swapped<A, B>(const p: specialize TPair<A, B>): specialize TPair<B, A>;
begin
  Result.Key := p.Value;
  Result.Value := p.Key;
end;

var
  ints: TIntList;
  strs: TStrList;
  loud: TLoudList;
  again: specialize TList<Integer>;   { the same type as TIntList }
  p: specialize TPair<string, Real>;
  q: specialize TPair<Real, string>;
  r: specialize TRange<Integer>;
  rr: specialize TRange<Real>;
  g: specialize TGrid<Char>;
  node: specialize TNode<string>;
  src: TIntSource;
  i, v: Integer;
begin
  ints := TIntList.Create;
  for i := 5 downto 1 do
    ints.Add(i * 3);
  ints[0] := 100;
  ints.Sort(@ByValue);
  for i := 0 to ints.Count - 1 do
    Write(ints[i], ' ');
  Writeln('total ', Total(ints));

  strs := TStrList.Create(Capacity := 1);
  strs.Add('pear');
  strs.Add('fig');
  strs.Add('banana');
  strs.Sort(@ByLength);
  Writeln(strs[0], ' ', strs[1], ' ', strs.Last, ' ', strs.Count);
  for var pair in strs.Pairs do
    Write(pair.Key, '=', pair.Value, ' ');
  Writeln;
  Writeln(TIntList.Describe, '; ', TStrList.Describe);
  Writeln(TIntList.Made, ' ', TStrList.Made);

  again := ints;
  Writeln(again.Last);
  loud := TLoudList.Create;
  loud.Add(7);
  again := loud;
  again.Add(8);
  Writeln(again.Count, ' ', TIntList.Made);

  p.Key := 'pi';
  p.Value := 3.14;
  q := specialize Swapped<string, Real>(p);
  Writeln(q.Key:0:2, ' ', q.Value);
  r.Lo := 5;
  r.Hi := 5;
  r.Widen(2);
  r.Widen(9);
  rr.Lo := 0;
  rr.Hi := 1.5;
  Writeln(r.Lo, '..', r.Hi, ' ', r.Contains(7), ' ', rr.Contains(2.0));
  g[1, 1] := 'a';
  g[2, 2] := 'z';
  Writeln(g[1, 1], g[2, 2]);

  node := specialize TNode<string>.Create('c', nil);
  node := specialize TNode<string>.Create('b', node);
  Writeln(node.Length, ' ', node.Value, node.Next.Value);
  node.Next.Free;
  node.Free;

  src := TCounter.Create(3);
  while src.Next(v) do
    Write(v, ' ');
  Writeln;
  src := nil;

  ints.Free;
  strs.Free;
  loud.Free;
end.
