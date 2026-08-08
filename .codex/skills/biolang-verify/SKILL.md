---
name: biolang-verify
description: Build BioLang and verify it against the example suite. Use after any change to src/ or Makefile, or when asked to verify/test the project.
---

# BioLang Verify

Build the interpreter and run the example suite as regression tests.

## Steps

1. Build: `make` (if a profile is involved, `make clean` first, then `make PROFILE=<name>`).
2. Sanity check the binary: `./bio -h` (or `./bio` for the built-in demos).
3. Run every example and confirm it exits 0:

```bash
for f in examples/[0-9]*.bio; do
  ./bio "$f" >/dev/null 2>&1 || echo "FAIL: $f"
done
```

4. Project end-to-end (need bundling + build + run):

```bash
cd examples/project && ../../bio build && ../../bio run
```

5. Report any `FAIL:` lines or build errors; do not claim success otherwise.

## Notes

- `bio -b` compile mode requires `libbio.a` — plain `make` builds it.
- Switching profiles requires `make clean` (make tracks timestamps, not values).
