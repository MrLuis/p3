{ Records reached only through pointers (field offsets must be known
  before the record is laid out for anything else), record function
  results (Turbo/FPC), and -g with untyped pointers. Found while porting
  selfhost/pcutil and pcdiag (S3). Expected output from Free Pascal 3.2.2.
  RUN: %pcc --mode=tp %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/records.expected %t.out
  RUN: %pcc --mode=tp -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/records.expected %t.g.out
}
program records;

type
  PNode = ^TNode;
  TNode = record
    id: longint;
    hash: cardinal;
    next: PNode;
  end;
  TRange = record
    b, e: longint;
  end;

var
  head: PNode;
  any: pointer;

{ fields used before anything mentions TNode by value or size }
function sum(n: PNode): int64;
var
  s: int64;
begin
  s := 0;
  while n <> nil do
  begin
    s := s + n^.id * 10 + n^.hash;
    n := n^.next;
  end;
  sum := s;
end;

procedure push(id: longint);
var
  n: PNode;
begin
  getmem(any, sizeof(TNode));
  n := PNode(any);
  n^.id := id;
  n^.hash := cardinal(id) * 3;
  n^.next := head;
  head := n;
end;

function makeRange(b, e: longint): TRange;
var
  r: TRange;
begin
  r.b := b;
  r.e := e;
  makeRange := r;
end;

function emptyRange: TRange;
begin
  exit(makeRange(0, 0));
end;

function width(const r: TRange): longint;
begin
  width := r.e - r.b;
end;

var
  r: TRange;
  i: longint;
begin
  head := nil;
  for i := 1 to 5 do
    push(i);
  writeln(sum(head));
  r := makeRange(3, 10);
  writeln(r.b, ' ', r.e, ' ', width(r), ' ', width(makeRange(1, 4)), ' ',
          makeRange(7, 9).e, ' ', emptyRange.e, ' ', width(emptyRange));
end.
