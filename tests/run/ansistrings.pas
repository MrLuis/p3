{ Ansistrings, also as plain string under long strings: lifetimes, copy on write, conversions to
  and from short strings, chars and pchars, parameters, results, records,
  arrays, classes, properties, typed constants, exceptions and the string
  routines. Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/ansistrings.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/ansistrings.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/ansistrings.expected %t.g.out
}
program ansistrings;
{$H+}
uses sysutils;
type
  TRec = record
    name: string;
    n: longint;
    tags: array[1..2] of string;
  end;
  PRec = ^TRec;
  TBase = class
  private
    FName: string;
    FItems: array[0..2] of string;
    function GetItem(i: longint): string;
    procedure SetItem(i: longint; const v: string);
    function GetUp: string;
    procedure SetUp(v: string);
  public
    constructor Create(const n: string);
    function Describe(const prefix: string): string; virtual;
    property Name: string read FName write FName;
    property Up: string read GetUp write SetUp;
    property Items[i: longint]: string read GetItem write SetItem; default;
  end;
  TKid = class(TBase)
    function Describe(const prefix: string): string; override;
  end;
const
  KC: string = 'typed const';
  KC2: array[0..1] of string = ('first', 'second');
  KR: TRec = (name: 'rec'; n: 3; tags: ('t1', 't2'));
  KE: string = '';
  KCH: string = 'x';
  PLAIN = 'plain';
var
  gs: string;
  ga: array[1..3] of ansistring;
  gr: TRec;
function TBase.GetItem(i: longint): string; begin GetItem := FItems[i]; end;
procedure TBase.SetItem(i: longint; const v: string); begin FItems[i] := v + '!'; end;
function TBase.GetUp: string; begin Result := FName + FName; end;
procedure TBase.SetUp(v: string); begin v := v + '?'; FName := v; end;
constructor TBase.Create(const n: string); begin FName := n; end;
function TBase.Describe(const prefix: string): string; begin Result := prefix + ':' + FName; end;
function TKid.Describe(const prefix: string): string; begin Result := inherited Describe(prefix + 'kid'); end;
function Rep(const s: string; n: longint): string;
var i: longint;
begin Result := ''; for i := 1 to n do Result := Result + s; end;
procedure ByVal(s: string);
begin s[1] := 'Z'; s := s + 'tail'; writeln('byval ', s); end;
procedure ByConst(const s: string);
begin writeln('byconst ', s, ' ', length(s)); end;
procedure ByVar(var s: string);
begin s := s + '+v'; end;
procedure ByShort(s: shortstring);
begin writeln('short ', s, ' ', length(s)); end;
procedure ByConstShort(const s: shortstring);
begin writeln('cshort ', s); end;
function RetExit(n: longint): string;
begin
  if n = 0 then exit('zero');
  if n = 1 then exit(RetExit(0) + 'one');
  RetExit := 'many';
end;
function Early(const s: string): string;
var t: string;
begin t := s + s; if s = 'x' then exit(t); Result := t + '#'; end;
function Boom(const s: string): string;
var t: string;
begin t := s + 'local'; raise Exception.Create('boom ' + t); Result := t; end;
function CatchIt(const s: string): string;
var a: string;
begin
  a := s + '1';
  try
    a := Boom(a + '2');
  except
    on e: Exception do a := a + '/' + e.Message;
  end;
  Result := a;
end;
function Fin(const s: string): string;
var a: string;
begin
  a := 'x';
  try
    a := a + s;
    Result := a;
    exit;
  finally
    a := a + 'f';
    writeln('fin ', a);
  end;
end;
function Nest(const s: string): string;
var cap: string;
  procedure Inner(const q: string);
  begin cap := cap + q + s; end;
begin cap := '['; Inner('a'); Inner('b'); Result := cap + ']'; end;
function Ch: char; begin Ch := 'q'; end;
function CStr(p: pchar): int64; cdecl; external 'c' name 'strlen';
var
  a, b, c: string;
  ss: shortstring;
  s5: string[5];
  i: longint;
  p: PRec;
  o: TBase;
  rc: TRec;
  rc2: TRec;
  pc: pchar;
  buf: array[0..9] of char;
  d: ansistring;
  arr2: array[1..2] of TRec;
begin
  writeln('[', a, '] ', length(a), ' ', a = '', ' ', '' = a);
  a := 'hello'; b := a; b[1] := 'J'; writeln(a, ' ', b);
  b := a; a := a + ' world'; writeln(a, ' ', b, ' ', length(a));
  c := Rep('0123456789', 100); writeln(length(c), ' ', c[1], c[500], c[1000], ' ', copy(c, 995, 100));
  c[500] := '#'; writeln(pos('#', c), ' ', copy(c, 498, 5));
  a := 'a' + 'b'; writeln(a);
  a := 'x'; writeln(a, length(a));
  a := Ch; writeln(a);
  a := Ch + 'r'; writeln(a); a := 'r' + Ch; writeln(a); a := Ch + Ch; writeln(a);
  a := PLAIN; writeln(a); a := a + PLAIN; writeln(a);
  a := ''; a := a + 'x'; writeln(a); a := a + ''; writeln(a); a := '' + a; writeln(a);
  ss := 'short'; a := ss; writeln(a); a := a + ss; writeln(a); a := ss + a; writeln(a);
  a := Rep('y', 300); ss := a; writeln(length(ss)); s5 := a; writeln(s5, length(s5));
  ss := a + 'z'; writeln(length(ss));
  a := Rep('ab', 3);
  writeln(a = 'ababab', a <> 'ababab', a < 'b', a > 'b', a <= 'ababab', a >= 'ababac', 'a' < a, a = ss);
  ss := 'ababab'; writeln(a = ss, ss = a, a <> ss, a < ss, a >= ss); writeln(a[1] = 'a', 'a' = a, a = 'a');
  b := 'abc'; c := 'abd'; writeln(b < c, c < b, b = c, b + 'x' = 'abcx');
  b := #200; c := 'a'; writeln(b > c);
  writeln(a:10, '|', a:2, '|', 'x':3); b := ''; writeln(b:3, '|');
  ByVal('literal'); a := 'var'; ByVal(a); writeln(a);
  a := 'keep'; b := a; ByVal(b); writeln(a, b);
  ByConst(a); ByConst('lit'); ByConst(ss); ByConst('c'); ByConst(Ch); ByConst(a + 'x'); ByConst('');
  ByVar(a); writeln(a); ByVar(ga[1]); writeln(ga[1]);
  ByShort(a); ByShort(a + 'y'); ByConstShort(a); ByConstShort(Rep('m', 3));
  writeln(RetExit(0), RetExit(1), RetExit(2));
  writeln(Early('x'), Early('y'));
  writeln(CatchIt('c'));
  writeln(Fin('z'));
  writeln(Nest('n'));
  writeln(KC, ' ', KC2[0], KC2[1], ' ', KR.name, KR.n, KR.tags[1], KR.tags[2], '[', KE, ']', KCH, length(KC));
  KC := KC + ' changed'; writeln(KC);
  gs := 'global'; gs := gs + gs; writeln(gs); ga[2] := gs; ga[3] := ga[2] + ga[2]; writeln(ga[3]);
  gr.name := 'gname'; gr.tags[1] := gr.name + '1'; rc := gr; rc.name[1] := 'G'; writeln(gr.name, ' ', rc.name, ' ', rc.tags[1]);
  rc2 := rc; rc2.tags[1] := 'changed'; writeln(rc.tags[1], ' ', rc2.tags[1]);
  arr2[1] := rc; arr2[2] := arr2[1]; arr2[2].name := 'two'; writeln(arr2[1].name, arr2[2].name);
  new(p); p^.name := 'heap' + a; p^.tags[2] := p^.name; writeln(p^.name, p^.tags[2]); dispose(p);
  with rc do begin name := name + '-with'; writeln(name); end;
  o := TKid.Create('kid' + a);
  writeln(o.Name, ' ', o.Describe('p'), ' ', o.Up);
  o.Name := 'renamed'; o.Up := 'upper'; writeln(o.Name);
  o[0] := 'item'; o.Items[1] := o[0] + 'x'; writeln(o[0], o.Items[1], '[', o[2], ']');
  o.Free;
  a := 'hello world';
  writeln(copy(a, 7, 5), '|', copy(a, 0, 2), '|', copy(a, 11, 9), '|', copy(a, 12, 1), '|', copy(a, 99, 1), '|', copy(a, 3, 0), '|', copy(a, 3, -1), '|', copy(a, 1, 99));
  b := copy(a, 1, 5); b[1] := 'X'; writeln(a, b);
  writeln(pos('o', a), pos('zz', a), pos('', a), pos('world', a), pos(ss, a), pos('o', ''), pos('l', 'hello'), pos(Ch, a));
  b := a; delete(b, 1, 6); writeln(b, a); delete(b, 3, 99); writeln(b); delete(b, 9, 1); writeln(b); delete(b, 1, 0); writeln(b); delete(b, 0, 1); writeln(b); delete(b, 1, 99); writeln('[', b, ']', length(b)); delete(b, 1, 1); writeln('[', b, ']');
  b := 'ac'; insert('b', b, 2); writeln(b); insert('X', b, 1); writeln(b); insert('Y', b, 99); writeln(b); insert('Z', b, 0); writeln(b); insert('', b, 1); writeln(b); c := ''; insert('q', c, 1); writeln(c); insert(ss, c, 1); writeln(c); insert(Ch, c, 2); writeln(c);
  b := 'abc'; setlength(b, 2); writeln(b, length(b)); setlength(b, 0); writeln('[', b, ']', length(b)); setlength(b, 3); b[1] := 'p'; b[2] := 'q'; b[3] := 'r'; writeln(b); c := b; setlength(c, 3); c[1] := 'C'; writeln(b, c); setlength(c, 1000); writeln(length(c), c[1], ord(c[3]));
  ss := 'abc'; setlength(ss, 2); writeln(ss); setlength(ss, 1); writeln(ss, length(ss));
  writeln(concat(a, 'x'), concat('y', a), concat(a, a, 'z'), concat('p', 'q'), length(concat(Rep('r', 200), Rep('s', 200))));
  writeln(sizeof(a), sizeof(ss));
  a := 'c string'; pc := pchar(a); writeln(CStr(pc), CStr(pchar(a)));
  b := ''; writeln(CStr(pchar(b)));
  buf[0] := 'L'; buf[1] := 'i'; buf[2] := #0; pc := @buf[0]; a := pc; writeln(a); a := ansistring(pc); writeln(a, length(a)); pc := nil; a := pc; writeln('[', a, ']');
  a := ansistring('cast'); writeln(a); ss := ansistring('x') + 'y'; writeln(ss); a := ansistring(ss); writeln(a); writeln(pointer(a) <> nil); b := ''; writeln(pointer(b) = nil);
  buf[0] := 'o'; buf[1] := 'k'; buf[2] := #0; a := pchar(@buf[0]); writeln(a);
  a := 'loop'; for i := 1 to 3 do a := a + IntToStr(i); writeln(a);
  a := ''; i := 0; while length(a) < 5 do begin a := a + '.'; i := i + 1; end; writeln(a, i);
  a := 'abc'; repeat a := copy(a, 2, 99); until length(a) <= 1; writeln(a);
  if a + 'x' = 'cx' then writeln('yes') else writeln('no');
  case a[1] of 'c': writeln('case c'); else writeln('other'); end;
  writeln(a[1] in ['a'..'c']);
  d := 'explicit'; a := d; d[1] := 'E'; writeln(a, d);
  a := 'with'#0'nul'; writeln(length(a), a[5] = #0, copy(a, 6, 3));
  writeln('done');
end.
