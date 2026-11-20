{ GetDelimited stops looking for a character that needs quotes at the first
  one (docs/classes-review.md CL-7): quoting is the same whether the
  character is first, last, in the middle or absent, with a space, a quote,
  the delimiter or a control character, strict or not. Output identical to
  Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-delimited.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-delimited.expected
}
program rtldelimited;
{$H+}
uses sysutils, classes;

var
  l: TStringList;
begin
  l := TStringList.Create;
  writeln('[', l.CommaText, ']');
  l.Add('');
  writeln('[', l.CommaText, ']');
  l.Add('');
  writeln('[', l.CommaText, ']');
  l.Clear;
  l.Add('plain');
  l.Add(' lead');
  l.Add('trail ');
  l.Add('mid dle');
  l.Add('say "hi"');
  l.Add('"');
  l.Add('a,b');
  l.Add('a,');
  l.Add(',a');
  l.Add('tab' + #9 + 'x');
  l.Add('nul' + #1 + 'x');
  l.Add('end"');
  writeln(l.CommaText);
  l.StrictDelimiter := true;
  writeln(l.CommaText);
  l.Delimiter := ';';
  l.QuoteChar := '''';
  writeln(l.DelimitedText);
  l.Clear;
  l.Add('only one');
  writeln(l.DelimitedText);
  l.Free;
end.
