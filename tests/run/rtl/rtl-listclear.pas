{ TList.Clear takes the items out from the last down, notifying each with
  lnDeleted and Count at its index (nil items are not notified), then
  shrinks the capacity once (docs/classes-review.md CL-4). Output
  identical to Free Pascal 3.2.2's.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-listclear.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-listclear.expected
}
program rtllistclear;
{$H+}
uses sysutils, classes;

type
  TLogList = class(TList)
  protected
    procedure Notify(Ptr: pointer; Action: TListNotification); override;
  end;

var
  log: string;
  cells: array[1..1000] of longint;

function IdOf(p: pointer): longint;
var
  k: longint;
begin
  for k := 1 to 1000 do
    if p = @cells[k] then
      exit(k);
  Result := 0;
end;

procedure TLogList.Notify(Ptr: pointer; Action: TListNotification);
begin
  if Action = lnDeleted then
    log := log + IntToStr(IdOf(Ptr)) + '@' + IntToStr(Count) + ' ';
end;

var
  l: TLogList;
  i: longint;
begin
  l := TLogList.Create;
  for i := 1 to 5 do
    l.Add(@cells[i]);
  l.Insert(2, nil);
  l.Clear;
  writeln(log, '| count ', l.Count);
  log := '';
  for i := 1 to 1000 do
    l.Add(@cells[i]);
  l.Clear;
  writeln('1000 cleared: count ', l.Count, ' log length ', length(log));
  writeln('first ', copy(log, 1, 14));
  l.Add(@cells[9]);
  writeln('after: ', l.Count, ' ', IdOf(l[0]));
  l.Free;
end.
