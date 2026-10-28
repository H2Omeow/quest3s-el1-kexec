# Synthetic Planner Fixture

This fixture checks the host planner using a real target `Image.el1` and synthetic RAM, DTB, and reservation addresses. **All addresses and DT contents here are test data, not Quest device information.** The fixture never invokes module ioctls or the `load`, `status`, `unload`, or `exec` commands.

From the repository root, run:

```sh
python3 tests/fixtures/synthetic-plan/run.py
```

The default inputs are `build/artifacts/Image.el1` and `user/quest-el1`. Build those first, or pass different paths with `--image` and `--planner`. The script needs `dtc` on `PATH`; generated DTBs live in a temporary directory and are removed on exit. It prints the four expected planner outcomes without writing machine-specific paths or image fingerprints into the repository.

The cases cover a baseline plan, an operator reservation overlapping the kernel, a DTB `/reserved-memory` entry overlapping the kernel, and a reservation inside the DTB's 2 MiB mapping block. Passing these checks says nothing about real Quest addresses, firmware state, module ABI, device compatibility, or handoff behavior.
