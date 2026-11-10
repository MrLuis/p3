{ pcc's unit library: output identical to Free Pascal 3.2.2's (fpc -Mobjfpc -Sh-).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t > %t.out && diff %S/rtl-strutils.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t
  RUN: %t > %t.out && diff %S/rtl-strutils.expected %t.out
}
program rtlstrutils;
uses strutils, sysutils;
begin
  writeln(ReverseString('abc'),'|',ReverseString(''));
  writeln(LeftStr('hello',2),'|',LeftStr('hi',9),'|',LeftStr('hi',0),'|',LeftStr('hi',-1));
  writeln(RightStr('hello',2),'|',RightStr('hi',9),'|',RightStr('hi',0),'|',RightStr('hi',-1));
  writeln(MidStr('hello',2,3),'|',MidStr('hello',9,3),'|',MidStr('hello',0,2),'|',MidStr('hello',2,0),'|',MidStr('hello',4,9));
  writeln(DupeString('ab',3),'|',DupeString('ab',0),'|',DupeString('ab',-1));
  writeln('[',PadLeft('a',3),'][',PadRight('a',3),'][',PadLeft('abcd',2),'][',PadRight('abcd',2),']');
  writeln(AddChar('0','7',3),'|',AddCharR('0','7',3),'|',AddChar('0','777',2));
  writeln(StartsStr('he','hello'),StartsStr('He','hello'),StartsStr('','x'),StartsStr('xx','x'));
  writeln(EndsStr('lo','hello'),EndsStr('Lo','hello'),EndsStr('','x'),EndsStr('xx','x'));
  writeln(StartsText('HE','hello'),EndsText('LO','hello'),StartsText('x','hello'));
  writeln(ContainsStr('hello','ell'),ContainsStr('hello','ELL'),ContainsStr('x',''),ContainsText('hello','ELL'));
  writeln(AnsiStartsStr('he','hello'),AnsiEndsStr('lo','hello'),AnsiContainsStr('hello','ll'));
  writeln(PosEx('l','hello',1),' ',PosEx('l','hello',4),' ',PosEx('l','hello',5),' ',PosEx('l','hello',0),' ',PosEx('l','hello',9),' ',PosEx('','hello',1),' ',PosEx('l','hello'));
  writeln(ReplaceStr('aXbX','X','y'),'|',ReplaceStr('aXbx','x','y'),'|',ReplaceText('aXbx','x','y'));
  writeln(StuffString('hello',2,3,'XY'),'|',StuffString('hello',1,0,'XY'),'|',StuffString('hello',9,1,'XY'));
  writeln(Copy2Symb('a,b',','),'|',Copy2Symb('ab',','),'|',Copy2Symb(',b',','));
  writeln(Hex2Dec('FF'),' ',Hex2Dec('$10'),' ',Hex2Dec('0'));
  writeln(Dec2Numb(255,4,16),' ',Dec2Numb(5,8,2),' ',Dec2Numb(0,3,10),' ',Dec2Numb(35,1,36));
  writeln(IntToBin(5,8),' ',IntToBin(-1,4),' ',IntToBin(1024,4));
end.
