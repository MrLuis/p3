{ Unit json, lazy access: JsonGet by a path or a JSON Pointer reads only
  up to the value; lazy values read their text when asked; the array and
  object iterators read one member at a time.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-lazy.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-lazy.expected
}
program jsonlazy;
{$H+}
uses sysutils, json;

var
  S, K: string;
  L, E: TJsonLazy;
  M: TJsonLazyMember;
  D: TJsonDocument;
  N: Integer;

begin
  S := '{"users": [{"name": "Ada", "langs": ["en", "fr"], "age": 36},' +
       ' {"name": "Alan", "langs": [], "age": 41}],' +
       ' "meta": {"count": 2, "a/b": true, "next": null}, "big": [' +
       '{"deep": [[[[1]]]], "s": "x]}\"y"}, 3]}';
  WriteLn(JsonGet(S, ['users', 0, 'name']).AsString);
  WriteLn(JsonGet(S, ['users', 1, 'age']).AsInt64);
  WriteLn(JsonGet(S, ['users', 0, 'langs']).Raw);
  WriteLn(JsonGet(S, ['users', 2]).Exists, ' ', JsonGet(S, ['nope', 0]).Exists,
          ' ', JsonGet(S, ['users', 'name']).Exists, ' ',
          JsonGet(S, ['meta', 0]).Exists);
  WriteLn(JsonGetPath(S, '/meta/a~1b').AsBoolean, ' ',
          JsonGetPath(S, '/meta/next').IsNull, ' ',
          JsonGetPath(S, '/big/1').AsDouble:0:1, ' ',
          JsonGetPath(S, '').Kind = jkObject);
  K := 'count';
  L := JsonGet(S, ['meta']);
  WriteLn(L.Get([K]).AsInt64, ' ', L.GetPath('/count').Raw, ' ',
          Ord(L.Kind), ' ', L.Raw);

  { iterators }
  for E in JsonArrayItems(JsonGet(S, ['users'])) do
    Write(E.Get(['name']).AsString, ':', E.Get(['age']).AsInt64, ' ');
  WriteLn;
  for M in JsonObjectItems(JsonGet(S, ['meta'])) do
    Write(M.Key, '=', M.Value.Raw, ' ');
  WriteLn;
  N := 0;
  for E in JsonArrayItems('[1, [2, 3], {"a": 4}, "five", null]') do
  begin
    Write(E.Raw, ' ');
    Inc(N);
  end;
  WriteLn('(', N, ')');
  { stopping early }
  for E in JsonArrayItems('[10, 20, 30]') do
  begin
    Write(E.AsInt64, ' ');
    if E.AsInt64 = 20 then
      Break;
  end;
  WriteLn;

  { a lazy value made a document }
  D := JsonGet(S, ['users', 0]).Parse;
  WriteLn(D.Root['langs'][1].AsString, ' ', D.ToJson);
  D.Free;

  { the values passed are not read: bad JSON after the value is not seen,
    bad JSON before it is }
  WriteLn(JsonGet('{"a": 1, "b": [}', ['a']).AsInt64);
  try
    JsonGet('{"x": [1, 2, "unclosed], "a": 1}', ['a']);
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
  try
    JsonGet(S, ['meta', 'count']).AsString;
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
  try
    for E in JsonArrayItems('{"not": "an array"}') do
      ;
  except
    on Ex: EJsonError do
      WriteLn(Ex.Message);
  end;
end.
