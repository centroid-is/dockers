"""Exercise every RSA-AES variant against the patched server.
5/129 keep the session encrypted; 6/130 drop to plaintext after auth."""
import socket, sys, hashlib, struct
from Crypto.PublicKey import RSA
from Crypto.Cipher import PKCS1_v1_5, AES
from Crypto.Random import get_random_bytes

HOST, PORT, USER, PW, TYPE = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], int(sys.argv[5])
NAMES = {5: "RA2 (AES-128, encrypted session)", 6: "RA2ne (AES-128, plaintext session)",
         129: "RA2_256 (AES-256, encrypted session)", 130: "RA2ne_256 (AES-256, plaintext session)"}
IS_256 = TYPE in (129, 130)
ENCRYPTED_SESSION = TYPE in (5, 129)
HASH = hashlib.sha256 if IS_256 else hashlib.sha1
HLEN = 32 if IS_256 else 20
KLEN = 32 if IS_256 else 16

class Cipher:
    def __init__(self, key):
        self.key, self.counter = key, bytearray(16)
    def _bump(self):
        for i in range(16):
            self.counter[i] = (self.counter[i] + 1) & 0xff
            if self.counter[i] != 0:
                break
    def make(self, msg):
        ad = struct.pack(">H", len(msg))
        c = AES.new(self.key, AES.MODE_EAX, nonce=bytes(self.counter), mac_len=16)
        c.update(ad)
        ct, tag = c.encrypt_and_digest(msg); self._bump()
        return ad + ct + tag
    def recv(self, length, blob):
        ad = struct.pack(">H", length)
        c = AES.new(self.key, AES.MODE_EAX, nonce=bytes(self.counter), mac_len=16)
        c.update(ad)
        pt = c.decrypt_and_verify(blob[:length], blob[length:length+16]); self._bump()
        return pt

s = socket.create_connection((HOST, PORT), timeout=30)
def rn(n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d: raise EOFError(f"eof at {len(b)}/{n}")
        b += d
    return b

rn(12); s.sendall(b"RFB 003.008\n")
types = list(rn(rn(1)[0]))
assert TYPE in types, f"type {TYPE} not offered (got {types})"
s.sendall(bytes([TYPE]))

skl_buf = rn(4); skl = struct.unpack(">I", skl_buf)[0]; skb = (skl + 7)//8
sn, se = rn(skb), rn(skb)
server_pub = RSA.construct((int.from_bytes(sn,"big"), int.from_bytes(se,"big")))
server_publickey = skl_buf + sn + se
ckl = 2048; ckb = ckl//8
ck = RSA.generate(ckl)
client_publickey = struct.pack(">I", ckl) + ck.n.to_bytes(ckb,"big") + ck.e.to_bytes(ckb,"big")
s.sendall(client_publickey)
cr = get_random_bytes(KLEN)
s.sendall(struct.pack(">H", skb) + PKCS1_v1_5.new(server_pub).encrypt(cr))
assert struct.unpack(">H", rn(2))[0] == ckb
sr = PKCS1_v1_5.new(ck).decrypt(rn(ckb), None)
assert sr and len(sr) == KLEN, f"bad server random ({sr and len(sr)}, wanted {KLEN})"
cc = Cipher(HASH(sr + cr).digest()[:KLEN])
sc = Cipher(HASH(cr + sr).digest()[:KLEN])
s.sendall(cc.make(HASH(client_publickey + server_publickey).digest()))
assert struct.unpack(">H", rn(2))[0] == HLEN
assert sc.recv(HLEN, rn(HLEN+16)) == HASH(server_publickey + client_publickey).digest()
assert struct.unpack(">H", rn(2))[0] == 1
sc.recv(1, rn(1+16))
u, p = USER.encode()[:255], PW.encode()[:255]
s.sendall(cc.make(bytes([len(u)]) + u + bytes([len(p)]) + p))

def read_msg():
    """Post-auth read: framed+encrypted for 5/129, raw for 6/130."""
    if not ENCRYPTED_SESSION:
        return None
    n = struct.unpack(">H", rn(2))[0]
    return sc.recv(n, rn(n + 16))

if ENCRYPTED_SESSION:
    buf = read_msg()
    result = struct.unpack(">I", buf[:4])[0]
else:
    result = struct.unpack(">I", rn(4))[0]
assert result == 0, f"auth failed ({result})"

s.sendall(cc.make(b"\x01") if ENCRYPTED_SESSION else b"\x01")
data = read_msg() if ENCRYPTED_SESSION else rn(24)
if ENCRYPTED_SESSION:
    while len(data) < 24:
        data += read_msg()
    w, h = struct.unpack(">H", data[0:2])[0], struct.unpack(">H", data[2:4])[0]
    nl = struct.unpack(">I", data[20:24])[0]
    while len(data) < 24 + nl:
        data += read_msg()
    name = data[24:24+nl].decode()
else:
    w, h = struct.unpack(">H", data[0:2])[0], struct.unpack(">H", data[2:4])[0]
    nl = struct.unpack(">I", data[20:24])[0]
    name = rn(nl).decode()

sess = "AES-EAX encrypted" if ENCRYPTED_SESSION else "plaintext"
print(f"  type {TYPE:<3} {NAMES[TYPE]:<40} OK — {w}x{h} {name!r}, session {sess}")
