# Batch review: where the rules came from

Ported across several earlier projects and corrected against real batches in each. Add to this
file rather than replacing it: the value is in the accumulated evidence, not in any single batch.
Read it when a rule looks wrong for your case, before changing it.

- The write-only rule exists because piecemeal feedback batches once rewrote main under a
  still-open stacked PR and killed it.
- The three-round cap exists because a measured batch ran seven reactive rounds with finding
  counts 6 → 2 → 1 → 3 → 2 → 5 — not convergence: four of the five later rounds were fixing
  defects a previous round's own fix had introduced. A separate repo independently recorded a
  "third instance in this batch of a fix introducing the next round's finding" the same week.
- The self-review-before-fan-out gate exists because of the same data: findings are an order of
  magnitude cheaper before the stack exists.
- The bot-budget asymmetry (cheap reviewer (rationed only by its plan's hourly allowance), budgeted expensive one) comes from a
  measured split: the cheap bot caught 6/6 of a mechanical bug class; every expensive-bot finding
  that mattered was cross-file. Spend the budget on aggregate diffs.
- The followup-exists pre-flight was added after a nine-PR stacked batch merged its entire train
  with no followup at all. Thirteen accepted findings landed on trunk untracked, including a
  warning that could never fire (production never set the logger it wrote to) and a
  nondeterminism trap in a package documented as pure — a replay hazard in a workflow engine.
  Nothing was red: CI was green, every thread had been triaged, and the merge control asked no
  questions. The findings were recovered only because the synthesis notes happened to still be in
  a live session. Every other rule here assumes the followup lands; this is the one that checks.
- The retarget-before-delete rule exists because `gh pr merge --delete-branch` closed a child PR
  in a live merge train when branch deletion raced GitHub's auto-retarget. Two sibling repos have
  now hit it independently — once on a pair of PRs, once **twice in a single night** — and both
  times the reopen was attempted with the base branch still gone, so both concluded a closed
  child "cannot be recovered at all" and went to fresh PRs. A third repo's own history (a closed
  child on its own retarget race, reopened cleanly *after* restoring the base branch first)
  showed that conclusion was too broad: the base being gone, not the closure itself, is what
  `gh pr reopen`/`gh pr edit --base` refuse. The rule is corrected in place to a judgment call —
  restore-then-reopen when it's worth the extra round trip and the parent's last SHA is still
  resolvable, fresh PR otherwise — rather than quietly swapped. Don't delete branches mid-train remains the cheaper fix either way.
- The manual → stacked transition was one-way by default until a sibling repo's batch
  took the reverse deliberately: an interstitial had grown to several
  commits, so the manual flavour's whole payoff — a merge commit wrapping exactly one reviewed
  commit — was already gone, and switching to the stacked squash train cost nothing further.
  `gh stack link` (not `--adopt`) is the mechanism that repo used and verified safe: it operates
  on the PRs GitHub-side, as they already are, without rewriting local tracking state or branch
  SHAs. Corrected in place to a judgment call rather than an absolute — with the guardrail that repo itself named: take this transition for a real
  structural reason, never to escape a problem, since that is what the one-way *stacked → manual*
  degrade already exists for.
- The only-the-top-PR-is-squash-safe rule makes explicit what the divergence mechanism (`gh-stack.md`,
  "Squash safety") already implied but never stated as a rule: raised when a sibling repo's own
  `[SQUASH]`/`[MERGE-COMMIT]` tagging was flagged as a *possible* instance of this confusion —
  unconfirmed which way that repo actually got it, but the choice clearly wasn't spelled out
  clearly enough for that question to be answerable at a glance. The mechanism was already
  documented; this names the conclusion it leads to, and the manual-flavour exception (restart
  children from fresh post-squash main) was already described for the main-moved and
  showstopper-fix cases — this just states it as the general rule those two are instances of.
- The independent fresh-eyes pass exists because a sibling repo's nine-PR delegated batch went
  through per-PR bot review on every PR, one aggregate bot review, and two orchestrator-run
  reactive rounds — and a fresh-eyes agent over the full diff then found three material problems
  all of that had missed, one of them a regression test that a reactive fix had left passing
  without exercising its branch. The same pass also *cleared* the batch's other claims by tracing
  them, which is what made the readiness report worth trusting.
- The spelled-out diff command exists because the shorthand `...<branch>` doesn't fail when
  copied — it silently diffs from HEAD, truncating the review surface — and the wrapped two-line
  form fails the same way (bare `git diff` is valid). Both were caught in review of the skill text
  itself before either bit a real batch.
- The overlap sweep exists because the same batch shared one file with a neighbouring open PR;
  one comment telling it to adapt after the train cost one API call and prevented a surprise
  conflict — and a parallel session honouring the don't-push-to-others'-branches rule the same
  night is what kept two sessions from colliding on one branch twice in a day.

- The stacked flavour as a default (where a repo chooses it) and the whole `gh stack` reference section came from a
  documentation and CLI audit (Aug 2026, `gh-stack` v0.1.0), **not** from a batch that went wrong —
  flagged as such so nobody cites it as incident evidence. What the audit found that contradicted
  this skill's own earlier text: stacked PRs left beta; `gh pr merge` is *rejected* for stacked PRs
  rather than merely awkward; merging is atomic top-down, which retires the bottom-up walk and the
  retarget-before-delete step for stacked flavour; and `gh stack merge` only exists from v0.1.0.
  The version pin matters — the machine this was written on had v0.0.8, which has no `merge` at all.
- The never-pass-a-bare-number merge procedure is a correction recorded in place:
  this skill's own earlier text said `gh stack merge <followup-pr>`, and a sibling repo's audit of
  the same text first proposed "inspect with `gh stack view --json` before merging" as the guard —
  a non-mitigation, since `view` takes no target and cannot resolve what a bare number would have
  merged. The independent-numbering hazard is observed fact, not inference: a stack number has been
  live while no PR had that number. The checkout-by-URL cycle replaced both wrong drafts.
- The "a returned command is not a finished merge" rule comes from the API contract: branch
  protection and repository rules are evaluated when the merge *runs*, not when it is submitted, so
  an accepted submit proves only that the PR is open and not a draft. This is the same
  false-green family as the review doc's green-signal table — an agent reporting success from the submit is reading
  the instrument, not the output.
- The `enqueued`-is-not-`merged` rule exists because it is a *terminal* status for the merge
  request while being a non-terminal state for the actual merge. A poll loop that stops on
  "not pending" and reports success is correct about the request and wrong about the world.
- The exit-0-on-aborted-sync caution comes from the upstream sync contract: on local/remote
  divergence `gh stack sync` prints both chains, changes nothing, and exits 0. Any wrapper that
  branches on exit status alone reads a refusal as a success.
- The `gh stack push` atomicity note is recorded as an unresolved conflict rather than a fact:
  v0.0.8's `--help` claims `--atomic` all-or-nothing while v0.1.0's reference documents per-branch
  leases that can partially apply. Both cannot be true of the same command; until it is worth
  testing, the safe reading is the weaker one.

- The review targeting in Phases 2, 5 and 6 (cheap self-review per commit, per-commit reads for
  risky classes, one aggregate read briefed with production facts, Codex once at the aggregate head
  and again at every head a fix round produces, a delta read per fix push) comes from an evaluation of every review on
  one repo across about 170 PRs (October 2026). Codex had 96-98% precision but a scarce window; the
  per-PR bot approved heads that held P1s; independent fresh-context reads found about 35 material
  issues, and the deploy and rollback HIGHs came from reads whose brief carried production facts.
  A cheap small-model self-review before opening was the best return per cost of the five reviewers
  on one PR. Codex still found P1s after the reads at least ten times, which is why neither replaces
  the other.
- The Copilot-specific rules (re-request on every push, open the suppressed-comments block) live in
  the review doc beside the Copilot row; that repo dropped Copilot in October 2026, and a repo that
  still runs it keeps them.

- GitHub batches are always `gh stack` trains, and manual stacking moved to its own reference, once
  the owner judged manual stacking no longer needed on GitHub. Before that the manual flavour was an
  operator choice per batch, which an unattended run had no one to make.
- The merge method is the repo's (`AGENTS.md`), not this skill's: the body once hard-coded
  `--squash`, which a user-level copy then imposed on repos with their own rule. A post-merge review
  raised it; squash-merged stacks are the default where a repo states no other rule.

Append this repo's own batch outcomes here as they accumulate.
