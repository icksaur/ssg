# Working process

## Roles

- **Parent / orchestrator:** owns scheduling, integration, test gates, and the
  milestone.
- **Child / implementer:** owns one task branch and its complete inner
  development loop.

`master` is the integration branch and integration worktree. All task branches
start from a recorded dependency-complete commit on `master`.

## Baseline prerequisite

Before creating the first child, the parent stages and commits every reviewed
planning artifact, including `process.md`, `copilot-instructions.md`,
`cpp-lib-values.md`, `code-quality.md`, `README.md`, `CMakeLists.txt`,
`doc/**`, `tasks/**`, public scaffolding, and tests. Untracked files do not
appear in worktrees.

The parent verifies required files are tracked and records:

```sh
git ls-files --error-unmatch process.md copilot-instructions.md \
    cpp-lib-values.md code-quality.md README.md CMakeLists.txt \
    doc/spec.md tasks/plan.md
git rev-parse master
```

No task starts until that commit contains every file its child is required to
read.

## Parent loop

1. Read `tasks/plan.md` and identify tasks whose dependencies are merged.
2. Record the dependency-complete base with `BASE_SHA=$(git rev-parse master)`.
3. Create the branch/worktree explicitly:

   ```sh
   git worktree add -b <slug>-task ../ssg-<slug> "$BASE_SHA"
   ```

4. Call `caco_herd create` with `cwd` set to the absolute worktree path, the
   chosen model, and the dispatch prompt defined below. `caco_herd` creates a
   session, not a git worktree.
5. Dispatch independent tasks in parallel up to the configured concurrency cap;
   the default is four active implementers. Serialize sanitizer and
   resource-intensive browser matrices when host pressure would make failures
   ambiguous.
6. Use `caco_herd_state` to collect terminal child responses. Only responses
   matching the terminal envelope below are terminal.
7. For `BLOCKED` or `UNCLEAR`, inspect the evidence, resolve the issue when
   possible, and use `caco_herd resume` with concrete guidance. The same child
   retains ownership. After two unsuccessful resumes for the same unresolved
   issue, mark the task parked in parent state, release exclusive resources,
   surface the decision to the user, and continue independent tasks. The
   child's last status remains `BLOCKED` or `UNCLEAR`; `PARKED` is not a child
   response.
8. For implementation `DONE`, verify the branch and reviewed commit SHA from
   the envelope, run every task and integration gate, and merge the task branch
   into `master` only when green.
9. If dependency changes require a rebase, resume the same child to rebase onto
   the new `master`, rerun gates, obtain a new Opus 4.8 diff review, and return a
   new `DONE` SHA. A reviewed pre-rebase commit is not mergeable.
10. Run the relevant gates again on merged `master`. Resume the same child for
    integration failures within task scope; create a dedicated integration-fix
    task when the failure crosses task boundaries.
11. After the reviewed merge is green, resume the same child and ask explicitly
    whether the task uncovered notable project tribal knowledge. Require a
    learnings `DONE` envelope. Triage durable cross-task findings into
    `doc/learnings.md`; promote invariant discoveries into
    `copilot-instructions.md` or `doc/spec.md` as appropriate. Do not record
    task summaries, transient state, or speculation.
12. Commit any triaged documentation update on `master`, then disown the child,
    remove its worktree with `git worktree remove`, and delete its merged local
    task branch.
13. Dispatch a new child/worktree for each newly ready task. Existing children
    are bound to their original cwd and are not repurposed.
14. Repeat until every task in the current milestone is merged and its
    milestone acceptance gates pass.

The parent owns dependency ordering and integration decisions. Children do not
merge their own branches or modify another child's worktree.

The parent does not run `git gc`, `git prune`, or destructive repository cleanup
while child worktrees exist. Worktrees share the repository object store, so
merging local child branches requires no push or fetch.

## Child dispatch prompt

Every `caco_herd create` prompt includes the absolute worktree, slug, branch,
base SHA, task file, and this contract:

```text
You are the implementer for <slug>.
Work only in <absolute-worktree> on branch <slug>-task based at <base-sha>.
Read process.md, copilot-instructions.md, tasks/plan.md, tasks/<slug>.md,
doc/spec.md, doc/learnings.md, and the referenced feature spec. Execute the complete child loop.
Do not send progress reports. Your final response must be exactly one
machine-parseable line using one terminal envelope from process.md.
```

Terminal envelopes are one line:

```text
DONE {"task":"<slug>","phase":"implementation","branch":"<slug>-task","sha":"<40-hex-reviewed-commit>"}
BLOCKED {"task":"<slug>","reason":"<reason>","attempted":"<research-and-fixes>","need":"<resource-or-decision>"}
UNCLEAR {"task":"<slug>","ambiguity":"<question>","options":"<options>","recommendation":"<best-supported-choice>"}
DONE {"task":"<slug>","phase":"learnings","learnings":["<durable-project-learning>"]}
```

An idle, inactive, errored, or free-form child response is not terminal. The
parent resumes that child with the envelope requirement.

For the learnings follow-up, an empty `learnings` array is valid. The parent,
not the child, decides what belongs in `doc/learnings.md`.

## Shared HTTP repository

`http-cross-platform` is a paired-worktree exception. The parent records the
current `../http` integration branch and SHA, then creates:

```sh
HTTP_BRANCH=$(git -C /home/carl/repo/http branch --show-current)
HTTP_BASE_SHA=$(git -C /home/carl/repo/http rev-parse HEAD)
SSG_BASE_SHA=$(git rev-parse master)
mkdir -p <task-root>
git worktree add -b http-cross-platform-task <task-root>/ssg "$SSG_BASE_SHA"
git -C /home/carl/repo/http worktree add \
    -b http-cross-platform-task <task-root>/http "$HTTP_BASE_SHA"
```

Both repositories use branch `http-cross-platform-task`; the child's cwd is the
SSG worktree, so its relative `../http` resolves to the isolated HTTP worktree.
The original `/home/carl/repo/http` checkout remains pinned for other children.

On `DONE`, the parent runs HTTP gates, merges the HTTP task branch into
`$HTTP_BRANCH`, records the resulting HTTP commit in
`tasks/dependencies/http.commit` on SSG `master`, then gates and merges the SSG
task branch. No downstream HTTP consumer starts before both integrations are
complete.

## Child loop

1. Read `copilot-instructions.md`, `process.md`, `doc/learnings.md`, the
   assigned task file, its referenced feature spec, and `doc/spec.md`.
2. Perform the task/spec sanity review required by `tasks/plan.md`.
3. Write the specified oracle or unit test first.
4. Implement the complete task scope.
5. Run the task tests and all applicable project gates.
6. Request a Claude Opus 4.8 diff review with the `task` tool.
7. Fold warranted review findings and rerun the gates.
8. Commit the complete reviewed task to its `<slug>-task` branch.
9. Return exactly one machine-parseable implementation envelope defined above:
   `DONE`, `BLOCKED`, or `UNCLEAR`.
10. After the parent merges and asks for tribal knowledge, inspect the completed
    investigation and return the learnings `DONE` envelope. Do not edit
    `doc/learnings.md`; triage belongs to the parent.

Before returning `BLOCKED` or `UNCLEAR`, the child must attempt to resolve the
issue independently. It dispatches focused research tasks with the `task` tool,
examines local code and documentation, and applies established project
contracts and best practices. `BLOCKED` and `UNCLEAR` envelopes include concise
evidence, what was attempted, and the exact decision or resource needed.

Children do not send progress reports. They continue the inner loop until they
can report `DONE`, `BLOCKED`, or `UNCLEAR`, and answer the required post-merge
learnings prompt before being disowned.

## Completion

The process continues through ready tasks and parallel waves until the current
milestone is complete on `master`. A milestone is complete only when all of its
task branches are merged, children are disowned, and the milestone acceptance
and test gates pass.
