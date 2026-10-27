<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# NIXL resiliency development

This checkout is the working repository for the NIXL resiliency feature.
The design document is currently at `~/nixl-resiliency-design.txt`; read the
relevant sections before implementing a feature. If that file is unavailable,
ask for its location rather than guessing the design.

## Branch workflow

- `nixl-resiliency` is the resiliency integration branch in this repository. Accumulate
  completed resiliency work here until the overall feature is ready.
- Develop each feature or independently reviewable change on a separate branch
  created from the integration branch. Use descriptive names such as
  `resiliency/fault-injection-adapter` or `resiliency/backend-error-tests`.
- Keep each feature branch focused on one coherent change. Use a separate Git
  worktree when working on multiple branches concurrently.
- Merge completed feature branches back into this repository's `nixl-resiliency` branch.
  Target this integration branch when opening feature pull requests.
- Before merging, review the diff, run checks appropriate to the change, and
  document the resulting behavior and any remaining limitations. Add regression
  coverage for failure handling and recovery changes.
- Treat eventual integration into upstream NIXL as a separate step from merging
  component features into this working branch.

## Keep the fork current

- `origin` is the personal fork, `git@github.com:grkerr-nv/nixl.git`.
- `upstream` is `https://github.com/ai-dynamo/nixl.git`. Fetch upstream changes;
  push working branches only to `origin`. Set `remote.pushDefault` to `origin`.
- Keep `main` free of feature commits so it can fast-forward to `upstream/main`.
- Sync regularly and before starting new feature branches. Start with a clean
  working tree; commit or otherwise preserve local changes first.
- Always update and push `main`, then update and push `nixl-resiliency`, before
  rebasing feature branches. Rebase feature PRs onto published
  `origin/nixl-resiliency`, never directly onto `main`.

### `#UPDATE_BRANCHES`

When the user sends `#UPDATE_BRANCHES`, run the full synchronization below,
including checks and pushes, without asking for the routine steps again.
The user may name a subset of feature branches; otherwise include active
resiliency PR branches and their dependency branches. Query PRs explicitly in
`grkerr-nv/nixl`, since GitHub CLI may otherwise select upstream.

1. Record the starting branch, inspect worktrees and local/remote divergence,
   and preserve all local work. Fetch `upstream` and `origin`. Record the fetched
   remote tips for push leases and create local backup refs before rewriting
   feature branches. Do not discard divergent commits or touch backup branches.
2. Fast-forward `main` to `origin/main` and `upstream/main`, then push it to
   `origin`. Inspect any divergence instead of resetting or force-pushing.
3. Fast-forward `nixl-resiliency` to `origin/nixl-resiliency`, merge `main` with
   sign-off, resolve conflicts, and run relevant checks. Push integration before
   rebasing feature branches. Never rebase or force-push either shared branch.
4. Rebase independent feature branches onto published `origin/nixl-resiliency`.
   Update stacked branches in dependency order, replaying only their own work
   onto the updated parent with `--onto` when needed. Preserve meaningful merge
   resolutions and the PR stack; do not replay obsolete dependency copies.
   Reuse new upstream functionality instead of retaining local substitutes.
5. Review each resulting PR diff and conflict resolution. Run focused checks
   for affected behavior, then publish rebased branches to `origin` using
   `--force-with-lease=refs/heads/<branch>:<recorded-remote-tip>`. Stop on a stale
   lease and inspect the new remote work. Never push to `upstream`.
6. Restore the starting branch and report updated branches, checks, and any
   remaining limitations. Keep the working tree clean.

Configure the remote once if it is absent:

```bash
git remote add upstream https://github.com/ai-dynamo/nixl.git
git config remote.pushDefault origin
```

Update the fork's main branch:

```bash
git fetch upstream
git fetch origin
git switch main
git merge --ff-only origin/main
git merge --ff-only upstream/main
git push origin main
```

If a fast-forward fails, inspect the divergence before proceeding. Do not reset
or force-push shared branches to make the update succeed.

Update and push integration before touching feature branches:

```bash
git switch nixl-resiliency
git merge --ff-only origin/nixl-resiliency
git merge --signoff main
# Resolve conflicts and run relevant checks before pushing.
git push origin nixl-resiliency
```

After the integration push succeeds, rebase each active feature PR branch:

```bash
git switch resiliency/forwarding-engine # Substitute the active feature branch.
git rebase origin/nixl-resiliency
# Resolve conflicts, reuse upstream code, run relevant checks, and review the PR diff.
git push --force-with-lease origin resiliency/forwarding-engine
```

Inspect local/remote feature divergence and preserve all work before rebasing.
Keep shared instructions in integration so feature PRs inherit the same AGENTS.md.
Use merges for shared integration history; never rebase or force-push `main` or
`nixl-resiliency`. Use `--force-with-lease` when publishing rebased feature branches.
Remove local substitutes when upstream now provides the same functionality.

## Test scope and CI cost

- When using Ponytail or generating tests, prioritize focused coverage that a
  reviewer can understand quickly. Test volume is not a goal.
- Reuse existing fixtures and helpers. Add the smallest set of cases that proves
  the behavior, catches the relevant regression, and protects distinct failure
  and cleanup paths.
- Avoid exhaustive combinations or testing every status against every method
  when representative cases exercise the same forwarding logic. Expand coverage
  when cases have different behavior or a specific requirement demands it.
- Keep lifecycle, error-handling, and safety checks that catch distinct bugs;
  remove redundant cases and assertions that merely repeat the implementation.
- Consider total CI cost: fixture compilation, shared libraries, dependencies,
  sanitizer builds, and test execution. Prefer extending existing test targets
  and fixtures over adding parallel infrastructure when it fits.
- Run focused checks during development. Before broad or repeated runs, identify
  what additional risk they cover. Explain unusually large test additions or
  expensive checks in the PR description.

## Repository conventions

- Before coding, read `CONTRIBUTING.md`, `.clang-tidy`, `.clang-format`, and
  applicable `.pre-commit-config.yaml` hooks. Nearby legacy code may use older
  conventions; do not treat it as the only style reference.
- Follow configured rules even when CI does not enforce them. Classes, structs,
  methods, and members use `camelBack` (`engineDeleter`). Type aliases use
  `lower_case_t` (`engine_ptr_t`); local variables and parameters use `lower_case`.
  Private members also have a trailing underscore (`pluginOwner_`).
- Preserve names required by existing APIs and test frameworks. Avoid unrelated
  renaming of existing code.
- Before committing, check new declarations against the naming rules and run
  applicable format/lint checks. If a tool is unavailable, inspect the relevant
  rules manually and report the missing check; do not silently skip conventions.

## Communication style

- Use Caveman for concise progress updates, code and review comments, commit
  messages, and pull request descriptions. Follow its skill instructions when
  available; otherwise use the concise style described here.
- Lead with the change or result. Use short, direct sentences. Cut filler,
  repeated context, and unnecessary narration.
- Keep the details needed to understand behavior, rationale, validation, and
  blockers. Brevity must preserve technical accuracy and clarity.
- Write short, imperative commit subjects. Add a body only when the reason,
  tradeoffs, or validation need explanation. Preserve required sign-offs.
- Add code comments when they explain intent or a non-obvious constraint.
  Avoid comments that merely repeat the code.

Follow [CONTRIBUTING.md](CONTRIBUTING.md) for repository coding, testing, and
contribution conventions. Keep these instructions current if the integration
branch or design-document location changes.
