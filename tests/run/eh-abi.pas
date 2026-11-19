{ The table-based exception ABI skeleton: parsing a synthetic LSDA and
  matching a catch type (optimization2 item 8a).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/eh-abi.expected
}
program eh_abi;
function __pas_eh_selftest: longint; external;
begin
  writeln(__pas_eh_selftest)
end.
