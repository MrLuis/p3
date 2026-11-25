{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
program d04_cmdline;
{ Dialect check L7: paramcount, paramstr, halt with an exit code.
  run.sh passes the arguments  alpha 'beta gamma'  and checks the status. }
var
  i: longint;
begin
  writeln(paramcount);
  for i := 1 to paramcount do
    writeln('[', paramstr(i), ']');
  writeln('halting');
  halt(3);
  writeln('not reached');
end.
