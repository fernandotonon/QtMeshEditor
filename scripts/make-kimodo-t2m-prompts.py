#!/usr/bin/env python3
"""Write a varied prompt list for the Kimodo text-to-motion corpus (dev tool, not shipped).

Deterministic (fixed seed). Combines gaits x directions x styles, single actions (gestures, combat,
interactions, sports, dance, reactions, idles), body-side variants and two-step sequences, then drops
anything already in --exclude (earlier corpus versions) and samples down to --count.

    python3 scripts/make-kimodo-t2m-prompts.py --exclude ~/t2m_kimodo/prompts_v1.txt \
        --count 2000 --out ~/t2m_kimodo/prompts_v2.txt
"""

import argparse
import itertools
import os
import random

GAITS = ["walks", "runs", "jogs", "sneaks", "limps", "marches", "strolls", "sprints", "tiptoes",
         "shuffles", "staggers", "skips", "creeps", "struts", "trudges", "paces", "hobbles", "dashes"]
DIRECTIONS = ["forward", "backward", "to the left", "to the right", "diagonally forward to the left",
              "diagonally forward to the right", "in a circle", "in a zigzag", "in place",
              "forward and turns left", "forward and turns right", "forward and turns around",
              "up a slope", "down a slope", "up stairs", "down stairs"]
STYLES = ["slowly", "quickly", "confidently", "nervously", "happily", "sadly", "angrily", "tiredly",
          "cautiously", "proudly", "lazily", "energetically", "drunkenly", "stiffly", "casually",
          "like an old man", "like a zombie", "like a robot", "like a soldier", "like a model",
          "with a heavy backpack", "with the hands in the pockets", "while looking at a phone",
          "while carrying a box", "while holding a rifle", "with the arms crossed", "hunched over"]

SIDED = [  # {side} = left / right
    "waves with the {side} hand", "points forward with the {side} hand", "kicks with the {side} leg",
    "punches with the {side} fist", "raises the {side} arm", "hops on the {side} foot",
    "throws a ball with the {side} hand", "looks over the {side} shoulder", "steps to the {side}",
    "turns to the {side}", "stretches the {side} arm", "scratches the head with the {side} hand",
    "shields the eyes with the {side} hand", "rubs the {side} shoulder", "lifts the {side} knee",
    "touches the {side} ear", "salutes with the {side} hand", "reaches up with the {side} hand",
    "picks something up with the {side} hand", "swings a sword with the {side} hand",
    "holds the {side} arm in pain", "kneels on the {side} knee", "leans to the {side}",
    "side steps to the {side}", "dodges to the {side}", "rolls to the {side}",
]

ACTIONS = [
    # idle / standing
    "stands still", "is idle", "stands and breathes heavily", "stands and looks around",
    "shifts weight from foot to foot", "stands with the arms crossed", "stands with the hands on the hips",
    "stands at attention", "stands guard", "waits and checks a watch", "yawns and stretches",
    "stretches the arms above the head", "rolls the shoulders", "cracks the neck", "taps a foot impatiently",
    "stands nervously", "shivers from the cold", "stands and sways",
    # gestures / communication
    "waves hello", "waves goodbye", "beckons someone to come closer", "shrugs", "nods yes",
    "shakes the head no", "gives a thumbs up", "claps the hands", "applauds enthusiastically",
    "bows politely", "bows deeply", "blows a kiss", "points at something on the ground",
    "points up at the sky", "gestures while talking", "argues with someone", "facepalms",
    "covers the mouth in surprise", "puts a finger to the lips", "calls someone with a hand by the mouth",
    "signals to stop", "signals to come forward", "counts on the fingers", "salutes",
    "raises both arms in victory", "cheers", "pumps a fist", "dances in celebration",
    # emotions
    "cries", "laughs", "is scared and backs away", "is angry and stomps", "is sad and looks down",
    "is excited and jumps up and down", "is surprised and steps back", "panics", "sighs",
    "is disappointed", "is exhausted and bends over", "is dizzy", "is cold and rubs the arms",
    # combat
    "throws a punch", "throws a left hook", "throws an uppercut", "does a combo of punches",
    "does a front kick", "does a roundhouse kick", "does a side kick", "blocks a punch",
    "blocks with a shield", "slashes with a sword", "stabs with a sword", "swings a sword overhead",
    "swings an axe", "swings a hammer", "thrusts a spear", "draws a sword", "sheathes a sword",
    "draws a bow and shoots an arrow", "aims a rifle", "fires a pistol", "reloads a rifle",
    "throws a grenade", "casts a spell with both hands", "casts a fireball", "charges forward to attack",
    "takes a fighting stance", "boxes", "does martial arts", "dodges a punch", "ducks",
    "parries an attack", "grapples with someone", "headbutts",
    # reactions / falls
    "gets hit in the stomach", "gets hit in the face", "gets hit from behind", "is pushed backward",
    "falls forward", "falls backward", "falls to the side", "trips and falls", "dies and falls down",
    "is knocked down and gets up", "gets up from the ground", "gets up from lying on the back",
    "lies down on the floor", "collapses", "stumbles", "recoils in pain", "clutches the chest",
    "limps after an injury", "is electrocuted",
    # interaction / work
    "picks up a box", "puts down a box", "carries a heavy box", "lifts something heavy",
    "pushes a heavy object", "pulls a heavy object", "opens a door", "closes a door",
    "knocks on a door", "turns a wheel", "pulls a lever", "presses a button", "types on a keyboard",
    "writes on a board", "reads a book", "drinks from a bottle", "eats food", "talks on the phone",
    "takes a photo", "sweeps the floor", "digs with a shovel", "chops wood", "hammers",
    "saws wood", "paints a wall", "waters plants", "washes the hands", "brushes the teeth",
    "puts on a jacket", "ties the shoes", "searches the ground", "inspects something on a table",
    "throws something away", "catches a ball", "hands something to someone", "shakes hands",
    "hugs someone", "high fives someone", "pats someone on the back", "helps someone up",
    "climbs a ladder", "climbs a wall", "climbs over an obstacle", "vaults over a fence",
    "crawls forward", "crawls on hands and knees", "army crawls", "swims", "treads water",
    "balances on one leg", "walks on a narrow beam",
    # sitting / ground
    "sits down on a chair", "stands up from a chair", "sits on the ground", "sits cross legged",
    "kneels down", "gets up from kneeling", "squats down", "crouches and hides", "lies on the back",
    "sits and fidgets", "sits and drinks", "sits and types", "sits and claps", "rests on the ground",
    # sport / exercise
    "does jumping jacks", "does squats", "does lunges", "does sit ups", "does push ups",
    "jumps rope", "stretches the legs", "touches the toes", "does yoga", "shadow boxes",
    "kicks a soccer ball", "shoots a basketball", "throws a baseball", "swings a golf club",
    "serves a tennis ball", "bowls", "throws a frisbee", "throws a javelin", "spikes a volleyball",
    "skates", "skis", "surfs", "rides a bicycle", "rows a boat", "paddles a canoe",
    "lifts dumbbells", "does a cartwheel", "does a backflip", "does a forward roll", "does a handstand",
    # jumping / movement
    "jumps up", "jumps forward", "jumps backward", "jumps over a gap", "jumps and spins",
    "jumps down from a ledge", "leaps forward", "hops forward", "hops in place", "spins around",
    "turns around", "sidesteps", "backs away slowly", "dodges left and right", "slides on the ground",
    "slips and catches the balance", "steps over an obstacle", "walks and stops suddenly",
    "starts running from standing", "runs and stops", "runs and jumps", "runs and slides",
    # dance
    "dances", "dances salsa", "dances hip hop", "dances the waltz", "dances ballet",
    "does the robot dance", "breakdances", "does a disco dance", "dances the twist",
    "moonwalks", "dances with the arms up", "headbangs", "sways to music", "does a tap dance",
    # music / misc
    "plays the guitar", "plays the drums", "plays the violin", "conducts an orchestra",
    "juggles", "does a magic trick", "fishes", "flies like an airplane", "acts like a monkey",
    "acts like a chicken", "pretends to be a statue", "mimes being trapped in a box",
]

THEN_ACTIONS = ["waves", "points forward", "sits down", "crouches down", "jumps", "turns around",
                "stops", "bows", "kneels down", "picks something up", "punches", "kicks", "raises both arms",
                "falls down", "looks around", "claps", "salutes", "stands still", "lies down",
                "runs away", "walks backward", "dances", "stretches", "throws something", "shrugs"]
THEN_STARTS = ["walks forward", "runs forward", "jogs forward", "stands still", "sits on a chair",
               "crouches down", "jumps", "turns around", "waves", "stretches", "dances", "picks up a box",
               "kneels down", "lies on the ground", "walks to the left", "walks to the right",
               "walks backward", "sneaks forward", "limps forward", "climbs a ladder"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exclude", action="append", default=[])
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rng = random.Random(a.seed)

    def p(s):
        return "a person " + s

    loco = [p(f"{g} {d}") for g, d in itertools.product(GAITS, DIRECTIONS)]
    styled = [p(f"{g} {d} {s}") for g, d, s in itertools.product(GAITS[:9], DIRECTIONS[:6], STYLES)]
    sided = [p(t.format(side=s)) for t in SIDED for s in ("left", "right")]
    single = [p(x) for x in ACTIONS]
    seq = [p(f"{x} {c} {y}") for x, y in itertools.product(THEN_STARTS, THEN_ACTIONS)
           for c in ("then",) if x.split()[0] != y.split()[0]]

    excluded = set()
    for path in a.exclude:
        with open(os.path.expanduser(path), encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    excluded.add(line.rpartition("@")[0].strip() if "@" in line else line)

    def fresh(xs):
        return [x for x in dict.fromkeys(xs) if x not in excluded]

    # Fixed shares: every single action and sided variant, then locomotion, styled locomotion and
    # sequences fill the rest (sequences get 5 s so both steps fit).
    pools = [(fresh(single), 1.0, None), (fresh(sided), 1.0, None), (fresh(loco), 0.6, None),
             (fresh(styled), 0.0, None), (fresh(seq), 0.0, 5.0)]
    chosen = []
    for xs, share, dur in pools:
        k = len(xs) if share >= 1.0 else int(len(xs) * share)
        chosen += [(x, dur) for x in rng.sample(xs, min(k, len(xs)))]
    rest = max(0, a.count - len(chosen))
    styled_x, seq_x = pools[3][0], pools[4][0]
    n_styled = min(len(styled_x), rest * 11 // 20)
    chosen += [(x, None) for x in rng.sample(styled_x, n_styled)]
    chosen += [(x, 5.0) for x in rng.sample(seq_x, min(len(seq_x), rest - n_styled))]
    chosen = chosen[:a.count]
    rng.shuffle(chosen)

    os.makedirs(os.path.dirname(os.path.abspath(os.path.expanduser(a.out))), exist_ok=True)
    with open(os.path.expanduser(a.out), "w", encoding="utf-8") as f:
        f.write("# Kimodo t2m corpus prompts, generated by scripts/make-kimodo-t2m-prompts.py\n")
        for x, dur in chosen:
            f.write(f"{x} @{dur:g}\n" if dur else f"{x}\n")
    print(f"wrote {len(chosen)} prompts to {a.out} "
          f"(single {len(pools[0][0])}, sided {len(pools[1][0])}, loco {int(len(pools[2][0]) * 0.6)}, "
          f"styled {n_styled}, sequences {len(chosen) - len(pools[0][0]) - len(pools[1][0]) - int(len(pools[2][0]) * 0.6) - n_styled})")


if __name__ == "__main__":
    main()
