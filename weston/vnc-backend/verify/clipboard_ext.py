"""Exercise the VNC clipboard bridge over the *extended* clipboard extension,
which is the path noVNC actually uses.

Same RA2ne handshake as ra2_all.py, then:
  - advertise pseudo-encoding ExtendedClipboard,
  - answer the server's caps message with our own (text, all actions,
    a non-zero max unsolicited size so the server sends Provide directly),
  - send one Provide, framed the way noVNC frames it,
  - decode every Provide the server pushes back.

Usage: clipboard_ext.py HOST PORT USER PW SECONDS [TEXT-AS-HEX]
"""
import socket, sys, hashlib, struct, time, zlib
from Crypto.PublicKey import RSA
from Crypto.Cipher import PKCS1_v1_5, AES
from Crypto.Random import get_random_bytes

HOST, PORT, USER, PW = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
SECONDS = float(sys.argv[5])
SEND = bytes.fromhex(sys.argv[6]) if len(sys.argv) > 6 and sys.argv[6] else None

TYPE = 6
HASH, HLEN, KLEN = hashlib.sha1, 20, 16

ENC_EXT_CLIPBOARD = 0xC0A1E5CE - (1 << 32)      # signed pseudo-encoding

FMT_TEXT = 1 << 0
CAPS = 1 << 24
ACT_REQUEST = 1 << 25
ACT_PEEK = 1 << 26
ACT_NOTIFY = 1 << 27
ACT_PROVIDE = 1 << 28


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
skb = (struct.unpack(">I", skl_buf)[0] + 7) // 8
sn, se = rn(skb), rn(skb)
server_pub = RSA.construct((int.from_bytes(sn, "big"), int.from_bytes(se, "big")))
server_publickey = skl_buf + sn + se
ckb = 2048 // 8
ck = RSA.generate(2048)
client_publickey = struct.pack(">I", 2048) + ck.n.to_bytes(ckb, "big") + ck.e.to_bytes(ckb, "big")
s.sendall(client_publickey)
cr = get_random_bytes(KLEN)
s.sendall(struct.pack(">H", skb) + PKCS1_v1_5.new(server_pub).encrypt(cr))
assert struct.unpack(">H", rn(2))[0] == ckb
sr = PKCS1_v1_5.new(ck).decrypt(rn(ckb), None)
cc = Cipher(HASH(sr + cr).digest()[:KLEN])
sc = Cipher(HASH(cr + sr).digest()[:KLEN])
s.sendall(cc.make(HASH(client_publickey + server_publickey).digest()))
assert struct.unpack(">H", rn(2))[0] == HLEN
assert sc.recv(HLEN, rn(HLEN + 16)) == HASH(server_publickey + client_publickey).digest()
assert struct.unpack(">H", rn(2))[0] == 1
sc.recv(1, rn(1 + 16))
u, p = USER.encode()[:255], PW.encode()[:255]
s.sendall(cc.make(bytes([len(u)]) + u + bytes([len(p)]) + p))
assert struct.unpack(">I", rn(4))[0] == 0, "auth failed"

s.sendall(b"\x01")
init = rn(24)
w, h = struct.unpack(">H", init[0:2])[0], struct.unpack(">H", init[2:4])[0]
name = rn(struct.unpack(">I", init[20:24])[0]).decode()
print(f"connected: {w}x{h} {name!r}", flush=True)

# SetEncodings: ExtendedClipboard only
s.sendall(b"\x02\x00" + struct.pack(">H", 1) + struct.pack(">i", ENC_EXT_CLIPBOARD))
print("sent SetEncodings: ExtendedClipboard", flush=True)


def send_ext(flags, payload=b""):
    body = struct.pack(">I", flags) + payload
    s.sendall(b"\x06\x00\x00\x00" + struct.pack(">i", -len(body)) + body)


# Our caps: text, every action, and a max unsolicited size big enough that the
# server sends Provide rather than Notify.
send_ext(CAPS | FMT_TEXT | ACT_REQUEST | ACT_PEEK | ACT_NOTIFY | ACT_PROVIDE,
         struct.pack(">I", 1 << 20))
print("sent caps", flush=True)

if SEND is not None:
    # Framed the way noVNC frames it: the length prefix counts the NUL.
    body = SEND + b"\0"
    blob = zlib.compress(struct.pack(">I", len(body)) + body)
    send_ext(ACT_PROVIDE | FMT_TEXT, blob)
    print(f"sent Provide: {len(SEND)} bytes, {SEND[:32].hex()}", flush=True)

deadline = time.monotonic() + SECONDS
while True:
    left = deadline - time.monotonic()
    if left <= 0:
        break
    s.settimeout(left)
    try:
        msg = rn(1)[0]
    except (socket.timeout, TimeoutError):
        break
    except EOFError as e:
        print(f"server closed: {e}", flush=True)
        break

    if msg != 3:
        print(f"recv unexpected message type {msg}, stopping", flush=True)
        break

    rn(3)
    length = struct.unpack(">i", rn(4))[0]
    if length >= 0:
        text = rn(length)
        print(f"recv legacy ServerCutText: {length} bytes {text[:32].hex()}",
              flush=True)
        continue

    body = rn(-length)
    flags = struct.unpack(">I", body[:4])[0]
    blob = body[4:]
    what = []
    for bit, nm in ((CAPS, "caps"), (ACT_REQUEST, "request"), (ACT_PEEK, "peek"),
                    (ACT_NOTIFY, "notify"), (ACT_PROVIDE, "provide")):
        if flags & bit:
            what.append(nm)
    print(f"recv ext: flags=0x{flags:08x} [{'|'.join(what)}] payload={len(blob)}",
          flush=True)

    if flags & CAPS:
        sizes = [struct.unpack(">I", blob[i:i + 4])[0] for i in range(0, len(blob), 4)]
        print(f"  server caps max unsolicited sizes: {sizes}", flush=True)
    elif flags & ACT_PROVIDE:
        d = zlib.decompressobj()
        raw = d.decompress(blob)
        declared = struct.unpack(">I", raw[:4])[0]
        payload = raw[4:]
        text = payload[:declared]
        digest = hashlib.sha256(text).hexdigest()[:16]
        print(f"  declared={declared} inflated_after_prefix={len(payload)} "
              f"trailing={payload[declared:]!r}", flush=True)
        print(f"  text: {len(text)} bytes {text[:32].hex()} sha256:{digest}",
              flush=True)
    elif flags & ACT_NOTIFY:
        print("  server offers text; requesting it", flush=True)
        send_ext(ACT_REQUEST | FMT_TEXT)

print("done", flush=True)
