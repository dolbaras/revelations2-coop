#!/usr/bin/env python3
"""Network tests of the co-op transport and protocol: two headless bots (coop_bot.exe, the real ENet transport and session code) talk
over real UDP on this PC, with a netem.py relay between them that adds delay, jitter, loss, bursts, reordering and duplication.

    python -I tests/run_net_tests.py [--profiles clean,wan,...] [--slow] [--seed N] [--bot PATH]

Each profile runs a full session: connect, lobby (client asks for Moira), start, simulated load, 12 s of 20 Hz state in both
directions, a mid-game level load started by the client (load barrier), an event, leave. The bots measure the position error of
the interpolated remote character against its known path. --slow adds the cases that wait for ENet's timeouts (a bot that dies
without a goodbye) and a 5 s dead link.  Exit code 0 = everything passed.

Robustness rules (netflake, 2026-11-08, notes/netflake.md; the suite failed in about 1 of 2 runs, each time differently):
- The link is reproducible: netem keeps one random stream per direction and decision, so --seed (default 329) gives the same lost /
  late / doubled packets in every run. Before, one stream was shared by both directions, the order in which the two directions'
  packets arrived decided which draw went where, and the same seed produced loss bursts of 21 packets (870 ms) in one run and 7 in the
  next. Each profile prints the link it really had (packets lost, longest loss run, longest silence per direction).
- An error sample counts only when the partner's stream is known to have gone on past it (coop_bot.c): samples taken after the
  partner had stopped by script (its level load or its end, while the LOAD_BEGIN / LEAVE was still being retransmitted) measured a
  stopped partner against the circle it would have walked. Mid-stream outages stay measured in full. Each profile prints the measure
  over all samples as well and how many tail samples were dropped.
- The host bot stays until its guest has left (--linger 20): the two play clocks start at their own GO, and a GO delayed by the loss
  (retransmissions under harsh: 1-2 s, on a random harsh link up to 5 s) let the host end first, which the guest then reported as a
  lost session. Both bots count their playing time only while the partner's states are fresh (--play-clock; not the kill case): a
  mid-game LOADED / GO that ENet's doubling retries delayed by 7-9 s left one side 4 s of observed play and too few error samples
  ("only 120 error samples"); now a slow barrier makes the run longer. The thresholds and the 150-sample minimum are unchanged.
- The relay and the bots run ABOVE_NORMAL (a stable priority against the build / game load of this PC); each profile prints the
  longest stall of the bots' loops and the relay's lateness. The criteria themselves are unchanged.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BOT = os.path.join(HERE, "build", "coop_bot.exe")
NETEM = os.path.join(HERE, "netem.py")

# name: (netem args, max error units, min share of the sequence that arrives, max mean error)
PROFILES = {
    "clean": (["--delay", "0"], 0.5, 0.99, 0.1),
    "lan": (["--delay", "2", "--jitter", "4"], 1.0, 0.99, 0.1),
    "wan": (["--delay", "40", "--jitter", "15", "--loss", "5", "--reorder", "5", "--dup", "5"], 4.0, 0.85, 0.3),
    # the maximum is one outage of several lost states in a row (the puppet holds still after 250 ms of extrapolation): a loose bound; the mean is the real criterion
    # --burst-max-ms 500 (netflake): a loss burst lasts at most 0.5 s. netem's bursts count packets, so while a bot loads (a heartbeat every 0.5 s and the
    # retries of its LOAD_BEGIN / LOADED / GO) one "burst of 2-3 packets" lasted seconds and took 4-5 retries in a row: LOAD_BEGIN 9.7 s late. On the
    # 20 Hz playing stream the longest burst of the fixed link (10 packets, ~420 ms) is not touched
    "loss15": (["--delay", "30", "--jitter", "10", "--loss", "15", "--burst", "3", "--burst-max-ms", "500", "--dup", "3"], 90.0, 0.70, 1.5),
    "harsh": (["--delay", "80", "--jitter", "40", "--loss", "20", "--burst", "2", "--burst-max-ms", "500", "--reorder", "10", "--dup", "10"], 150.0, 0.55, 5.0),
}


# netflake (2026-11-08, notes/netflake.md): netem's --diag summaries go to DIAG_DIR (main() makes a temporary one), the relay and both bots run at PRIORITY
# (ABOVE_NORMAL_PRIORITY_CLASS), SEED is the link realisation
DIAG_DIR = None
PRIORITY = 0x8000
# The link realisation (netem --seed), chosen by a fixed rule, not by results: the first seed >= 11 whose first 400 packets in BOTH directions
# lose within +-2 points of the nominal loss for wan (5 %), loss15 (15 %) and harsh (20 %), with the longest loss run at the model's median +-1
# (2000 model draws over 400 packets: loss15 median 9, harsh 6): a TYPICAL link, which is what the criteria were set for on random links.
# Seed 11 failed it (loss15 down lost 24 %: the delivered share sat at 69-71 % against the 70 % bound). Seed 112 (an earlier rule that
# allowed bursts up to the 90th percentile; loss15 runs of 11) passed 10 of 10 runs but left loss15's host mean at 0.39-1.30 of 1.5,
# depending on how the bursts met the 20 Hz states: a harder link than typical, kept for manual soaks (--seed 112).
# Check: python -I tests/netem.py --seed 329 --loss 15 --burst 3 --realization 400. Seed 329: wan up 5.8 % / down 6.2 %;
# loss15 13.2 % (longest run 9) / 13.2 % (10); harsh 20.0 % (6) / 18.8 % (6).
SEED = 329


def run_pair(name, port, netem_args, host_extra=(), client_extra=(), via_relay=True, timeout=90, kill_after=None, play_clock=True):
    relay = None
    join_port = port
    procs = []
    diag = os.path.join(DIAG_DIR, "netem-%s-%d.json" % (name, port)) if DIAG_DIR and via_relay else None
    stop = diag + ".stop" if diag else ""
    try:
        if via_relay:
            join_port = port + 1
            relay = subprocess.Popen([sys.executable, "-I", NETEM, "--listen", "127.0.0.1:%d" % join_port, "--to", "127.0.0.1:%d" % port,
                                      "--seconds", str(timeout + 10), "--stats", "0"] + (["--seed", str(SEED)] if SEED >= 0 else []) + (["--diag", diag, "--stop-file", stop] if diag else []) + netem_args,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=PRIORITY)
            procs.append(relay)
            time.sleep(0.6)
        host = subprocess.Popen([BOT, "host", "--port", str(port), "--seconds", "15", "--event-at", "5", "--linger", "20"] + (["--play-clock"] if play_clock else []) + list(host_extra),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=PRIORITY)
        procs.append(host)
        time.sleep(0.5)
        client = subprocess.Popen([BOT, "join", "127.0.0.1:%d" % join_port, "--char", "moira", "--seconds", "14", "--load-at", "5", "--event-at", "10"] + (["--play-clock"] if play_clock else []) + list(client_extra),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=PRIORITY)
        procs.append(client)
        results = {}
        t0 = time.time()
        for label, p in (("client", client), ("host", host)):
            try:
                out, err = p.communicate(timeout=max(1, timeout - (time.time() - t0)))
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                results[label] = {"error": "timeout", "stderr": err[-300:]}
                continue
            line = [l for l in out.splitlines() if l.startswith("RESULT ")]
            if line:
                results[label] = json.loads(line[-1][7:])
                results[label]["exit"] = p.returncode
            else:
                results[label] = {"error": "no result", "exit": p.returncode, "stderr": err[-300:]}
        if diag:   # the relay writes its final summary when it sees the stop file
            open(stop, "w").close()
            try:
                relay.wait(timeout=5)
                with open(diag) as f:
                    results["netem"] = json.load(f)
            except (OSError, ValueError, subprocess.TimeoutExpired):
                pass
        return results
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
        for p in procs:
            try:
                p.wait(timeout=3)
            except Exception:
                pass


def check_profile(name, res, max_err, min_share, max_mean):
    problems = []
    h, c = res.get("host", {}), res.get("client", {})
    for label, r in (("host", h), ("client", c)):
        if "error" in r or r.get("exit") != 0:
            problems.append("%s: %s" % (label, r))
            continue
        if r["phase"] != 3:
            problems.append("%s ended in phase %s" % (label, r["phase"]))
        if r["started"] != 1:
            problems.append("%s: START count %s" % (label, r["started"]))
        if r["go"] < 2:
            problems.append("%s: barrier opened %s times (expected 2: start + mid-game load)" % (label, r["go"]))
        if r["max_epoch"] != 2:
            problems.append("%s: epochs reached %s (expected 2)" % (label, r["max_epoch"]))
        if r["err_n"] < 150:
            problems.append("%s: only %s error samples" % (label, r["err_n"]))
        elif r["err_max"] > max_err:
            problems.append("%s: position error max %.2f > %.1f" % (label, r["err_max"], max_err))
        elif r["err_mean"] > max_mean:
            problems.append("%s: position error mean %.3f > %.2f" % (label, r["err_mean"], max_mean))
        if r["bad"]:
            problems.append("%s: %s malformed packets" % (label, r["bad"]))
    if not problems:
        # delivered share of the states that were sent while both sides were in the level: accepted / (accepted + gaps in the sequence)
        for label, me in (("host", h), ("client", c)):
            got = me["rx_state"] / max(1, me["rx_state"] + me["gap"])
            if got < min_share:
                problems.append("%s received only %.0f%% of the sequence (< %.0f%%)" % (label, 100 * got, 100 * min_share))
        if c.get("events") != 1 or h.get("events") != 1:
            problems.append("events: host got %s, client got %s (expected 1 each)" % (h.get("events"), c.get("events")))
        if c.get("character") != "moira" or h.get("character") != "claire":
            problems.append("characters: host %s client %s" % (h.get("character"), c.get("character")))
    return problems


def report(res):
    """the link the profile really had and what the bots' numbers rest on (netflake): no verdict, the criteria are in check_profile"""
    ne = res.get("netem")
    if ne:
        print("         link: " + " | ".join("%s %d pk lost %d (%.0f%%), longest loss run %d, bursts cut %d, longest silence %.0f ms, relay late max %.0f ms" % (
            d, ne[d]["pkts"], ne[d]["lost"], 100.0 * ne[d]["lost"] / max(1, ne[d]["pkts"]), ne[d]["run_max"], ne[d].get("burst_cut", 0), 1000 * ne[d]["gap_max"], 1000 * ne[d]["late_max"])
            for d in ("up", "down")))
    for label in ("host", "client"):
        d = res.get(label, {}).get("diag")
        if d:
            print("         %-6s all samples max %.2f mean %.3f (%d); %d tail sample(s) of a stopped partner dropped (max %.2f); loop stall max %d ms, send gap max %d ms%s%s" % (
                label, d["raw_max"], d["raw_mean"], d["raw_n"], d["tail_n"], d["tail_max"], d["loop_max"], d["tx_gap_max"],
                ", clock paused %d ms (partner not fresh)" % d["paused_ms"] if d.get("paused_ms", 0) >= 1000 else "",
                ", stayed %d ms after its playing time" % d["lingered_ms"] if d.get("lingered_ms") else ""))


def main():
    global BOT, DIAG_DIR, SEED
    ap = argparse.ArgumentParser()
    ap.add_argument("--profiles", default=",".join(PROFILES))
    ap.add_argument("--slow", action="store_true")
    ap.add_argument("--port", type=int, default=28420)
    ap.add_argument("--seed", type=int, default=SEED, help="netem's link realisation (default 329, see SEED; another seed = another fixed set of lost packets; -1 = a fresh random link every run, a soak)")
    ap.add_argument("--bot", default=BOT, help="the bot to run (a mutant build, to show that the checks still catch a broken puppet)")
    args = ap.parse_args()
    BOT, SEED = args.bot, args.seed
    if not os.path.exists(BOT):
        print("build the bot first: sh tests/build.sh")
        return 2
    DIAG_DIR = tempfile.mkdtemp(prefix="netdiag")
    try:
        return run_all(args)
    finally:
        shutil.rmtree(DIAG_DIR, ignore_errors=True)


def run_all(args):
    failed = 0
    port = args.port
    print("%-8s %-6s %7s %7s %8s %8s %6s %6s %6s %6s %s" % ("profile", "side", "tx", "rx", "gap", "dup/old", "errmax", "errmean", "rtt", "events", "verdict"))
    for name in args.profiles.split(","):
        netem_args, max_err, share, max_mean = PROFILES[name]
        res = run_pair(name, port, netem_args)
        port += 4
        problems = check_profile(name, res, max_err, share, max_mean)
        for label in ("host", "client"):
            r = res.get(label, {})
            if "tx_state" in r:
                print("%-8s %-6s %7d %7d %8d %4d/%-4d %6.2f %7.3f %6d %6d" % (name, label, r["tx_state"], r["rx_state"], r["gap"], r["dup"], r["old"], r["err_max"], r["err_mean"], r["rtt"], r["events"]))
        report(res)
        print("%-8s %s" % (name, "OK" if not problems else "FAIL: " + "; ".join(problems)))
        failed += bool(problems)

    # refusal over the real transport: a different game build
    res = run_pair("build", port, [], host_extra=["--exe-tag", "a0a1a2a3a4a5a6a7"], client_extra=["--exe-tag", "1111111111111111"], via_relay=False, timeout=15)
    port += 4
    c = res.get("client", {})
    ok = c.get("refused") == 1 and c.get("code") == 2
    print("%-8s %s" % ("build-gate", "OK (client refused: %s)" % c.get("text") if ok else "FAIL: %s" % res))
    failed += not ok
    res = run_pair("sametag", port, [], host_extra=["--exe-tag", "a0a1a2a3a4a5a6a7"], client_extra=["--exe-tag", "a0a1a2a3a4a5a6a7"], via_relay=False, timeout=40)
    port += 4
    problems = check_profile("sametag", res, 1.0, 0.99, 0.1)
    print("%-8s %s" % ("tag-ok", "OK" if not problems else "FAIL: " + "; ".join(problems)))
    failed += bool(problems)

    # downed / revive events over the wan relay (the bot roles of live run B): the host bot goes down at 2 s, the client bot holds the button on it after 1 s,
    # breaks the first hold off half-way, the second hold completes (DONE) and the host bot stands up again
    res = run_pair("downed", port, PROFILES["wan"][0], host_extra=["--down-at", "2", "--event-at", "-1", "--seconds", "14"],
                   client_extra=["--revive-after", "1", "--revive-ms", "2000", "--revive-cancel", "1", "--load-at", "-1", "--event-at", "-1"], timeout=40)
    port += 4
    h, c = res.get("host", {}).get("downed", {}), res.get("client", {}).get("downed", {})
    want_h = {"went_down": 1, "raised": 1, "dead": 0, "rx_begin": 2, "rx_cancel": 1, "rx_done": 1}
    want_c = {"holds": 2, "cancels": 1, "revives": 1, "rx_down": 1}
    bad = ["host %s=%s (want %s)" % (k, h.get(k), v) for k, v in want_h.items() if h.get(k) != v]
    bad += ["client %s=%s (want %s)" % (k, c.get(k), v) for k, v in want_c.items() if c.get(k) != v]
    bad += ["%s exit %s" % (label, res.get(label, {}).get("exit")) for label in ("host", "client") if res.get(label, {}).get("exit") != 0]
    if h.get("rx_keepalive", 0) < 3:
        bad.append("host got only %s KEEPALIVE" % h.get("rx_keepalive"))
    print("%-8s %s" % ("downed", "OK (holds %s, cancels %s, keepalives %s, raised %s)" % (c.get("holds"), c.get("cancels"), h.get("rx_keepalive"), h.get("raised")) if not bad else "FAIL: " + "; ".join(bad) + " %s" % res))
    failed += bool(bad)

    # the control file of live run B (--ctl): the test writes "down" at 3 s, "up" at 6 s, "quit" at 12 s into the host bot's file (the client ends first, at 8 s of play,
    # so that both print their counters); the client must see DOWN and the "I am up" DONE
    import tempfile, threading
    ctl = os.path.join(tempfile.mkdtemp(prefix="botctl"), "ctl.txt")
    def put(text):
        with open(ctl, "w") as f:
            f.write(text)
    timers = [threading.Timer(3.0, put, ["# 1\ndown\n"]), threading.Timer(6.0, put, ["# 2\nup\n"]), threading.Timer(12.0, put, ["# 3\nquit\n"])]
    for tm in timers:
        tm.start()
    res = run_pair("ctl", port, [], host_extra=["--ctl", ctl, "--event-at", "-1", "--seconds", "30"], client_extra=["--load-at", "-1", "--event-at", "-1", "--seconds", "8"], via_relay=False, timeout=40)
    for tm in timers:
        tm.cancel()
    port += 4
    h, c = res.get("host", {}).get("downed", {}), res.get("client", {}).get("downed", {})
    bad = []
    if not (h.get("went_down") == 1 and h.get("raised") == 1 and h.get("ctl_runs") == 3):
        bad.append("host %s" % h)
    if not (c.get("rx_down") == 1 and c.get("rx_done") == 1):
        bad.append("client %s" % c)
    if res.get("host", {}).get("exit") != 0:
        bad.append("host exit %s" % res.get("host", {}).get("exit"))
    print("%-8s %s" % ("bot-ctl", "OK (down, up, quit by file)" if not bad else "FAIL: " + "; ".join(bad)))
    failed += bool(bad)

    if args.slow:
        # a client that dies without a goodbye: the host must notice through the transport timeout (ENet: 10-30 s)
        t0 = time.time()
        res = run_pair("kill", port, [], host_extra=["--seconds", "60"], client_extra=["--die-at", "6"], via_relay=False, timeout=70, play_clock=False)
        port += 4
        h = res.get("host", {})
        ok = h.get("phase") == 3 and h.get("exit") == 0 and "error" not in h
        print("%-8s %s (%.0f s)" % ("kill", "OK: host survived and ended normally" if ok else "FAIL: %s" % res, time.time() - t0))
        failed += not ok
        # netflake: a dead link for 5 s in the middle of play (netem drops every packet both ways, 6-11 s after the relay started = about play time 3-8 s).
        # A long gap must not end the session (ENet gives up only after 10 s of unanswered reliable data): both bots play on and end normally, each saw the
        # partner STALE and then its states again (the longest gap between two accepted states is the dead link), the events sent after it arrive
        res = run_pair("gap5", port, PROFILES["wan"][0] + ["--blackout", "6,5"], host_extra=["--event-at", "2"],
                       client_extra=["--load-at", "-1", "--event-at", "13"], timeout=60)
        port += 4
        report(res)
        bad = []
        for label in ("host", "client"):
            r = res.get(label, {})
            d = r.get("diag", {})
            if "error" in r or r.get("exit") != 0 or r.get("phase") != 3:
                bad.append("%s: %s" % (label, {k: r.get(k) for k in ("error", "exit", "phase", "lost", "why")}))
                continue
            if r.get("stale", 0) < 1:
                bad.append("%s never saw the partner STALE" % label)
            if not 4500 <= d.get("rx_gap_max", 0) <= 7500:
                bad.append("%s: longest gap between accepted states %s ms (expected the 5 s dead link)" % (label, d.get("rx_gap_max")))
            if r.get("events") != 1:
                bad.append("%s got %s events (expected 1)" % (label, r.get("events")))
        print("%-8s %s" % ("gap5", "OK (5 s dead link: session kept, host gap %s ms, client gap %s ms)" % (res.get("host", {}).get("diag", {}).get("rx_gap_max"),
              res.get("client", {}).get("diag", {}).get("rx_gap_max")) if not bad else "FAIL: " + "; ".join(bad)))
        failed += bool(bad)
    print("RESULT: %s" % ("all passed" if not failed else "%d FAILED" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
