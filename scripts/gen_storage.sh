#!/bin/bash
# Generate test MP3 and BMP, create FATFS image
apt-get update -qq && apt-get install -y -qq dosfstools mtools ffmpeg 2>/dev/null || true

# Generate MP3 (5 second 440Hz sine wave)
python3 -c "
import struct, math
p = b''
for i in range(44100 * 5):
    s = int(32767 * 0.3 * math.sin(2 * math.pi * 440 * i / 44100))
    p += struct.pack('<h', s)
open('test.raw', 'wb').write(p)
"
ffmpeg -f s16le -ar 44100 -ac 1 -i test.raw -codec:a libmp3lame -b:a 128k -y test.mp3 2>/dev/null

# Generate BMP (240x320 blue-green gradient)
python3 -c "
import struct
W, H = 240, 320
# BMP header (24-bit, no compression)
row_size = ((W * 3 + 3) // 4) * 4
file_size = 14 + 40 + row_size * H
buf = b''
# File header
buf += struct.pack('<2sIHHI', b'BM', file_size, 0, 0, 14 + 40)
# DIB header
buf += struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, 0, row_size * H, 2835, 2835, 0, 0)
# Pixel data (BGR, bottom-up)
for y in range(H - 1, -1, -1):
    row = b''
    for x in range(W):
        r = int(255 * x / W)
        g = int(255 * y / H)
        b = int(128 + 127 * (x / W))
        row += struct.pack('BBB', int(b*0.5), g, r)
    row += b'\x00' * (row_size - W * 3)
    buf += row
open('test.bmp', 'wb').write(buf)
print('BMP generated:', W, 'x', H)
"

# Create FATFS
STORAGE_SIZE=12320768
dd if=/dev/zero of=storage.img bs=1024 count=12032 2>/dev/null
mkfs.fat -F 16 -S 4096 storage.img 2>/dev/null
mmd -i storage.img ::/music 2>/dev/null || true
mmd -i storage.img ::/images 2>/dev/null || true
mmd -i storage.img ::/images/1 2>/dev/null || true
mcopy -i storage.img test.mp3 ::/music/1.mp3 2>/dev/null
mcopy -i storage.img test.bmp ::/images/1/1.bmp 2>/dev/null
echo "storage.img created"
ls -la storage.img
