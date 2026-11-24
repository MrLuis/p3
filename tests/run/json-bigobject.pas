{ Unit json, objects with many members (a lookup table behind Get and Put):
  repeated and escaped keys in parsed text, Put replacing and adding,
  Delete, Clear, and 3000 members found one by one.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-bigobject.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-bigobject.expected
}
program jsonbigobject;
{$H+}
uses sysutils, json;

function Show(const V: TJsonValue): string;
begin
  if not V.Exists then
    Result := '-'
  else
    Result := IntToStr(V.AsInt64);
end;

var
  D: TJsonDocument;
  O: TJsonValue;
  Text: string;
  I, Bad: Integer;
begin
  { parsed: a repeated key (the first is found), an escaped key }
  Text := '{';
  for I := 0 to 39 do
    Text := Text + '"k' + IntToStr(I) + '":' + IntToStr(I) + ',';
  Text := Text + '"k5":500,"ab":77,"k39":390}';
  D := TJsonDocument.Create;
  D.Load(Text);
  O := D.Root;
  Writeln('parsed: ', O.Count, ' k0=', Show(O.Get('k0')), ' k5=', Show(O.Get('k5')),
          ' k39=', Show(O.Get('k39')), ' ab=', Show(O.Get('ab')), ' none=', Show(O.Get('none')));
  O.Put('k5', Int64(5000));
  O.Put('new', Int64(1));
  Writeln('after put: ', O.Count, ' k5=', Show(O.Get('k5')), ' new=', Show(O.Get('new')),
          ' ab=', Show(O.Get('ab')));
  O.Delete('k7');
  Writeln('after delete: ', O.Count, ' k7=', Show(O.Get('k7')), ' k8=', Show(O.Get('k8')),
          ' new=', Show(O.Get('new')));
  O.Put('k7', Int64(70));
  Writeln('put again: ', O.Count, ' k7=', Show(O.Get('k7')), ' key at end=', O.KeyAt(O.Count - 1));
  O.Clear;
  Writeln('cleared: ', O.Count, ' k0=', Show(O.Get('k0')));
  for I := 0 to 29 do
    O.Put('m' + IntToStr(I), Int64(I * 3));
  Writeln('refilled: ', O.Count, ' m29=', Show(O.Get('m29')), ' k0=', Show(O.Get('k0')));
  D.Free;

  { built: 3000 members, every one found; each replaced once }
  D := TJsonDocument.Create;
  O := D.NewObject;
  for I := 0 to 2999 do
    O.Put('member' + IntToStr(I), Int64(I));
  Bad := 0;
  for I := 0 to 2999 do
    if O.Get('member' + IntToStr(I)).AsInt64 <> I then
      Inc(Bad);
  for I := 0 to 2999 do
    O.Put('member' + IntToStr(I), Int64(I + 1));
  for I := 0 to 2999 do
    if O.Get('member' + IntToStr(I)).AsInt64 <> I + 1 then
      Inc(Bad);
  Writeln('built: ', O.Count, ' wrong=', Bad, ' none=', Show(O.Get('member3000')));
  D.Free;
end.
