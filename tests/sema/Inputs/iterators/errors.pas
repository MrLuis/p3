{$H+}
program errors;
{$modeswitch advancedrecords}

type
  TRec = record
    function Items: iterator Integer;
  end;

function TRec.Items: iterator Integer;
begin
  yield 1;
end;

function ByRef(var x: Integer): iterator Integer;
begin
  yield x;
end;

function Wrong: iterator Integer;
begin
  yield 'text';
  try
  finally
    yield 2;
  end;
  exit(3);
  yield from 5;
end;

procedure Outer;
  function Inner: iterator Integer;
  begin
    yield 1;
  end;
begin
  Inner;
end;

procedure NotIt;
var f: reference to procedure;
begin
  yield 1;
  f := procedure => begin yield 2; end;
end;

var
  yield: Integer;
  n: Integer;
begin
  yield := 3; { (yield is not reserved) }
  Wrong;
  Writeln(IteratorNext(n));
end.
