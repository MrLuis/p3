program d02_mem;
{ Dialect check L2 (typecasts), L3 (raw memory) and L4 (open-ended arrays
  through a pointer to a large array type, indexed with range checks off). }
type
  TColor = (red, green, blue);
  PNode = ^TNode;
  TNode = record
    kind: longint;
    value: int64;
    next: PNode;
  end;
  TPtrArray = array[0..$FFFFFFF] of pointer;
  PPtrArray = ^TPtrArray;
  TPtrList = record
    items: PPtrArray;
    count, cap: longint;
  end;

procedure listInit(var l: TPtrList);
begin
  l.items := nil;
  l.count := 0;
  l.cap := 0;
end;

procedure listAdd(var l: TPtrList; p: pointer);
begin
  if l.count = l.cap then
  begin
    if l.cap = 0 then l.cap := 4 else l.cap := l.cap * 2;
    reallocmem(l.items, l.cap * sizeof(pointer));
  end;
  {$R-}
  l.items^[l.count] := p;
  {$R+}
  l.count := l.count + 1;
end;

function listGet(const l: TPtrList; i: longint): pointer;
begin
  {$R-}
  listGet := l.items^[i];
  {$R+}
end;

var
  l: TPtrList;
  n: PNode;
  p: pointer;
  i, k: longint;
  total: int64;
  col: TColor;
  src, dst: array[0..7] of char;
begin
  { typecasts }
  col := TColor(2);
  k := 300;
  writeln(ord(col), ' ', longint(green), ' ', byte(k), ' ', char(65),
          ' ', ord(TColor(0) = red));
  { getmem / freemem / sizeof / pointer casts }
  listInit(l);
  for i := 1 to 100 do
  begin
    getmem(p, sizeof(TNode));
    n := PNode(p);
    n^.kind := i mod 3;
    n^.value := int64(i) * 1000000000;
    n^.next := nil;
    listAdd(l, n);
  end;
  total := 0;
  for i := 0 to l.count - 1 do
    total := total + PNode(listGet(l, i))^.value;
  writeln(l.count, ' ', l.cap, ' ', total);
  for i := 0 to l.count - 1 do
    freemem(listGet(l, i));
  freemem(l.items);
  { fillchar / move }
  fillchar(src, sizeof(src), ord('.'));
  src[0] := 'p'; src[1] := 'c'; src[2] := 'c';
  fillchar(dst, sizeof(dst), 0);
  move(src, dst, 5);
  for i := 0 to 7 do
    if dst[i] = #0 then write('0') else write(dst[i]);
  writeln;
end.
