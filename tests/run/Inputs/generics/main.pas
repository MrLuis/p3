program main;
{$H+}
uses gutil;
begin
  writeln(specialize Twice<longint>(21), ' ', specialize Twice<string>('ab'));
  specialize WriteAll<longint>([1, 2, 3]);
end.
