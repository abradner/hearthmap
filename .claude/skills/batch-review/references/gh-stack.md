# `gh stack` and merge mechanics: the reference

Read when the batch reaches the step that needs it. GitHub publishes an upstream agent skill for the
CLI from the `gh-stack` repo's own `skills/` directory: install it per that repo's current
instructions and treat it as authoritative for the full command surface. What follows is the subset a
batch trips over, kept because the failures are silent and mid-train.

## The TTY trap

`gh stack` branches on whether stdout is a TTY: piped, most commands print static text; under a PTY
the same command opens a wizard or a full-screen TUI and **blocks forever**. Harnesses differ, so
always pass the flag.

| Always | Never bare | Why |
|---|---|---|
| `gh stack view --json` | `gh stack view` | TUI under a PTY |
| `gh stack submit --auto` | `gh stack submit` | prompts per new PR |
| `gh stack checkout <pr-url>`, then `gh stack merge --yes --<method>` | `gh stack merge <number>`, `gh pr merge` | below |
| `gh stack checkout <target>` | `gh stack checkout` | selection menu |
| `gh stack up`/`down`/`top`/`bottom` | `gh stack switch`, `gh stack modify` | TUI only |

## Reading state

`gh stack view --json` writes JSON to stdout and status to stderr: parse the former, branch on exit
codes, never scrape stderr. Fields: `trunk`, `currentBranch`, `branches[]` of
`name/head/base/isCurrent/isMerged/isQueued/needsRebase`, plus `branches[].pr` of `number/url/state`.
`base` is the last known parent SHA, not the parent's tip; `needsRebase` is the real signal.

Exit codes: `9` stacked PRs not enabled for the repo (tell the operator; `manual-stacking.md` until
then); `3` rebase conflict (resolve, then
`gh stack rebase --continue`, or `--abort`); `8` stack file locked by another process (retry after
about 5 s); `2` not in a stack; `7` rebase already in progress; `1` generic (read stderr).

Multiple remotes: `push`, `submit`, `sync`, `rebase` and `link` need `--remote <name>` unless
`remote.pushDefault` is set; `checkout` and `trunk` need the config.

## Success lines that lied

- `gh stack sync` has reported "✓ synced" without pushing rewritten history (`gh stack push`
  pushed it). Compare local and origin SHAs after any restack, never the tick.
- `gh stack sync` never opens PRs; that is `submit`.
- `gh stack sync` on local/remote divergence prints both chains, changes nothing, and **exits 0**
  with "Sync aborted". On exit 3 it has already restored the stack: run `gh stack rebase` to recreate
  the conflict, resolve, `--continue`.
- Stack commands run against the **local** `main` ref. In a worktree with a stale local main, `init`
  based branches on old history and `rebase` printed "rebased onto main" without doing it (fixed
  upstream in v0.1.0; pin the version you reason about). Verify a claimed trunk rebase with
  `git merge-base --is-ancestor origin/main <branch>`.
- Amending an interstitial and then `gh stack rebase` replayed the superseded pre-amend commits of
  every ancestor as conflicts. Skip each superseded replay by commit id, never by subject (the amended
  twin shares it), or rebase each branch explicitly bottom-up:
  `git rebase --onto <new-parent> <old-parent> <child>`.
- `gh stack push --atomic`: v0.0.8's help says all-or-nothing, v0.1.0's reference says per-branch
  leases. Assume the weaker.
- After any restack, compare each branch with its reviewed state: `git diff <parent>..<branch> | git
  patch-id --stable` equal means the same change (it ignores whitespace: read a whitespace-sensitive
  file's diff yourself). Review threads have survived cascade rebases and the stack merge itself;
  spot-check `position != null` anyway.

## When the tooling breaks mid-batch

`gh stack unstack` dissolves the stack object; PRs stay open with correct bases. Finish the batch with
`manual-stacking.md`. `gh pr merge` works again, so the merge gate is policy only. Merged and queued PRs
cannot be unstacked. A batch that had to go manual can be re-adopted once the tooling is back:
`gh stack link <branch|pr-url>...`, bottom to top, never a bare number; `link` works GitHub-side and
rewrites no branch. Verify each base afterwards.

## Why never a bare number

`gh stack merge <n>` treats `n` first as a **stack** number, then as a PR number, and the two are
independent sequences over the same small integers (a stack number can be live while no PR has
that number, and vice versa). Pass the followup's PR number when it is also a live stack
number and an atomic merge lands a different train. `merge` takes integers only and `view` takes no
target, so you cannot check what a number resolves to. `checkout` accepts a PR URL, which cannot be
misread: check out, `view --json`, merge with no argument.

Always pass `--yes` and a method flag: without `--yes` a wizard opens; without a method it silently
reuses your last-used one.

**Two merge methods** (interstitials and followup differ) need two checkout-then-merge cycles,
because `merge_method` covers the whole group. That the followup is still stack-mergeable once
everything below it has landed is inferred, not observed: try it on a low-stakes batch first.

## The async merge API

`gh stack merge` wraps it, and it is the only supported way to merge a stacked PR (the synchronous
endpoint and the `mergePullRequest` mutation are rejected). For automation, not as a way round an
outdated extension:

```bash
echo '{"merge_method":"squash","merge_action":"default"}' \
  | gh api --method PUT repos/OWNER/REPO/pulls/<n>/merge-async --input -
gh api repos/OWNER/REPO/pulls/<n>/merge-async/<uuid>     # poll about once a second
```

Submit answers 202 `pending` with a uuid, 200 `merged` if already merged, 400 `failed` if closed or
draft, 409 if a request exists (returning *that* uuid, whose options may differ from yours). `status`
is `pending` | `merged` (`details.sha`) | `enqueued` | `failed`. The `sha` field is a head-match guard:
use it when the branch could move under you. `commit_title`, `commit_message` and `merge_method` are
ignored on merge-queue actions. Results expire 24 hours after their last update.

Branch protection and rulesets are evaluated **when the merge runs**, not when it is submitted, so an
accepted submit proves only that the PR is open and not a draft. `enqueued` is terminal for the
request and not a merge: the queue decides, may land the PRs in separate groups, and picks the method.
`failed` merged nothing: read `details.message` for the cause before retrying. Auto-merge and rule bypass do not exist for stacked PRs.
