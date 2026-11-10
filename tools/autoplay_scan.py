#!/usr/bin/env python3
"""
Scans one AI parameter (or several parameters tied to the same value) in autoplay mode and reports the faction yield
for every value.

Every run starts the game from the same save with  -autoplay -set name=value  and reads the "yield round" lines
from autoplay.log. Each of them holds the mean over the AI factions of the nutrient surplus, the gross mineral intake (before unit support) and
economy + labs. The result of a run is the mean of the last --tail rounds of
    score = wn * nutrient + wm * mineral + we * energy
with the reference weights given by --weights (default 1.0,1.5,0.5, the thinker.ini defaults). The weights are the
yardstick and are independent of the scanned parameters. The raw means are saved in the output CSV, so the same runs
can be scored again with other weights (--rescore) without running the game.

Runs are not repeatable (the game uses random numbers), so every value is run --runs times and
the mean, the standard deviation and the standard error of the mean are reported.

Examples (from the repository root):
  python tools/autoplay_scan.py --baseline --runs 5 --turns 30
  python tools/autoplay_scan.py --param ai_resource_score_mineral+ai_terraforming_mineralWeight --values 0.75,1.5,3.0 --runs 4 --turns 30
  python tools/autoplay_scan.py --rescore patch/autoplay_scan.csv --weights 1,1,1
--param accepts several names joined with + ; they all get the same value. Extra settings for every run: --set name=value.
"""
import argparse
import csv
import math
import os
import re
import statistics
import subprocess
import sys
import time

YIELD_LINE = re.compile(r"yield round, turn (\d+): nutrient ([-\d.]+) mineral ([-\d.]+) energy ([-\d.]+)")


def read_rounds(log_path):
    """Returns the list of (turn, nutrient, mineral, energy) and whether the run reached its turn limit."""
    rounds = []
    finished = False
    with open(log_path, "r", errors="replace") as log:
        for line in log:
            match = YIELD_LINE.search(line)
            if match:
                rounds.append((int(match.group(1)), float(match.group(2)), float(match.group(3)), float(match.group(4))))
            if "autoplay finished (turn limit)" in line:
                finished = True
    return rounds, finished


def run_game(command, patch_dir, log_path, timeout, stall):
    """Runs the game and kills it when it exceeds the timeout or autoplay.log stops growing for `stall` seconds
    (a window that waits for a click). Returns the reason it was killed, or "" after a normal exit."""
    process = subprocess.Popen(command, cwd=patch_dir)
    started = time.time()
    last_change = started
    last_size = -1
    while process.poll() is None:
        time.sleep(2)
        size = os.path.getsize(log_path) if os.path.exists(log_path) else -1
        if size != last_size:
            last_size = size
            last_change = time.time()
        reason = ""
        if time.time() - started > timeout:
            reason = "timeout"
        elif time.time() - last_change > stall:
            reason = "stalled, no log output for %d s" % stall
        if reason:
            process.kill()
            process.wait()
            return reason
    return ""


def run_once(patch_dir, exe, save, turns, settings, timeout, stall):
    log_path = os.path.join(patch_dir, "autoplay.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    command = [exe, "-autoplay", "-windowed", "-turns", str(turns)]
    for setting in settings:
        command += ["-set", setting]
    command.append(save)
    started = time.time()
    # the game exits with code 1 after a normal run, so the exit code is not checked
    killed = run_game(command, patch_dir, log_path, timeout, stall)
    elapsed = time.time() - started
    if killed:
        return None, elapsed, killed
    if not os.path.exists(log_path):
        return None, elapsed, "no autoplay.log"
    rounds, finished = read_rounds(log_path)
    if not finished:
        return None, elapsed, "did not reach the turn limit"
    if not rounds:
        return None, elapsed, "no yield lines in autoplay.log"
    return rounds, elapsed, ""


def score_of(weights, nutrient, mineral, energy):
    return weights[0] * nutrient + weights[1] * mineral + weights[2] * energy


def print_table(candidates, scores):
    print()
    print("%-60s %5s %10s %10s %10s" % ("candidate", "runs", "mean", "stdev", "stderr"))
    for name in candidates:
        values = scores.get(name, [])
        if not values:
            print("%-60s %5d" % (name, 0))
            continue
        stdev = statistics.stdev(values) if len(values) > 1 else float("nan")
        stderr = stdev / math.sqrt(len(values)) if len(values) > 1 else float("nan")
        print("%-60s %5d %10.3f %10.3f %10.3f" % (name, len(values), statistics.mean(values), stdev, stderr))


def rescore(path, weights):
    scores = {}
    order = []
    with open(path, "r", newline="") as source:
        for row in csv.DictReader(source):
            name = row["candidate"]
            if name not in scores:
                scores[name] = []
                order.append(name)
            scores[name].append(score_of(weights, float(row["nutrient"]), float(row["mineral"]), float(row["energy"])))
    print("weights nutrient/mineral/energy = %s" % "/".join("%g" % w for w in weights))
    print_table(order, scores)


def main():
    repository = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--patch-dir", default=os.path.join(repository, "patch"), help="game folder (default: <repository>/patch)")
    parser.add_argument("--exe", default="terranx_mod.exe", help="game executable inside the game folder")
    parser.add_argument("--save", default=os.path.join("saves", "game-begin.SAV"), help="save to start from, relative to the game folder")
    parser.add_argument("--turns", type=int, default=30, help="full turns to play in every run")
    parser.add_argument("--tail", type=int, default=5, help="average the last N rounds to get the result of a run")
    parser.add_argument("--runs", type=int, default=4, help="runs per value")
    parser.add_argument("--weights", default="1.0,1.5,0.5", help="reference weights nutrient,mineral,energy of the yield score")
    parser.add_argument("--baseline", action="store_true", help="run only with the current settings to measure the run-to-run noise")
    parser.add_argument("--param", help="parameter name from thinker.ini to scan; join several names with + to tie them to one value")
    parser.add_argument("--values", help="comma separated values to scan")
    parser.add_argument("--set", action="append", default=[], metavar="NAME=VALUE", help="extra setting for every run (repeatable)")
    parser.add_argument("--rescore", metavar="CSV", help="do not run the game, score the runs saved in this CSV with --weights")
    parser.add_argument("--timeout", type=int, default=3600, help="seconds allowed for one run")
    parser.add_argument("--stall", type=int, default=300, help="kill a run whose autoplay.log did not grow for this many seconds")
    parser.add_argument("--out", default="autoplay_scan.csv", help="CSV with the result of every run, written to the game folder")
    args = parser.parse_args()

    weights = [float(part) for part in args.weights.split(",")]
    if len(weights) != 3:
        parser.error("--weights needs three numbers: nutrient,mineral,energy")

    if args.rescore:
        rescore(args.rescore, weights)
        return 0

    if args.baseline:
        candidates = [("baseline", [])]
    elif args.param and args.values:
        names = args.param.split("+")
        candidates = [("%s=%s" % (args.param, value), ["%s=%s" % (name, value) for name in names]) for value in args.values.split(",")]
    else:
        parser.error("give --baseline, or --param with --values, or --rescore")

    exe = os.path.join(args.patch_dir, args.exe)
    scores = {}
    out_path = os.path.join(args.patch_dir, args.out)
    with open(out_path, "w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["candidate", "run", "nutrient", "mineral", "energy", "score", "rounds", "seconds"])
        # interleave the candidates so slow drifts of the machine do not favor one value
        for run in range(1, args.runs + 1):
            for name, settings in candidates:
                rounds, seconds, problem = run_once(args.patch_dir, exe, args.save, args.turns, settings + args.set, args.timeout, args.stall)
                if rounds is None:
                    print("%-60s run %d FAILED (%s) after %.0f s" % (name, run, problem, seconds), flush=True)
                    continue
                tail = rounds[-args.tail:]
                nutrient = sum(item[1] for item in tail) / len(tail)
                mineral = sum(item[2] for item in tail) / len(tail)
                energy = sum(item[3] for item in tail) / len(tail)
                score = score_of(weights, nutrient, mineral, energy)
                scores.setdefault(name, []).append(score)
                writer.writerow([name, run, "%.4f" % nutrient, "%.4f" % mineral, "%.4f" % energy, "%.4f" % score, len(rounds), "%.0f" % seconds])
                out.flush()
                print("%-60s run %d score %.3f (n %.2f m %.2f e %.2f, %d rounds, %.0f s)" %
                      (name, run, score, nutrient, mineral, energy, len(rounds), seconds), flush=True)

    print_table([name for name, _ in candidates], scores)
    print("results per run: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
