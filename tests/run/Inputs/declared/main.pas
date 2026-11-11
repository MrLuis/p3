program main;
uses files;
{ a fallback, as Free Pascal's sources do, when a unit lacks a type }
{$if not declared(TFilenameCaseMatch)}
type TFilenameCaseMatch = (mkNone, mkExactMatch);
{$endif}
{$if not declared(TFileSearchOption)}
type TFileSearchOption = (sfoImplicitFilename);
{$endif}
type TFoo = integer;
const K = 1;
{$if declared(TFoo) and declared(K)} const A1 = 'decls'; {$else} const A1 = 'nodecls'; {$endif}
{$if declared(TBar)} const A2 = 'TBar'; {$else} const A2 = 'noTBar'; {$endif}
{$if declared(writeln) and declared(integer) and declared(Describe) and declared(mkAmbiguous)} const A3 = 'std+unit'; {$else} const A3 = 'missing'; {$endif}
{$if declared(Later)} const A4 = 'Later'; {$else} const A4 = 'noLater'; {$endif}
{$if declared(PathDelim)} const A8 = 'indirect'; {$else} const A8 = 'noIndirect'; {$endif}
procedure P(Param: integer);
var
  Loc: integer;
begin
  Loc := Param;
  {$if declared(Loc) and declared(Param)} writeln('Loc and Param inside ', Loc); {$endif}
end;
{$if declared(Loc)} const A5 = 'LocOutside'; {$else} const A5 = 'noLocOutside'; {$endif}
{$if declared(P) and declared(files)} const A6 = 'P+unitname'; {$else} const A6 = 'no'; {$endif}
var
  Later: integer;
  m: TFilenameCaseMatch;
  o: TFileSearchOption;
begin
  Later := 0;
  m := mkAmbiguous;
  o := sfoImplicitFilename;
  writeln(A1, ' ', A2, ' ', A3, ' ', A4, ' ', A5, ' ', A6, ' ', A8, ' ', ord(m), ord(o), Later);
  writeln(Describe);
  P(7);
end.
