{ VectorMask, VectorLoad, VectorStore: a byte scan 16 at a time (the
  positions of a character through a lane comparison, its mask and
  CountTrailingZeros), a mask of a comparison, a store at an odd address.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/vector-mask.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/vector-mask.expected
  RUN: not %p3 --mode=objfpc -c %S/Inputs/vector-mask-errors.pas -o %t.o 2>&1 | FileCheck %s
  CHECK: 'VectorMask' expects a vector of booleans (a comparison of vectors), not 'TB16'
  CHECK: 'VectorLoad' expects a vector type, not 'integer'
  CHECK: 'VectorLoad' expects an address, not 'integer'
  CHECK: 'VectorStore' expects a vector, not 'integer'
  CHECK: 'VectorStore' expects an address, not 'TB16'
}
program vm;
{$H+}
type
  TB16 = vector[16] of Byte;

{ the positions (1-based) of '"' in S, 16 bytes at a time }
procedure Quotes(const S: string);
var
  I, N: Integer;
  M: QWord;
  V: TB16;
begin
  I := 1;
  N := Length(S);
  while I + 15 <= N do
  begin
    V := VectorLoad(TB16, @S[I]);
    M := VectorMask(V = Ord('"'));
    while M <> 0 do
    begin
      Write(I + CountTrailingZeros(M), ' ');
      M := M and (M - 1);
    end;
    Inc(I, 16);
  end;
  while I <= N do
  begin
    if S[I] = '"' then
      Write(I, ' ');
    Inc(I);
  end;
  WriteLn;
end;

var
  S: string;
  Buf: array[0..31] of Byte;
  V: TB16;
  I: Integer;
begin
  Quotes('{"a": "bc", "def": "ghijklmnop", "q": 1, "r": "' + 'x"}');
  Quotes('no quotes here at all, none, nothing to see.');
  S := 'ABCDEFGHIJKLMNOP';
  WriteLn(VectorMask(VectorLoad(TB16, PChar(S)) >= Ord('H')));
  for I := 0 to 31 do Buf[I] := 0;
  for I := 1 to 16 do V[I] := I;
  VectorStore(@Buf[3], V + 100);
  for I := 0 to 20 do Write(Buf[I], ' ');
  WriteLn;
end.
