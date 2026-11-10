{
  RUN: %pcc --mode=objfpc %s -o %t && %t | diff - %S/strpass.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.o2 && %t.o2 | diff - %S/strpass.expected
  RUN: %pcc --mode=objfpc -O2 -fno-inline-runtime -g %s -o %t.g && %t.g | diff - %S/strpass.expected
}
{ Short strings in calls: value parameters are the callee's own copies (cut
  to its size), const parameters read the caller's string, functions write
  their result into a temporary of the caller; s := s + x appends in
  place. Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc), except
  that a result never assigned (Unset) is empty here and undefined there. }
program strpass;

type
  TShort = string[5];
  TNamer = class
    FName: string;
    function Name: string; virtual;
    function Tagged(tag: string): string;
  end;
  TLoud = class(TNamer)
    function Name: string; override;
  end;

var
  g: string;

function TNamer.Name: string;
begin
  Name := FName;
end;

function TNamer.Tagged(tag: string): string;
begin
  tag := tag + ':';
  Tagged := tag + Name;
end;

function TLoud.Name: string;
begin
  Name := inherited Name + '!';
end;

{ the parameter is a copy: changing it or the caller's variable does not
  change the other }
procedure Change(s: string);
begin
  s := s + ' (changed)';
  g := 'global changed';
  writeln('in Change: ', s);
end;

procedure Both(a: string; var b: string);
begin
  b := 'b changed';
  writeln('Both: a=', a, ' b=', b);
end;

procedure Cut(s: TShort; const t: TShort);
begin
  writeln('Cut: [', s, '] ', length(s), ' [', t, ']');
  s := s + 'xyz';
  writeln('Cut: [', s, '] ', length(s));
end;

function Rev(s: string): string;
var i: integer;
begin
  Result := '';
  for i := length(s) downto 1 do
    Result := Result + s[i];
end;

function Stars(n: integer): string;
begin
  if n = 0 then
    Stars := ''
  else
    Stars := Stars(n - 1) + '*';
end;

function Short(s: string): TShort;
begin
  Short := s;
end;

function Build(n: integer): string;
  procedure Add(c: char);
  begin
    Result := Result + c;
  end;
var i: integer;
begin
  Build := '<';
  for i := 1 to n do
    Add(chr(ord('a') + i - 1));
  Result := Result + '>';
end;

function Upper(s: string): string;
var i: integer;
begin
  for i := 1 to length(s) do
    s[i] := upcase(s[i]);
  Result := s;
  if Result = '' then
    Result := '(empty)';
end;

function Unset(n: integer): string;
begin
  { never assigned: empty }
  if n > 100 then
    Unset := 'big';
end;

var
  s, t: string;
  w: string[8];
  o: TNamer;
  i: integer;
  c: char;
begin
  g := 'global';
  Change(g);
  writeln('after Change: ', g);
  s := 'one';
  Both(s, s);
  writeln('after Both: ', s);
  Cut('abcdefgh', 'ijklmnop');
  s := 'abcdefgh';
  Cut(s, s);
  writeln(Rev('hello'), ' ', Rev(Rev('hello')), ' ', Rev(''), '|');
  writeln(Stars(5), ' ', length(Stars(40)));
  writeln(Short('abcdefgh'), ' ', length(Short('ab')));
  w := Rev('0123456789');
  writeln(w, ' ', length(w));
  writeln(Build(5), ' ', Upper('mixed Case'), ' ', Upper(''), ' [', Unset(1), '] ', Unset(101));
  o := TLoud.Create;
  o.FName := 'loud';
  writeln(o.Name, ' ', o.Tagged('tag'), ' ', Rev(o.Tagged(Rev('gat'))));
  o.Free;
  c := 'x';
  writeln(Rev(c), Rev('a' + c + 'b'));
  { appending in place }
  s := 'ab';
  s := s + 'cd';
  s := s + c;
  s := s + s;
  writeln(s, ' ', length(s));
  s := s + Rev(s);
  writeln(s);
  t := '';
  for i := 1 to 300 do
    t := t + chr(ord('0') + i mod 10);
  writeln(length(t), ' ', t[1], t[255]);
  t := t + 'more';
  writeln(length(t));
  w := 'abc';
  w := w + 'defgh';
  w := w + 'ijk';
  writeln(w, ' ', length(w));
  w := 'abcd';
  w := w + w + w;
  writeln(w, ' ', length(w));
  w := 'ab';
  w := w + '';
  writeln(w, '|');
end.
