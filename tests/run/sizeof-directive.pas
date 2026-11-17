{ sizeof(predefined type) in $IF: each size the lexer knows, checked against
  the sizeof of the generated code. Not compared with Free Pascal, whose
  integer is 2 bytes in objfpc mode.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/sizeof-directive.expected %t.out
  RUN: %p3 --mode=tp %s -o %t.tp
  RUN:  %t.tp > %t.tp.out
  RUN: diff %S/sizeof-directive.expected %t.tp.out
}
program sizeofdirective;

procedure check(const name: string; actual, expected: int64);
begin
  if actual = expected then
    writeln(name, ' ', actual)
  else
    writeln(name, ': $IF says ', expected, ', sizeof says ', actual);
end;

procedure wrong(const name: string);
begin
  writeln(name, ': no $IF branch matched');
end;

begin
  {$if sizeof(char) = 1} check('char', sizeof(char), 1); {$else} wrong('char'); {$endif}
  {$if sizeof(boolean) = 1} check('boolean', sizeof(boolean), 1); {$else} wrong('boolean'); {$endif}
  {$if sizeof(byte) = 1} check('byte', sizeof(byte), 1); {$else} wrong('byte'); {$endif}
  {$if sizeof(shortint) = 1} check('shortint', sizeof(shortint), 1); {$else} wrong('shortint'); {$endif}
  {$if sizeof(word) = 2} check('word', sizeof(word), 2); {$else} wrong('word'); {$endif}
  {$if sizeof(smallint) = 2} check('smallint', sizeof(smallint), 2); {$else} wrong('smallint'); {$endif}
  {$if sizeof(integer) = 4} check('integer', sizeof(integer), 4); {$else} wrong('integer'); {$endif}
  {$if sizeof(longint) = 4} check('longint', sizeof(longint), 4); {$else} wrong('longint'); {$endif}
  {$if sizeof(cardinal) = 4} check('cardinal', sizeof(cardinal), 4); {$else} wrong('cardinal'); {$endif}
  {$if sizeof(longword) = 4} check('longword', sizeof(longword), 4); {$else} wrong('longword'); {$endif}
  {$if sizeof(int64) = 8} check('int64', sizeof(int64), 8); {$else} wrong('int64'); {$endif}
  {$if sizeof(real) = 8} check('real', sizeof(real), 8); {$else} wrong('real'); {$endif}
  {$if sizeof(double) = 8} check('double', sizeof(double), 8); {$else} wrong('double'); {$endif}
  {$if sizeof(pointer) = 8} check('pointer', sizeof(pointer), 8); {$else} wrong('pointer'); {$endif}
  {$if sizeof(pchar) = 8} check('pchar', sizeof(pchar), 8); {$else} wrong('pchar'); {$endif}
  {$if sizeof(ansistring) = 8} check('ansistring', sizeof(ansistring), 8); {$else} wrong('ansistring'); {$endif}
  {$if sizeof(shortstring) = 256} check('shortstring', sizeof(shortstring), 256); {$else} wrong('shortstring'); {$endif}
  {$if sizeof(sizeint) = 8} check('sizeint', sizeof(sizeint), 8); {$else} wrong('sizeint'); {$endif}
  {$if sizeof(ptrint) = 8} check('ptrint', sizeof(ptrint), 8); {$else} wrong('ptrint'); {$endif}
  {$if sizeof(nativeint) = 8} check('nativeint', sizeof(nativeint), 8); {$else} wrong('nativeint'); {$endif}
  {$if sizeof(dword) = 4} check('dword', sizeof(dword), 4); {$else} wrong('dword'); {$endif}
  {$if sizeof(qword) = 8} check('qword', sizeof(qword), 8); {$else} wrong('qword'); {$endif}
  {$if sizeof(uint64) = 8} check('uint64', sizeof(uint64), 8); {$else} wrong('uint64'); {$endif}
  {$if sizeof(nativeuint) = 8} check('nativeuint', sizeof(nativeuint), 8); {$else} wrong('nativeuint'); {$endif}
  {$if sizeof(ptruint) = 8} check('ptruint', sizeof(ptruint), 8); {$else} wrong('ptruint'); {$endif}
  {$if sizeof(sizeuint) = 8} check('sizeuint', sizeof(sizeuint), 8); {$else} wrong('sizeuint'); {$endif}
  {$if sizeof(pguid) = 8} check('pguid', sizeof(pguid), 8); {$else} wrong('pguid'); {$endif}
  {$if sizeof(tguid) = 16} check('tguid', sizeof(tguid), 16); {$else} wrong('tguid'); {$endif}
  {$if sizeof(SizeInt)>sizeof(Integer)} writeln('sizeint is wider'); {$endif}
  {$IF SIZEOF(CHAR)=2} writeln('wide char'); {$ELSE} writeln('narrow char'); {$ENDIF}
end.
