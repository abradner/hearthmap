# Manual stacking

For a batch **off GitHub**, or on GitHub only when stacked PRs are not available (`gh stack` exit 9
until the operator enables them) or the tooling broke mid-batch and the stack was dissolved with
`gh stack unstack`. On GitHub, every batch is otherwise a `gh stack` train. Everything in SKILL.md
applies unchanged except the steps below, which replace their stacked counterparts. Commands are
GitHub's; on another host use its equivalents.

## Build (Phase 3)

Branch each interstitial off its parent and open its PR against the parent branch (`gh pr create
--base <parent-branch>`), the base against the default branch. Once the cap is open, one edit pass
stamps every Batch block with the roster (`<slug> (#<first>–#<last>)`), the position
(`interstitial (3/7)`) and "manual stack", since no badge shows them.

## Bake (Phase 4)

- **Main moved**, merge-commit repos: merge main up through the stack; never rebase a reviewed
  branch. First find each branch's true payload (`git log HEAD --not origin/main --oneline`);
  afterwards audit `git diff origin/main...HEAD --stat` and run the focused tests. Squash repos:
  restart affected branches from fresh main after their parents land.
- **A showstopper fix low in the stack**, merge-commit repos: it usually reunifies through the merge
  commits; propagate it up only where a child conflicts or needs it. Squash repos: propagate it into
  each child explicitly.

## Merge (Phase 7)

On GitHub, if `gh stack` works, link the stack first (`gh stack link <bottom-pr-url> … <top-pr-url>`)
and land it as a train (SKILL.md Phase 7); what follows is for a stack that cannot be linked. Use the
method the repo's `AGENTS.md` picks; any other method needs the operator's word.

Bottom-up, one at a time, rapidly and consecutively (where PRs are independent the order is the
operator's call: recommend one). Per PR: merge without `--delete-branch`; poll until `MERGED`
(`enqueued` is not merged); confirm the next PR's base is now the default branch (retarget if not).
Delete merged branches once the whole stack has landed, and keep any branch another lane is based
on until that lane has retargeted. The followup merges last, in the same
sitting.

**Squash repos**, where later PRs edit lines earlier ones add (see Squash safety): squash bottom-up,
restacking as you go. Each restack rewrites a published branch, so get the operator's current
go-ahead for the restack (naming the stack) before the first step. Per step:

```bash
set -euo pipefail   # any failed step ends the restack before the next branch is touched
gh pr merge <bottom> --squash --match-head-commit <its head sha>
git fetch origin main
squash=$(gh pr view <bottom> --json mergeCommit -q .mergeCommit.oid)
[ -n "$squash" ]                                  # not merged yet (enqueued): poll, then rerun
git merge-base --is-ancestor "$squash" origin/main  # main lacks the squash: fetch again, then rerun
old=$(git rev-parse <next-branch>)
git rebase --onto origin/main <old parent tip> <next-branch>     # only its own commit moves
git push --force-with-lease=<next-branch>:"$old" origin <next-branch>
gh pr edit <next> --base main
```

Then wait for its CI. Where its patch-id differs from the reviewed commit
(the restack had to resolve something), it needs a delta read and the scarce reviewer's verdict
again before it merges; where the patch-id matches, the review stands. Then the next step. This
costs a CI run per step, which is the price of a stack that cannot be linked.

**Rebase-merge instead, only when the repo's `AGENTS.md` picks it or the operator says so for this
batch**: make the stack one linear history on main's tip and retarget the tip to main **before
Phase 7's pre-flights**: the rewrite moves every sha, so the tip needs CI, a delta read and the
scarce reviewer's verdict again (comparing patch-ids against the reviewed commits shows what really
changed). That new review of the rewritten tip stands in for pre-flights 1 and 3; run pre-flights 2
and 4 as usual, then rebase-merge it pinned to its head, and close the other PRs naming the commit
each landed as. The commits land with rewritten, unsigned shas: tell the operator first.

## Squash safety

Squash-merging a parent writes a **new** commit with the parent's payload while the child's merge base
stays pre-batch. A child carrying only the parent's payload still merges cleanly; the conflict comes
from **divergence**, where the child also changed the same hunks. Predict conflicts per hunk, not per
file. So in a manual stack only the top PR is squash-safe by default; everything below merges with
a merge commit where the repo picks that method, or each child is restacked onto fresh main after its
parent squashes (Merge, above). `gh stack merge`
needs none of this: its squash path rebases each remaining PR `--onto` the post-squash parent.

## A child PR closed by a branch delete

`gh pr merge --delete-branch` on a parent can close its child when the deletion beats GitHub's
auto-retarget. Prevention: do not delete branches until the whole stack has landed. Recovery, when
the parent really did merge:

```bash
gh api -X POST repos/{owner}/{repo}/git/refs -f ref=refs/heads/BASE_BRANCH \
  -f sha="$(gh pr view PARENT --json headRefOid -q .headRefOid)"   # restore the base FIRST
gh api -X PATCH repos/{owner}/{repo}/pulls/CHILD_PR_NUMBER -f state=open
gh api -X PATCH repos/{owner}/{repo}/pulls/CHILD_PR_NUMBER -f base=main
gh api -X DELETE repos/{owner}/{repo}/git/refs/heads/BASE_BRANCH     # only once CHILD is retargeted
```

`CHILD_PR_NUMBER` is a bare number (a REST path segment); `PARENT` takes anything `gh pr view`
accepts. If the branch is gone for any other reason, restoring it is the whole fix: do not delete it
again. If the parent's last SHA is gone, open a fresh PR from the still-live head with `Closes #N`,
and budget a review round: the thread trail does not come with it.
