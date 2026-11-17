{ class var sections (static class fields, objfpc) in advanced records and
  type helpers, as in classes and as in Free Pascal: private or public, one
  variable for the type (not part of a record's size), used by its methods
  (also static ones) and through the type (TRec.Count, longint.Full).
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/classvars-records.expected
  RUN: %p3 --mode=objfpc -O2 -g %s -o %t.O2
  RUN: %t.O2 | diff - %S/classvars-records.expected
}
program classvarsrecords;
{$H+}
{$modeswitch advancedrecords}
{$modeswitch typehelpers}
type
  TObj = class
  private
    class var FFull : string;
  public
    class var Count: integer;
    class function Full: string; static;
  end;
  TRec = record
  private
    class var FFull : string;
  public
    x: integer;
    class var Count: integer;
    class function Full: string; static;
    procedure Bump;
  end;
  THelp = type helper for longint
  private
    class var FFull : string;
  public
    class function Full: string; static;
    function Tag: string;
  end;
class function TObj.Full: string; begin FFull := FFull + 'o'; Full := FFull; end;
class function TRec.Full: string; begin FFull := FFull + 'r'; Full := FFull; end;
procedure TRec.Bump; begin Inc(Count); FFull := FFull + '!'; end;
class function THelp.Full: string; begin FFull := FFull + 'h'; Full := FFull; end;
function THelp.Tag: string; begin Tag := FFull + chr(48 + Self); end;
var r, r2: TRec; i: longint;
begin
  writeln(TObj.Full, TObj.Full, ' ', TRec.Full, ' ', longint.Full);
  TObj.Count := 3; writeln(TObj.Count);
  r.Bump; r2.Bump; writeln(TRec.Count, ' ', TRec.Full, ' ', SizeOf(TRec));
  i := 5; writeln(i.Tag, ' ', longint.Full);
end.
