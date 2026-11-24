{ Unit json, TJsonWriter: integers of every size (Int64 and QWord extremes),
  in arrays and objects, compact and pretty.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-writer.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-writer.expected
}
program jsonwriter;
{$H+}
uses sysutils, json;

var
  W: TJsonWriter;
  I: Int64;
  Q: QWord;
begin
  W := TJsonWriter.Create;
  W.BeginArray;
  W.Value(Int64(0));
  W.Value(Int64(5));
  W.Value(Int64(-1));
  W.Value(Int64(10));
  W.Value(Int64(-100));
  W.Value(Int64(123456789));
  I := 9223372036854775807;
  W.Value(I);
  I := -I - 1;
  W.Value(I);
  Q := 18446744073709551615;
  W.ValueUInt64(Q);
  W.ValueUInt64(0);
  W.EndArray;
  Writeln(W.Text);
  W.Free;

  W := TJsonWriter.Create(True);
  W.BeginObject;
  W.Key('a'); W.Value(Int64(-7));
  W.Key('b'); W.BeginArray; W.Value(Int64(1)); W.Value(Int64(20)); W.EndArray;
  W.EndObject;
  Writeln(W.Text);
  W.Free;
end.
