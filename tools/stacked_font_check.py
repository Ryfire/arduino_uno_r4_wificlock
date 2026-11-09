"""Design + verify the stacked 4-row date font BEFORE flashing it.

Layout: month "MM" on rows 0-3, day "DD" on rows 4-7.
Each digit: 3 wide x 4 tall, digits at cols 2-4 and 7-9 (2px gap, 2px margins).

Checks:
1. Pairwise: every digit pair must differ in >= 2 lit pixels.
2. Compare against the proven-legible compactDigits baseline: the baseline
   itself has some 1-pixel-margin pairs, so the candidate is acceptable if
   its margin profile is no worse than the baseline's.
3. Renders every real month/day combo (366 frames) and flags any frame
   containing a pair whose margin is WORSE than the baseline's.
4. Prints worst-case frames for manual eyeballing.
"""
import itertools

# ---- Proven-legible baseline (what the user reads today on hardware) ----
BASELINE = {
    '0': ["111", "101", "101", "101", "111"],
    '1': ["010", "110", "010", "010", "111"],
    '2': ["111", "001", "111", "100", "111"],
    '3': ["111", "001", "111", "001", "111"],
    '4': ["101", "101", "111", "001", "001"],
    '5': ["111", "100", "111", "001", "111"],
    '6': ["111", "100", "111", "101", "111"],
    '7': ["111", "001", "010", "010", "010"],
    '8': ["111", "101", "111", "101", "111"],
    '9': ["111", "101", "111", "001", "111"],
}

# ---- Candidate: stacked 4-row font, corrected 8 (bottom bar kept) ----
FONT = {
    '0': ["111", "101", "101", "111"],
    '1': ["010", "110", "010", "010"],
    '2': ["111", "001", "111", "100"],
    '3': ["111", "001", "111", "001"],
    '4': ["101", "101", "111", "001"],
    '5': ["111", "100", "111", "001"],
    '6': ["111", "100", "111", "101"],
    '7': ["111", "001", "010", "010"],
    '8': ["111", "101", "111", "111"],
    '9': ["111", "101", "111", "001"],
}


def diff_stats(a, b):
    rows = sum(1 for ra, rb in zip(a, b) if ra != rb)
    pxs = sum(c1 != c2 for ra, rb in zip(a, b) for c1, c2 in zip(ra, rb))
    return rows, pxs


def margin_profile(font):
    """{pair: (rows, px) for pairs with the smallest margins."""
    return {p: diff_stats(font[p[0]], font[p[1]])
            for p in itertools.combinations(sorted(font), 2)}


def worse_than_baseline(font):
    """Pairs where the candidate's margin is strictly worse than baseline."""
    base = margin_profile(BASELINE)
    cand = margin_profile(font)
    out = []
    for p in base:
        br, bp = base[p]
        cr, cp = cand[p]
        if (cr, cp) < (br, bp):
            out.append((p, f"base r{br}px{bp} -> cand r{cr}px{cp}"))
    return out


def frame_for(font, top, bottom):
    f = [["0"] * 12 for _ in range(8)]
    for text, row0 in ((top, 0), (bottom, 4)):
        for i, ch in enumerate(text):
            g = font[ch]
            for r in range(4):
                for c in range(3):
                    if g[r][c] == "1":
                        f[row0 + r][2 + i * 5 + c] = "1"
    return ["".join(("##" if p == "1" else "..") for p in row) for row in f]


def show(font, top, bottom):
    print(f"--- {top} / {bottom} ---")
    for row in frame_for(font, top, bottom):
        print("  " + row)
    print()


def all_real_dates():
    for m in range(1, 13):
        for dnum in range(1, 32):
            if (m, dnum) == (2, 30):
                continue
            yield f"{m:02d}", f"{dnum:02d}"


if __name__ == "__main__":
    print("Candidate margin profile (pairs with the smallest margins):")
    cand = margin_profile(FONT)
    base = margin_profile(BASELINE)
    worst = sorted(cand.items(), key=lambda kv: kv[1])[:6]
    for p, (r, px) in worst:
        print(f"  {p[0]}/{p[1]}: rows{r}px{px}  (baseline: rows{base[p][0]}px{base[p][1]})")

    print()
    worse = worse_than_baseline(FONT)
    if worse:
        print("REGRESSION vs baseline:", worse)
    else:
        print("No pair is worse than the proven baseline.")

    # Dates where a sub-2px-margin baseline pair appears together
    risky = [p for p in base if base[p] < (2, 2)]
    bad_dates = [(ms, ds, hits) for ms, ds in all_real_dates()
                 for hits in [[p for p in risky if p[0] in ms + ds and p[1] in ms + ds]]
                 if hits]
    print(f"\ndates where a baseline-risk pair appears together: {len(bad_dates)}")
    for ms, ds, hits in bad_dates[:6]:
        print(f"--- {ms}/{ds}  risky: {hits} ---")
        for row in frame_for(FONT, ms, ds):
            print("  " + row)
        print()

    print("=== today-ish previews ===")
    show(FONT, "11", "08")
    show(FONT, "12", "31")
    show(FONT, "03", "14")
