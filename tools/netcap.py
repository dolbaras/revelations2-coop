#!/usr/bin/env python3
"""netcap.py: read the .bin file of a REV2_NETCAP capture (native/net.c).

  python -I netcap.py <prefix.bin> [--kind N] [--hex N] [--stats]

Record layout (little endian), after a 16-byte magic "REV2NET1":
  u32 t_ms, u32 tid, u16 kind, u16 chan, u32 a, u32 b, u32 c, u32 len, u32 caplen, caplen bytes of payload (max 2048)
Kinds: 1 winsock send, 2 recv, 3 sendto, 4 recvfrom (a socket, b ip, c port); 10 SendP2PPacket, 11 ReadP2PPacket
(a/b = peer steamid low/high, c = send type for 10, chan = channel or 65535 when the interface has none);
12 SendLobbyChatMsg, 13 GetLobbyChatEntry (a/b lobby id low/high); 14 Steam callback (chan = callback id); 15 marker text.
"""
import struct
import sys
from collections import Counter

HDR = struct.Struct("<IIHHIIIII")
NAMES = {1: "ws_send", 2: "ws_recv", 3: "ws_sendto", 4: "ws_recvfrom", 10: "p2p_send", 11: "p2p_read", 12: "lobby_chat_send",
         13: "lobby_chat_entry", 14: "callback", 15: "mark"}


def records(path):
    data = open(path, "rb").read()
    if data[:8] != b"REV2NET1":
        raise SystemExit("not a REV2NET1 file")
    o = 16
    while o + HDR.size <= len(data):
        t, tid, kind, chan, a, b, c, ln, cap = HDR.unpack_from(data, o)
        o += HDR.size
        payload = data[o:o + cap]
        o += cap
        yield dict(t=t, tid=tid, kind=kind, chan=chan, a=a, b=b, c=c, len=ln, data=payload)


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    kind = int(args[args.index("--kind") + 1]) if "--kind" in args else None
    hexn = int(args[args.index("--hex") + 1]) if "--hex" in args else 32
    recs = [r for r in records(path) if kind is None or r["kind"] == kind]
    if "--stats" in args:
        cnt = Counter((NAMES.get(r["kind"], r["kind"]), r["chan"] if r["kind"] in (10, 11, 14) else 0) for r in recs)
        for k, v in sorted(cnt.items(), key=lambda x: str(x[0])):
            print(k, v)
        return
    for r in recs:
        print("%8d ms t%-6d %-16s ch%-5d a=%08x b=%08x c=%08x len=%-5d %s" % (r["t"], r["tid"], NAMES.get(r["kind"], r["kind"]), r["chan"], r["a"], r["b"],
              r["c"], r["len"], r["data"][:hexn].hex()))


if __name__ == "__main__":
    main()
