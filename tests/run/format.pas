{ Format: every conversion, with its quirks and errors, on
  output identical to Free Pascal 3.2.2's (fpc -Mobjfpc) on 64-bit targets
  without extended reals.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/format.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/format.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/format.expected %t.g.out
}
program format;
{$H+}
uses sysutils;
procedure t(const f: string; const a: array of const);
begin
  try writeln('[', f, '] -> [', Format(f, a), ']');
  except on e: Exception do writeln('[', f, '] ! ', e.ClassName, ': ', e.Message); end;
end;
procedure vt(const a: array of const);
var i: longint;
begin write(high(a), ' ', low(a), ' ', length(a), ':'); for i := 0 to high(a) do write(' ', a[i].VType); writeln; end;
var i: longint=42; n: int64=1234567890123; c: cardinal=4000000000; b: byte=7; d: double=3.14159; ss: shortstring='short'; s: string='ansi'; p: pchar; bo: boolean=true; ch: char='c'; o: TObject; w: word=9; si: smallint=-3; pb: array[0..2] of char;
begin
  pb[0]:='p'; pb[1]:='c'; pb[2]:=#0; p:=@pb[0];
  vt([]); vt([1]); vt([i, n, c, b, w, si, d, ss, s, p, bo, ch, 'x', 'lit', 1.5, nil]);
  o := nil; vt([o, TObject, pointer(p)]);
  t('%d', [i]); t('%d', [n]); t('%d', [c]); t('%d', [-5]); t('%5d|%-5d|%05d', [1,2,3]); t('%.3d', [5]); t('%.3d', [-5]); t('%8.3d', [5]); t('%-8.3d|', [5]); t('%d', ['x']); t('%d', [1.5]); t('%d', []); t('%d %d', [1]);
  t('%u', [5]); t('%u', [-5]); t('%u', [n]); t('%u', [int64(-1)]); t('%5.3u', [7]);
  t('%x', [255]); t('%X', [255]); t('%x', [-1]); t('%x', [int64(-1)]); t('%.4x', [255]); t('%8x|%-8x|', [255,255]); t('%x', ['a']);
  t('%s', ['lit']); t('%s', [s]); t('%s', [ss]); t('%s', [p]); t('%s', [ch]); t('%s', ['c']); t('%5s|%-5s|', ['a','b']); t('%.2s', ['abcdef']); t('%5.2s|', ['abcdef']); t('%s', [1]); t('%s', [1.5]); t('%s', [bo]); t('%s', [nil]);
  t('%f', [1.5]); t('%f', [d]); t('%.0f', [2.5]); t('%.0f', [3.5]); t('%.1f', [0.25]); t('%.3f', [1.0005]); t('%8.2f|%-8.2f|', [1.5,1.5]); t('%f', [1]); t('%f', ['x']); t('%f', [-1.5]); t('%f', [1e20]); t('%.2f', [1e15]); t('%.2f', [0.001]);t('%.20f', [1.5]);
  t('%e', [1.5]); t('%e', [d]); t('%.3e', [1234.5]); t('%.1e', [1234.5]); t('%.0e', [1234.5]); t('%e', [0.0]); t('%e', [-1e-10]); t('%e', [1e100]); t('%12.3e|', [1.5]);
  t('%g', [1.5]); t('%g', [d]); t('%g', [100.0]); t('%g', [1e20]); t('%g', [1e-5]); t('%g', [0.0001]); t('%g', [1234567.0]); t('%.3g', [1234.5]); t('%.3g', [1.23456]); t('%g', [1e15]); t('%g', [1e14]); t('%12g|', [1.5]);
  t('%n', [1234567.891]); t('%n', [1.5]); t('%n', [-1234.5]); t('%.0n', [1234.5]); t('%.3n', [999.9999]); t('%n', [0.5]); t('%15n|', [1234.5]); t('%n', [123456789012.5]);
  t('%m', [1234.5]); t('%m', [-1.5]); t('%.0m', [5.0]); t('%15m|', [5.0]);
  t('%p', [pointer(nil)]); t('%p', [nil]); t('%p', [1]); t('%.4p', [pointer(nil)]);
  t('%%', []); t('100%%', []); t('a%%b%s', ['c']); t('%', []); t('%z', [1]); t('abc', [1,2]); t('', []); t('%5', [1]); t('%-', [1]);
  t('%1:s %0:s', ['a','b']); t('%0:s %s %s', ['a','b','c']); t('%1:s %s', ['a','b','c']); t('%2:s', ['a']); t('%s %0:s', ['a']); t('%-1:s', ['a']);t('%0:5s|', ['a']);t('%0:-5s|', ['a']);
  t('%*d', [5, 1]); t('%-*d|', [5, 1]); t('%.*d', [3, 1]); t('%*.*f', [8, 2, 1.5]); t('%*d', ['x', 1]); t('%*:s', [1,'a','b']); t('%*d', [-5, 1]);
  t('%.0f', [-0.001]); t('%.2f', [-0.001]); t('%f', [0.0]); t('%n', [-0.001]); t('%.20e', [1.5]); t('%.16e', [1.5]); t('%.0g', [1.5]); t('%.20g', [1.5]); t('%g', [1e-6]); t('%g', [-1.5e30]); t('%.2g', [0.00123]); t('%.20x', [255]); t('%.40x', [int64(255)]); t('%.17x', [255]); t('%.3u', [int64(5)]); t('%e', [999999999999999.9]); t('%.2e', [9.999]); t('%.3f', [999.9996]); t('%.0n', [999.5]); t('%.1f', [0.05]); t('%.1f', [0.04]); t('%.0f', [0.5]); t('%.0f', [0.4]);
  t('%D %S %F %E %G %N %M %U %X %P', [1,'a',1.5,1.5,1.5,1.5,1.5,1,1,pointer(nil)]);
  t('%d%s%f', [1,'x',1.0]); writeln(Format('%s=%d', ['k', 3]));
  try raise Exception.CreateFmt('code %d: %s', [7, 'bad']); except on e: Exception do writeln(e.ClassName, ' ', e.Message); end;
end.
