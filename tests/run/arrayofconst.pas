{ Array of const: TVarRec's fields, forwarding, overloads, methods, CreateFmt;
  output identical to Free Pascal 3.2.2's (fpc -Mobjfpc) on 64-bit targets
  without extended reals.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/arrayofconst.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/arrayofconst.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/arrayofconst.expected %t.g.out
}
program arrayofconst;
{$H+}
uses sysutils;
type
  TKind = class end;
  TFoo = class
    n: string;
    procedure Log(const a: array of const);
    constructor Make(const f: string; const a: array of const);
  end;
  EMine = class(Exception);
  ESub = class(EMine);
  TColor = (red, green);
  TCSet = set of TColor;
function Describe(const a: array of const): string;
var i: longint;
begin
  Result := '';
  for i := low(a) to high(a) do
  begin
    if i > 0 then Result := Result + ',';
    case a[i].VType of
      vtInteger: Result := Result + 'int:' + IntToStr(a[i].VInteger);
      vtInt64: Result := Result + 'i64:' + IntToStr(a[i].VInt64^);
      vtBoolean: Result := Result + 'bool:' + BoolToStr(a[i].VBoolean, true);
      vtChar: Result := Result + 'chr:' + a[i].VChar;
      vtExtended: Result := Result + 'flt:' + FloatToStr(a[i].VExtended^);
      vtString: Result := Result + 'str:' + a[i].VString^;
      vtPChar: Result := Result + 'pch:' + StrPas(a[i].VPChar);
      vtAnsiString: Result := Result + 'ans:' + string(a[i].VAnsiString);
      vtPointer: Result := Result + 'ptr:' + BoolToStr(a[i].VPointer = nil, true);
      vtObject: Result := Result + 'obj:' + a[i].VObject.ClassName;
      vtClass: Result := Result + 'cls:' + a[i].VClass.ClassName;
    else Result := Result + '?';
    end;
  end;
end;
function Count(const a: array of const): longint;
begin Count := length(a); end;
function Pass(const f: string; const a: array of const): string;
begin Pass := Format('<' + f + '>', a) + IntToStr(Count(a)); end;
function ByValue(a: array of const): longint;
begin ByValue := high(a) + 1; end;
procedure TFoo.Log(const a: array of const);
begin writeln(n, ': ', Describe(a)); end;
constructor TFoo.Make(const f: string; const a: array of const);
begin n := Format(f, a); end;
function Over(const a: array of const): string; overload;
begin Over := 'const'; end;
function Over(c: TCSet): string; overload;
begin Over := 'set'; end;
function Over(x: longint): string; overload;
begin Over := 'int'; end;
function Up(const s: string): string;
begin Up := UpperCase(s); end;
var
  v: TVarRec;
  o: TFoo;
  ks: shortstring = 'shrt';
  k: longint;
  i64: int64 = 7;
  r: double = 2.5;
  s: string = 'an';
  buf: array[0..1] of char;
  ch: char = 'z';
  ba: packed array[1..3] of char;
  sr: 1..9 = 5;
  w: word = 65535;
begin
  buf[0] := 'b'; buf[1] := #0; ba[1] := 'x'; ba[2] := 'y'; ba[3] := 'z';
  writeln(Describe([]));
  writeln(Describe([1, -1, sr, w, i64, r, 1.5, ks, s, 'lit', '', 'c', ch, true, false, pchar(@buf[0]), nil, pointer(nil)]));
  o := TFoo.Create; o.n := 'foo';
  writeln(Describe([o, TFoo, TObject]));
  o.Log([1, 'two']);
  o.Free;
  o := TFoo.Make('%d-%s', [3, 'x']); writeln(o.n); o.Free;
  writeln(Pass('%s %s', ['a', 'b']));
  writeln(Pass('none', []));
  writeln(ByValue([1, 2, 3]));
  writeln(Over([1, 'a']), Over(5));
  for k := 1 to 3 do writeln(Format('%d:%s', [k, Up(s + IntToStr(k))]));
  writeln(Format('%s %s', [s + 'x', Up('y')]));
  try raise EMine.CreateFmt('m %d', [1]); except on e: EMine do writeln(e.ClassName, ' ', e.Message); end;
  try raise ESub.CreateFmt('n %s', ['s']); except on e: Exception do writeln(e.ClassName, ' ', e.Message); end;
  try raise Exception.CreateFmt('%d', ['bad']); except on e: Exception do writeln(e.ClassName, ' ', e.Message); end;
  try writeln(Format('%q %d', ['x'])); except on e: EConvertError do writeln(e.ClassName, ' ', e.Message); end;
  v.VType := vtInteger; v.VInteger := 9; writeln(v.VType, v.VInteger);
end.
