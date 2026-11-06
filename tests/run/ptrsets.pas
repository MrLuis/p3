{ M9: pointers (new/dispose, linked list, binary tree) and sets.
  RUN: %pcc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/ptrsets.expected %t.out
  RUN: %pcc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/ptrsets.expected %t.O2.out
}
program PtrSets(output);
type
  PNode = ^Node;
  Node = record value: integer; next: PNode end;
  PTree = ^Tree;
  Tree = record key: integer; left, right: PTree end;
  Digit = 0..9;
  Digits = set of Digit;
  CharSet = set of char;
var
  head, p: PNode;
  root: PTree;
  i, n: integer;
  evens, odds, small, d: Digits;
  vowels: CharSet;
  c: char;
  sieve: set of 2..100;

procedure push(var list: PNode; v: integer);
var q: PNode;
begin
  new(q); q^.value := v; q^.next := list; list := q
end;

procedure insert(var t: PTree; k: integer);
begin
  if t = nil then begin
    new(t); t^.key := k; t^.left := nil; t^.right := nil
  end
  else if k < t^.key then insert(t^.left, k)
  else insert(t^.right, k)
end;

procedure inorder(t: PTree);
begin
  if t <> nil then begin
    inorder(t^.left); write(t^.key, ' '); inorder(t^.right)
  end
end;

function count(list: PNode): integer;
begin
  if list = nil then count := 0 else count := 1 + count(list^.next)
end;

begin
  head := nil;
  for i := 1 to 5 do push(head, i * i);
  p := head;
  while p <> nil do begin write(p^.value, ' '); p := p^.next end;
  writeln('(', count(head), ' nodes)');
  while head <> nil do begin p := head; head := head^.next; dispose(p) end;

  root := nil;
  insert(root, 50); insert(root, 30); insert(root, 70);
  insert(root, 20); insert(root, 40); insert(root, 60);
  inorder(root); writeln;

  evens := [0, 2, 4, 6, 8]; odds := [1, 3, 5, 7, 9]; small := [0..4];
  d := evens * small;
  for i := 0 to 9 do if i in d then write(i); writeln(' = evens*small');
  d := evens + small;
  for i := 0 to 9 do if i in d then write(i); writeln(' = evens+small');
  d := evens - small;
  for i := 0 to 9 do if i in d then write(i); writeln(' = evens-small');
  writeln(evens * odds = [], ' ', [2, 4] <= evens, ' ', evens >= [1], ' ', evens <> odds);
  vowels := ['a', 'e', 'i', 'o', 'u'];
  n := 0;
  for c := 'a' to 'z' do if c in vowels then n := n + 1;
  writeln('vowels: ', n, ' ', 'y' in vowels, ' ', 300 in [1..9], ' ', -1 in [0..5]);
  sieve := [2..100];
  for i := 2 to 10 do
    if i in sieve then begin
      n := i * i;
      while n <= 100 do begin sieve := sieve - [n]; n := n + i end
    end;
  for i := 2 to 100 do if i in sieve then write(i, ' ');
  writeln
end.
