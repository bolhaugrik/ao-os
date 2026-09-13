#!/usr/bin/env python3
"""ChaCha20-Poly1305 (RFC 8439) + HChaCha20 kulcsszarmaztatas, tiszta Pythonban, fuggoseg nelkul.
A hid es a tesztek hasznaljak; az AOP-keretek titkositasa PSK-val (docs/AOP.md).

  python tools/aocrypto.py --selftest     RFC 8439 tesztvektorok
  python tools/aocrypto.py --gen-psk      uj 32 bajtos kulcs hexben"""
import os
import struct
import sys


def _rotl(v, c):
    return ((v << c) & 0xFFFFFFFF) | (v >> (32 - c))


def _qr(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = _rotl(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = _rotl(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = _rotl(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = _rotl(s[b] ^ s[c], 7)


def _rounds(s):
    for _ in range(10):
        _qr(s, 0, 4, 8, 12); _qr(s, 1, 5, 9, 13); _qr(s, 2, 6, 10, 14); _qr(s, 3, 7, 11, 15)
        _qr(s, 0, 5, 10, 15); _qr(s, 1, 6, 11, 12); _qr(s, 2, 7, 8, 13); _qr(s, 3, 4, 9, 14)


def chacha20_block(key, counter, nonce):
    st = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574] + list(struct.unpack("<8I", key)) + \
         [counter] + list(struct.unpack("<3I", nonce))
    w = list(st)
    _rounds(w)
    return struct.pack("<16I", *[(w[i] + st[i]) & 0xFFFFFFFF for i in range(16)])


def chacha20_xor(key, counter, nonce, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        ks = chacha20_block(key, counter + i // 64, nonce)
        chunk = data[i:i + 64]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
    return bytes(out)


def hchacha20(key, nonce16):
    """XChaCha20 kulcsszarmaztatas: 32 bajtos alkulcs a 16 bajtos nonce-bol."""
    st = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574] + list(struct.unpack("<8I", key)) + \
         list(struct.unpack("<4I", nonce16))
    _rounds(st)
    return struct.pack("<4I", *st[0:4]) + struct.pack("<4I", *st[12:16])


def poly1305(key32, msg):
    r = int.from_bytes(key32[:16], "little") & 0x0ffffffc0ffffffc0ffffffc0fffffff
    s = int.from_bytes(key32[16:], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        blk = msg[i:i + 16]
        n = int.from_bytes(blk + b"\x01", "little")
        acc = ((acc + n) * r) % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def _pad16(b):
    return b"\x00" * ((16 - len(b) % 16) % 16)


# Gyorsitas: a 'cryptography' csomag (C-ben) ugyanezt az RFC 8439 AEAD-ot adja, ~1000x gyorsabban.
# Tiszta Pythonban a hid 0,25 MB/s-mal bont, ami a netbook feltolteseit fojtotta (nulla-ablak).
try:
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305 as _Fast
except Exception:  # nincs telepitve: marad a tiszta Python
    _Fast = None

FAST = _Fast is not None


def aead_encrypt(key, nonce, aad, plaintext):
    if _Fast is not None:
        return _Fast(key).encrypt(nonce, bytes(plaintext), bytes(aad))
    otk = chacha20_block(key, 0, nonce)[:32]
    ct = chacha20_xor(key, 1, nonce, plaintext)
    mac_data = aad + _pad16(aad) + ct + _pad16(ct) + struct.pack("<QQ", len(aad), len(ct))
    return ct + poly1305(otk, mac_data)


def aead_decrypt(key, nonce, aad, ct_tag):
    if len(ct_tag) < 16:
        return None
    if _Fast is not None:
        try:
            return _Fast(key).decrypt(nonce, bytes(ct_tag), bytes(aad))
        except Exception:  # rossz tag
            return None
    ct, tag = ct_tag[:-16], ct_tag[-16:]
    otk = chacha20_block(key, 0, nonce)[:32]
    mac_data = aad + _pad16(aad) + ct + _pad16(ct) + struct.pack("<QQ", len(aad), len(ct))
    if poly1305(otk, mac_data) != tag:
        return None
    return chacha20_xor(key, 1, nonce, ct)


class Channel:
    """Egy AOP-kapcsolat titkositott csatornaja: alkulcs = HChaCha20(psk, c_nonce||s_nonce),
    keretenkent nonce = [irany 4 bajt][szamlalo 8 bajt], AAD = a 12 bajtos AOP-fejlec."""

    def __init__(self, psk, client_nonce8, server_nonce8, is_server):
        self.key = hchacha20(psk, client_nonce8 + server_nonce8)
        self.tx_dir = b"srv\x00" if is_server else b"cli\x00"
        self.rx_dir = b"cli\x00" if is_server else b"srv\x00"
        self.tx_ctr = 0
        self.rx_ctr = 0

    def seal(self, header, payload):
        nonce = self.tx_dir + struct.pack("<Q", self.tx_ctr)
        self.tx_ctr += 1
        return aead_encrypt(self.key, nonce, header, payload)

    def open(self, header, ct_tag):
        nonce = self.rx_dir + struct.pack("<Q", self.rx_ctr)
        pt = aead_decrypt(self.key, nonce, header, ct_tag)
        if pt is None:
            return None
        self.rx_ctr += 1
        return pt


def selftest():
    key = bytes(range(32))
    nonce = bytes.fromhex("000000000000004a00000000")
    ks = chacha20_block(key, 1, nonce)
    assert ks[:16] == bytes.fromhex("224f51f3401bd9e12fde276fb8631ded"), "chacha20 block"
    pkey = bytes.fromhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b")
    tag = poly1305(pkey, b"Cryptographic Forum Research Group")
    assert tag == bytes.fromhex("a8061dc1305136c6c22b8baf0c0127a9"), "poly1305"
    akey = bytes.fromhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f")
    anonce = bytes.fromhex("070000004041424344454647")
    aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
    pt = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
          b"sunscreen would be it.")
    ct = aead_encrypt(akey, anonce, aad, pt)
    assert ct[-16:] == bytes.fromhex("1ae10b594f09e26a7e902ecbd0600691"), "aead tag"
    assert aead_decrypt(akey, anonce, aad, ct) == pt, "aead roundtrip"
    assert aead_decrypt(akey, anonce, aad, ct[:-1] + b"\x00") is None, "aead tamper"
    hk = hchacha20(bytes.fromhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"),
                   bytes.fromhex("000000090000004a0000000031415927"))
    assert hk == bytes.fromhex("82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc"), "hchacha20"
    print(f"aocrypto: minden tesztvektor OK ({'cryptography (C)' if FAST else 'tiszta Python'})")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
    elif "--gen-psk" in sys.argv:
        print(os.urandom(32).hex())
    else:
        print(__doc__)
