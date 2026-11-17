{ Conditional compilation: $DEFINE / $UNDEF, $IFDEF / $IFNDEF, $IF with
  defined(), not, and, or, true, false and parentheses,
  $ELSEIF / $ELSE / $ENDIF / $IFEND, nesting, skipped text that would not
  compile (unterminated strings, bad characters, directives), (* *) and //
  directives and comments, -d on the command line and the predefined P3
  symbols. The output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc -dP3 -dP3_TP -dEXTRA).
  RUN: %p3 --mode=tp -dEXTRA %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/conditionals.expected %t.out
  RUN: %p3 --mode=tp -O2 -d extra %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/conditionals.expected %t.O2.out
  RUN: %p3 --mode=tp -g -dExtra %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/conditionals.expected %t.g.out
}
program conditionals;

{$DEFINE Debug}
{$define LEVEL2}

{$ifdef debug}
const Mode = 'debug';
{$else}
const Mode = 'release'; 'an unterminated string ? # %
  { a comment: $ENDIF inside the skipped text is still a comment }
  // and so is this one: {$endif}
  (* and this: {$else} *)
  {$I no-such-file.inc}
{$endif}

{$IF defined(DEBUG) and not defined(RELEASE)}
const Level = 1;
{$ELSEIF defined(LEVEL2)}
const Level = 2;
{$ELSE}
const Level = 3;
{$IFEND}

{$undef LEVEL2}
{$ifndef LEVEL2} const Gone = true; {$else} const Gone = false; {$endif}

{$IFDEF EXTRA}
procedure Extra;
begin
  writeln('extra was defined on the command line');
end;
{$ENDIF}

var
  i: integer;

begin
  writeln('mode ', Mode, ', level ', Level, ', level2 gone ', Gone);
{$ifdef P3}
  writeln('compiled by p3');
  {$ifdef P3_TP} writeln('in tp mode'); {$endif}
  {$ifdef P3_ISO} writeln('in iso mode'); {$endif}
{$else}
  writeln('compiled by something else');
{$endif}
  {$IFDEF EXTRA} Extra; {$ENDIF}
  {$IFNDEF EXTRA} writeln('no extra'); {$ENDIF}

  { Nested conditionals; an inner one inside a skipped branch is skipped
    whatever its condition. }
  for i := 1 to 2 do
  begin
    {$ifdef DEBUG}
      {$if defined(NOPE)}
        writeln('nope');
      {$elseif true}
        writeln(i, ': debug, elseif true');
      {$endif}
    {$else}
      {$ifndef NOPE}
        writeln('not reached');
      {$else}
        writeln('not reached either');
      {$endif}
    {$endif}
  end;

  {$IF (defined(A) or defined(debug)) and (not defined(B) and true)}
  writeln('parentheses');
  {$IFEND}
  {$IF false}
  writeln('false');
  {$ELSEIF not true}
  writeln('not true');
  {$ELSEIF not not true}
  writeln('second elseif');
  {$ELSEIF true}
  writeln('a later true branch is not taken');
  {$ELSE}
  writeln('else');
  {$ENDIF}
  (*$define LATE*)
  (*$ifdef late*) writeln('(*$...*) directives'); (*$endif*)
  writeln('the end');
end.
