{ Unit json, documents: parse, read (V['a'][0], Path, iterators), change,
  write (compact, pretty, numbers as written, shortest doubles), errors.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-dom.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-dom.expected
}
program jsondom;
{$H+}
uses sysutils, json;

function U(const Hex: string): string;
begin
  { a \u escape (written in pieces) }
  Result := '\' + 'u' + Hex;
end;

var
  Sample: string;

function MakeSample: string;
begin
  Result := '{"name": "Ada", "age": 36, "tags": ["math", "poet' + U('00e9') + '"],' +
    ' "address": {"city": "London", "zip": null},' +
    ' "scores": [1.5, -2e3, 18446744073709551615, 0.1],' +
    ' "a/b": 1, "m~n": 2, "esc": "line\nnext"}';
end;

var
  D, D2: TJsonDocument;
  V, E, Arr, Obj: TJsonValue;
  M: TJsonMember;
  I: Integer;
begin
  Sample := MakeSample;
  D := JsonParse(Sample);
  V := D.Root;
  WriteLn(V['name'].AsString, ' ', V['age'].AsInt64, ' ', V.Count);
  WriteLn(V['tags'][1].AsString, ' ', V['address']['city'].AsString);
  WriteLn(V['address']['zip'].IsNull, ' ', V['nope'].Exists, ' ',
          V['tags'][9].Exists, ' ', V['nope']['deeper'][0].Exists);
  WriteLn(V['scores'][2].AsUInt64, ' ', V['scores'][1].AsDouble:0:1, ' ',
          V['scores'][2].IsInteger, ' ', V['scores'][0].IsInteger);
  WriteLn(V.Path('/address/city').AsString, ' ', V.Path('/tags/0').AsString,
          ' ', V.Path('/a~1b').AsInt64, ' ', V.Path('/m~0n').AsInt64, ' ',
          V.Path('/tags/01').Exists, ' ', V.Path('').Count);
  WriteLn(V['esc'].AsString = 'line'#10'next');
  WriteLn(V['age'].AsStringDef('none'), ' ', V['name'].AsInt64Def(-1), ' ',
          V['address'].AsBooleanDef(True));
  for E in JsonItems(V['tags']) do
    Write(E.AsString, ';');
  WriteLn;
  for M in JsonMembers(V['address']) do
    Write(M.Key, '=', M.Value.ToJson, ';');
  WriteLn;
  for I := 0 to V.Count - 1 do
    Write(V.KeyAt(I), ' ');
  WriteLn;

  { written back: the source's text for strings and numbers }
  WriteLn(V.ToJson);
  WriteLn(V['address'].ToJson(True));

  { changes }
  V.Put('age', 37);
  V.Put('ok', True);
  V.Put('ratio', 0.25);
  V.PutNull('none');
  V['tags'].Add('new "one"');
  V['tags'].Remove(0);
  Arr := V.PutArray('list');
  Arr.Add(1);
  Arr.Add(2.5);
  Arr.AddObject.Put('k', 'v');
  Obj := V['address'];
  Obj.Delete('zip');
  WriteLn(V.Delete('esc'), ' ', V.Delete('esc'));
  V.Delete('scores');
  V.Delete('a/b');
  V.Delete('m~n');
  WriteLn(V.ToJson);
  WriteLn(V.ToJson(True));

  { building from nothing, and a value from another document }
  D2 := TJsonDocument.Create;
  Obj := D2.NewObject;
  Obj.Put('copied', D.Root['address']);
  Obj.Put('pi', 3.141592653589793);
  Obj.Put('third', 1 / 3);
  Obj.Put('big', 1e300);
  Obj.Put('small', 5e-324);
  Obj.Put('int', -9223372036854775807 - 1);
  Obj.Put('whole', 2.0);
  Obj.PutArray('empty');
  Obj.PutObject('nothing');
  D2.SetRoot(Obj);
  D.Free;
  WriteLn(D2.ToJson);
  WriteLn(D2.ToJson(True));
  D2.Free;

  { a document read back from its own text is the same }
  D := JsonParse(' [ 1 , {"a" : [ ] } , "x' + U('0041') + '" , -0.0 , 1E2 ] ');
  WriteLn(D.ToJson);
  D2 := JsonParse(D.ToJson(True));
  WriteLn(D2.ToJson = D.ToJson);
  D.Free;
  D2.Free;

  { errors }
  try
    JsonParse('{"a": [1, 2,]}');
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
  D := JsonParse('{"a": "text", "n": 1.5}');
  try
    D.Root['a'].AsInt64;
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
  try
    D.Root['n'].AsInt64;
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
  try
    D.Root['b'].AsString;
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
  try
    D.Root.Add(1);
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
  D.Free;
  WriteLn(JsonValid('[1, 2]'), ' ', JsonValid('[1, 2'), ' ', JsonValid(''));
  WriteLn(JsonFloat(0.1), ' ', JsonFloat(100), ' ', JsonFloat(1e21), ' ',
          JsonFloat(-1.5e-7), ' ', JsonFloat(123456789.125));
end.
