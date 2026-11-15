program errors;
{$H+}
type
  TRec = record
    a: longint;
  end;
  TColor = (Red, Green);

function Tag(const parts: array of string; const values: array of const): string;
begin
  Tag := parts[0];
end;

procedure NoTag(n: longint);
begin
end;

var
  x: longint;
  r: TRec;
  p: ^longint;
  s: string;
  f: real;
  a: array[1..3] of longint;
  st: set of TColor;
  c: TColor;

const
  K = `constant ${x}`;

begin
  s := `a ${r} b ${p} c ${a} d ${st}`;
  s := `width ${x:f} digits ${f:2:f} only reals ${x:2:3}`;
  s := `unknown ${nope} and ${x + s}`;
  x := `not a number ${x}`;
  s := `fine ${x} ${f:0:2} ${c} ${s} ${'lit'} ${x > 1}`;
  s := Tag`ok ${x} ${f}` + Tag`plain`;
  s := x`tag ${x}`;
  s := Tag`no record ${r}`;
  NoTag`not a tag ${x}`;
  s := `value ${Tag`inner ${r}`}`;
end.
