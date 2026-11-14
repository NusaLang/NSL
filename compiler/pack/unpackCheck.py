import sys
def decompress(src, size):
    out=bytearray(); i=0
    while i < len(src):
        t=src[i]; i+=1
        lit=t>>4
        if lit==15:
            while True:
                b=src[i]; i+=1; lit+=b
                if b!=255: break
        out+=src[i:i+lit]; i+=lit
        if i>=len(src): break
        off=src[i]|(src[i+1]<<8); i+=2
        m=t&15
        if m==15:
            while True:
                b=src[i]; i+=1; m+=b
                if b!=255: break
        m+=4
        for _ in range(m): out.append(out[-off])
    return bytes(out)
