## M<p>.<n>: <title>

### What this delivers
_(copy the Summary from docs/logs/M<p>.<n>.md)_

![M<p>.<n> final boot screen](https://github.com/gwbrickner/bongos/blob/<commit-sha>/docs/screenshots/M<p>.<n>.png?raw=true)

### Done-when checks
- [ ] check 1: the test that proves it (ktest/host test name)
- [ ] ...

### Verification
- `make format-check` / `make host-tests` / `make test` / `make test-full`: <results>
- Release profile (`make clean && make RELEASE=1 && make RELEASE=1 test`): <result>

### Sweep and review
_Replace each placeholder line with exactly `Sweeper: PASS (docs/sweeps/M<p>.<n>.md)` and `Reviewer: PASS`, each on its own line, only when they're true. `pr-policy.yml` auto-merges only on exact matches of both._

Sweeper: <PASS or FAIL> (docs/sweeps/M<p>.<n>.md)
Reviewer: <PASS or FAIL>

_List any deferred Should-fix items, and why._

### Policy
needs-owner: <yes or no>
_("yes" if the roadmap marks this milestone needs-owner, or it touches memory, interrupts/SMP, the scheduler, security/crypto, on-disk formats, the boot ABI, `.github/`, or `.claude/`.)_

### Owner hardware check
_(None, or the step-by-step instructions, which are also listed in STATUS.md under "Waiting on owner".)_
