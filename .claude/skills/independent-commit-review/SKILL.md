---
name: independent-commit-review
description: Adversarial, fresh-eyes review of committed work by reviewers with no memory of writing it - one independent subagent per commit, a whole-change read briefed with production facts when the change carries a runbook, rollback note or data step, and a delta read after every fix push. Findings fixed with revert-and-confirm, local history rebuilt so each commit reads correctly, and a closing pass by a reviewer from a different model family. Use before pushing or fanning out commits, for any commit touching deletion, auth, user-visible payloads or stored data, after a fix moves a reviewed head, when the scarce bot reviewer (Codex) is out, or whenever the user asks for an independent/adversarial/fresh-eyes review.
user-invocable: false
---

# Independent commit review

Don't review your own work and call it independent. The session that wrote the code carries its
author's assumptions; this process buys fresh eyes by giving the work to subagents that have never
seen the conversation, and then (step 8) to a reviewer that is not even the same kind of model,
because agents of one family share blind spots.

**Which copy.** This is hearthmap's copy. Claude Code loads a user-level copy in place of a repo's
copy of the same name, so if this repo's `.claude/skills/independent-commit-review/SKILL.md`
differs from what you are reading, read that one and follow it. **Which commits get a read, and
when**, is the repo's review doc (`docs/pr-review-machinery.md` §1, "Targeting").

Why it earns its cost: in earlier projects, after-the-fact reads of fast-built commits found most of
them needed real fixes, and independent reads made dozens of material catches, many in code a bot
had already reviewed and passed.

## The three kinds of read

- **Per-commit**: one reader per commit, before the PR opens where possible. The default unit.
- **Whole-change**: one reader over the full diff (`git diff origin/<default>...<branch>`, spelled
  out on one line: `...<branch>` alone silently diffs from HEAD). Required for anything with a
  runbook, a rollback note or a data step, and for a batch's aggregate. Per-commit readers cannot
  see cross-commit bugs or system behaviour.
- **Delta**: after every push that fixes a finding, one reader over just that delta, aimed at the
  finding it answers. A fix for a review finding can reopen it.

## The brief

Each reader gets, in its prompt:

- the exact range (`git show <sha>`, or the spelled-out diff) and "review this as a stranger's PR";
- read the repo's `AGENTS.md` / `CLAUDE.md` first: its invariants are review criteria;
- **adversarial framing**: assume at least one real problem and hunt for it, with concrete things to
  check for this diff, not generic advice;
- for anything touching an access-control or trust boundary: *who is the caller, how do we know,
  and what happens if they lie?* "We trust what they tell us" is the finding;
- **production facts, for a whole-change read of anything that deploys with steps**: flags and
  their live values, CDN and cache behaviour, backup state, the rollout shape (old and new servers side
  by side), and what clients persist (service-worker caches, client-side stores and cursors, URLs
  saved on a device). On one PR a read briefed without these facts missed a HIGH (display devices
  resuming on URLs the release retired); a later read briefed with them found it. A read is only as good as its brief;
- **questions whose answer can be "no"** ("is the undo token filtered from the request logs?") rather
  than "does this look fine?". A list of properties to confirm finds more;
- **read-only rules**: no edits, checkouts or stashes in your worktree (experiments on copies in a
  scratch dir); on a shared box never start, kill or signal a process it did not start. Reviewers
  that moved HEAD in a shared worktree once corrupted a union test run;
- a capped output: severity-ranked findings, about 400-600 words, no praise.

A reader can run tests and revert-and-run fixes, in a copy. Ask it to.

## Procedure (local commits, not yet pushed)

1. **Safety snapshot.** `git branch snapshot/<branch>-<date>` at the current tip. Kept permanently.
2. **Units.** One per commit by default; group a change with its immediate fixup. Skip pure
   docs/config commits unless they carry risk (CI, permissions, a runbook, anything
   security-adjacent).
3. **Dispatch** one reader per unit, in parallel, with the brief above.
4. **Triage live**: must-fix in the commit / comment, not code / defer to the tracker. A finding is
   a claim: reproduce before accepting, and expect to decline some. **A flagged hole is not a fixed
   hole**: check the fix closes the case the reader named (a per-commit reader flagged an authorisation
   gap; the fix never covered the lower-privilege session type, and only the different-family
   reviewer noticed).
5. **Fix with revert-and-confirm.** Write the regression test, see it pass with the fix, revert just
   the fix, see it fail for the right reason, restore. A test never seen to fail has proven nothing.
6. **Rebuild history** so each commit is correct on first read: on a fresh branch from the base,
   cherry-pick in order, amending each fix into the commit it belongs to. Lockfile and manifest
   conflicts resolve toward the final intended state.
7. **Verify clean, then swap.** Full suite from a clean state (a fresh database and services if the
   suite uses them). `git status --short` must be empty
   (the snapshot keeps commits, not uncommitted edits, and the swap's `git reset --hard` destroys
   those silently). Then point the real branch at the rebuilt history.
8. **A reviewer from a different family.** Every reader above is the same model with the same
   habits. Before merging, put the assembled change in front of a different family (`@codex review`
   with an aimed prompt; a human) with a brief that says **what the agents already found and asks for
   what they missed**. Evidence: ten per-commit agents found nine defects on one branch; a single
   Codex pass then found a P1 all ten had missed. Heterogeneity beats volume. Codex's findings are
   still claims: of four, three were real.

## When the commits are already pushed or merged

Published history stays put: skip steps 1, 6 and 7, and never rewrite a pushed branch. Everything
else applies. Where the fixes go depends on where the change is:

- **Pushed, PR still open**: add fix commits to that PR's branch (single PR), or to the batch's
  followup (a batch interstitial: write-only until synthesis, per `batch-review`).
- **Merged**: one followup PR off the current default branch.

Either way the review leaves a trail on the original PR: what was read at which SHAs, the findings
and their dispositions, where the fixes landed. Re-check each finding against the current tip before
triage: later commits may already have fixed or moved it.

## Recording it

Put the review of record on the PR (a comment, or the merge body): who read what range at which
head, findings and dispositions, and anything the read could not cover (no different-family pass
available; a browser it could not run). Ready lines cite it; whoever coordinates the merge checks it.
