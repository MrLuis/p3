{ pcc's unit library: output identical to Free Pascal 3.2.2's (fpc -Mobjfpc -Sh-).
  Run in its own directory: it creates and removes directories.
  RUN: rm -rf %t && mkdir -p %t && %pcc --mode=objfpc %s -o %t/a
  RUN: cd %t && env T_SET_VAR=val ./a > out && diff %S/rtl-sysutils.expected out
  RUN: %pcc --mode=objfpc -O2 %s -o %t/a2
  RUN: cd %t && env T_SET_VAR=val ./a2 > out2 && diff %S/rtl-sysutils.expected out2
}
program rtlsysutils;
uses sysutils;
var i: longint; i64: int64; d: double; w1,w2,w3,w4: word; dt: TDateTime; s: string;
procedure si(const t: string);
var v: longint; ok: boolean;
begin v := -7; ok := TryStrToInt(t, v); writeln('[', t, '] ', ok, ' ', v); end;
procedure si64(const t: string);
var v: int64; ok: boolean;
begin v := -7; ok := TryStrToInt64(t, v); writeln('[', t, '] ', ok, ' ', v); end;
procedure sf(const t: string);
var v: double; ok: boolean;
begin v := -7; ok := TryStrToFloat(t, v); writeln('[', t, '] ', ok, ' ', FloatToStr(v)); end;
procedure ff(x: double);
begin writeln(FloatToStr(x)); end;
procedure fn(const n: string);
begin
  writeln('[', n, '] path=', ExtractFilePath(n), ' dir=', ExtractFileDir(n), ' name=', ExtractFileName(n),
          ' ext=', ExtractFileExt(n), ' chg=', ChangeFileExt(n, '.x'), ' inc=', IncludeTrailingPathDelimiter(n), ' exc=', ExcludeTrailingPathDelimiter(n));
end;
function sgn(x: longint): longint;
begin if x<0 then sgn:=-1 else if x>0 then sgn:=1 else sgn:=0 end;
begin
  writeln(IntToStr(0),' ',IntToStr(-1),' ',IntToStr(2147483647),' ',IntToStr(low(int64)),' ',IntToStr(high(int64)));
  i:=-1; i64:=-1; writeln(IntToHex(i,8),' ',IntToHex(i,1),' ',IntToHex(i,12),' ',IntToHex(i64,1),' ',IntToHex(i64,18)); i:=255; i64:=255; writeln(IntToHex(i,1),' ',IntToHex(i,3),' ',IntToHex(i64,4),' ',IntToHex(int64(4096),16),' ',IntToHex(int64(1),20));
  si('12'); si(' 12'); si('12 '); si('+5'); si('-5'); si('$FF'); si('0xff'); si('0XFF'); si('&17'); si('%101'); si(''); si(' '); si('-'); si('+'); si('$'); si('x'); si('2147483647'); si('-2147483648'); si('1a'); si('-$10');
  si64('9223372036854775807'); si64('9223372036854775808'); si64('-9223372036854775808'); si64('-9223372036854775809'); si64('$7FFFFFFFFFFFFFFF');
  writeln(StrToIntDef('zz',42),' ',StrToIntDef('7',42),' ',StrToInt64Def('zz',43),' ',StrToInt('99'),' ',StrToInt64('-99'));
  ff(0); ff(1); ff(-1); ff(3.5); ff(0.1); ff(100); ff(1e14); ff(1e15); ff(1e16); ff(123456789012345); ff(1234567890123456); ff(1e20); ff(1.5e20); ff(0.001); ff(0.0001); ff(0.00001); ff(0.000012); ff(0.000001); ff(1.5e-7); ff(1/3); ff(2/3); ff(-1234.5678); ff(1e100); ff(1e-100); ff(999999999999999); ff(9999999999999995);
  sf('1.5'); sf(' 1.5'); sf('1.5 '); sf('.5'); sf('5.'); sf('1e3'); sf('1E-3'); sf('-2.5'); sf('+2.5'); sf(''); sf('x'); sf('inf'); sf('nan'); sf('0x10'); sf('1e'); sf('--1'); sf('INF'); sf('-inf'); sf('NaN'); sf('infinity'); sf('1.5e'); sf('1.2.3');
  writeln(StrToFloatDef('zz',1.5):0:2,' ',StrToFloat('2.25'):0:2);
  writeln(BoolToStr(true),' ',BoolToStr(false),' ',BoolToStr(true,true),' ',BoolToStr(false,true),' ',BoolToStr(true,false));
  writeln('[',Trim('  a b '#9),'][',TrimLeft('  a '),'][',TrimRight(' a  '),'][',Trim(''),'][',Trim('   '),']');
  writeln(UpperCase('aBc1z'),' ',LowerCase('aBc1Z'),' ',sgn(CompareStr('a','b')),sgn(CompareStr('b','a')),sgn(CompareStr('a','a')),sgn(CompareStr('a','ab')),sgn(CompareStr('A','a')));
  writeln(sgn(CompareText('a','B')),sgn(CompareText('ABC','abc')),sgn(CompareText('abd','ABC')),' ',SameText('Ab','aB'),' ',SameText('a','b'));
  writeln(QuotedStr('it''s'),' ',QuotedStr(''));
  writeln(StringReplace('aXbXc','X','--',[]),' ',StringReplace('aXbXc','X','--',[rfReplaceAll]),' ',StringReplace('axbXc','X','-',[rfReplaceAll,rfIgnoreCase]),' ',StringReplace('abc','','-',[rfReplaceAll]),' ',StringReplace('aaa','a','aa',[rfReplaceAll]));
  writeln(IsDelimiter(',;','a,b',2),' ',IsDelimiter(',;','a,b',1),' ',IsDelimiter(',;','a,b',9),' ',LastDelimiter(',;','a,b;c'),' ',LastDelimiter(',','abc'));
  fn('/a/b/c.txt'); fn('c.tar.gz'); fn('/a/b/'); fn('/'); fn(''); fn('a/b'); fn('a\b.c'); fn('.bashrc'); fn('/x/.bashrc'); fn('a.b/c'); fn('//a//b'); fn('a.'); fn('/a');
  dt := EncodeDate(2026,11,10); writeln(dt:0:1); DecodeDate(dt,w1,w2,w3); writeln(w1,'-',w2,'-',w3,' ',DayOfWeek(dt));
  dt := EncodeDate(2024,2,29); writeln(dt:0:1); DecodeDate(dt,w1,w2,w3); writeln(w1,'-',w2,'-',w3,' ',DayOfWeek(dt));
  dt := EncodeDate(1900,1,1); writeln(dt:0:1); dt := EncodeDate(1,1,1); writeln(dt:0:1); DecodeDate(dt,w1,w2,w3); writeln(w1,'-',w2,'-',w3);
  dt := EncodeDate(9999,12,31); writeln(dt:0:1); DecodeDate(dt,w1,w2,w3); writeln(w1,'-',w2,'-',w3);
  dt := EncodeTime(13,45,30,500); writeln(dt:0:9); DecodeTime(dt,w1,w2,w3,w4); writeln(w1,':',w2,':',w3,'.',w4);
  dt := EncodeDate(2000,3,1)+EncodeTime(23,59,59,999); DecodeTime(dt,w1,w2,w3,w4); writeln(w1,':',w2,':',w3,'.',w4); DecodeDate(dt,w1,w2,w3); writeln(w1,'-',w2,'-',w3);
  writeln(IsLeapYear(2024),IsLeapYear(1900),IsLeapYear(2000),IsLeapYear(2023));
  try i:=StrToInt('abc'); writeln(i) except on e: EConvertError do writeln(e.ClassName,': ',e.Message) end;
  try i64:=StrToInt64('9x'); writeln(i64) except on e: Exception do writeln(e.ClassName,': ',e.Message) end;
  try d:=StrToFloat('q'); writeln(d) except on e: EConvertError do writeln(e.Message) end;
  try dt:=EncodeDate(2023,2,29); writeln(dt) except on e: EConvertError do writeln(e.ClassName) end;
  try dt:=EncodeTime(24,0,0,0); writeln(dt) except on e: EConvertError do writeln(e.ClassName) end;
  writeln(PathDelim,' ',PathSep,' ',DirectorySeparator);
  s := GetCurrentDir; writeln(s<>'', DirectoryExists(s));
  writeln(FileExists('t_none'),DirectoryExists('t_none'));
  writeln(ForceDirectories('t_d/x/y'),DirectoryExists('t_d/x/y'),FileExists('t_d/x/y'));
  writeln(CreateDir('t_d'),RemoveDir('t_d/x/y'),RemoveDir('t_d/x'),RemoveDir('t_d'),RemoveDir('t_d'));
  writeln(DeleteFile('t_none'),RenameFile('t_none','t_none2'));
  writeln(GetEnvironmentVariable('T_NONE_VAR')='',GetEnvironmentVariable('T_SET_VAR'));
  writeln(GetTickCount64>0, Now>40000, Date>40000, Time>=0);
end.
