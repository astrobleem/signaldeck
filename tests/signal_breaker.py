#!/usr/bin/env python3
"""
SIGNAL BREAKER / シグナル・ブレイカー  (v2 - whole-chip arrangement)
Original stage theme for Signal Deck. Target: YM2203 (OPN) @ 3.9936 MHz (PC-9801-26K).
Writes signal_breaker.vgm and signal_breaker.s98.

150 BPM. 16th note = 100 ms = 10 frames of 10 ms.
Structure: Intro(4) | A(8) B(8) C(8) -> loop to A

Chip features used
  FM1     lead, 4-op (alg 4), software vibrato
  FM2     plucked bass (Intro/A/B), swelling brass counter-line (C)
  FM3     CH3 SPECIAL MODE (reg 27h = 40h): two independent 2-op voices
            X = slot1->slot2  freq A9h/ADh + AAh/AEh   key bits 30h
            Y = slot3->slot4  freq A8h/ACh + A2h/A6h   key bits C0h
          Intro/A: two-note pad with SSG-EG (90h) on the modulators -> shimmering timbre
          B:       X = twin-lead harmony a diatonic 3rd below, Y = held chord root
          C:       two-note off-beat chord stabs
  SSG A   16th chord arpeggio
  SSG B   lead echo (Intro/A/B); HARDWARE-ENVELOPE SAWTOOTH BUZZ BASS (C)
  SSG C   drums: tone-sweep kick, tone+noise snare, noise hat
"""
import math, struct

CLOCK = 3993600
FRAME_SAMPLES = 441
FPS = 10

NOTE = {'c':0,'c#':1,'d':2,'d#':3,'e':4,'f':5,'f#':6,'g':7,'g#':8,'a':9,'a#':10,'b':11}
def midi(n): return 12 * (int(n[-1]) + 1) + NOTE[n[:-1]]
def hz(m):   return 440.0 * 2 ** ((m - 69) / 12)
def seq(s):
    t = s.split()
    return [(None if t[i] == 'r' else midi(t[i]), int(t[i + 1])) for i in range(0, len(t), 2)]

def fm_freq(f):
    for blk in range(8):
        fn = round(f * 72 * 2 ** (21 - blk) / CLOCK)
        if fn < 2048:
            return blk, fn
    return 7, 2047
def ssg_period(f): return max(1, min(4095, round(CLOCK / (32 * f))))
def env_period(f): return max(1, min(65535, round(CLOCK / (512 * f))))   # sawtooth = one env cycle

# ------------------------------------------------------------------ song
SECTIONS = [('intro', ['Am', 'Am', 'Am', 'E']),
            ('A', ['Am', 'Am', 'F', 'G', 'Am', 'Am', 'F', 'E']),
            ('B', ['Dm', 'E', 'Am', 'G', 'F', 'G', 'E', 'E']),
            ('C', ['F', 'G', 'Em', 'Am', 'Dm', 'G', 'C', 'E'])]
LOOP_BAR = 4
CHORD = {'Am': ['a', 'c', 'e'], 'F': ['f', 'a', 'c'], 'G': ['g', 'b', 'd'], 'E': ['e', 'g#', 'b'],
         'Dm': ['d', 'f', 'a'], 'Em': ['e', 'g', 'b'], 'C': ['c', 'e', 'g']}

LEAD = seq(
    # intro pickup
    "r 16 r 16 r 16 r 8 e5 2 g#5 2 b5 2 e6 2 "
    # A
    "a5 4 e5 2 a5 2 b5 4 c6 4 "
    "b5 2 c6 2 b5 2 a5 10 "
    "c6 4 a5 2 c6 2 f6 4 e6 4 "
    "d6 6 c6 2 b5 4 g5 4 "
    "a5 4 e5 2 a5 2 b5 4 c6 4 "
    "e6 4 d6 2 c6 2 d6 2 e6 6 "
    "f6 4 e6 2 d6 2 c6 4 a5 4 "
    "b5 4 g#5 2 e5 2 g#5 4 b5 4 "
    # B
    "f6 6 e6 2 d6 4 a5 4 "
    "g#5 6 a5 2 b5 4 e6 4 "
    "c6 2 b5 2 a5 2 b5 2 c6 2 d6 2 e6 4 "
    "d6 6 b5 2 g5 4 d6 4 "
    "c6 6 a5 2 f5 4 c6 4 "
    "b5 2 c6 2 d6 2 e6 2 f6 2 e6 2 d6 2 b5 2 "
    "e6 8 d6 2 c6 2 b5 2 g#5 2 "
    "b5 8 g#5 2 a5 2 b5 2 c6 2 "
    # C
    "c6 6 a5 2 c6 4 f6 4 "
    "g6 6 f6 2 e6 4 d6 4 "
    "e6 8 b5 4 g5 4 "
    "a5 6 b5 2 c6 4 e6 4 "
    "f6 6 e6 2 d6 4 a5 4 "
    "b5 6 c6 2 d6 4 g6 4 "
    "e6 6 f6 2 g6 4 e6 4 "
    "d6 4 b5 2 g#5 2 e5 4 g#5 2 b5 2")

BASS_PAT = {'intro': "L...L...L...L...", 'A': "LLHLLHLLHLLHLHLH", 'B': "LHLHLHLHLHLHLHLH"}
BUZZ_PAT = "L.L.H.L.L.H.L.H."
DRUM_PAT = {
    'intro': "K...H...K...H.H.", 'introF': "K...S...S.S.SSSS",
    'A': "K.H.S.HK.HK.S.HH", 'B': "K.HKS.HKK.HKS.HH",
    'half': "K.H.H.H.S.H.H.HK", 'halfF': "K.H.H.H.S.H.SSSS",
    'fill': "K.H.S.HK.HS.SSSS", 'fill2': "K.H.S.SKS.SSSSSS",
}

# ------------------------------------------------------------------ patches
# op = (DT, MUL, TL, KS, AR, DR, SR, SL, RR[, SSG-EG])   order op1..op4
PATCH = {
    'lead':  dict(alg=4, fb=5, ops=[(3, 1, 28, 0, 31, 6, 0, 3, 6), (3, 1, 6, 1, 31, 4, 1, 2, 7),
                                    (7, 2, 32, 0, 31, 8, 0, 4, 6), (7, 1, 8, 1, 31, 4, 1, 2, 7)]),
    'bass':  dict(alg=4, fb=5, ops=[(0, 1, 24, 0, 31, 12, 2, 6, 10), (0, 1, 8, 1, 31, 6, 3, 3, 10),
                                    (0, 2, 34, 0, 31, 14, 0, 8, 10), (0, 0, 12, 1, 31, 4, 2, 2, 10)]),
    'brass': dict(alg=4, fb=6, ops=[(0, 1, 26, 0, 22, 4, 1, 2, 6), (0, 1, 14, 1, 24, 3, 1, 1, 7),
                                    (3, 1, 30, 0, 22, 4, 1, 2, 6), (7, 1, 16, 1, 24, 3, 1, 1, 7)]),
    # FM3 special-mode patches: alg 4 = two independent 2-op voices (X: op1->op2, Y: op3->op4)
    'pad3':  dict(alg=4, fb=2, ops=[(0, 1, 30, 0, 31, 7, 7, 15, 5, 0x0E), (3, 1, 20, 0, 16, 2, 0, 1, 6),
                                    (0, 2, 32, 0, 31, 6, 6, 15, 5, 0x0E), (7, 1, 20, 0, 16, 2, 0, 1, 6)]),
    'twin':  dict(alg=4, fb=4, ops=[(3, 1, 30, 0, 31, 6, 0, 3, 6), (3, 1, 14, 1, 31, 4, 1, 2, 7),
                                    (0, 1, 38, 0, 31, 4, 0, 2, 6), (0, 1, 22, 1, 24, 2, 0, 1, 6)]),
    'stab':  dict(alg=4, fb=5, ops=[(0, 1, 26, 0, 31, 12, 4, 6, 10), (0, 1, 14, 1, 31, 8, 4, 4, 12),
                                    (0, 1, 26, 0, 31, 12, 4, 6, 10), (0, 1, 14, 1, 31, 8, 4, 4, 12)]),
}
OP_OFF = [0, 8, 4, 12]

def patch_writes(ch, p):
    w = [(0xB0 + ch, (p['fb'] << 3) | p['alg'])]
    for i, op in enumerate(p['ops']):
        dt, mul, tl, ks, ar, dr, sr, sl, rr = op[:9]
        eg = op[9] if len(op) > 9 else 0
        o = OP_OFF[i] + ch
        w += [(0x30 + o, (dt << 4) | mul), (0x40 + o, tl), (0x50 + o, (ks << 6) | ar),
              (0x60 + o, dr), (0x70 + o, sr), (0x80 + o, (sl << 4) | rr), (0x90 + o, eg)]
    return w

DRUM = {
    'K': (15, [(160, True, 15), (110, False, 14), (85, False, 13), (68, False, 11),
               (55, False, 8), (48, False, 5), (44, False, 2)]),
    'S': (10, [(240, True, 14), (200, True, 13), (None, True, 12), (None, True, 10),
               (None, True, 8), (None, True, 6), (None, True, 4), (None, True, 2)]),
    'H': (1,  [(None, True, 9), (None, True, 5)]),
}

# ------------------------------------------------------------------ arrangement
def chord_stack(name, octv=4):
    r = midi(CHORD[name][0] + str(octv)); out = [r]
    for n in CHORD[name][1:]:
        m = midi(n + str(octv))
        while m <= out[-1]: m += 12
        out.append(m)
    return out + [r + 12]

def harmony_third_below(m, chord):
    pcs = sorted([9, 11, 0, 2, 4, 5, 8 if chord == 'E' else 7])
    if m % 12 not in pcs: return m - 3
    tones = [o * 12 + pc for o in range(11) for pc in pcs]
    return tones[tones.index(m) - 2]

def build():
    bars = [(sec, ch, i) for sec, chs in SECTIONS for i, ch in enumerate(chs)]
    T = {k: [] for k in ('lead', 'fm2', 'x', 'y', 'arp', 'ssgb', 'drum')}
    fm2_patch, fm3_patch, sec_of_step = {}, {}, {}
    s = 0
    for m, l in LEAD:
        if m is not None:
            T['lead'].append(dict(s=s, l=l, m=m, gate=l * FPS - 1, vib=True))
        s += l
    for b, (sec, ch, i) in enumerate(bars):
        base = b * 16
        for k in range(16): sec_of_step[base + k] = sec
        st4 = chord_stack(ch, 4)
        r1, r2 = midi(CHORD[ch][0] + '1'), midi(CHORD[ch][0] + '2')
        # ---- FM2
        if sec == 'C':
            if i == 0: fm2_patch[base] = 'brass'
            T['fm2'] += [dict(s=base, l=8, m=st4[2] - 12, gate=76, vib=False),
                         dict(s=base + 8, l=8, m=st4[1] - 12, gate=76, vib=False)]
        else:
            if b in (0, LOOP_BAR): fm2_patch[base] = 'bass'
            bp = BASS_PAT['A'] if (sec == 'intro' and i >= 2) else BASS_PAT[sec]
            T['fm2'] += [dict(s=base + k, l=1, m=r2 + (12 if c == 'H' else 0), gate=7, vib=False)
                         for k, c in enumerate(bp) if c != '.']
        # ---- FM3 (special mode, voices X and Y)
        if sec in ('intro', 'A'):
            if i == 0: fm3_patch[base] = 'pad3'
            T['x'].append(dict(s=base, l=16, m=st4[1], gate=159, vib=False))
            T['y'].append(dict(s=base, l=16, m=st4[2], gate=159, vib=False))
        elif sec == 'B':
            if i == 0: fm3_patch[base] = 'twin'
            T['y'].append(dict(s=base, l=16, m=st4[0] - 12, gate=159, vib=False))
        else:
            if i == 0: fm3_patch[base] = 'stab'
            for k in (2, 6, 10, 14):
                T['x'].append(dict(s=base + k, l=2, m=st4[1], gate=5, vib=False))
                T['y'].append(dict(s=base + k, l=2, m=st4[2], gate=5, vib=False))
        # ---- SSG A arpeggio
        order = {'B': [0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1, 0, 1, 2, 3],
                 'C': [0, 2, 1, 3] * 4}.get(sec, [0, 1, 2, 3] * 4)
        vol = {'intro': 7, 'A': 9, 'B': 10, 'C': 9}[sec]
        T['arp'] += [dict(s=base + k, l=1, m=st4[o], v=vol, gate=9) for k, o in enumerate(order)]
        # ---- SSG B buzz bass in C
        if sec == 'C':
            T['ssgb'] += [dict(s=base + k, l=2, m=(r1 if c == 'L' else r2), mode='buzz', gate=14)
                          for k, c in enumerate(BUZZ_PAT) if c != '.']
        # ---- drums
        if sec == 'intro':
            dp = DRUM_PAT['intro'] if i < 2 else (DRUM_PAT['A'] if i == 2 else DRUM_PAT['introF'])
        elif sec == 'C':
            dp = DRUM_PAT['half'] if i < 3 else DRUM_PAT['halfF'] if i == 3 else \
                 DRUM_PAT['fill2'] if i == 7 else DRUM_PAT['A']
        elif i in (3, 7):
            dp = DRUM_PAT['fill']
        else:
            dp = DRUM_PAT[sec]
        T['drum'] += [dict(s=base + k, kind=c) for k, c in enumerate(dp) if c != '.']
    # harmony in B, echo outside C
    for e in T['lead']:
        bar = e['s'] // 16
        if bars[bar][0] == 'B':
            T['x'].append(dict(s=e['s'], l=e['l'], m=harmony_third_below(e['m'], bars[bar][1]),
                               gate=e['l'] * FPS - 1, vib=True))
        es = e['s'] + 3
        if sec_of_step.get(es) != 'C' and es < len(bars) * 16:
            T['ssgb'].append(dict(s=es, l=e['l'], m=e['m'], mode='echo', gate=e['l'] * FPS - 2))
    return T, fm2_patch, fm3_patch, len(bars) * 16

# ------------------------------------------------------------------ renderer
class Chip:
    def __init__(self): self.shadow, self.out = {}, []
    def w(self, r, v, force=False):
        if not force and r != 0x28 and self.shadow.get(r) == v: return
        self.shadow[r] = v; self.out.append((r, v))

def vib(freq, age):
    if age > 15:
        freq *= 2 ** (20 * math.sin(2 * math.pi * 6 * (age - 15) / 100) / 1200)
    return freq

class Voice:
    """Tracks one monophonic note stream; returns the sounding note per frame."""
    def __init__(self, events):
        self.ev = {e['s']: e for e in events}; self.cur = None
    def tick(self, f):
        step, sub = divmod(f, FPS)
        if sub == 0 and step in self.ev:
            self.cur = dict(self.ev[step], start=f)
        if self.cur and f - self.cur['start'] >= self.cur['gate']:
            self.cur = None
        return self.cur

def render():
    T, fm2_patch, fm3_patch, total_steps = build()
    loop_step = LOOP_BAR * 16
    chip, frames = Chip(), []
    lead, fm2, vx, vy = Voice(T['lead']), Voice(T['fm2']), Voice(T['x']), Voice(T['y'])
    arp, ssgb = Voice(T['arp']), Voice(T['ssgb'])
    drums = {e['s']: e['kind'] for e in T['drum']}
    drum = None
    key = {0: None, 1: None, 2: None}          # last written key-on mask per channel
    started = {0: None, 1: None, 'b': None}
    loop_idx = None

    def init(force_patches):
        chip.w(0x2D, 0x00, True)                # prescaler FM 1/6, SSG 1/4
        chip.w(0x27, 0x40, True)                # CH3 special mode, timers off
        for c in range(3): chip.w(0x28, c); key[c] = 0
        for r in (0x08, 0x09, 0x0A): chip.w(r, 0, True)
        for ch, name in force_patches:
            for r, v in patch_writes(ch, PATCH[name]): chip.w(r, v, True)

    init([(0, 'lead'), (1, 'bass'), (2, 'pad3')])
    for f in range(total_steps * FPS):
        step, sub = divmod(f, FPS)
        if f == loop_step * FPS:
            loop_idx = len(frames)
            chip.shadow.clear()
            init([(0, 'lead'), (1, 'bass'), (2, 'pad3')])
            for k in started: started[k] = None
        # patch changes at section boundaries (key off channel first)
        if sub == 0:
            if step in fm2_patch and not (step == loop_step):
                chip.w(0x28, 1); key[1] = 0
                for r, v in patch_writes(1, PATCH[fm2_patch[step]]): chip.w(r, v, True)
            if step in fm3_patch and step not in (0, loop_step):
                chip.w(0x28, 2); key[2] = 0
                for r, v in patch_writes(2, PATCH[fm3_patch[step]]): chip.w(r, v, True)

        # ---- FM1 / FM2 (normal 4-op channels)
        for ch, v in ((0, lead), (1, fm2)):
            n = v.tick(f)
            mask = 0
            if n:
                blk, fn = fm_freq(vib(hz(n['m']), f - n['start']) if n['vib'] else hz(n['m']))
                chip.w(0xA4 + ch, (blk << 3) | (fn >> 8)); chip.w(0xA0 + ch, fn & 0xFF)
                mask = 0xF0
                if started[ch] != n['start'] and key[ch]:      # retrigger
                    chip.w(0x28, ch); key[ch] = 0
                started[ch] = n['start']
            if key[ch] != mask:
                chip.w(0x28, mask | ch); key[ch] = mask

        # ---- FM3 special mode: X (slots 1,2) and Y (slots 3,4)
        # every FM3 note's gate ends at least 1 frame early, so a changed mask = clean retrigger
        mask = 0
        for v, regs, bits in ((vx, ((0xAD, 0xA9), (0xAE, 0xAA)), 0x30),     # X: slot1, slot2
                              (vy, ((0xAC, 0xA8), (0xA6, 0xA2)), 0xC0)):    # Y: slot3, slot4
            n = v.tick(f)
            if n:
                freq = vib(hz(n['m']), f - n['start']) if n['vib'] else hz(n['m'])
                blk, fn = fm_freq(freq)
                for hi, lo in regs:
                    chip.w(hi, (blk << 3) | (fn >> 8)); chip.w(lo, fn & 0xFF)
                mask |= bits
        if key[2] != mask:
            chip.w(0x28, mask | 2); key[2] = mask

        # ---- SSG A arpeggio
        n = arp.tick(f); vol = 0
        if n:
            tp = ssg_period(hz(n['m'])); chip.w(0x00, tp & 0xFF); chip.w(0x01, tp >> 8)
            vol = max(5, n['v'] - (f - n['start']) // 2)
        chip.w(0x08, vol)

        # ---- SSG B echo / hardware-envelope buzz
        n = ssgb.tick(f); vol, buzz = 0, False
        if n:
            age = f - n['start']
            if n['mode'] == 'echo':
                tp = ssg_period(hz(n['m'])); chip.w(0x02, tp & 0xFF); chip.w(0x03, tp >> 8)
                vol = max(4, 8 - age // 6)
            else:
                buzz = True
                ep = env_period(hz(n['m']))
                chip.w(0x0B, ep & 0xFF); chip.w(0x0C, ep >> 8)
                if started['b'] != n['start']:
                    chip.w(0x0D, 0x08, True)        # repeating saw \|\|\ (restart each note)
                    started['b'] = n['start']
                vol = 0x10                           # volume follows envelope
        chip.w(0x09, vol)

        # ---- SSG C drums + mixer
        if sub == 0 and step in drums:
            drum = dict(kind=drums[step], start=f)
        mixer = 0x80 | 0x08 | 0x10 | (0x02 if buzz else 0)
        dvol = 0
        if drum:
            period, shape = DRUM[drum['kind']]
            age = f - drum['start']
            if age < len(shape):
                tone, noise, dvol = shape[age]
                if tone:
                    tp = ssg_period(tone); chip.w(0x04, tp & 0xFF); chip.w(0x05, tp >> 8)
                else: mixer |= 0x04
                if noise: chip.w(0x06, period)
                else: mixer |= 0x20
            else:
                drum = None
        if not drum: mixer |= 0x24
        chip.w(0x07, mixer); chip.w(0x0A, dvol)
        frames.append(chip.out); chip.out = []
    return frames, loop_idx

# ------------------------------------------------------------------ writers
TAGS = dict(title_en="Signal Breaker", title_jp="シグナル・ブレイカー",
            game_en="Signal Deck", game_jp="シグナルデッキ",
            system_en="NEC PC-9801", system_jp="NEC PC-9801",
            author_en="Signal Deck Project", author_jp="Signal Deck Project", date="2026",
            notes="Original composition. YM2203 (OPN) @ 3.9936 MHz, 150 BPM. "
                  "FM3 special mode, SSG-EG, SSG hardware envelope.")

def write_vgm(frames, loop_idx, path):
    data, wait, loop_off = bytearray(), 0, None
    def flush():
        nonlocal wait
        while wait > 0:
            n = min(wait, 65535); data.extend(b'\x61' + struct.pack('<H', n)); wait -= n
    for i, fr in enumerate(frames):
        if i == loop_idx: flush(); loop_off = len(data)
        if fr:
            flush()
            for r, v in fr: data.extend(bytes((0x55, r, v)))
        wait += FRAME_SAMPLES
    flush(); data.append(0x66)
    total = len(frames) * FRAME_SAMPLES
    loop_samples = (len(frames) - loop_idx) * FRAME_SAMPLES
    gd3s = [TAGS['title_en'], TAGS['title_jp'], TAGS['game_en'], TAGS['game_jp'],
            TAGS['system_en'], TAGS['system_jp'], TAGS['author_en'], TAGS['author_jp'],
            TAGS['date'], "signal_breaker.py", TAGS['notes']]
    body = b''.join(s.encode('utf-16-le') + b'\0\0' for s in gd3s)
    gd3 = b'Gd3 ' + struct.pack('<II', 0x100, len(body)) + body
    HDR = 0x80; h = bytearray(HDR)
    h[0:4] = b'Vgm '
    struct.pack_into('<I', h, 0x08, 0x151)
    struct.pack_into('<I', h, 0x14, HDR + len(data) - 0x14)
    struct.pack_into('<I', h, 0x18, total)
    struct.pack_into('<I', h, 0x1C, HDR + loop_off - 0x1C)
    struct.pack_into('<I', h, 0x20, loop_samples)
    struct.pack_into('<I', h, 0x34, HDR - 0x34)
    struct.pack_into('<I', h, 0x44, CLOCK)
    h[0x7A] = 0x01
    struct.pack_into('<I', h, 0x04, HDR + len(data) + len(gd3) - 4)
    open(path, 'wb').write(bytes(h) + bytes(data) + gd3)
    return total, loop_samples

def write_s98(frames, loop_idx, path):
    HDR = 0x30; data, syncs, loop_off = bytearray(), 0, None
    def flush():
        nonlocal syncs
        if syncs == 1: data.append(0xFF)
        elif syncs > 1:
            n = syncs - 2; data.append(0xFE)
            while True:
                b = n & 0x7F; n >>= 7; data.append(b | (0x80 if n else 0))
                if not n: break
        syncs = 0
    for i, fr in enumerate(frames):
        if i == loop_idx: flush(); loop_off = len(data)
        if fr:
            flush()
            for r, v in fr: data.extend(bytes((0x00, r, v)))
        syncs += 1
    flush(); data.append(0xFD)
    tag = (b'[S98]' + b'\xef\xbb\xbf' + (
        f"title={TAGS['title_jp']} / {TAGS['title_en']}\nartist={TAGS['author_en']}\n"
        f"game={TAGS['game_en']}\nyear={TAGS['date']}\nsystem={TAGS['system_en']}\n"
        f"comment={TAGS['notes']}\n").encode('utf-8') + b'\0')
    h = bytearray(HDR); h[0:4] = b'S983'
    struct.pack_into('<IIIIIII', h, 0x04, 10, 1000, 0, HDR + len(data), HDR, HDR + loop_off, 1)
    struct.pack_into('<IIII', h, 0x20, 2, CLOCK, 0, 0)
    open(path, 'wb').write(bytes(h) + bytes(data) + tag)

if __name__ == '__main__':
    frames, loop_idx = render()
    total, loop = write_vgm(frames, loop_idx, 'signal_breaker.vgm')
    write_s98(frames, loop_idx, 'signal_breaker.s98')
    print(f"length {total/44100:.1f}s, loop {loop/44100:.1f}s, writes {sum(map(len, frames))}")
