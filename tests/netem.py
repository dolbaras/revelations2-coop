#!/usr/bin/env python3
"""netem.py: a UDP relay that makes a loopback link behave like an Internet link (lab tool, stdlib only).

        game client --UDP--> [relay listens on --listen] --UDP--> game server (--to)

Every packet goes through a link model, in each direction separately (up = client->server, down = server->client):
  loss        random loss in percent; with --burst N the loss comes in bursts of N packets on average (two-state
              Gilbert model: the overall loss stays --loss, only its clustering changes). The bursts count packets, not time:
              on a sparse stream (a connect handshake, a loading game's heartbeat and retries 0.5-2 s apart) one burst spans
              seconds, which a real link rarely does (netflake notes, sec 7 / 13); --burst-max-ms ends a burst after that long
  delay       one-way delay in ms (RTT = up + down), plus jitter: extra random 0..jitter ms (a normal-shaped spread,
              clipped), packets never overtake each other because of jitter unless --jitter-reorder
  reorder     in percent: that share of packets is held back an extra --reorder-delay ms (default delay+30) and so
              arrives behind later ones
  duplicate   in percent: the packet is delivered twice
  rate        bandwidth limit in KB/s (1 KB = 1024 bytes) per direction: packets queue behind each other (serialisation
              delay) and are tail-dropped when the queue would hold more than --queue ms (default 300)
All options apply to both directions; "--up-loss 5" style overrides exist for loss, delay, jitter, rate (up-/down- prefix).

Several clients are possible: each source address gets its own upstream socket, so the server sees one peer per client.
Run it on the machine where both games are (the N100): the server copy must listen elsewhere than the relay
(TS_PORT_SHIFT=20 for the server copy: it binds 7797; the client copy joins 127.0.0.1:7777 = the relay and uses
TS_PORT_SHIFT=10 for its own sockets). See lab\README.md "netem" and lab\netem_run.sh.

  python -I netem.py --listen 127.0.0.1:7777 --to 127.0.0.1:7797 --delay 40 --jitter 10 --loss 15 --burst 3 \
        --rate 256 --seconds 600 --log netem.log
  python -I netem.py --selftest [--delay 40 --loss 15 ...]     # checks the model with an echo server, no game

Prints a statistics line every --stats seconds (default 10) and a summary (per direction: packets, bytes, dropped by
loss and by the queue, duplicated, reordered-by-design, the peak queue delay) to the log and, at the end, stdout.
Stops after --seconds (0 = until killed) or when the file given as --stop-file appears.
"""
import argparse
import heapq
import os
import random
import select
import socket
import sys
import threading
import time


def parse_addr(s):
    host, _, port = s.rpartition(":")
    return (host or "127.0.0.1", int(port))


class Link:
    """One direction of the path."""

    def __init__(self, name, a, seed):
        self.name = name
        # One random stream per direction and per decision (netflake, 2026-11-08). With one stream shared by both directions and all decisions, the k-th
        # packet's fate depended on how the two directions' packets interleaved in time (scheduling), so --seed did not reproduce the link: the same seed gave a
        # loss burst of 21 packets in one run and of 7 in the next. Now the k-th packet of a direction always meets the same loss / jitter / hold-back / copy
        # draws for a given seed (str seeds are hashed with SHA-512 by random.Random: stable across runs and Python builds); without --seed every run differs.
        def stream(kind):
            return random.Random("netem/%s/%s/%s" % (seed, name, kind)) if seed is not None else random.Random()
        self.rng_loss, self.rng_jitter, self.rng_reorder, self.rng_dup = (stream(k) for k in ("loss", "jitter", "reorder", "dup"))
        self.loss = a["loss"] / 100.0
        self.burst = max(1.0, a["burst"])
        self.burst_max = a["burst_max_ms"] / 1000.0 if a["burst_max_ms"] else 0.0
        self.bad_since = 0.0
        self.delay = a["delay"] / 1000.0
        self.jitter = a["jitter"] / 1000.0
        self.reorder = a["reorder"] / 100.0
        self.reorder_delay = (a["reorder_delay"] if a["reorder_delay"] is not None else a["delay"] + 30) / 1000.0
        self.dup = a["dup"] / 100.0
        self.rate = a["rate"] * 1024.0 if a["rate"] > 0 else 0.0   # bytes/s
        self.queue = a["queue"] / 1000.0
        self.jitter_reorder = a["jitter_reorder"]
        # Gilbert model: bad state drops every packet; mean bad run = burst; mean good run set so that loss holds
        self.p_bad_good = 1.0 / self.burst
        self.p_good_bad = (self.loss * self.p_bad_good / (1.0 - self.loss)) if 0 < self.loss < 1 else 0.0
        self.bad = False
        self.free_at = 0.0
        self.last_due = 0.0
        self.st = dict(pkts=0, bytes=0, lost=0, qdrop=0, dup=0, held=0, qmax=0.0, delivered=0, blackout=0, burst_cut=0)
        self.blackout = None   # (from, to) in perf_counter seconds, set by the relay (--blackout)
        # --diag (netflake, 2026-11-08): the longest run of packets lost in a row, the longest time between two deliveries, how late the relay sent a packet
        # against its due time (a sign that this process was not scheduled); times are QPC ms (time.perf_counter, the clock of the bots' cs_now)
        self.dg = dict(run=0, run_max=0, run_max_at=0, gap_max=0.0, gap_max_at=0, gaps=[], late_max=0.0, late_max_at=0, late20=0)
        self.cur_run = 0
        self.last_deliv = 0.0

    def lose(self, now=None):
        if self.loss <= 0:
            return False
        if self.burst <= 1.0:
            return self.rng_loss.random() < self.loss
        x = self.rng_loss.random()   # one draw per packet in every case: the k-th packet keeps its draw
        if self.bad:
            if x < self.p_bad_good:
                self.bad = False
            elif self.burst_max and now is not None and now - self.bad_since >= self.burst_max:
                # --burst-max-ms (netflake): a burst ends after this long however few packets came. The bursts count packets, so on a sparse stream (a
                # connect, a game that loads and sends a heartbeat every 0.5 s while its LOAD_BEGIN / LOADED / GO is retried) one burst of "2 packets"
                # lasted seconds and ate 4-5 retries in a row; a real link's bursts are bounded in time
                self.bad = False
                self.st["burst_cut"] += 1
        else:
            if x < self.p_good_bad:
                self.bad = True
                self.bad_since = now if now is not None else 0.0
        return self.bad

    def note_delivery(self, now, due):
        dg = self.dg
        late = now - due
        if late > dg["late_max"]:
            dg["late_max"], dg["late_max_at"] = late, qms(due)
        if late > 0.020:
            dg["late20"] += 1
        if self.last_deliv:
            gap = now - self.last_deliv
            if gap > dg["gap_max"]:
                dg["gap_max"], dg["gap_max_at"] = gap, qms(self.last_deliv)
            if gap > 0.3 and len(dg["gaps"]) < 40:
                dg["gaps"].append([qms(self.last_deliv), round(gap * 1000)])
        self.last_deliv = now

    def schedule(self, now, size):
        """Returns a list of delivery times (empty = dropped)."""
        st = self.st
        st["pkts"] += 1
        st["bytes"] += size
        if self.blackout and self.blackout[0] <= now < self.blackout[1]:
            st["blackout"] += 1
            return []
        if self.lose(now):
            st["lost"] += 1
            self.cur_run += 1
            if self.cur_run > self.dg["run_max"]:
                self.dg["run_max"], self.dg["run_max_at"] = self.cur_run, qms(now)
            return []
        self.cur_run = 0
        t = now
        if self.rate:
            start = max(now, self.free_at)
            if start - now > self.queue:
                st["qdrop"] += 1
                return []
            self.free_at = start + size / self.rate
            t = self.free_at
            st["qmax"] = max(st["qmax"], start - now)
        due = t + self.delay
        if self.jitter:
            due += min(max(self.rng_jitter.gauss(self.jitter / 2, self.jitter / 4), 0.0), self.jitter)
        if not self.jitter_reorder:
            due = max(due, self.last_due)
        self.last_due = max(self.last_due, due)
        if self.reorder and self.rng_reorder.random() < self.reorder:
            due += self.reorder_delay        # not stored in last_due: the next packets overtake it
            st["held"] += 1
        out = [due]
        if self.dup and self.rng_dup.random() < self.dup:
            out.append(due + 0.001)
            st["dup"] += 1
        return out


def qms(t):
    """perf_counter seconds -> the bots' session clock (cn_now_ms: QPC ms modulo 2^32)"""
    return int(t * 1000) & 0xFFFFFFFF


def link_args(args, side):
    d = {}
    for k in ("loss", "burst", "burst_max_ms", "delay", "jitter", "reorder", "reorder_delay", "dup", "rate", "queue", "jitter_reorder"):
        v = getattr(args, k)
        o = getattr(args, side + "_" + k, None) if hasattr(args, side + "_" + k) else None
        d[k] = o if o is not None else v
    return d


def enable_fine_timer():
    try:
        import ctypes
        ctypes.windll.winmm.timeBeginPeriod(1)
    except Exception:
        pass


def no_connreset(sock):
    """Windows reports an ICMP 'port unreachable' of an earlier send as an error of recvfrom: switch that off."""
    try:
        sock.ioctl(socket.SIO_UDP_CONNRESET, False)
    except Exception:
        pass


class Relay:
    def __init__(self, args, log):
        self.args = args
        self.log = log
        self.up = Link("up", link_args(args, "up"), args.seed)
        self.down = Link("down", link_args(args, "down"), args.seed)
        self.listen = parse_addr(args.listen)
        self.server = parse_addr(args.to)
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.front.bind(self.listen)
        no_connreset(self.front)
        self.clients = {}       # client addr -> upstream socket
        self.by_sock = {}       # upstream socket -> client addr
        self.heap = []
        self.seq = 0
        self.stop = threading.Event()
        self.t0 = time.perf_counter()
        if args.blackout:
            b0, blen = (float(x) for x in args.blackout.split(","))
            self.up.blackout = self.down.blackout = (self.t0 + b0, self.t0 + b0 + blen)

    def now(self):
        return time.perf_counter()

    def push(self, due, sock, data, dest):
        self.seq += 1
        heapq.heappush(self.heap, (due, self.seq, sock, data, dest))

    def upstream(self, caddr):
        s = self.clients.get(caddr)
        if s is None:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            s.bind(("127.0.0.1", 0))
            no_connreset(s)
            self.clients[caddr] = s
            self.by_sock[s] = caddr
            self.log("client %s:%d -> upstream port %d" % (caddr[0], caddr[1], s.getsockname()[1]))
        return s

    def run(self):
        a = self.args
        loop_prev, loop_max, loop_max_at, next_diag = 0.0, 0.0, 0, self.now() + 0.5
        next_stats = self.now() + a.stats
        end = self.now() + a.seconds if a.seconds else None
        self.log("relay %s:%d -> %s:%d; up %s; down %s" % (self.listen + self.server + (self.describe(self.up), self.describe(self.down))))
        while not self.stop.is_set():
            now = self.now()
            if end and now >= end:
                break
            if a.stop_file and os.path.exists(a.stop_file):
                self.log("stop file seen")
                break
            timeout = 0.05
            if self.heap:
                timeout = max(0.0, min(timeout, self.heap[0][0] - now))
            r, _, _ = select.select([self.front] + list(self.by_sock), [], [], timeout)
            now = self.now()
            if loop_prev and now - loop_prev - timeout > loop_max:   # time beyond the select timeout: this process did not run
                loop_max, loop_max_at = now - loop_prev - timeout, qms(loop_prev)
            loop_prev = now
            if a.diag and now >= next_diag:
                next_diag = now + 0.5
                self.write_diag(loop_max, loop_max_at)
            for s in r:
                try:
                    data, src = s.recvfrom(65535)
                except OSError:
                    continue
                if s is self.front:
                    for due in self.up.schedule(now, len(data)):
                        self.push(due, self.upstream(src), data, self.server)
                else:
                    caddr = self.by_sock[s]
                    for due in self.down.schedule(now, len(data)):
                        self.push(due, self.front, data, caddr)
            now = self.now()
            while self.heap and self.heap[0][0] <= now:
                due, _, sock, data, dest = heapq.heappop(self.heap)
                try:
                    sock.sendto(data, dest)
                    l = self.up if sock is not self.front else self.down
                    l.st["delivered"] += 1
                    l.note_delivery(now, due)
                except OSError:
                    pass
            if a.stats and now >= next_stats:
                next_stats = now + a.stats
                self.log("t=%ds %s | %s" % (now - self.t0, self.brief(self.up), self.brief(self.down)))
        self.log("summary up:   " + self.full(self.up))
        self.log("summary down: " + self.full(self.down))
        if a.diag:
            self.write_diag(loop_max, loop_max_at)

    def write_diag(self, loop_max, loop_max_at):
        import json
        d = {"q_now": qms(self.now()), "loop_max": round(loop_max * 1000, 1), "loop_max_at": loop_max_at}
        for l in (self.up, self.down):
            d[l.name] = dict(l.dg, lost=l.st["lost"], pkts=l.st["pkts"], delivered=l.st["delivered"], burst_cut=l.st["burst_cut"])
        try:
            with open(self.args.diag + ".tmp", "w") as f:
                json.dump(d, f)
            os.replace(self.args.diag + ".tmp", self.args.diag)
        except OSError:
            pass

    @staticmethod
    def describe(l):
        return "loss %.1f%% burst %.1f delay %.0f ms jitter %.0f ms reorder %.1f%% dup %.1f%% rate %s queue %.0f ms" % (
            l.loss * 100, l.burst, l.delay * 1000, l.jitter * 1000, l.reorder * 100, l.dup * 100,
            ("%.0f KB/s" % (l.rate / 1024)) if l.rate else "unlimited", l.queue * 1000)

    @staticmethod
    def brief(l):
        s = l.st
        return "%s %d pk %d KB lost %d qdrop %d" % (l.name, s["pkts"], s["bytes"] // 1024, s["lost"], s["qdrop"])

    @staticmethod
    def full(l):
        s = l.st
        n = max(1, s["pkts"])
        return "packets %d, bytes %d, lost %d (%.1f%%), dropped by the rate queue %d (%.1f%%), duplicated %d, held back %d, peak queue %.0f ms" % (
            s["pkts"], s["bytes"], s["lost"], 100.0 * s["lost"] / n, s["qdrop"], 100.0 * s["qdrop"] / n, s["dup"], s["held"], s["qmax"] * 1000)


def selftest(args):
    """An echo server and a sender through the relay: checks delay, loss and ordering of the model itself."""
    base = 39000 + os.getpid() % 500
    args.listen = "127.0.0.1:%d" % base
    args.to = "127.0.0.1:%d" % (base + 1)
    args.seconds = 0
    args.stats = 0
    lines = []
    rel = Relay(args, lambda m: lines.append(m))
    th = threading.Thread(target=rel.run, daemon=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.bind(("127.0.0.1", base + 1))
    srv.settimeout(0.2)
    stop = threading.Event()

    def echo():
        while not stop.is_set():
            try:
                d, a = srv.recvfrom(65535)
                srv.sendto(d, a)
            except OSError:
                pass
    te = threading.Thread(target=echo, daemon=True)
    th.start()
    te.start()
    cl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    cl.bind(("127.0.0.1", 0))
    no_connreset(cl)
    n, size, gap = args.selftest_packets, args.selftest_size, args.selftest_gap / 1000.0
    sent = {}
    got = []
    t_end = time.perf_counter() + n * gap + 2.0 + 2 * (args.delay + args.jitter + args.reorder_delay_or(0)) / 1000.0 + 2.0
    i = 0
    nxt = time.perf_counter()
    while time.perf_counter() < t_end:
        now = time.perf_counter()
        if i < n and now >= nxt:
            sent[i] = now
            cl.sendto(i.to_bytes(4, "big") + b"x" * (size - 4), ("127.0.0.1", base))
            i += 1
            nxt += gap
        r, _, _ = select.select([cl], [], [], 0.002)
        if r:
            d, _ = cl.recvfrom(65535)
            got.append((int.from_bytes(d[:4], "big"), time.perf_counter()))
    stop.set()
    rel.stop.set()
    th.join(2)
    rtts = sorted((t - sent[k]) * 1000 for k, t in got)
    seen = [k for k, _ in got]
    ooo = sum(1 for a, b in zip(seen, seen[1:]) if b < a)
    ex = len(got) - len(set(seen))
    print("selftest: sent %d, echoes %d (round trip delivered %.1f%%), duplicates %d, out of order %d" % (
        n, len(got), 100.0 * len(set(seen)) / n, ex, ooo))
    if rtts:
        print("round trip ms: min %.1f median %.1f p95 %.1f max %.1f (expect about %.0f + 2 x jitter/2)" % (
            rtts[0], rtts[len(rtts) // 2], rtts[int(len(rtts) * 0.95)], rtts[-1], 2 * args.delay))
    print("expected one-way loss %.1f%% -> round trip delivery about %.1f%%" % (args.loss, 100 * (1 - args.loss / 100) ** 2))
    for l in lines:
        print(l)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="127.0.0.1:7777")
    ap.add_argument("--to", default="127.0.0.1:7797")
    for k, d, h in (("loss", 0.0, "random loss %"), ("burst", 1.0, "mean loss burst length in packets (1 = independent)"),
                    ("delay", 0.0, "one-way delay ms"), ("jitter", 0.0, "jitter ms"), ("reorder", 0.0, "% of packets held back"),
                    ("dup", 0.0, "% duplicated"), ("rate", 0.0, "KB/s limit per direction (0 = none)"),
                    ("queue", 300.0, "queue length in ms before tail drop (rate limit)")):
        ap.add_argument("--" + k, type=float, default=d, help=h)
        if k in ("loss", "delay", "jitter", "rate"):
            ap.add_argument("--up-" + k, type=float, default=None)
            ap.add_argument("--down-" + k, type=float, default=None)
    ap.add_argument("--reorder-delay", dest="reorder_delay", type=float, default=None, help="ms (default delay+30)")
    ap.add_argument("--burst-max-ms", dest="burst_max_ms", type=float, default=0.0, help="a loss burst ends after this long however few packets came (0 = no limit)")
    ap.add_argument("--jitter-reorder", action="store_true", help="let jitter reorder packets")
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--stats", type=float, default=10)
    ap.add_argument("--log", default="")
    ap.add_argument("--stop-file", default="")
    ap.add_argument("--blackout", default="", help="START,LEN seconds: drop every packet of both directions for LEN s from START s after the relay started (a dead link)")
    ap.add_argument("--diag", default="", help="netflake: keep a JSON summary of loss runs, delivery gaps and relay lateness in this file (rewritten every 0.5 s)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--realization", type=int, default=0, help="print the loss fates of the first N packets per direction for this seed and these options, then exit")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--selftest-packets", type=int, default=400)
    ap.add_argument("--selftest-size", type=int, default=200)
    ap.add_argument("--selftest-gap", type=float, default=10.0, help="ms between packets")
    args = ap.parse_args()
    args.reorder_delay_or = lambda d: args.reorder_delay if args.reorder_delay is not None else d
    for side in ("up", "down"):
        for k in ("reorder", "dup", "burst", "burst_max_ms", "queue", "reorder_delay", "jitter_reorder"):
            setattr(args, side + "_" + k, None)
    if args.realization:
        for side in ("up", "down"):
            l = Link(side, link_args(args, side), args.seed)
            fates = [l.lose() for _ in range(args.realization)]
            run = best = 0
            for x in fates:
                run = run + 1 if x else 0
                best = max(best, run)
            print("%-4s first %d packets: lost %d (%.1f %%), longest loss run %d; first 60: %s" % (side, len(fates), sum(fates), 100.0 * sum(fates) / len(fates), best,
                                                                                               "".join("x" if x else "." for x in fates[:60])))
        return
    enable_fine_timer()
    if args.selftest:
        selftest(args)
        return
    f = open(args.log, "a", buffering=1) if args.log else None

    def log(m):
        line = time.strftime("%H:%M:%S ") + m
        if f:
            f.write(line + "\n")
        else:
            print(line, flush=True)
    rel = Relay(args, log)
    try:
        rel.run()
    except KeyboardInterrupt:
        pass
    if f:
        f.close()


if __name__ == "__main__":
    main()
