---
name: single-pr
description: Ship one self-contained change as a single PR with the batch flow's rigour - cheap self-review and the independent reads the targeting rules call for before opening, so findings cost an amend not a round; the scarce reviewer (Codex by default) aimed at the final head; harvest all three comment surfaces; react immediately under a three-round cap with a delta read after every fix push; verify before claiming green; merge only on the operator's go-ahead. Use for any ordinary change that fits in one PR - a feature, a fix, a refactor, a docs pass - which is this repo's default flow. Use `.claude/skills/batch-review` instead when the work is genuinely several PRs opened together as one body of work.
user-invocable: false
---

# Single PR

The default flow: **react to feedback immediately, merge when green.** One PR at a time, so there
is no reason to withhold reaction; that discipline exists only to stop churn across several
in-flight PRs.

Almost everything that makes `batch-review` work is not about batching: it is about getting the
change actually reviewed and actually verified. That machinery lives in
**`docs/pr-review-machinery.md`**: read it now, before Phase 1, and especially §1 "Targeting",
which decides which reviews this PR owes. This skill covers only what is specific to one PR.

Escalate to `.claude/skills/batch-review` when the work is genuinely several PRs opened together (a
planned fan-out, an overnight batch, a stack). The tell is reviewer attention fragmenting across
live threads, not diff size: one large coherent PR belongs here.

## Phase 1 — Review before opening

A finding surfaced before the PR exists costs a `git commit --amend`; the same finding after
opening costs a reactive round, and rounds introduce bugs (§4 of the review doc).

1. **Cheap self-review of each commit** (targeting rule 1): one pointed question, "what did this
   change make newly risky?", delegated to a small-model subagent. Fix what it finds in the commit.
2. **Per-commit independent reads** for any commit touching deletion, auth, a user-visible payload
   or stored data (rule 2), via `.claude/skills/independent-commit-review`.
3. **A whole-change read briefed with production facts** if the PR carries a runbook, a rollback
   note or a data step (rule 3).
4. **Per-commit reads on every commit** when the work was substantial or built by delegated
   subagents, whatever it touches: the session that orchestrated has read reports, not the diff, and
   inside one PR a defect in commit 2 rides in on commit 5's plausibility.

For anything touching an access-control or trust boundary, the question is: **who is the caller,
how do we know, and what happens if they lie?**

## Phase 2 — Open it

- **Open ready-for-review, not draft.** Which reviewers fire on which edge is in the review doc §1.
- Run the repo's full pre-push gate first (lint, build, tests). There is no followup to fix a red.
- **Sweep neighbouring open PRs** for file overlap (`gh pr list`, then `gh pr view <n> --json
  files`). Default: comment and let them adapt after this lands. Never absorb their work or rebase
  their branch. Irreversible collisions (competing migration numbers) are operator decisions.
- The PR body says what changed and why, and names what the diff cannot show: a convention this PR
  proposes (one `AGENTS.md` §4 or §8 does not state is a proposal), an assumption, a deliberate
  omission. The test plan says what was actually run, including what could **not** be verified
  here. Name bots without the at-sign: a handle in a body invokes them.

## Phase 3 — Solicit review

Work from this repo's roster in the review doc, not from memory: a repo may list different bots, or
none.

- **The scarce reviewer at the final head, with an aimed prompt** (review doc §1 says how to aim it):
  intent, highest risk, comments only. For a single PR the head you ask the operator to merge is the
  final head; if you expect fix rounds, ask once the Phase 1 reads are folded in, not before.
- Ask every other reviewer the roster lists that does not fire on its own.
- Wait about ten minutes (longer for a bot that queues), then
  **harvest all three surfaces**: issue comments, review bodies, inline comments. Findings are
  usually inline; a clean Codex verdict, where Codex is the scarce reviewer, is an issue comment naming
  the commit.

## Phase 4 — React

This is the one phase that differs from `batch-review`: **respond to findings as they arrive.**
Triage by verifying (a finding is a claim; triage by content, not severity label), fix, push.

- **A delta read after every fix push** (targeting rule 6), aimed at the finding the fix answers.
- **Ask the scarce reviewer again at each head a fix round produces** (the head that merges needs its verdict);
  ask the per-PR bot again where it does not re-review on push. Check the review's commit,
  not that one exists.
- **Three reactive rounds, then defer** to the tracker, through a declared closing round (review doc
  §4). Three or more findings on one sub-feature: revert it and ticket it.
- **Answer every thread** (fixed with the commit, deferred with the ticket, declined with the
  reason) and resolve it yourself. Never a bulk "resolve all" bot command.
- Fix bugs with **revert-and-confirm**: the regression test passes with the fix, fails for the
  right reason with the fix reverted, passes again restored.
- Keep the steering docs current **in this PR** when the change moved a boundary or documented
  behaviour (`AGENTS.md`, and the repo's boundary map if it keeps one). Discovered work goes to the
  tracker.

## Phase 5 — Verify, then report ready

Work the review doc's §5 green-signal table: check **which CI jobs actually ran**, confirm a review
was delivered **for the current head** (§6's check, plus any issue-comment verdict the review doc describes), and name
the artifact the change should have produced and go look at it: the rendered page, the written
file, the rows.

Then send one ready line and **stop**: `#<n> ready: head <full sha>, CI <state> at that head, scarce reviewer
<verdict at sha, or the rule-9 fallback>, independent reads <who, what range>, threads 0, deferrals
<ticket links>`, plus anything CI did not run. Readiness is not authorisation.

## Phase 6 — Merge

The gate is `AGENTS.md` §8: a live operator go-ahead, and every new session starts at the manual
gate. On the go-ahead, **re-run the review doc's §6 check
first**: anything pushed since Phase 5 moved the head past what was reviewed. If the head moved,
also repeat Phase 5's CI check at the new head: merge only when that head's CI is complete and
passing. Then merge per the repo's strategy, bound to that head (`--match-head-commit <sha>`), delete the branch only if nothing is based on it, and confirm the merge landed
rather than trusting the command's return. Restart the working branch from fresh main:

```bash
git fetch origin main && git checkout -B <branch> origin/main
```

A merge is not a release, and the go-ahead to merge does not authorise a tag (`AGENTS.md` §8).

## Where these rules came from

Shared evidence is in the review doc. Specific to this flow: the open-ready rule drew a finding in
three consecutive rounds of the PR that introduced it, each time because this skill kept its own
copy of the trigger detail; the copy is gone. The re-ask-after-push rule exists because a batch once
read an approving verdict that predated three rounds of changes. Append this repo's single-PR
outcomes here as they accumulate.
