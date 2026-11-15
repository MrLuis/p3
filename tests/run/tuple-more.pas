{ Tuples (objfpc) with reference-counted elements in dynamic arrays, in a
  recursive function (each activation builds its tuples in its own hidden
  variables), across exceptions, in a nested routine, and as the initial
  value of a variable.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/tuple-more.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/tuple-more.expected
}
{$H+}
program tuplemore;
type
  TPair = (Integer, string);
  TList = array of TPair;
  TIntGen = record
    v: (Integer, Integer);
  end;
var
  list: TList;
  i: Integer;
  g: TIntGen;
  ex: (Integer, Integer) = (1, 2);

function F(n: Integer): (Integer, Integer);
begin
  if n = 0 then
    exit((0, 1));
  var (a, b) := F(n - 1);
  Result := (b, a + b);
end;

function Boom(n: Integer): (Integer, string);
begin
  if n > 2 then
    raise TObject.Create;
  Result := (n, 'ok');
end;

procedure Local;
  procedure Inner(k: Integer);
  begin
    var t := (k, 'inner');
    Writeln(t.Item1, t.Item2);
    i := i + 1;
  end;
begin
  Inner(5);
end;

begin
  SetLength(list, 3);
  for i := 0 to 2 do
    list[i] := (i * i, 'n' + chr(48 + i));
  (list[0], list[2]) := (list[2], list[0]);
  for i := 0 to 2 do
    Writeln(list[i].Item1, ' ', list[i].Item2);
  Writeln(F(20).Item1);
  g.v := (3, 4);
  Writeln(g.v.Item1 + g.v.Item2);
  for i := 1 to 4 do
    try
      var (n, s) := Boom(i);
      Writeln(n, s);
    except
      Writeln('caught');
    end;
  Local;
  Writeln(ex.Item1, ex.Item2);
  with list[1] do
    Writeln(Item1, Item2);
end.
