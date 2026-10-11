-- title:  sound test
-- author: FB Tools
-- desc:   a beep every second, to check sound output
-- script: lua

t=0
function TIC()
 cls(0)
 print("SOUND TEST",84,50,12)
 print("a beep plays every second",44,70,6)
 print("touch the screen for another",36,84,6)
 t=t+1
 if t%60==1 then sfx(0,"E-5",20,0,15) end
 local mx,my,ml=mouse()
 if ml then circ(mx,my,4,10) if t%10==0 then sfx(0,"C-5",8,1,15) end end
end

-- <WAVES>
-- 000:ffffffffffffffff0000000000000000
-- </WAVES>

-- <SFX>
-- 000:000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000304000000000
-- </SFX>
