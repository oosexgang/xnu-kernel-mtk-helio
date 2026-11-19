#!/usr/bin/env python3
"""
fbkept.py LOG OUT.png: picture 1 of a run's fbshot (boot-arg fbshot_keep) out of the host's kept lines, four times as big,
whether or not its "end" line came through: what is missing is padded with black (tools/fbshot.py refuses a short
one). The kept lines are 64 KB; a run whose own lines fill them cuts the picture's tail.
"""
import base64,sys,zlib,struct
text=open(sys.argv[1],'rb').read().decode('latin-1')
kept=[l for l in text.split('\n') if l.startswith('fbshot 1: ')]
size=None;data=[]
for l in kept:
    w=l.split(' ',3)
    if w[2]=='begin': size=(int(w[3].split()[0]),int(w[3].split()[1])); continue
    if w[2]=='end': break
    try: data.append(base64.b64decode(w[2]))
    except Exception: data.append(b'\0'*(len(w[2])*3//4))
W,H=size; px=b''.join(data)[:W*H*3]; print(len(px),W*H*3); px+=b'\0'*(W*H*3-len(px))
s=4;rows=b''
for y in range(H*s):
    r=px[(y//s)*W*3:(y//s+1)*W*3]; rows+=b'\0'+b''.join(r[x*3:x*3+3]*s for x in range(W))
def ch(k,d): return struct.pack('>I',len(d))+k+d+struct.pack('>I',zlib.crc32(k+d))
open(sys.argv[2],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',W*s,H*s,8,2,0,0,0))+ch(b'IDAT',zlib.compress(rows))+ch(b'IEND',b''))
