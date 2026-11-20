#!/usr/bin/env python3
"""Turn the original's keys_rec.txt (instrument.py --record) into a compare.sh key file.

A change that the begin step sees at trace counter c is replayed by keyboard_key_press/release
at counter c-1: delivered at the next poll, it is first seen at c, as in the recording.
On the first frame of a new room (follows_transition = 1), c-1 would be cleared by the room
change, so the change goes to c instead and is seen one frame late; those are counted.

usage: rec2keys.py keys_rec.txt OUT.keys
"""
import sys

events, late = [], 0
for line in open(sys.argv[1], encoding='latin-1'):
    p = line.split()
    if len(p) != 4:
        continue
    c, vk, down, tr = int(p[0]), int(p[1]), int(float(p[2])) != 0, int(float(p[3])) != 0
    if tr:
        late += 1
    events.append((c if tr else max(0, c - 1), vk, down))

out = []
for i, (f, vk, down) in enumerate(events):
    if not down:
        continue
    end = next((g for g, v, d in events[i + 1:] if v == vk and not d), None)
    out.append(f'{f}:{vk}:{max(1, end - f) if end is not None else 1}')
open(sys.argv[2], 'w').write(','.join(out) + '\n')
print(f'{len(out)} key presses written to {sys.argv[2]}; '
      f'{late} changes on the first frame of a room replay one frame late')
