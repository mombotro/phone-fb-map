-- title:  beat box
-- author: FB Tools
-- desc:   loops that play on the beat. Turn each on or off, and make each faster or slower.
-- script: lua

BPMMIN,BPMMAX=60,200
RATES={0.25,0.5,1,2,4,8}
RL={"/4","/2","x1","x2","x4","x8"}
-- name, sound, notes (one per hit, in turn), color, channel, start rate
T={
 {"KICK",0,{"C-2"},2,0,3},
 {"SNARE",1,{"C-5"},3,2,2},
 {"HAT",2,{"C-7"},4,3,4},
 {"CLAP",3,{"C-5"},14,2,1},
 {"BASS",4,{"C-2","C-2","G-2","F-2"},9,1,3},
 {"BLEEP",5,{"C-5","E-5","G-5","C-6"},11,1,4},
 {"TOM",6,{"G-2","D-3","A-2"},5,0,2},
 {"ZAP",7,{"C-4"},10,3,1},
}
on={} ri={} last={} hit={} nx={}
for i,k in ipairs(T) do on[i]=false ri[i]=k[6] last[i]=0 hit[i]=0 nx[i]=1 end
bpm=120 beat=0 play=true pm=false

function inside(x,y,a,b,w,h) return x>=a and x<a+w and y>=b and y<b+h end

function TIC()
 local mx,my,ml=mouse()
 local click=ml and not pm
 pm=ml
 -- top bar: tempo, pause, all off
 if click then
  if inside(mx,my,0,0,16,13) then bpm=math.max(BPMMIN,bpm-5) end
  if inside(mx,my,66,0,16,13) then bpm=math.min(BPMMAX,bpm+5) end
  if inside(mx,my,92,0,44,13) then play=not play end
  if inside(mx,my,142,0,50,13) then for i=1,#T do on[i]=false end end
 end
 for i,k in ipairs(T) do
  local y=15+(i-1)*14
  if click then
   if inside(mx,my,0,y,26,14) then
    on[i]=not on[i]
    last[i]=math.floor(beat*RATES[ri[i]])
   end
   if inside(mx,my,118,y,20,14) then ri[i]=math.max(1,ri[i]-1) last[i]=math.floor(beat*RATES[ri[i]]) end
   if inside(mx,my,168,y,20,14) then ri[i]=math.min(#RATES,ri[i]+1) last[i]=math.floor(beat*RATES[ri[i]]) end
  end
 end
 -- the beat clock, and every loop that is on
 if play then
  beat=beat+bpm/3600
  for i,k in ipairs(T) do
   if on[i] then
    local n=math.floor(beat*RATES[ri[i]])
    if n>last[i] then
     last[i]=n
     local notes=k[3]
     sfx(k[2],notes[nx[i]],-1,k[5],15)
     nx[i]=nx[i]%#notes+1
     hit[i]=6
    end
   end
  end
 end
 -- draw
 cls(0)
 rect(0,0,240,13,15)
 rect(1,1,14,11,8) print("-",6,4,12,false,1,true)
 print("BPM "..bpm,22,4,12,false,1,true)
 rect(66,1,14,11,8) print("+",71,4,12,false,1,true)
 rect(92,1,44,11,play and 6 or 2) print(play and "PLAYING" or "PAUSED",97,4,12,false,1,true)
 rect(142,1,50,11,8) print("ALL OFF",149,4,12,false,1,true)
 local f=(beat%1)
 rect(200,2,36,9,f<0.25 and 12 or 14)
 for i,k in ipairs(T) do
  local y=15+(i-1)*14
  rect(1,y+1,24,12,on[i] and k[4] or 15)
  print(on[i] and "ON" or "OFF",on[i] and 8 or 7,y+4,on[i] and 0 or 13,false,1,true)
  print(k[1],30,y+4,on[i] and 12 or 14,false,1,true)
  rect(118,y+1,20,12,8) print("-",126,y+4,12,false,1,true)
  print(RL[ri[i]],146,y+4,12,false,1,true)
  rect(168,y+1,20,12,8) print("+",176,y+4,12,false,1,true)
  if hit[i]>0 then rect(196,y+2,40,10,k[4]) hit[i]=hit[i]-1 else rect(196,y+2,40,10,15) end
 end
end

-- <WAVES>
-- 000:ffffffffffffffff0000000000000000
-- 001:ffffffff000000000000000000000000
-- 002:0123456789abcdeffedcba9876543210
-- 003:0012223444566678889aaabcccdeeeff
-- 004:89acdeefffeedca98653211000112356
-- 005:a4c123b1612dd272d1371c17149d4395
-- 006:36b3216fdaeeb975729fae923d5a4fd1
-- </WAVES>

-- <SFX>
-- 000:0407240534045403640274019400a400b400c400d400e400e400f400f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000b04000000000
-- 001:050025004500550075008500a500b500c500d500e500f500f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000304000000000
-- 002:360066009600c600e600f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000304000000000
-- 003:1500f5003500f500450065008500a500c500d500e500f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000304000000000
-- 004:030013002300330043004300530063007300830083009300a300a300b300c300c300d300d300e300e300e300f300f300f000f000f000f000f000f000304000000000
-- 005:00002000400060008000a000b000d000e000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000304000000000
-- 006:040714062405440454036402740184009400a400b400b400c400d400d400e400e400f400f000f000f000f000f000f000f000f000f000f000f000f000b04000000000
-- 007:21084109510a710b810c910da10ec10fc100d101e102f103f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000f000b04000000000
-- </SFX>
