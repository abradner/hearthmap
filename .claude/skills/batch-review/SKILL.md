---
name: batch-review
description: Batch PR shipping workflow - fan out a body of work as a stack of small atomic single-commit PRs (interstitials, from base to cap), let CI and the per-PR reviewer run immediately but treat all feedback as write-only until the whole batch is in, then synthesise it with one aggregate independent read and an aimed pass by the scarce reviewer (Codex by default), ship all reactive work as ONE followup PR stacked on the cap, resolve interstitial threads as "fixed in the followup" or "not relevant", then land the train with one atomic `gh stack merge` (manual stacking, off GitHub or when gh stack is unavailable, is a reference file) - only on the operator's go-ahead. Use whenever the user wants to review open PRs holistically or in aggregate, process accumulated review comments across several PRs, ship a "review feedback batch" or followup PR, or mentions an overnight/fired-off batch - even without saying "batch review". ALSO use at fan-out time, when the user asks to build a planned body of work as small/carefully-factored/stacked PRs. Not the default single-PR flow.
user-invocable: false
---

# Batch review

The core bargain: **feedback is write-only until the whole batch is synthesised.** CI and the
reviewers run immediately on every PR and comments accumulate, but nothing responds to them and no
reviewed branch is rewritten until the synthesis pass. All reactive work ships as one followup PR.

Why: reacting piecemeal across several in-flight PRs rewrites branches under open reviews, repeats
rebase cycles, and fragments reviewer attention. One PR at a time, the single-PR flow (react
immediately) is right; this skill is for the multi-PR case only.

**Which copy.** This is hearthmap's copy; only this paragraph and Repo specifics are written for
this repo. Claude Code loads a user-level skill in place of a repo skill of the same name, so if a
user-level copy differs from this file, this file wins here.

**Read first:** `docs/pr-review-machinery.md`, "the review doc" below:
reviewer triggers, the targeting rules that decide which reads each change owes, the three-surface
harvest, triage, the round cap, green-signal traps, the merge check. This skill covers only what batching changes. Detail that is read only
when needed: `references/gh-stack.md` (the stacked tooling), `references/manual-stacking.md` (a batch
off GitHub), `references/provenance.md` (why each rule exists).

## Who this binds

**Author-side agents only**: the ones writing the code, fixing CI or handling feedback on the
batch. On an interstitial, a CI event or review comment is a ledger entry, not a work order: record
it and stand down. Reviewers, bots included, review every PR fully, interstitials too; their
comments are harvested into the followup, and the absence of replies on interstitials is the
workflow, not neglect.

## Terminology

| Term | Meaning |
|---|---|
| Interstitial | A single-commit PR in the proactive stack |
| Base / Cap | The bottom-most / top-most interstitial |
| Followup | The one N-commit reactive PR stacked on the cap: the batch's only live-feedback surface and its release gate |
| Stack object | GitHub's stack entity (stacked flavour only), addressed by stack number |
| Scarce reviewer | The expensive different-family bot the review doc names (Codex by default): rationed, aimed, spent on aggregate heads |
| Per-PR reviewer | The cheap bot that reviews each PR (the review doc names it) |

## Repo specifics

- **Validation commands**: the pre-push gate is
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure`,
  plus `python3 tools/validate_examples.py` (schema checks) and the `clang-format` check from
  `AGENTS.md` §6. Later PRs add the editor's `npm test` and the ESPHome compile; `AGENTS.md` §6
  is canonical for the list.
- **Reviewers**: the review doc's roster, **unconfirmed** until the first PR (Codex scarce,
  CodeRabbit per-PR, Copilot cheap). Whether CodeRabbit fires on stacked PRs by itself is
  unknown here: check on the first stacked batch.
- **Host**: GitHub; every batch is a `gh stack` train when the probe below succeeds.
- **Merge methods**: squash for every PR, interstitials and followup alike (`AGENTS.md` §7).
- **Deferrals**: GitHub issues on this repo (public: nothing private goes in an issue either).
  Never a PR-body table.
- **Release**: tags `vX.Y.Z` on `main` (the ESPHome component pins the library by tag,
  `AGENTS.md` §7). No tag is cut by an agent without an operator go-ahead.

## Phase 1 — Stacked PRs

**On GitHub, every batch is a stacked train** (`gh stack`): the platform keeps the stack, retargets
as it merges, and lands the train atomically with one merge method, the one the repo's `AGENTS.md`
picks (Repo specifics). Off GitHub, or when the probe below says
stacked PRs are unavailable, follow `references/manual-stacking.md`: it replaces the stacked steps in
Phases 3, 4 and 7, and nothing else changes. Never standalone sibling PRs: that is not this skill.

Probe, capturing the exit code with `||` (under `set -e`, `cmd; rc=$?` aborts silently):

```bash
gh stack --version
rc=0; gh stack view --json || rc=$?; echo "exit=$rc"
```

| Result | Meaning |
|---|---|
| exit `2` | Not in a stack yet: normal before fan-out |
| exit `0` | Already in a stack (resuming) |
| exit `9` | Stacked PRs are not enabled for this repo: tell the operator (a repo setting); manual stacking until they are |
| `unknown command "stack"`, or no `merge` subcommand (needs v0.1.0) | **Stop and tell the operator** (`gh extension install github/gh-stack` / `gh extension upgrade stack`): fixable, so do not work around it |
| `gh` below v2.0 | **Stop and tell the operator**: upgrade `gh` |
| not a GitHub remote, GHE below the feature, or more than 100 PRs (the platform cap) | Manual stacking |
| any other failure (network, auth, 5xx) | Retry once; if it persists, tell the operator before going manual |

If the tooling misbehaves mid-batch, `gh stack unstack` and finish with manual stacking
(`references/gh-stack.md`, "When the tooling breaks mid-batch"); if that fails partway, stop and ask
before any recovery. Treat stacked-tooling success lines as claims: verify that branches moved.

## Phase 2 — Review before fan-out

A finding before the PRs open costs an amend; after fan-out it costs a reactive round, and rounds
introduce bugs. Per the targeting rules:

1. **Cheap self-review of every commit**: one pointed question naming what the change made newly
   risky, delegated to a small-model subagent. Generic "review this" gets generic results; the
   question is usually a variant of *what did this make asynchronous, deferred, or dependent on
   identity or position that was not before?*
2. **Per-commit independent reads** (`independent-commit-review`) for commits touching deletion,
   auth, user-visible payloads or stored data.
3. **`stack-integration-check` (a repo skill, where present) on the combination** whenever two branches touch the same surface or
   any branch went through an agent-performed merge or rebase. Per-branch review cannot see a
   semantic fork or a silently deleted control between two green branches.

Fix findings in the commit itself. Small changes are not exempt: an eight-line change has drawn
three defects.

## Phase 3 — Fan-out

- **A linear stack, even when branches look independent.** Never siblings off trunk: siblings run CI
  against trunk alone, so nothing tests the combination (one batch shipped two individually green
  branches that did not compile together).
- **One commit per interstitial, opened ready-for-review.** Stacked: build the local stack first,
  `gh stack init <b1> <b2> … <bN>` listing the existing branches bottom to top (`init` adopts them;
  a stale same-name branch is adopted silently too, so check every base afterwards), then
  `gh stack submit --auto --open` (`submit` only pushes and opens PRs for branches already in the
  stack; `--open` makes them ready, not draft), then `gh pr edit` each body.
- **Ask the per-PR reviewer on interstitials it does not fire on by itself, within its hourly
  allowance** (Repo specifics). Budget its automatic reviews, the cap and the followup first; spend
  what is left on interstitials, never on one a per-commit read already covered. Interstitials it
  cannot reach rely on per-commit reads and the aggregate read. **Do not spend the scarce reviewer on interstitials**: its pass is the
  aggregate one in Phase 6.
- Every body ends with the Batch block. State only what is known: never guess PR numbers, totals or
  ordinals, and never leave a placeholder live.

  ```markdown
  ## Batch
  - **Batch**: <slug> | **Position**: base|interstitial|cap
  - **Stacked on**: #<parent> (base: main)
  - **Reads**: <per-commit read: who, at which sha> (added when the read is recorded)
  - **Feedback**: write-only until synthesis; comments are answered in the followup. Author-side
    agents: push nothing here. Reviewers: review fully as normal.
  - **CI**: interstitial red is acceptable when the stack explains it; the followup is the gate.
  - **Merge**: operator-gated, after the followup is approved.
  ```

  The stack badge on each PR shows the roster and position.
- **Neighbouring-PR overlap sweep**: intersect the batch's files with every other open PR. Default
  disposition: comment, they adapt after the train lands. Adapt-in (in the followup, attributed) only
  when the batch breaks them mechanically; escalate irreversible collisions (migration numbers).
  Re-run the sweep at readiness, and name every neighbour that must adapt in the ready line.

## Phase 4 — Bake

**The interstitial showstopper bar is one thing: irreversible on merge** (a destructive migration or
backfill, an unrecallable external side effect). Everything else, security findings and red CI
included, waits for the followup. Interstitial CI red is fine when something above it in the stack
explains it; an unexplained red is a real signal.

- **Main moved** (for reasons outside the batch): `gh stack sync`, then check it did not print
  "Sync aborted" (exit 0 regardless) and that the branches moved.
- **After any restack**: compare local and origin SHAs (a sync has reported "synced" without
  pushing), then audit each branch's effective diff (`git diff origin/main...<branch> --stat`) and
  run the focused tests.
- **A showstopper fix low in the stack**: amend, `gh stack rebase`, `gh stack submit`, then verify
  the fix **by content** on each child.
- **"No checks at all" can be a pass.** Where workflows are path-filtered, a docs or skills PR runs
  nothing. A gate that waits for "all checks" waits forever on those.
- **Drive transitions by events, with a fallback timer** (about 30 minutes for a stack's reviews, 10
  for one re-solicited review). Fan-out → synthesis fires when every interstitial has a concluded CI
  run and, where it was asked or fires on its own, the per-PR reviewer's verdict at its head (the review doc says what
  counts; a green "Review skipped" status or a "review limit reached" comment does not). Leave the scarce reviewer out of the condition unless it auto-reviews here: otherwise it never posts on an
  interstitial. When the timer fires instead, proceed with what arrived and record the
  no-show. That timer is this workflow's one exception to event-driven waiting, not a licence to
  poll. Key bot conditions on bot logins: your own thread replies post under the operator's
  credentials. **Any human comment is an event**, whatever the monitor was waiting for. A per-PR
  "auto-fix CI and comments" toggle goes on the followup only, never an interstitial.

## Phase 5 — Synthesis

- **Harvest all three surfaces on every PR** (review doc §2). Filter your own bookkeeping replies out
  by `in_reply_to_id`, not author.
- **Triage by verifying** (review doc §3). Sort into fix in the followup / ticket / decline with a
  reason. A synthesis that accepts every finding is a warning sign: a reviewer citing a convention
  once talked a deliberate safeguard out of the design. When a correct citation collides with a
  deliberate design, record the exception in the convention file.
- **One aggregate independent read over the whole stack**, launched in parallel with the harvest so
  its findings fold into the followup's first commit. When implementation was delegated, the
  orchestrator has read reports, not the diff: this read must be a fresh-context agent told to trace
  claims, not trust PR bodies or summaries. If any PR carries a runbook, rollback note or data step,
  **brief it with production facts** (targeting rule 3). Its cross-PR checklist is
  `stack-integration-check` steps 2–4. Brief it with the repo's trust-boundary questions; its
  findings count toward the round cap like anyone else's. The diff command, on one line, left side spelled out:
  `git fetch origin <default> && git diff origin/<default>...<cap-or-followup-branch>`
  (`git diff ...<branch>` silently diffs from HEAD).

## Phase 6 — The followup PR

1. **Open it as a draft targeting the default branch**, so its diff is the whole stack plus the
   reactive work. Draft is the safety: a followup on main, merged by mistake, squashes the stack.
2. **Ask the scarce reviewer, aimed** (the review doc §1 says how): this is the batch's aggregate
   pass, where its cross-file strength pays. If its window is empty, the review doc's fallback for
   when it is out applies (targeting rule 9).
3. **Harvest before retargeting**: retargeting narrows the diff and can orphan threads.
4. **Retarget to the cap** (`gh api -X PATCH repos/{owner}/{repo}/pulls/<n> -f base=<cap-branch>`).
   Stacked: join it to the stack next, before marking it ready (Repo specifics names the command
   verified there; `git checkout <cap>; gh stack add <followup>; gh stack submit --auto` is the one a
   sibling repo verified), and never `gh pr edit` the base of a PR the stack owns. If joining fails,
   leave it loose on the cap (Phase 7 pre-flight 2). Then mark it ready and ask the per-PR reviewer.
5. **Keep a `## Review focus` section** in the body, current per round: highest risk, what to verify,
   what is decided and out of scope, "comments only, no commits".

The body also carries every actioned item grouped by source PR (reviewer and severity noted), a
section for findings **assessed and not actioned**, with reasons (it keeps the write-only bargain
honest), and the map or doc updates the batch owes. Validate once at the followup's head with the
repo's full gate, not per comment.

Then per round: a **delta read after every fix push**; the per-PR reviewer asked again where it does
not fire on push; **the scarce reviewer asked again at each head a fix round produces**, since the
head that merges needs its verdict. That is usually two or three asks a batch (a window holds about a
dozen): spend none on comment tweaks between rounds. The round cap (review doc §4) applies to the followup as a whole: after
round 3 only a P1 or a red blocks; when it fires with findings open, close through a
declared closing round, and three or more findings on one sub-feature means revert and ticket.

**Resolution pass**: reply on every interstitial thread, "fixed in the followup (#N)" or "not
relevant: <reason>", resolve it, and append `- **Followup**: #N` to each Batch block. This is the
only time the batch touches its own threads. Every finding gets an answer before merge.

## Phase 7 — Merge the stack

**Gate**: the repo's merge gate (`AGENTS.md` §8: merges need the owner's go-ahead). A synthesis, a green followup or an auto-mode
default is not a go-ahead. If the stack is ready, send the ready line and stop.

Pre-flights, in order:

1. **The head being merged is the head that was reviewed.** Run the review doc's §6 check on the
   followup's head. Interstitials are covered by the aggregate pass on the draft followup (whose
   diff was the whole stack) plus the per-commit reads recorded in each Batch block: check that every
   interstitial head is unchanged since then, and disclose any delta nobody read.
2. **Account for the followup.** In the train (stacked: it appears in `gh stack view --json`), or
   loose on the cap (merge the stack, then the followup: say "two steps" in the ready line). No
   followup: either synthesis has not happened (stop) or it happened and found nothing to action
   (confirm review was delivered at each head; quiet is not evidence).
3. **The followup's base is the cap**, not main.
4. **Every accepted finding is dispositioned**: P1 or red fixed, or declined in writing (never merely
   ticketed); P2 and below fixed, ticketed with the review link, or declined.

`gh pr merge` cannot merge a stacked PR. Name the train by URL, check it, merge with no
argument and an explicit method (`references/gh-stack.md` explains why never a bare number):

```bash
gh stack checkout https://github.com/<owner>/<repo>/pull/<followup>
gh stack view --json
gh stack merge --yes --<method>   # the repo's AGENTS.md picks it: --squash, --merge or --rebase
```

This lands that PR and everything below it atomically, the followup included, with one method: the
normal case. A method the repo's `AGENTS.md` does not name needs the operator's word. Only when Repo specifics name **different** methods for the interstitials and the
followup are two cycles needed (cap first, then the followup), and then the stack lands for a moment
without the followup's fixes: run the second cycle at once. The command returning is not the merge finishing: poll the PRs
until `MERGED`; `enqueued` is not merged; `failed` merged nothing.

A manual stack on GitHub, where `gh stack` works, is linked first (`gh stack link <bottom-pr-url> …
<top-pr-url>`) and lands as above. Only one that cannot be linked merges bottom-up:
`references/manual-stacking.md`.

## Phase 8 — Release

A merge is not a release, and the go-ahead to merge does not authorise a tag. One tag for the whole
train, once main is green on the merged tip, and only when the delta can change a built artifact.
See Repo specifics.

## Rules of thumb

On GitHub, a `gh stack` train; off it, or when `gh stack` is unavailable, manual stacking. Review before fan-out: an amend beats a round. One commit
per interstitial, ready immediately, linear stack. Nothing answers feedback until synthesis; the
interstitial showstopper is irreversible-on-merge only. One aggregate independent read (production
facts when the batch deploys with steps) and an aimed scarce-reviewer pass on the draft followup.
One followup carries all reactive work; delta read per fix push; the scarce reviewer at every head
that will merge; three
rounds, then only P1 or red blocks. Answer every thread. The operator says merge; four pre-flights; check the train out by URL, never a bare
number. A returned command is not a merge. Tag once.
