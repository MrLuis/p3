{ pcc's unit library, Classes: TList and TStringList. Output identical to
  Free Pascal 3.2.2's (fpc -Mobjfpc -Sh-). Run in its own directory: it
  creates and removes files.
  RUN: rm -rf %t && mkdir -p %t && %pcc --mode=objfpc %s -o %t/a
  RUN: cd %t && ./a > out && diff %S/rtl-classes.expected out
  RUN: %pcc --mode=objfpc -O2 %s -o %t/a2
  RUN: cd %t && ./a2 > out2 && diff %S/rtl-classes.expected out2
  RUN: %pcc --mode=objfpc -fseparate-units %s -o %t/a3
  RUN: cd %t && ./a3 > out3 && diff %S/rtl-classes.expected out3
}
program rtlclasses;
uses sysutils, classes;
type
  PCell = ^longint;
  TNL = class(TList)
  protected
    procedure Notify(Ptr: pointer; Action: TListNotification); override;
  end;
  TBox = class
    v: longint;
    destructor Destroy; override;
  end;
var
  cells: array[0..1000] of longint;
function P(n: longint): pointer;
begin P := @cells[n]; end;
function V(q: pointer): longint;
begin if q = nil then V := 0 else V := PCell(q)^; end;
procedure TNL.Notify(Ptr: pointer; Action: TListNotification);
begin
  case Action of
    lnAdded: write('+');
    lnExtracted: write('x');
    lnDeleted: write('-');
  end;
  writeln(V(Ptr));
end;
destructor TBox.Destroy;
begin
  writeln('box ', v, ' freed');
  inherited Destroy;
end;
function ByValue(a, b: pointer): longint;
begin ByValue := V(a) - V(b); end;
function ByTens(a, b: pointer): longint;
begin ByTens := V(a) div 10 - V(b) div 10; end;
function ByLen(l: TStringList; a, b: longint): longint;
begin ByLen := length(l[a]) - length(l[b]); end;
procedure show(l: TList);
var i: longint;
begin
  write('[', l.Count, '/', l.Capacity, ']');
  for i := 0 to l.Count - 1 do write(' ', V(l[i]));
  writeln;
end;
procedure shows(s: TStrings);
var i: longint;
begin
  write('{', s.Count, '}');
  for i := 0 to s.Count - 1 do write(' <', s[i], '>');
  writeln;
end;
procedure tryl(l: TList; n: longint);
begin
  try
    case n of
      0: l.Delete(5);
      1: l.Capacity := -1;
      2: writeln(V(l[99]));
      3: l.Count := -1;
      4: l.Exchange(0, 99);
      5: l.Exchange(99, 0);
      6: l.Move(99, 0);
      7: l.Move(0, 99);
      8: l.Insert(99, nil);
      9: l[99] := nil;
      10: l.Capacity := 0;
    end;
    writeln(n, ' ok');
  except
    on e: Exception do writeln(n, ' ', e.ClassName, ': ', e.Message);
  end;
end;
procedure trys(s: TStringList; n: longint);
var i: longint;
begin
  try
    case n of
      0: s.Insert(0, 'x');
      1: s.Exchange(0, 1);
      2: writeln(s[99]);
      3: s.Add('b');
      4: s.Move(0, 1);
      5: s.Delete(50);
      6: s[99] := 'q';
      7: s.Objects[99] := nil;
      8: s.Capacity := -1;
      9: s.Exchange(99, 0);
      10: s.Insert(99, 'z');
      11: writeln(s.Find('a', i), ' ', i);
    end;
    writeln(n, ' ok');
  except
    on e: Exception do writeln(n, ' ', e.ClassName, ': ', e.Message);
  end;
end;
procedure commas(const t: string);
var s: TStringList;
begin
  s := TStringList.Create;
  s.CommaText := t;
  write('[', t, '] ');
  shows(s);
  writeln(' -> ', s.CommaText);
  s.Free;
end;
procedure delim(const t: string; d, q: char; strict: boolean);
var s: TStringList;
begin
  s := TStringList.Create;
  s.Delimiter := d;
  s.QuoteChar := q;
  s.StrictDelimiter := strict;
  s.DelimitedText := t;
  write('[', t, '] ');
  shows(s);
  writeln(' -> ', s.DelimitedText);
  s.Free;
end;
procedure texts(const t: string);
var s: TStringList; i: longint; r: string;
begin
  s := TStringList.Create;
  s.Text := t;
  shows(s);
  r := s.Text;
  for i := 1 to length(r) do write(ord(r[i]), ' ');
  writeln;
  s.Free;
end;
function c_fopen(n, m: pchar): pointer; cdecl; external 'c' name 'fopen';
function c_fputc(c: longint; f: pointer): longint; cdecl; external 'c' name 'fputc';
function c_fclose(f: pointer): longint; cdecl; external 'c' name 'fclose';
procedure writefile(const name, data: string);
var f: pointer; i: longint; tmp: string;
begin
  tmp := name + #0;
  f := c_fopen(@tmp[1], 'wb');
  for i := 1 to length(data) do c_fputc(ord(data[i]), f);
  c_fclose(f);
end;
procedure loads(const data: string);
var s: TStringList;
begin
  writefile('t_load', data);
  s := TStringList.Create;
  s.Add('old');
  s.LoadFromFile('t_load');
  shows(s);
  s.Free;
  DeleteFile('t_load');
end;
var
  l: TList; n: TNL; s, s2: TStringList; ts: TStrings; i: longint; b: TBox; b2: TBox; o: TList;
begin
  for i := 0 to 1000 do cells[i] := i;
  l := TList.Create;
  show(l);
  writeln(l.First = nil, l.Last = nil, l.IndexOf(P(1)));
  for i := 1 to 5 do writeln(l.Add(P(i * 11)));
  show(l);
  for i := 6 to 20 do l.Add(P(i * 11));
  show(l);
  writeln(V(l.First), ' ', V(l.Last), ' ', V(l.Items[2]), ' ', V(l[3]), ' ', l.IndexOf(P(44)), ' ', l.IndexOf(P(45)));
  l[0] := P(7); l.Items[1] := P(8); show(l);
  l.Delete(0); l.Delete(l.Count - 1); show(l);
  l.Insert(0, P(1)); l.Insert(l.Count, P(2)); l.Insert(3, P(3)); show(l);
  l.Exchange(0, 1); show(l);
  l.Move(0, 4); show(l);
  l.Move(4, 1); show(l);
  l.Move(2, 2); show(l);
  writeln(l.Remove(P(3)), ' ', l.Remove(P(999)));
  show(l);
  writeln(V(l.Extract(P(1))), ' ', V(l.Extract(P(999))));
  show(l);
  l.Sort(@ByValue); show(l);
  l.Clear; show(l);
  l.Capacity := 3; show(l);
  l.Add(P(1)); l.Add(nil); l.Add(P(3)); l.Add(nil); l.Add(P(5)); show(l);
  l.Pack; show(l);
  l.Count := 6; show(l);
  l.Count := 1; show(l);
  writeln(l.Expand = l); show(l);
  l.Capacity := 1; show(l);
  writeln(V(l.List^[0]));
  for i := 0 to 10 do tryl(l, i);
  l.Clear;
  for i := 1 to 40 do l.Add(P((i * 37) mod 41));
  l.Sort(@ByTens); show(l);
  l.Sort(@ByValue); show(l);
  l.Clear;
  for i := 1 to 300 do l.Add(P(i));
  writeln(l.Capacity);
  for i := 1 to 290 do l.Delete(0);
  show(l);
  l.Free;
  n := TNL.Create;
  n.Add(P(1)); n.Add(nil); n.Insert(0, P(2)); n[0] := P(3); n[1] := nil; n.Delete(2); n.Add(P(4)); n.Extract(P(4)); n.Remove(P(3)); n.Add(P(5)); n.Add(P(6)); n.Count := 1; n.Count := 3; n.Add(P(7));
  writeln('free');
  n.Free;

  s := TStringList.Create;
  writeln(s.Count, s.Capacity, ' [', s.Text, '] [', s.CommaText, '] ', s.Delimiter, s.QuoteChar, s.NameValueSeparator, s.Sorted, s.CaseSensitive, ord(s.Duplicates));
  for i := 1 to 5 do writeln(s.Add('s' + IntToStr(i)));
  writeln(s.Capacity);
  for i := 6 to 20 do s.Add('s' + IntToStr(i));
  writeln(s.Capacity);
  for i := 21 to 70 do s.Add('s' + IntToStr(i));
  writeln(s.Capacity);
  for i := 71 to 300 do s.Add('s' + IntToStr(i));
  writeln(s.Capacity, ' ', s.Count, ' ', s[0], ' ', s.Strings[299]);
  s.Clear; writeln(s.Count, s.Capacity);
  s.Add('one'); s.Add('two'); s.Add('three'); shows(s);
  s[1] := 'TWO'; s.Strings[0] := ''; shows(s);
  s.Insert(0, 'x'); s.Insert(s.Count, 'y'); shows(s);
  s.Delete(1); shows(s);
  s.Exchange(0, 1); shows(s);
  s.Move(0, 2); shows(s);
  writeln(s.IndexOf('two'), s.IndexOf('TWO'), s.IndexOf('nope'));
  s.Append('z'); shows(s);
  writeln(s.Text = 'TWO'#10'three'#10'x'#10'y'#10'z'#10);
  writeln(s.CommaText);
  s.Clear;
  s.Capacity := 10; writeln(s.Capacity); s.Add('a'); s.Add('b'); s.Add('c'); s.Capacity := 2; writeln(s.Capacity, s.Count); shows(s); s.Capacity := 0; writeln(s.Capacity, s.Count);
  s.Add('b'); s.Add('c'); s.Add('A'); shows(s);
  s.Sort; shows(s);
  s.Sorted := true; s.Add('aa'); s.Add('B1'); s.Add('0'); shows(s);
  writeln(s.IndexOf('AA'), s.IndexOf('zz'));
  writeln(s.Find('c', i), i, s.Find('bb', i), i, s.Find('zz', i), i, s.Find('', i), i);
  s.Duplicates := dupIgnore; writeln(s.Add('C')); shows(s);
  s.Duplicates := dupAccept; s.Add('C'); shows(s);
  writeln(s.Find('c', i), i);
  s.Clear; s.Sorted := false; s.Duplicates := dupIgnore;
  s.Add('b'); s.Add('a'); s.Sorted := true; s.Duplicates := dupError;
  for i := 0 to 11 do if i <> 4 then trys(s, i);
  shows(s);
  s.Sorted := false;
  s.Clear;
  trys(s, 11);
  s.Add('a1'); s.Add('a_'); s.Add('a^'); s.Add('B'); s.Add('a~'); s.Sort; shows(s);
  s.CaseSensitive := true; s.Sort; shows(s);
  writeln(s.IndexOf('b'), s.IndexOf('B'));
  s.Sorted := true; s.CaseSensitive := false; shows(s); s.CaseSensitive := true; shows(s);
  s.Sorted := false; s.CaseSensitive := false; s.Clear;
  s.Add('aaa'); s.Add('b'); s.Add('cc'); s.CustomSort(@ByLen); shows(s);
  s.Clear;
  s.Add('name=value'); s.Add('k=v=w'); s.Add('plain'); s.Add('=x'); s.Add('e=');
  for i := 0 to s.Count - 1 do writeln('[', s.Names[i], '][', s.ValueFromIndex[i], ']');
  writeln(s.Values['name'], ' ', s.Values['NAME'], ' ', s.Values['k'], ' [', s.Values['nope'], '] [', s.Values['e'], ']');
  writeln(s.IndexOfName('K'), s.IndexOfName('plain'), s.IndexOfName(''));
  s.Values['name'] := 'new'; s.Values['added'] := 'yes'; s.Values['k'] := ''; s.Values['nothing'] := ''; shows(s);
  s.ValueFromIndex[0] := 'vi'; shows(s);
  s.ValueFromIndex[1] := ''; shows(s);
  s.AddPair('p', 'q'); shows(s);
  s.NameValueSeparator := ':'; s.Add('x:y'); writeln(s.Values['x'], s.Names[s.Count - 1]); shows(s);
  s.Free;
  commas(''); commas('a'); commas('a,b'); commas('a b,c'); commas(' a , b '); commas('"a b",c'); commas('"a,b",c'); commas('"a""b"'); commas('a,,b'); commas(','); commas('a,'); commas(',a'); commas('""'); commas('"",""'); commas('"a'); commas('a"b,c'); commas('"a"b,c'); commas('a'#9'b');
  delim('a;b c;d', ';', '"', false); delim('a;b c;d', ';', '"', true); delim('''x y'';z', ';', '''', false); delim(' a ; b ', ';', '"', true); delim('a;;', ';', '"', true); delim('"a;b";c', ';', '"', true);
  texts(''); texts('a'); texts('a'#10); texts('a'#10'b'); texts('a'#13#10'b'#13#10); texts('a'#13'b'); texts(#10); texts(#13#10); texts(#10#10); texts(#13#13); texts('a'#10#13'b'); texts('a'#10#10'b');
  loads(''); loads('a'); loads('a'#10); loads('a'#10'b'); loads('a'#13#10'b'#13#10); loads('a'#13'b'); loads(#10); loads(#10#10); loads(#13#10#13#10); loads('a'#10#13'b');
  ts := TStringList.Create;
  ts.Add('p'); ts.Add('q r');
  ts.SaveToFile('t_save');
  s := TStringList.Create; s.LoadFromFile('t_save'); shows(s); writeln(s.Equals(ts)); s.Add('z'); writeln(s.Equals(ts));
  DeleteFile('t_save');
  try s.LoadFromFile('/no/such'); except on e: Exception do writeln(e.ClassName, ': ', e.Message); end;
  shows(s);
  try s.SaveToFile('/no/such/x'); except on e: Exception do writeln(e.ClassName, ': ', e.Message); end;
  s2 := TStringList.Create;
  s2.Delimiter := ';'; s2.QuoteChar := ''''; s2.NameValueSeparator := ':'; s2.StrictDelimiter := true;
  s2.Add('c d'); s2.Sorted := true;
  s.Assign(s2); shows(s); writeln(s.Delimiter, s.QuoteChar, s.NameValueSeparator, s.StrictDelimiter, s.Sorted);
  s2.AddStrings(ts); shows(s2);
  try s.Assign(nil); except on e: Exception do writeln(e.ClassName, ': ', e.Message); end;
  o := TList.Create;
  try s.Assign(TPersistent(pointer(s2))); shows(s); except on e: Exception do writeln(e.ClassName, ': ', e.Message); end;
  o.Free;
  writeln(ts is TStrings, ts is TStringList, ts is TPersistent);
  ts.Free; s2.Free; s.Free;
  s := TStringList.Create;
  b := TBox.Create; b.v := 1; b2 := TBox.Create; b2.v := 2;
  writeln(s.AddObject('x', b), s.AddObject('y', b2));
  writeln(TBox(s.Objects[1]).v, s.IndexOfObject(b), s.IndexOfObject(nil));
  s.InsertObject(0, 'w', b2); writeln(s.Objects[0] = b2);
  s.Move(0, 2); writeln(s.Objects[2] = b2, s[2]);
  s.Exchange(0, 1); writeln(s.Objects[0] = b2, s[0]);
  s.Objects[2] := nil; s.Delete(2); s.Delete(1);
  s.BeginUpdate; s.BeginUpdate; s.EndUpdate; s.EndUpdate; s.EndUpdate;
  s.OwnsObjects := true; s.Objects[0] := b; s.AddObject('q', b2); s.Delete(0); writeln('deleted'); s.Clear; writeln('cleared');
  s.Free;
  writeln('done');
end.
