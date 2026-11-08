program main;
uses counters;
type
  TDouble = class(TCounter)
    function Next: integer; override;
  end;

function TDouble.Next: integer;
begin
  Next := 2 * inherited Next;
end;

procedure Run;
var
  a, b: ICounter;
  t: TCounter;
begin
  a := NewCounter('a');
  t := TDouble.Create('double');
  b := t;
  t.Partner := a;
  writeln(a.Next, ' ', a.Next, ' ', b.Next, ' ', b.Next, ' ', Last.Name);
  Last := nil;
  a := nil;
  writeln('a still held by double');
end;

begin
  Run;
  writeln('done');
end.
