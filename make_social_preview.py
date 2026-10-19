from PIL import Image, ImageDraw, ImageFont
import pathlib
R=str(pathlib.Path(__file__).resolve().parent.parent)
W,H=1280,640; MARGIN=80
bg=Image.new("RGB",(W,H)); px=bg.load()
for y in range(H):
    for x in range(W):
        t=(x+y)/(W+H); px[x,y]=(int(8+6*t),int(14+46*t),int(34+96*t))
bg=bg.convert("RGBA")
LOGO=300; lx=MARGIN; ly=(H-LOGO)//2
logo=Image.open(R+'/assets/logo_1024.png').resize((LOGO,LOGO),Image.LANCZOS); bg.paste(logo,(lx,ly),logo)
d=ImageDraw.Draw(bg)
BOLD="/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"; REG="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
tx=lx+LOGO+50; maxw=W-MARGIN-tx
def fit(text,path,size,maxw):
    while size>14:
        f=ImageFont.truetype(path,size)
        if f.getlength(text)<=maxw: return f
        size-=1
    return ImageFont.truetype(path,14)
lines=[("Bens Audio Router",BOLD,70,(255,255,255)),
       ("Audiophiler Node-Router für Windows",REG,34,(150,200,255)),
       ("Sonos · FLAC 24 Bit · AirPlay-Empfänger",REG,30,(150,200,255)),
       ("Autopilot-Latenzausgleich",REG,30,(150,200,255)),
       ("Buccloud Software (DE) · www.buccloud.de",REG,26,(200,215,240)),
       ("Open Source (MIT)",REG,26,(200,215,240))]
fonts=[fit(t,p,s,maxw) for t,p,s,c in lines]
gaps=[0,22,14,8,40,8]
total=sum(f.size+int(f.size*0.25) for f in fonts)+sum(gaps)
y=(H-total)//2
for (t,p,s,c),f,g in zip(lines,fonts,gaps):
    y+=g; d.text((tx,y),t,font=f,fill=c); y+=f.size+int(f.size*0.25)
    assert tx+f.getlength(t)<=W-MARGIN+1, (t,tx+f.getlength(t))
bg.convert("RGB").save(R+'/assets/social-preview.png'); print("OK, alle Zeilen innerhalb des Rands, Schriftgroessen:",[f.size for f in fonts])
