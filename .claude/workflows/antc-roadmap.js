export const meta = {
  name: 'antc-roadmap',
  description: 'Implement one milestone of ROADMAP.md: plan, implement, test, review, refactor, commit per step',
  whenToUse: 'Run with args {milestone: "M0".."M4", steps?: [ids], maxIter?: 4, dryRun?: true}',
}

// ---------------------------------------------------------------- step table
const REPO = '/Users/davidklassen/work/davidklassen/ant-src/spec'
const RUN = 'python3 -I src/tests/run.py'
const PY = '--run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}"'
const A0 = '--timeout 600 --run "python3 -I bootstrap/antpy.py run src/antc/main.ant run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run src/antc/main.ant run --unchecked {file} {args}"'
const AN = '--run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}"'
const CN = '--check "bin/antc check {file}" ' + AN
const RPU = '--tier run --tier panic --tier unchecked'
const sec = (...names) => names.map(n => `--filter /${n}/`).join(' ')
const CONC = '--filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer'
const NOCONC = '--exclude /concurrency/ --exclude race-memory-safe --exclude parallel-sum --exclude producer-consumer'

const F01 = sec('smoke', 'lexical', 'variables', 'functions', 'expressions')
const F02 = F01 + ' ' + sec('types', 'nullability', 'errors')
const F03 = F02 + ' ' + sec('stdlib')
const F31 = sec('smoke', 'lexical', 'variables', 'types') + ' --filter programs/smoke'
const F32 = F31 + ' ' + sec('functions', 'expressions', 'stdlib')
const F33 = F32 + ' ' + sec('nullability', 'errors', 'concurrency')

const STEPS = {
  M0: [
    { id: '0.1', title: 'antpy-core', dirs: 'bootstrap/', accept: `${RUN} ${PY} ${RPU} ${F01}` },
    { id: '0.2', title: 'antpy-types-errors', dirs: 'bootstrap/', accept: `${RUN} ${PY} ${RPU} ${F02}` },
    { id: '0.3', title: 'antpy-stdlib', dirs: 'bootstrap/', accept: `${RUN} ${PY} ${RPU} ${F03}` },
    { id: '0.4', title: 'antpy-concurrency', dirs: 'bootstrap/', accept: `${RUN} ${PY} ${RPU} --tier programs ${F03} ${CONC}` },
    { id: '0.5', title: 'antpy-modules', dirs: 'bootstrap/', accept: `${RUN} ${PY} ${RPU} --tier programs ${F03} ${CONC} --filter /modules/ --filter programs/` },
    { id: '0.6', title: 'antpy-io', dirs: 'bootstrap/', accept: `${RUN} ${PY} ${RPU} --tier programs --tier io ${F03} ${CONC} --filter /modules/ --filter programs/ --filter io/` },
    { id: '0.G', title: 'bootstrap gate', dirs: 'bootstrap/', gate: true, accept: `${RUN} ${PY} --consistency --tier run --tier panic --tier unchecked --tier programs --tier io` },
  ],
  M1: [
    { id: '1.1', title: 'antc-lexer', dirs: 'src/antc/, tools/compare_frontend.py', accept: 'python3 -I tools/compare_frontend.py lex --sample' },
    { id: '1.2', title: 'antc-parser', dirs: 'src/antc/, tools/compare_frontend.py', accept: 'python3 -I tools/compare_frontend.py parse --sample' },
    { id: '1.G', title: 'frontend gate', dirs: 'src/antc/, tools/compare_frontend.py', gate: true, accept: 'python3 -I tools/compare_frontend.py lex --all && python3 -I tools/compare_frontend.py parse --all' },
  ],
  M2: [
    { id: '2.1', title: 'rt-core', dirs: 'runtime/, src/antc/, Makefile', accept: `${RUN} ${A0} ${RPU} ${F01}` },
    { id: '2.2', title: 'rt-types-errors', dirs: 'runtime/, src/antc/, Makefile', accept: `${RUN} ${A0} ${RPU} ${sec('smoke', 'types', 'nullability', 'errors')}` },
    { id: '2.3', title: 'rt-stdlib', dirs: 'runtime/, src/antc/, Makefile', accept: `${RUN} ${A0} ${RPU} ${sec('smoke', 'stdlib')}` },
    { id: '2.4', title: 'rt-modules', dirs: 'runtime/, src/antc/, Makefile', accept: `${RUN} ${A0} ${RPU} --tier programs ${sec('smoke', 'modules')} --filter programs/ ${NOCONC}` },
    { id: '2.5', title: 'self-host', dirs: 'runtime/, src/antc/, Makefile', accept: `make fixpoint && ${RUN} ${AN} ${RPU} --tier programs ${NOCONC}` },
    { id: '2.6', title: 'rt-concurrency', dirs: 'runtime/, src/antc/, Makefile', accept: `make fixpoint && ${RUN} ${AN} ${RPU} --tier programs ${CONC}` },
    { id: '2.7', title: 'rt-io', dirs: 'runtime/, src/antc/, Makefile', accept: `make fixpoint && ${RUN} ${AN} --tier io` },
    { id: '2.G', title: 'runtime gate', dirs: 'runtime/, src/antc/, Makefile', gate: true, accept: `make fixpoint && ${RUN} ${AN} --consistency --tier run --tier panic --tier unchecked --tier programs --tier io` },
  ],
  M3: [
    { id: '3.1', title: 'check-core', dirs: 'src/antc/, Makefile', accept: `make fixpoint && ${RUN} ${CN} --tier check ${RPU} --tier programs ${F31}` },
    { id: '3.2', title: 'check-inference', dirs: 'src/antc/, Makefile', accept: `make fixpoint && ${RUN} ${CN} --tier check ${RPU} --tier programs ${F32}` },
    { id: '3.3', title: 'check-flow', dirs: 'src/antc/, Makefile', accept: `make fixpoint && ${RUN} ${CN} --tier check ${RPU} --tier programs ${F33}` },
    { id: '3.4', title: 'check-modules', dirs: 'src/antc/, Makefile', accept: `make fixpoint && ${RUN} ${CN} --tier check ${RPU} --tier programs --tier io && bin/antc check src/antc/main.ant` },
    { id: '3.G', title: 'checker gate', dirs: 'src/antc/, Makefile', gate: true, accept: `make fixpoint && ${RUN} ${CN} --consistency` },
  ],
  M4: [
    { id: '4.1', title: 'final-polish', dirs: 'bootstrap/, runtime/, src/antc/, Makefile, tools/compare_frontend.py', accept: `make fixpoint && ${RUN} ${CN} --consistency && python3 -I tests/run_tests.py` },
  ],
}

// ---------------------------------------------------------------- schemas
const PLAN = { type: 'object', properties: {
  summary: { type: 'string' }, files: { type: 'array', items: { type: 'string' } }, risks: { type: 'array', items: { type: 'string' } },
}, required: ['summary', 'files', 'risks'] }
const IMPL = { type: 'object', properties: {
  status: { type: 'string', enum: ['green', 'red', 'stuck'] }, summary: { type: 'string' },
  testChanges: { type: 'array', items: { type: 'string' } },
}, required: ['status', 'summary', 'testChanges'] }
const TEST = { type: 'object', properties: {
  pass: { type: 'boolean' }, summaryLine: { type: 'string' },
  failures: { type: 'array', items: { type: 'string' } }, scopeViolations: { type: 'array', items: { type: 'string' } },
}, required: ['pass', 'summaryLine', 'failures', 'scopeViolations'] }
const REVIEW = { type: 'object', properties: {
  blocking: { type: 'array', items: { type: 'object', properties: { file: { type: 'string' }, issue: { type: 'string' } }, required: ['file', 'issue'] } },
  refactors: { type: 'array', items: { type: 'string' } }, testChangesOk: { type: 'boolean' }, notes: { type: 'string' },
}, required: ['blocking', 'refactors', 'testChangesOk', 'notes'] }
const DONE = { type: 'object', properties: { ok: { type: 'boolean' }, summary: { type: 'string' } }, required: ['ok', 'summary'] }

// ---------------------------------------------------------------- prompts
const RULES = `Repository: ${REPO}. Run every command from there. Read ROADMAP.md first: it defines the steps, directories, command-line contracts, and rules; for M2+ steps also read docs/runtime-design.md and follow it. The language spec is ant-lang.md and stdlib.md; the test contract (tiers, exit codes, 'panic: <message>' on stderr, checker report format, canonical phrases) is src/tests/README.md; src/tests/GAPS.md lists the readings the tests assume.
Hard rules:
- Never modify: ant-lang.md, stdlib.md, grammar.ebnf, tools/antparse.py, src/tests/run.py, src/tests/oracle.py, src/tests/README.md, src/tests/lib/, ROADMAP.md, docs/, .claude/.
- Never modify a test under src/tests unless its expectation contradicts the spec; then fix it minimally, cite the section, and append a line "file | change | citation" to work/TEST-CHANGES.md. Never delete, skip, or weaken a test.
- Never special-case test files, names, or outputs in the implementation. antc and the runtime may execute nothing but clang and the C toolchain. No network.
- Write only inside the step's directories and work/<step-id>/. Do not commit, stash, rebase, or switch branches.
- The acceptance command is the definition of done: it must exit 0 and its last line must report 0 failures. If a command may take longer than 10 minutes, run it in the background with output redirected to work/<step-id>/test.log and wait for it to finish.`

const header = (step) => `Roadmap step ${step.id} "${step.title}" (see its row in ROADMAP.md). Allowed directories: ${step.dirs} (plus work/${step.id}/).
Acceptance command (verbatim):
${step.accept}
`

const planner = (step) => `You are the planner for one roadmap step.
${RULES}

${header(step)}
Read the step's row, the spec sections its deliverable names, the test files its acceptance command selects (look at a representative sample of each section), the current state of the allowed directories, and every existing work/*/report.md to learn what earlier steps built and decided. Then write work/${step.id}/plan.md: the files to create or change, the approach (data structures, module layout, the order to tackle the test sections), spec rules that are easy to get wrong, and risks. Keep it to the point: an implementer will follow it literally. Do not implement anything.
Return JSON: summary (3-5 sentences), files (paths you expect to create or change), risks.`

const implementer = (step, plan, feedback, iteration) => `You are the implementer for one roadmap step${iteration ? ` (round ${iteration + 1})` : ''}.
${RULES}

${header(step)}
${plan ? `The plan is in work/${step.id}/plan.md (summary: ${plan.summary}). Follow it; deviate only with a reason written in your report.` : 'This is a gate step: there is no plan. Make the acceptance command pass.'}
${feedback ? `Feedback from the previous round, which you must address first:\n${feedback}\n` : ''}
Work method: implement incrementally; use the runner's --filter / --tier options to iterate on single sections or tests, then run the full acceptance command. Read a failing test and the spec section it cites before changing code; the test is right unless it contradicts the spec. Keep the code plain and readable: a later milestone rewrites some of it in Ant, and reviewers read all of it.
When done (or stuck after a serious attempt), write work/${step.id}/report.md: what you built, decisions and deviations, the exact acceptance output's last line, test changes with citations (if any), known limitations.
Return JSON: status (green = acceptance passed; red = still failing; stuck = you cannot make progress), summary, testChanges (one entry per changed test file, or empty).`

const tester = (step) => `You are the independent tester for one roadmap step. You verify; you change nothing.
${RULES}

${header(step)}
Do exactly this:
1. Run the acceptance command verbatim from the repository root (in the background with a log file if it may exceed 10 minutes) and capture its full output and exit status.
2. pass is true only if the exit status is 0 and the last line reports 0 failures.
3. Run 'git status --short' and report, as scopeViolations, any changed or new path outside the allowed directories and work/${step.id}/ (changes under src/tests count as violations unless work/TEST-CHANGES.md documents them).
Return JSON: pass, summaryLine (the runner's last line, or the error if it did not run), failures (up to 30 entries, each the test path and the first lines of its failure), scopeViolations.`

const reviewer = (step, plan) => `You are the independent reviewer for one roadmap step. You read; you change nothing.
${RULES}

${header(step)}
The step's changes are the uncommitted working-tree changes: run 'git status --short' and 'git diff', and read new files in full. ${plan ? `Compare against work/${step.id}/plan.md.` : ''} Check, against the spec and docs/runtime-design.md where relevant:
- Correctness of the semantics implemented, especially section 12 rules and the canonical phrases; evaluation order; unwinding; scoping.
- Cheating: special-casing of test names or outputs, hard-coded expected values, stubs that only satisfy the selected tests, skipped checks.
- Scope: edits outside the allowed directories; edits under src/tests (each must be justified in work/TEST-CHANGES.md with a spec citation; verify the citation).
- Code quality that would hurt later steps: duplicated logic, unclear structure, missing error handling at the boundaries the step owns.
Return JSON: blocking (findings that must be fixed before the step is accepted, with file and issue; empty if none), refactors (safe improvements worth doing now, as instructions; keep to the few that matter), testChangesOk (true when every test change is justified, or there are none), notes.`

const refactorer = (step, refactors) => `You are the refactorer for one roadmap step. The tree is green; your job is to leave it green and simpler.
${RULES}

${header(step)}
1. Run the 'simplify' skill (Skill tool, name "simplify") on the step's changes: the uncommitted working tree ('git status --short', 'git diff'). It reviews the changed code for reuse of existing helpers, simplification, efficiency, and altitude cleanups and applies the fixes. If the Skill tool is not available to you, apply that recipe yourself: look for duplicated logic that an existing helper covers, code that can be shorter without losing clarity, needless work in hot paths, and abstractions at the wrong level.
${refactors.length ? `2. Then apply these reviewer suggestions, one at a time, without changing behavior:\n${refactors.map((r, i) => `   ${i + 1}. ${r}`).join('\n')}` : '2. The reviewer had no further suggestions.'}
3. After each change run a representative --filter subset of the acceptance command, and the full command at the end. If the full acceptance command is not green after your changes, revert your own edits (git checkout -- <files you touched>; delete files you added) so the tree is back to the green state, and say so. Append a short "Refactor" section to work/${step.id}/report.md listing what changed.
Return JSON: ok (true when the tree is green at the end), summary.`

const committer = (step) => `Commit the completed roadmap step ${step.id} "${step.title}" in ${REPO}.
Run from the repository root: ensure the current branch is 'antc' (if not, 'git checkout antc', creating it from the current HEAD with 'git checkout -b antc' if it does not exist); then 'git add -A' and 'git commit -m "${step.id} ${step.title}" -m "Acceptance: ${step.accept.replace(/"/g, '\\"')}" -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"'. If there is nothing to commit, say so. Do not push.
Return JSON: ok, summary (the commit hash and the one-line stat).`

const blocker = (step, feedback) => `In ${REPO}, write work/${step.id}/BLOCKED.md for roadmap step ${step.id} "${step.title}": the acceptance command, the last feedback below, and a short list of what a human should look at first. Do not change anything else.
Last feedback:
${feedback}
Return JSON: ok, summary.`

// ---------------------------------------------------------------- driver
const milestone = (args && args.milestone) || 'M0'
const maxIter = (args && args.maxIter) || 4
const only = args && args.steps
const dryRun = !!(args && args.dryRun)
const steps = (STEPS[milestone] || []).filter(s => !only || only.includes(s.id))
if (!steps.length) throw new Error(`no steps for milestone ${milestone}`)

const results = []
let blocked = null
for (const step of steps) {
  const ph = `${step.id} ${step.title}`
  phase(ph)
  log(`▶ ${step.id} ${step.title}`)
  if (dryRun) {
    log(`   accept: ${step.accept}`)
    results.push({ id: step.id, status: 'dry-run' })
    continue
  }
  let plan = null
  if (!step.gate) plan = await agent(planner(step), { schema: PLAN, label: `plan ${step.id}`, phase: ph })

  let feedback = ''
  let green = false
  let iter = 0
  for (; iter < maxIter && !green; iter++) {
    let test
    if (step.gate && iter === 0) {
      test = await agent(tester(step), { schema: TEST, label: `test ${step.id} #${iter + 1}`, phase: ph })
    } else {
      const impl = await agent(implementer(step, plan, feedback, iter), { schema: IMPL, label: `implement ${step.id} #${iter + 1}`, phase: ph })
      if (!impl) log(`   implementer returned nothing`)
      else if (impl.status !== 'green') log(`   implementer: ${impl.status}: ${impl.summary}`)
      test = await agent(tester(step), { schema: TEST, label: `test ${step.id} #${iter + 1}`, phase: ph })
    }
    if (!test) { feedback = 'The tester agent produced no result; rerun the acceptance command yourself and fix what fails.'; continue }
    if (!test.pass || test.scopeViolations.length) {
      feedback = `Tester result: ${test.summaryLine}\n` +
        (test.failures.length ? `Failures:\n${test.failures.map(f => '- ' + f).join('\n')}\n` : '') +
        (test.scopeViolations.length ? `Scope violations (revert or justify):\n${test.scopeViolations.map(v => '- ' + v).join('\n')}\n` : '')
      log(`   red: ${test.summaryLine}`)
      continue
    }
    const review = await agent(reviewer(step, plan), { schema: REVIEW, label: `review ${step.id} #${iter + 1}`, phase: ph })
    if (review && (review.blocking.length || !review.testChangesOk)) {
      feedback = `Reviewer blocking findings:\n${review.blocking.map(b => `- ${b.file}: ${b.issue}`).join('\n')}` +
        (review.testChangesOk ? '' : '\n- Test changes under src/tests are not justified against the spec: revert them or document the citation in work/TEST-CHANGES.md.')
      log(`   review blocked: ${review.blocking.length} finding(s)`)
      continue
    }
    if (!step.gate) {
      const r = await agent(refactorer(step, review ? review.refactors : []), { schema: DONE, label: `refactor ${step.id}`, phase: ph, agentType: 'general-purpose' })
      const retest = await agent(tester(step), { schema: TEST, label: `retest ${step.id}`, phase: ph })
      if (!retest || !retest.pass || retest.scopeViolations.length) {
        feedback = `After refactoring the acceptance command is red: ${retest ? retest.summaryLine : 'no tester result'}. Restore a green tree.`
        log(`   refactor left the tree red${r ? ': ' + r.summary : ''}`)
        continue
      }
    }
    green = true
  }

  if (!green) {
    await agent(blocker(step, feedback), { schema: DONE, label: `blocked ${step.id}`, phase: ph, effort: 'low' })
    blocked = { id: step.id, feedback }
    results.push({ id: step.id, status: 'blocked', iterations: iter })
    log(`✖ ${step.id} blocked after ${iter} round(s); see work/${step.id}/BLOCKED.md`)
    break
  }
  const commit = await agent(committer(step), { schema: DONE, label: `commit ${step.id}`, phase: ph, effort: 'low' })
  results.push({ id: step.id, status: 'done', iterations: iter, commit: commit ? commit.summary : null })
  log(`✔ ${step.id} done in ${iter} round(s)`)
}

return { milestone, steps: results, blocked }
