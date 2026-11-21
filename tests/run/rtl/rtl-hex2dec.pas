{ Hex2Dec parses the digits in place (docs/strutils-review.md ST-12): the
  same answers as StrToInt('$' + s) gave, with its longint range, and the
  same error, which the last resort still raises. Valid values agree with
  Free Pascal 3.2.2's (fpc -Mobjfpc); Free Pascal's own messages for
  invalid input differ (they name a Val code), so p3's own messages are
  printed as they are here.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-hex2dec.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-hex2dec.expected
}
program rtlhex2dec;
{$H+}
uses sysutils, strutils;

var
  seed: longint;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := (seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := (seed shr 8) mod n;
end;
{$Q+}{$R+}

{ the way it was }
function OldHex2Dec(const s: string): longint;
begin
  if (s <> '') and (s[1] = '$') then
    Result := StrToInt(s)
  else
    Result := StrToInt('$' + s);
end;

procedure Ask(const s: string; var value: longint; var raised: boolean;
              var msg: string; which: longint);
begin
  raised := false;
  msg := '';
  value := 0;
  try
    if which = 0 then
      value := Hex2Dec(s)
    else
      value := OldHex2Dec(s);
  except
    on E: EConvertError do
    begin
      raised := true;
      msg := E.Message;
    end;
  end;
end;

procedure Show(const s: string);
var
  value, oldvalue: longint;
  raised, oldraised: boolean;
  msg, oldmsg: string;
begin
  Ask(s, value, raised, msg, 0);
  Ask(s, oldvalue, oldraised, oldmsg, 1);
  if (value <> oldvalue) or (raised <> oldraised) or (msg <> oldmsg) then
    writeln('DIFF "', s, '"');
  try
    write('Hex2Dec("', s, '") = ', Hex2Dec(s));
  except
    on E: EConvertError do
      write('Hex2Dec("', s, '") raises: ', E.Message);
  end;
  writeln;
end;

procedure Compare(const s: string; var bad: longint);
var
  value, oldvalue: longint;
  raised, oldraised: boolean;
  msg, oldmsg: string;
begin
  Ask(s, value, raised, msg, 0);
  Ask(s, oldvalue, oldraised, oldmsg, 1);
  if (value <> oldvalue) or (raised <> oldraised) then
  begin
    bad := bad + 1;
    writeln('DIFF"', s, '" ', value, '/', oldvalue, ' ', raised, '/', oldraised);
  end;
end;

var
  i, n, h, k, bad: longint;
  s: string;
  c: char;
begin
  Show('FF');
  Show('$10');
  Show('0');
  Show('0A');
  Show('1A2B3C');
  Show('deadBEEF');
  Show('7FFFFFFF');
  Show('80000000');
  Show('FFFFFFFFFFFFFFFF');
  Show('');
  Show('$');
  Show('$$12');
  Show('$FF');
  Show('$ 12');
  Show('$0x10');
  Show('$%101');
  Show('123');
  Show('+123');
  Show('-$FF');
  Show(' -$FF');
  Show(' 123');
  Show('12 34');
  Show('1$2');
  Show('1#2');
  Show('gg');
  Show('%101');
  Show('00000000000000FF');
  Show('FFFF');
  Show('80');

  { 20000 random strings of hex digits with near-misses, against the old
    form }
  seed := 12345;
  bad := 0;
  for i := 1 to 20000 do
  begin
    s := '';
    for n := 1 to Rnd(9) do
    begin
      h := Rnd(20);
      case h of
        0..9:    c := chr(Ord('0') + h);
        10..15:  c := chr(Ord('a') + h - 10);
        16, 17:  c := chr(Ord('A') + Rnd(6));
        18:      c := chr(1 + Rnd(255));
      else
        c := chr(Ord('0') + Rnd(128));
      end;
      s := s + c;
    end;
    k := Rnd(4);
    if k = 0 then
      s := '$' + s
    else if k = 1 then
      s := ' ' + s
    else if k = 2 then
      s := '$' + s + ' ';
    Compare(s, bad);
  end;
  writeln('random differences: ', bad);
end.
