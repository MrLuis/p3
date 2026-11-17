{ p3's unit library: output identical to Free Pascal 3.2.2's (fpc -Mobjfpc -Sh-).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t > %t.out && diff %S/rtl-math.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t
  RUN: %t > %t.out && diff %S/rtl-math.expected %t.out
}
program rtlmath;
uses math, sysutils;
var zero: double;
begin
  zero := 0;
  writeln(Floor(2.5),' ',Floor(-2.5),' ',Ceil(2.5),' ',Ceil(-2.5),' ',Floor(3.0),' ',Ceil(3.0),' ',Floor64(1e12+0.5),' ',Ceil64(-1e12-0.5));
  writeln(Power(2,10):0:4,' ',Power(2,0.5):0:6,' ',Power(9,0):0:4,' ',Power(0,0):0:4,' ',Power(0,2):0:4,' ',IntPower(2,10):0:4,' ',IntPower(2,-2):0:4,' ',IntPower(-2,3):0:4);
  writeln(Log10(1000):0:6,' ',Log2(1024):0:6,' ',LogN(3,81):0:6,' ',Hypot(3,4):0:6,' ',FMod(7.5,2):0:6,' ',FMod(-7.5,2):0:6);
  writeln(Tan(1):0:6,' ',Cotan(1):0:6,' ',ArcSin(0.5):0:6,' ',ArcCos(0.5):0:6,' ',ArcTan2(1,2):0:6,' ',ArcTan2(-1,-2):0:6);
  writeln(Sinh(1):0:6,' ',Cosh(1):0:6,' ',Tanh(1):0:6,' ',DegToRad(180):0:6,' ',RadToDeg(3.14159265358979):0:6);
  writeln(Min(1,2),' ',Max(1,2),' ',Min(int64(-5),int64(5)),' ',Max(int64(-5),int64(5)),' ',Min(1.5,2.5):0:2,' ',Max(1.5,2.5):0:2);
  writeln(Sign(int64(-3)),' ',Sign(int64(0)),' ',Sign(int64(3)),' ',Sign(-3.5),' ',Sign(0.0),' ',Sign(3.5));
  writeln(InRange(5,1,9),' ',InRange(10,1,9),' ',InRange(1.5,1.0,2.0),' ',EnsureRange(10,1,9),' ',EnsureRange(0,1,9),' ',EnsureRange(5,1,9),' ',EnsureRange(9.5,1.0,2.0):0:2);
  zero := zero + 0;
end.
