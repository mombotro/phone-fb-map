#!/usr/bin/env python3
"""Writes beatbox.lua (a loop machine for TIC-80) and a fixed soundtest.lua next to this script.
The sounds are made in the cart's own SFX and WAVES sections. In the cart text, every byte is written low nibble first.
A tick of a sound is: volume (stored as 15 minus the loudness), wave, chord (arpeggio step), pitch (a signed nibble)."""
import math, os, random

HERE = os.path.dirname(os.path.abspath(__file__))

def wave_text(samples): return "".join("%x" % (max(0, min(15, int(round(s)))) & 15) for s in samples)

random.seed(7)
WAVES = [
    [15] * 16 + [0] * 16,                                                   # 0 square
    [15] * 8 + [0] * 24,                                                    # 1 pulse
    [i / 31 * 30 if i < 16 else (31 - i) / 31 * 30 for i in range(32)],     # 2 triangle (0..15)
    [i / 2 for i in range(32)],                                             # 3 saw
    [7.5 + 7.5 * math.sin(2 * math.pi * i / 32) for i in range(32)],        # 4 sine
    [random.randint(0, 15) for _ in range(32)],                             # 5 noise
    [random.randint(0, 15) for _ in range(32)],                             # 6 noise, other grain
]

def sfx_text(ticks, pitch16x=0, speed=0, octave=3):
    """ticks: up to 30 tuples (loudness 0-15, wave, pitch -8..7). The rest are silent."""
    out = ""
    for i in range(30):
        v, w, p = ticks[i] if i < len(ticks) else (0, 0, 0)
        out += "%x%x%x%x" % (15 - v, w, 0, p & 15)                          # low nibble first: volume, wave, chord, pitch
    b0 = (octave & 7) | (pitch16x << 3) | ((speed & 7) << 4)
    return out + "%x%x" % (b0 & 15, b0 >> 4) + "40" + "00000000"           # the tail: octave, pitch16x, speed, note, loops

def decay(n, start=15): return [round(start * (1 - i / n) ** 1.5) for i in range(n)]

SFX = [
    # 0 kick: a sine that falls fast
    (sfx_text([(v, 4, p) for v, p in zip(decay(14), [7, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0])], pitch16x=1)),
    # 1 snare: noise
    (sfx_text([(v, 5, 0) for v in decay(12)])),
    # 2 hat: short noise
    (sfx_text([(v, 6, 0) for v in decay(5, 12)])),
    # 3 clap: noise in bursts
    (sfx_text([(v, 5, 0) for v in [14, 0, 12, 0, 11, 9, 7, 5, 3, 2, 1]])),
    # 4 bass: a saw that fades
    (sfx_text([(v, 3, 0) for v in decay(24, 15)])),
    # 5 bleep: a square
    (sfx_text([(v, 0, 0) for v in decay(10)])),
    # 6 tom: a sine that falls slowly
    (sfx_text([(v, 4, p) for v, p in zip(decay(18), [7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])], pitch16x=1)),
    # 7 zap: a pulse that climbs
    (sfx_text([(v, 1, p) for v, p in zip(decay(12, 13), [-8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3])], pitch16x=1)),
]

CART = '''-- title:  beat box
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
@WAVES@-- </WAVES>

-- <SFX>
@SFX@-- </SFX>
'''
waves = "".join("-- %03d:%s\n" % (i, wave_text(w)) for i, w in enumerate(WAVES))
sfx = "".join("-- %03d:%s\n" % (i, s) for i, s in enumerate(SFX))
assert all(len(s) == 132 for s in SFX), [len(s) for s in SFX]
assert all(len(wave_text(w)) == 32 for w in WAVES)
open(os.path.join(HERE, "beatbox.lua"), "w").write(CART.replace("@WAVES@", waves).replace("@SFX@", sfx))

SOUND = '''-- title:  sound test
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
-- 000:@W0@
-- </WAVES>

-- <SFX>
-- 000:@S0@
-- </SFX>
'''
open(os.path.join(HERE, "soundtest.lua"), "w").write(SOUND.replace("@W0@", wave_text(WAVES[0])).replace("@S0@", sfx_text([(15, 0, 0)] * 30)))
print("wrote beatbox.lua and soundtest.lua")
