"""RA2ne handshake from vnc-backend/verify/clipboard.py, then a pointer script.

    pointer.py HOST PORT USER PW CMD...   CMD = mX,Y | sSECONDS
"""
import socket, sys, hashlib, struct, time
from Crypto.PublicKey import RSA
from Crypto.Cipher import PKCS1_v1_5, AES
from Crypto.Random import get_random_bytes

HOST, PORT, USER, PW = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]

TYPE = 6  # RA2ne, AES-128, plaintext session after auth
HASH, HLEN, KLEN = hashlib.sha1, 20, 16


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
        ct, tag = c.encrypt_and_digest(msg)
        self._bump()
        return ad + ct + tag

    def recv(self, length, blob):
        ad = struct.pack(">H", length)
        c = AES.new(self.key, AES.MODE_EAX, nonce=bytes(self.counter), mac_len=16)
        c.update(ad)
        pt = c.decrypt_and_verify(blob[:length], blob[length:length + 16])
        self._bump()
        return pt


s = socket.create_connection((HOST, PORT), timeout=30)


def rn(n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise EOFError(f"eof at {len(b)}/{n}")
        b += d
    return b


rn(12)
s.sendall(b"RFB 003.008\n")
types = list(rn(rn(1)[0]))
assert TYPE in types, f"type {TYPE} not offered (got {types})"
s.sendall(bytes([TYPE]))

skl_buf = rn(4)
skl = struct.unpack(">I", skl_buf)[0]
skb = (skl + 7) // 8
sn, se = rn(skb), rn(skb)
server_pub = RSA.construct((int.from_bytes(sn, "big"), int.from_bytes(se, "big")))
server_publickey = skl_buf + sn + se
ckl = 2048
ckb = ckl // 8
ck = RSA.generate(ckl)
client_publickey = struct.pack(">I", ckl) + ck.n.to_bytes(ckb, "big") + ck.e.to_bytes(ckb, "big")
s.sendall(client_publickey)
cr = get_random_bytes(KLEN)
s.sendall(struct.pack(">H", skb) + PKCS1_v1_5.new(server_pub).encrypt(cr))
assert struct.unpack(">H", rn(2))[0] == ckb
sr = PKCS1_v1_5.new(ck).decrypt(rn(ckb), None)
assert sr and len(sr) == KLEN
cc = Cipher(HASH(sr + cr).digest()[:KLEN])
sc = Cipher(HASH(cr + sr).digest()[:KLEN])
s.sendall(cc.make(HASH(client_publickey + server_publickey).digest()))
assert struct.unpack(">H", rn(2))[0] == HLEN
assert sc.recv(HLEN, rn(HLEN + 16)) == HASH(server_publickey + client_publickey).digest()
assert struct.unpack(">H", rn(2))[0] == 1
sc.recv(1, rn(1 + 16))
u, p = USER.encode()[:255], PW.encode()[:255]
s.sendall(cc.make(bytes([len(u)]) + u + bytes([len(p)]) + p))

result = struct.unpack(">I", rn(4))[0]
assert result == 0, f"auth failed ({result})"

s.sendall(b"\x01")                                  # ClientInit, shared
init = rn(24)
w, h = struct.unpack(">H", init[0:2])[0], struct.unpack(">H", init[2:4])[0]
name = rn(struct.unpack(">I", init[20:24])[0]).decode()
print(f"connected: {w}x{h} {name!r}", flush=True)
for cmd in sys.argv[5:]:
    if cmd[0] == "m":
        x, y = map(int, cmd[1:].split(","))
        s.sendall(struct.pack(">BBHH", 5, 0, x, y))  # PointerEvent, no buttons
        print(f"move {x},{y}", flush=True)
    elif cmd[0] == "s":
        time.sleep(float(cmd[1:]))
s.close()
print("closed", flush=True)
