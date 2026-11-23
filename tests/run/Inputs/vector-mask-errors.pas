program vectormaskerrors;
type
  TB16 = vector[16] of Byte;
var
  V: TB16;
  I: Integer;
  Q: QWord;
begin
  Q := VectorMask(V);
  V := VectorLoad(Integer, @I);
  V := VectorLoad(TB16, I);
  VectorStore(@I, I);
  VectorStore(V, V);
end.
