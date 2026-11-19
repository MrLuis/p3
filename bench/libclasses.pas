{ Library classes: TList (Add, IndexOf, Sort), TStringList (Add, Sort,
  Find, IndexOf) and a generic class specialized for two types. }
program libclasses;
{$H+}
uses Classes, SysUtils;

type
  TItem = class
    key: longint;
    constructor Create(k: longint);
  end;

  generic TStack<T> = class
  private
    FItems: array of T;
    FCount: longint;
  public
    procedure Push(const x: T);
    function Pop: T;
    property Count: longint read FCount;
  end;

  TIntStack = specialize TStack<longint>;
  TStrStack = specialize TStack<ansistring>;

constructor TItem.Create(k: longint);
begin
  key := k;
end;

procedure TStack.Push(const x: T);
begin
  if FCount = length(FItems) then
    SetLength(FItems, 2 * FCount + 4);
  FItems[FCount] := x;
  FCount := FCount + 1;
end;

function TStack.Pop: T;
begin
  FCount := FCount - 1;
  Result := FItems[FCount];
end;

function CompareItems(a, b: pointer): longint;
begin
  Result := TItem(a).key - TItem(b).key;
end;

function ListRounds(n: longint): int64;
var
  list: TList;
  items: array[0..499] of TItem;
  r, i: longint;
  total: int64;
begin
  total := 0;
  for i := 0 to 499 do
    items[i] := TItem.Create((i * 7919) mod 1000);
  list := TList.Create;
  for r := 1 to n do
  begin
    list.Clear;
    for i := 0 to 499 do
      list.Add(items[(i + r) mod 500]);
    total := total + list.IndexOf(items[(r * 13) mod 500]);
    list.Sort(@CompareItems);
    total := total + TItem(list[0]).key + TItem(list[250]).key + TItem(list[499]).key;
  end;
  list.Free;
  for i := 0 to 499 do
    items[i].Free;
  ListRounds := total;
end;

function StringRounds(n: longint): int64;
var
  sl: TStringList;
  r, i, idx: longint;
  total: int64;
begin
  total := 0;
  sl := TStringList.Create;
  for r := 1 to n do
  begin
    sl.Clear;
    sl.Sorted := false;
    for i := 0 to 299 do
      sl.Add('key' + IntToStr((i * 7919 + r) mod 1000));
    total := total + sl.IndexOf('key' + IntToStr(r mod 1000));
    sl.Sort;
    total := total + length(sl[0]) + length(sl[299]);
    sl.Sorted := true;
    for i := 0 to 49 do
      if sl.Find('key' + IntToStr(i * 20), idx) then
        total := total + idx;
  end;
  sl.Free;
  StringRounds := total;
end;

function StackRounds(n: longint): int64;
var
  ints: TIntStack;
  strs: TStrStack;
  r, i: longint;
  total: int64;
begin
  total := 0;
  ints := TIntStack.Create;
  strs := TStrStack.Create;
  for r := 1 to n do
  begin
    for i := 1 to 200 do
      ints.Push(i * r mod 977);
    for i := 1 to 20 do
      strs.Push(IntToStr(i + r));
    while ints.Count > 0 do
      total := total + ints.Pop;
    while strs.Count > 0 do
      total := total + length(strs.Pop);
  end;
  ints.Free;
  strs.Free;
  StackRounds := total;
end;

begin
  writeln(ListRounds(2000));
  writeln(StringRounds(300));
  writeln(StackRounds(20000));
end.
