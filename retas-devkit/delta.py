"""Version-specific offline image transitions for the Stylos host adapter."""
import hashlib
import re
import struct


def transition(a, b):
    spans = []
    for pos in range(0, len(b), 65536):
        chunk = b[pos:pos+65536]
        old = a[pos:pos+len(chunk)].ljust(len(chunk), b'\0')
        mask = (int.from_bytes(chunk,'little') ^ int.from_bytes(old,'little')).to_bytes(len(chunk),'little')
        for m in re.finditer(rb'[^\0]+(?:\0{1,8}[^\0]+)*',mask):
            spans.append((pos+m.start(),chunk[m.start():m.end()]))
    data = bytearray(hashlib.sha256(a).digest() + hashlib.sha256(b).digest() + struct.pack('<ii',len(b),len(spans)))
    for offset,value in spans:
        data.extend(struct.pack('<ii',offset,len(value)) + value)
    return bytes(data)
