{ An empty program produces no output and exits with status 0.
  RUN: %p3 %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/empty.expected %t.out
}
program Empty;
begin
end.
