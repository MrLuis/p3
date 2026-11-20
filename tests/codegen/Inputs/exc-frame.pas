{ A routine's body is protected (an exception frame, a setjmp) only when
  its landing has something to release. }
program excframe;
{$H+}
type
  TCmp = function(const a, b: string): longint;
  TPair = record
    s: string;
    n: longint;
  end;

function Lexical(const a, b: string): longint;
begin
  if a < b then Result := -1
  else if a > b then Result := 1
  else Result := 0;
end;

{ Passes strings on, holds none: no frame. }
function PassOn(f: TCmp; const p, q: TPair): longint;
begin
  Result := f(p.s, q.s);
end;

{ A string local: released if f raises, so a frame. }
function WithLocal(f: TCmp; const a: string): longint;
var
  t: string;
begin
  t := a + '!';
  Result := f(t, a);
end;

{ A string temporary: released if f raises, so a frame. }
function WithTemp(f: TCmp; const a: string): longint;
begin
  Result := f(a + '?', a);
end;

var
  p, q: TPair;
begin
  p.s := 'apple';
  q.s := 'banana';
  writeln(PassOn(@Lexical, p, q), ' ', WithLocal(@Lexical, 'x'), ' ',
          WithTemp(@Lexical, 'y'));
end.
