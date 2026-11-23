program shop;
{ Source generators and compiletime functions (see README.md): the types
  of unit orders get ToJson, and TStatus gets its names, from the
  generators in generators/; Crc32 uses a table the compiler computed. }
uses sysutils, orders;

{ a title underlined, computed while compiling }
function Underlined(const S: string): string; compiletime;
var
  I: Integer;
begin
  Result := S + #10;
  for I := 1 to Length(S) do
    Result := Result + '=';
end;

const
  Title = Underlined('Orders today');

var
  O: TOrder;
  S: TStatus;
  Arg: string;
begin
  WriteLn(Title);
  O.Number := 1042;
  O.Customer.Id := 7;
  O.Customer.Name := 'Ada "the Countess"';
  O.Customer.IsVip := True;
  O.Status := Paid;
  O.Total := 99.5;
  O.Note := 'leave at the door';
  WriteLn(O.ToJson);
  WriteLn('status: ', O.Status.ToString);
  if ParamCount > 0 then
    Arg := ParamStr(1)
  else
    Arg := 'shipped';
  if TStatus.TryParse(Arg, S) then
    WriteLn('''', Arg, ''' is ', S.ToString, ' (', Ord(S), ')')
  else
    WriteLn('''', Arg, ''' is not a status');
  WriteLn('crc32: ', IntToHex(Crc32(O.ToJson), 8));
end.
