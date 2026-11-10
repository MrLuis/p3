program noraise;
{$H+}
uses sysutils;

{ strings only: nothing here can raise }
function Join(const a, b: string): string;
begin
  Join := a + '-' + b;
end;

{ calls only routines that cannot raise }
function Twice(const a: string): string;
begin
  Twice := Join(a, a);
end;

{ raises }
function Checked(const a: string): string;
begin
  if a = '' then
    raise Exception.Create('empty');
  Checked := a + '!';
end;

{ calls a routine that may raise }
function Outer(const a: string): string;
begin
  Outer := Checked(a) + '?';
end;

{ calls through a procedural value: may raise }
type TFn = function(const a: string): string;
function Through(f: TFn; const a: string): string;
begin
  Through := f(a) + '.';
end;

begin
  writeln(Twice('a'), Outer('b'), Through(@Twice, 'c'));
end.
