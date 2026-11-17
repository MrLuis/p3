{ TStringList's OnChange and OnChanging: which operations fire them, and
  BeginUpdate/EndUpdate. Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  Run in its own directory: it creates a file.
  RUN: rm -rf %t && mkdir -p %t && %p3 --mode=objfpc %s -o %t/a
  RUN: cd %t && ./a > out && diff %S/rtl-onchange.expected out
}
program rtlonchange;
{$H+}
uses classes;
type
  TW = class
    n: longint;
    procedure C(Sender: TObject);
    procedure Cg(Sender: TObject);
  end;
procedure TW.C(Sender: TObject); begin writeln('  changed ', TStringList(Sender).Count); end;
procedure TW.Cg(Sender: TObject); begin writeln('  changing ', TStringList(Sender).Count); end;
var
  w: TW; s, t: TStringList; ev: TNotifyEvent; i: longint;
procedure L(const m: string); begin writeln(m); end;
begin
  w := TW.Create; s := TStringList.Create; t := TStringList.Create;
  writeln(Assigned(s.OnChange), Assigned(s.OnChanging));
  s.OnChange := @w.C; s.OnChanging := @w.Cg;
  ev := s.OnChange; writeln(Assigned(ev), Assigned(s.OnChange));
  L('add'); s.Add('b');
  L('addobj'); s.AddObject('c', nil);
  L('insert'); s.Insert(0, 'a');
  L('insobj'); s.InsertObject(0, 'io', nil); s.Delete(0);
  L('put'); s[1] := 'B';
  L('putobj'); s.Objects[1] := nil;
  L('exchange'); s.Exchange(0, 2);
  L('move'); s.Move(0, 1);
  L('delete'); s.Delete(0);
  L('sort'); s.Add('a'); s.Sort;
  L('sorted'); s.Sorted := true; 
  L('addsorted'); s.Add('m'); 
  L('dup'); s.Duplicates := dupIgnore; s.Add('m');
  L('casesens'); s.CaseSensitive := true;
  s.Sorted := false;
  L('clear'); s.Clear;
  L('clear2'); s.Clear;
  L('text'); s.Text := 'x'#10'y'#10'z';
  L('comma'); s.CommaText := 'p,q';
  L('delim'); s.DelimitedText := 'r,s';
  L('values'); s.Values['k'] := 'v';
  L('values2'); s.Values['k'] := 'w';
  L('vfi'); s.ValueFromIndex[0] := 'zz';
  L('addpair'); s.AddPair('a', 'b');
  L('append'); s.Append('t');
  t.Add('1'); t.Add('2');
  L('addstrings'); s.AddStrings(t);
  L('addarr'); s.AddStrings(['u', 'v']);
  L('assign'); s.Assign(t);
  L('update'); s.BeginUpdate; s.Add('a1'); s.Add('a2'); s.BeginUpdate; s.Add('a3'); s.EndUpdate; L(' mid'); s.Add('a4'); s.EndUpdate;
  L('update-empty'); s.BeginUpdate; s.EndUpdate;
  L('extra-end'); s.EndUpdate;
  L('capacity'); s.Capacity := 100;
  L('nil'); s.OnChange := nil; s.OnChanging := nil; s.Add('quiet'); writeln(Assigned(s.OnChange));
  L('load'); s.OnChange := @w.C; s.OnChanging := @w.Cg; s.SaveToFile('t_ch'); s.LoadFromFile('t_ch');
  L('free'); s.Free; t.Free; w.Free;
end.
