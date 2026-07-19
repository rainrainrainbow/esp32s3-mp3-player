import struct
w, h = 240, 320
rs = (w * 3 + 3) & ~3
with open('test.bmp', 'wb') as f:
    f.write(struct.pack('<HIHHI', 0x4D42, 14 + 40 + rs * h, 0, 0, 54))
    f.write(struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, rs * h, 0, 0, 0, 0))
    for y in range(h):
        for x in range(w):
            b = int(255 * (h - y) / h)
            f.write(struct.pack('BBB', b, 0, 0))
        f.write(b'\x00' * (rs - w * 3))
print('BMP OK')
