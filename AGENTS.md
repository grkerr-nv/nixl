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
