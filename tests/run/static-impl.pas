{ static repeated on the implementation of a class method declared static
  (a type helper's, an advanced record's, a class's), with inline, as in Free
  Pascal's RTL (Class Function TORDINALHELPER.Parse(...): TORDINALTYPE;
  inline; static;); on a plain routine it has no effect. Output is
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/static-impl.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/static-impl.expected
}
program staticimpl;
{$modeswitch typehelpers}
{$modeswitch advancedrecords}
type
  TORDINALTYPE = longint;
  TORDINALHELPER = type helper for TORDINALTYPE
    class function Parse(const AString: string): TORDINALTYPE; inline; static;
    class function Twice(x: longint): longint; static;
  end;
  TRec = record
    class function Make: longint; static;
  end;
  TObj = class
    class function Name: string; static;
    class procedure Hello; static;
  end;
Class Function TORDINALHELPER.Parse(const AString: string): TORDINALTYPE; inline; static;
begin
  Parse := Length(AString);
end;
class function TORDINALHELPER.Twice(x: longint): longint; static;
begin
  Twice := x * 2;
end;
class function TRec.Make: longint; static;
begin
  Make := 7;
end;
class function TObj.Name: string; static;
begin
  Name := 'obj';
end;
class procedure TObj.Hello; static;
begin
  writeln('hello');
end;

procedure Plain; static;
begin
  writeln('plain');
end;
begin
  writeln(TORDINALTYPE.Parse('abc'), ' ', TORDINALTYPE.Twice(4), ' ', TRec.Make, ' ', TObj.Name);
  TObj.Hello;
  Plain;
end.
