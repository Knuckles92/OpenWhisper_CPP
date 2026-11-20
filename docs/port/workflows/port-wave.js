export const meta = {
  name: 'port-wave',
  description: 'Build one wave of port work packages: each is implemented in its own git worktree, then independently reviewed against the spec and the Python source and fixed',
  phases: [
    { title: 'Implement', detail: 'one worker per package, each in its own worktree and build directory' },
    { title: 'Review', detail: 'an independent reviewer rebuilds, checks the package against spec and Python, and fixes what it finds', model: 'opus' },
    { title: 'Second pass', detail: 'only for packages the reviewer could not get ready', model: 'opus' },
    { title: 'Integrate', detail: 'squash-merges the ready packages into port/parity, applies the lead fixes, runs the merge gate, cleans up', model: 'opus' },
    { title: 'Linux check', detail: 'pushes to the Linux host and runs the core gate there, when asked', model: 'sonnet' },
  ],
}

// args: { wave: string, concurrency?: number,
//         packages: [{ id, name, model, effort?, spec, archSections,
//                      resumeFrom?: '<worktree of an interrupted implementer>',
//                      reviewOnly?: true (wp/<id> already holds the implementation) }] }
// after?: [ids of packages of the same wave]. The package starts once those are ready, merges their
// wp/ branches into its worktree first, and marks that state as branch base/<id>. Merge those
// packages into port/parity before this one.
// base?: a branch or commit the package's own changes are measured from, when the lead has
// already merged port/parity into wp/<id> (a package reviewed after its wave was merged).
// priority?: a number; when agents wait for a free slot, the highest goes first (default 0). A
// review or second pass always goes before a waiting implementer.
//
// integrate?: { order?: [ids], resultsFile?: '<JSON of an earlier run of this script>',
//               leadWork?: string, gateJobs?: number, linux?: true }
// With it, once the packages are reviewed one agent does the lead's mechanical work in the main
// checkout: merge, lead fixes, gate, clean-up. With no packages and order + resultsFile it
// integrates a wave that ran earlier. linux: true then runs the Linux core gate on the host.
//
// integrate.stages?: [[ids], ...] and stageWork?: [string, ...] integrate in steps; debug: true
// adds a Debug build and test after the last gate. In integrated waves, workers run the affected
// package checks and the integration stage runs the full windows-ci gate. Standalone waves run it per package.
// jobs?: build and test jobs per worker (default 4). maxJobs?: machine-wide budget (default 8).
// Simultaneous workers are limited to floor(maxJobs / jobs); an isolated gate can use maxJobs.
//
// The repository is on the machine's NVMe SSD (it was on D:, a hard disk that one build
// saturated). .claude\worktrees must stay a real folder: the harness refuses to create
// worktrees through a link. It also creates them from the session's folder, so the session
// must stay in the repository for as long as a run is going: moved elsewhere, every worker
// that starts afterwards fails. A windows-ci build tree is about 20 GB and C: is the system
// drive, so the runner frees a ready package's tree after review. An interrupted or unsuccessful
// review keeps its cache in addition to the trees of active workers.
//
// Or args: { concurrency?, waves: [{ wave, packages, integrate }] } to run waves back to back. The
// chain stops after a wave whose gate is not green or that left a package not ready.

const OW = 'D:\\coding\\whisper_local'

function jobCount(value, fallback, name) {
  const count = value ?? fallback
  if (!Number.isSafeInteger(count) || count < 1) throw new Error(name + ' must be a positive integer')
  return count
}

const MAX_JOBS = jobCount(args.maxJobs, 8, 'maxJobs')
const JOBS = Math.min(jobCount(args.jobs, 4, 'jobs'), MAX_JOBS)

// The harness starts a worktree at the repository's default branch (main, the first commit,
// since the repository has a remote), not at the session's branch.
const WHERE_FRESH = 'Where you are: a fresh git worktree of the OpenWhisper_CPP repository, on your own branch. Your current directory is that worktree. The harness may have started it at the repository\'s default branch, which holds almost nothing: before anything else run "git reset --hard port/parity" there (it has no work of yours yet) and check that docs/port/ARCHITECTURE.md exists. Do all work inside your worktree. Never modify the main checkout at C:\\coding\\OpenWhisper_CPP outside your worktree, never switch or move the port/parity branch, never push.'

const COMMON = `OpenWhisper_CPP is a C++23 / Qt 6 Widgets rewrite of OpenWhisper (Python 3.12 + PyQt6) aiming at full feature parity: same features, same behavior, same look. The OpenWhisper checkout at ${OW} is the behavior reference and is READ-ONLY: never create, edit or delete anything under it, never start that app, never run git or any other command inside it that can write (even "git status" there refreshes its index), and do not read or quote the owner's live data in its root (openwhisper_settings.json, openwhisper.db, logs, recordings, meetings, .env).

@@WHERE@@

Building in your worktree (PowerShell):
  . .\\scripts\\vsenv.ps1
  cmake --preset windows-ci
  cmake --build --preset windows-ci               (add --target <target> for a faster loop)
  ctest --preset windows-ci                       (add -L <label> or -R <regex> for a faster loop)
Use that one preset for everything. It is the merge gate's own configuration (RelWithDebInfo, warnings as errors, the lints), so checks reuse the objects already built in this worktree. Do not configure or build the windows or windows-debug presets: a second build tree doubles the compile time for nothing.
Several workers share this machine, and unlimited parallel builds have already exhausted its memory once (MSVC C1076 and C3859, then a crashed session). In every shell you build or test from, first set $env:CMAKE_BUILD_PARALLEL_LEVEL = '${JOBS}' and $env:CTEST_PARALLEL_LEVEL = '${JOBS}', and never pass a higher -j or --parallel. If a compile fails with C1076, C3859 or "out of memory", it is the machine, not your code: wait a minute and run it again.
Scratch files (build scripts, logs, probes) go in a folder of your own under your worktree's out/ directory, which git ignores; never in a temp folder another worker might share.
The repository and every worktree are on the machine's NVMe SSD, which is also the owner's system drive, and a windows-ci build tree is about 20 GB: keep to the one build tree in your worktree's out folder, copy none, and write nothing else large. The out folder is an ordinary folder; create no link (junction or symbolic link) anywhere. An older copy of the repository at D:\\coding\\OpenWhisper_CPP is not yours to read from or write to: everything you need is in your worktree.
If a build fails with C1083 on a "<name>.moc" file after you added a .cpp that includes its own .moc, delete the timestamp_* files under that build directory's autogen folders and build again; it is not your code. Every target compiles with QT_NO_KEYWORDS: in Qt code write Q_SIGNALS:, public Q_SLOTS: and Q_EMIT, never signals:, slots: or emit (those are ordinary names here, as in fv::Signal::emit).
The first configure in a worktree restores the vcpkg libraries from the machine's binary cache; a library not yet in the cache is built from source, which for FFmpeg, Boost and OpenSSL can take more than an hour in total. Run long commands in the background and wait for them; never abandon or kill a vcpkg build because it is slow. Never run a test labelled interactive (they take over the owner's keyboard, microphone, speakers and clipboard): run ctest only through a test preset, all of which exclude that label except windows-interactive, which you never use, or pass -LE interactive yourself. Never start the GUI app, and never start a widget test executable directly: run tests through ctest, which sets the offscreen display plugin (started bare, a failure puts a dialog on the owner's screen).

Tests that decode real audio need these environment variables, which are valid on this machine:
  $env:OPENWHISPER_CPP_TEST_RUNTIME_DIR = "$env:LOCALAPPDATA\\OpenWhisper\\components\\asr-nvidia-cuda"
  $env:OPENWHISPER_CPP_TEST_MODEL = "$env:LOCALAPPDATA\\OpenWhisper\\speech-models\\parakeet-v3\\parakeet-tdt-0.6b-v3.q8_0.gguf"
  $env:OPENWHISPER_CPP_TEST_WAV = "${OW}\\recorded_audio.wav"
  $env:OPENWHISPER_CPP_TEST_PDFIUM = "${OW}\\.venv\\Lib\\site-packages\\pypdfium2_raw\\pdfium.dll"
  $env:OPENWHISPER_CPP_TEST_ONNXRUNTIME = "${OW}\\.venv\\Lib\\site-packages\\onnxruntime\\capi\\onnxruntime.dll"
  $env:OPENWHISPER_CPP_TEST_SPEAKER_MODEL = "$env:LOCALAPPDATA\\OpenWhisper\\models\\speaker-id\\voxceleb_resnet34_LM.onnx"
Those files are the owner's installed copies: read them, never modify or delete them. The last three matter only to the PDF text tests and the speaker embedding tests, which skip without them.
Every test executable starts with the per-user folders of its own process pointed at a new temporary folder (cmake/fv_test_isolation.cpp, compiled into each by fv_test), so a test cannot reach the owner's real %LOCALAPPDATA%\\OpenWhisper_CPP or OpenWhisper data by accident. Do not undo that in a test: a test that needs a per-user folder builds it under its own temporary folder and passes it in, or moves the variables further with the scoped helpers in tests/support.`

function common(where) {
  return COMMON.replace('@@WHERE@@', () => where)
}

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    id: { type: 'string' },
    branch: { type: 'string', description: 'wp/<id>' },
    sha: { type: 'string', description: 'git rev-parse HEAD after your final commit' },
    worktree: { type: 'string', description: 'the absolute path of your worktree: the output of "git rev-parse --show-toplevel"' },
    summary: { type: 'string', description: 'at most 120 words: what now exists' },
    targets: { type: 'array', items: { type: 'string' } },
    filesChanged: { type: 'number' },
    linesAdded: { type: 'number' },
    testsTotal: { type: 'number', description: 'from the checks run for this package' },
    testsPassed: { type: 'number' },
    testsFailed: { type: 'number' },
    testsSkipped: { type: 'number' },
    notDone: { type: 'array', items: { type: 'string' }, description: 'every item of the spec that is missing, partial or stubbed' },
    deviations: { type: 'array', items: { type: 'string' }, description: 'every place you departed from the spec or the architecture, and why' },
    blockers: { type: 'array', items: { type: 'string' }, description: 'changes needed outside your owned files, stated exactly' },
    findings: { type: 'array', items: { type: 'string' }, description: 'facts the lead should know: checked claims, measurements, surprises (up to 12)' },
  },
  required: ['id', 'branch', 'sha', 'worktree', 'summary', 'testsTotal', 'testsPassed', 'testsFailed', 'notDone', 'deviations', 'blockers'],
}

const REVIEW_SCHEMA = {
  type: 'object',
  properties: {
    id: { type: 'string' },
    verdict: { type: 'string', enum: ['ready', 'ready-with-notes', 'not-ready'] },
    sha: { type: 'string', description: 'git rev-parse HEAD after your final commit (the tip of wp/<id>)' },
    summary: { type: 'string', description: 'at most 100 words' },
    buildClean: { type: 'boolean', description: 'the required build targets configured and built successfully in this worktree' },
    testsTotal: { type: 'number' },
    testsPassed: { type: 'number' },
    testsFailed: { type: 'number' },
    testsSkipped: { type: 'number' },
    fixed: { type: 'array', items: { type: 'string' }, description: 'defects you found and fixed (up to 15)' },
    open: {
      type: 'array',
      description: 'defects or gaps still open',
      items: {
        type: 'object',
        properties: {
          severity: { type: 'string', enum: ['high', 'medium', 'low'] },
          what: { type: 'string' },
          where: { type: 'string' },
        },
        required: ['severity', 'what', 'where'],
      },
    },
    ownershipViolations: { type: 'array', items: { type: 'string' }, description: 'changed files outside what the spec lets the package own' },
  },
  required: ['id', 'verdict', 'sha', 'summary', 'buildClean', 'testsTotal', 'testsPassed', 'testsFailed', 'fixed', 'open'],
}

function hasDeps(p) {
  return Array.isArray(p.after) && p.after.length > 0
}

// The commit a package's own changes are measured from.
function baseOf(p) {
  if (p.base) return `"git diff ${p.base} HEAD" (${p.base} is the commit the package's own changes are measured from; everything before it is other packages' merged work)`
  if (hasDeps(p)) return `"git diff base/${p.id} HEAD" (branch base/${p.id} is the commit the package started from, after it merged the packages it builds on)`
  return '"git diff <base> HEAD", where <base> is the output of "git merge-base port/parity HEAD" (the commit the package started from)'
}

function afterNote(p) {
  if (!hasDeps(p)) return ''
  const branches = p.after.map(id => `wp/${id}`).join(', ')
  return `Your package builds on ${p.after.join(', ')}, finished and reviewed earlier in this wave but not yet merged into port/parity. Before anything else, in your worktree: ${p.after.map(id => `"git merge --no-edit wp/${id}"`).join(', then ')}, then "git branch -f base/${p.id} HEAD". Everything those merges brought (${branches}) belongs to those packages, not to you: do not change it, and measure your own changes with "git diff base/${p.id} HEAD", not from port/parity.

`
}

function continuationNote(p) {
  if (!p.resumeFrom) return ''
  return `You are not starting from nothing. An earlier worker on this package was interrupted by a crash well into the job, before it committed anything. Its uncommitted work is still in the worktree at ${p.resumeFrom}. Read it there; do not build, test or change anything inside that folder.
- Start by bringing that work into your own worktree: "git -C ${p.resumeFrom} status --short -uall" lists what it added, changed, renamed and deleted relative to the commit it started from; copy the added and changed files to the same paths here and repeat its renames and deletions. Skip anything under out/ or build/. Your worktree starts from a newer commit of port/parity than that work did, so where a file it changed has also changed here, merge by hand instead of overwriting.
- Then treat what you copied as an unreviewed draft by someone else. You do not know how far it got, whether it builds, or whether its tests pass or mean anything. Read all of it against the spec and the Python before trusting any of it, keep what is right, fix what is wrong, and finish what is missing. Generated files (golden vectors, reference images, tables) are only trustworthy if regenerating them with the harness gives the same result.

`
}

// A reviewer works in the implementer's worktree when the report names it: the tree is built
// there, so the review compiles only what it changes. Its own scratch worktree stays unused.
function reuses(impl) {
  return !!(impl && impl.worktree)
}

function whereOf(impl) {
  if (!reuses(impl)) return 'Where you are: your shell starts in a fresh scratch worktree. A previous package worktree may already hold the matching build. Use the setup checks below to choose the working directory. Leave the main checkout and port/parity alone. Keep all work local; never push.'
  return `Where you are: your shell starts in a scratch worktree that you do not use. You work in the implementer's worktree, ${impl.worktree}: a git worktree of the OpenWhisper_CPP repository on its own branch, whose build tree for the windows-ci preset is already built, so that you compile only what you change. Begin every command by making that folder the current directory, and do all work inside it. Never modify the main checkout at C:\\coding\\OpenWhisper_CPP, never switch or move the port/parity branch, never push.`
}

function setUp(p, impl) {
  if (!reuses(impl)) return `Set up: use "git worktree list --porcelain" to find an idle package worktree under C:\\coding\\OpenWhisper_CPP\\.claude\\worktrees at the exact tip of wp/${p.id}. Reuse its build only if the worktree is clean and out/build/windows-ci/CMakeCache.txt names that worktree as CMAKE_HOME_DIRECTORY. Otherwise start the fresh scratch worktree at wp/${p.id}. Keep interrupted edits intact. In the selected directory, read out/port-implementation.json if it exists and its id and sha match the package and current commit. Verify its claims independently. The package's changes are ${baseOf(p)}.`
  return `Set up: in ${impl.worktree}, "git rev-parse HEAD" must equal "git rev-parse wp/${p.id}" and "git status --short" must be empty. If either check differs, keep that worktree and its edits. Set your fresh scratch worktree to wp/${p.id} and review it there. Read the diff in ${impl.worktree} and carry verified package fixes forward into your scratch worktree. The package's changes are ${baseOf(p)}.`
}

const FULL_CHECK = 'Run "cmake --workflow --preset windows-ci" with the decode variables set. Report the results of this run.'
const PACKAGE_CHECK = `Configure with "cmake --preset windows-ci".
- Build the affected test targets with "cmake --build --preset windows-ci --target fv_<module>_tests" and any affected executables. Include direct consumers of a changed public contract.
- Run "ctest --preset windows-ci -L ^<module>$" for each affected module and consumer. Run applicable lints, standalone tool tests and the package-specific acceptance checks. Verify each selection ran tests and report the targets, labels and actual totals.
- Use the full windows-ci workflow for shared build or test infrastructure changes, or when the affected checks cannot be identified. The integration stage runs the full merge gate on the combined changes and satisfies the spec's full-gate requirement.`

function packageCheck(p) {
  return p.integrating ? PACKAGE_CHECK : FULL_CHECK
}

function implPrompt(p) {
  return `You are building one work package of the OpenWhisper_CPP port: ${p.id} ${p.name}.

${common(WHERE_FRESH)}

${afterNote(p)}${continuationNote(p)}Read first
1. CLAUDE.md (already in your context).
2. docs/port/ARCHITECTURE.md: the "Lead amendments" at the top, then ${p.archSections}, and any other section your package touches. The names it gives (targets, directories, classes, headers, functions) are binding.
3. Your package spec, below. The headers already in the tree are the contracts as built: where the spec names a type or member differently from a header, follow the header and say so in your report (lead amendment 12).
4. The area maps your spec names (docs/port/map/*.md), then the Python source they point to. The Python is the reference for behavior: where a map and the Python disagree, the Python wins.
5. docs/port/open-items.md: every row that names your package (${p.id}) is work earlier packages left for you.

Your package spec
${p.spec}

Rules of the job
- Own only the directories and files your spec lists. If the job needs a change anywhere else (a contract header is wrong, a dependency is missing, a shared file needs an entry), do not make it: finish what you can and report the exact change under "blockers".
- Port behavior exactly: strings, defaults, limits, timings, ordering, edge cases, error messages. Where OpenWhisper_CPP deliberately differs, the spec or the architecture says so; add nothing else and drop nothing silently.
- Be honest about completeness. Anything missing, partial or stubbed goes in "notDone". Never leave a stub that looks finished, a test that asserts nothing, or a test weakened until it passes.
- Tests are part of the package (architecture section 9): port the OpenWhisper tests your spec lists and keep their names; replay golden vectors where the spec lists them; add reference-screenshot tests for new widgets once the harness exists in the tree.
- Comments say what the code cannot: the reason, the constraint, the unit. No narration, no references to this task or to "the port".
- Before you finish: ${packageCheck(p)}
- Format the C++ you added or changed with clang-format (lead amendment 3).
- Finish with: git add -A; git commit -m "${p.id} ${p.name}: <one line saying what it adds>" (no attribution lines); then git branch -f wp/${p.id} HEAD. Your work exists for the lead only through that branch. Stay on your worktree's own branch: never check out or switch to wp/${p.id}, or the reviewer cannot move it.

Save the same structured report to out/port-implementation.json in your worktree after committing, with the id, final sha, and the targets and test labels actually run. Include the key Python paths and C++ tests in findings so the reviewer can go directly to them. Return that report.`
}

function releaseBuild(p) {
  return `After committing and verifying the wp/${p.id} tip, if your verdict is ready or ready-with-notes, reclaim the build output in the worktree where you actually tested. Check that HEAD equals wp/${p.id}, the worktree is clean, and no build is running there. Resolve the absolute worktree and out/build paths. The worktree must be a direct child of C:\\coding\\OpenWhisper_CPP\\.claude\\worktrees and the target must be its ordinary out\\build directory, with no junction or symbolic link in that path. Remove only that validated build directory with PowerShell Remove-Item -LiteralPath. Keep the cache of an unfinished review. If cleanup is unavailable or ownership is uncertain, leave the directory and report its path in "open".`
}

function reviewPrompt(p, impl) {
  return `You are the independent reviewer of work package ${p.id} ${p.name} of the OpenWhisper_CPP port. Find what is wrong or missing, and fix it. The implementer's work is the tip of branch wp/${p.id}.

${common(whereOf(impl))}

${setUp(p, impl)}

What the package was asked to do: docs/port/ARCHITECTURE.md (lead amendments, ${p.archSections}) and this spec:
${p.spec}

What the implementer reported (treat every claim as unverified):
${JSON.stringify(impl, null, 1)}

Review method
1. Independently verify the package. ${packageCheck(p)}
2. Completeness: walk the spec item by item and find the code and the test for each. List what is missing, partial or stubbed, whether or not the implementer admitted it.
3. Fidelity: for each ported behavior, open the Python it came from and compare: strings, defaults, limits, timings, branch order, edge cases, error text. Hunt for silent simplifications, TODOs, hard-coded returns, swallowed errors, tests that assert nothing or were weakened.
4. Rules: changed files stay inside what the spec lets the package own (the --name-only form of the diff above); layering and "no Qt in core" hold; CLAUDE.md's engine rules hold; names match the architecture; no product name, path or environment variable spelled as a literal where the architecture provides an identity for it.
5. Design: does it do what later packages need? Look for interfaces that will force a rewrite, thread-safety holes, lifetime bugs, and anything that only works on this machine.
6. Fix every defect you can inside the package's owned files, with tests. Repeat the checks from step 1 after your fixes. Then: git add -A; git commit -m "${p.id} ${p.name}: review fixes" (no attribution lines; skip the commit if you changed nothing); git branch -f wp/${p.id} HEAD.
${reuses(impl) ? `   If a write in the implementer's worktree is refused (some workers may write only inside the worktree their shell started in), do not stop at a proposal: do the whole review in your own scratch worktree instead. There: "git reset --hard wp/${p.id}", build from scratch, fix, rebuild and rerun, commit as above. Say in your summary that you worked in your own worktree.
` : ''}   If "git branch -f wp/${p.id} HEAD" is refused because another worktree has that branch checked out, move it with "git update-ref refs/heads/wp/${p.id} HEAD". Your fixes count only when they are the tip of wp/${p.id}, built and run: never leave them on another branch, and never return a change you could not build and test.

7. A change the package needs in a file it does not own. When the gate in this worktree fails, or a spec item cannot work, only because of a small and exact change somewhere else (an entry in a list or table of another package, a one-line call or an include in a composition file), make that change on this branch in the same commit, with the test that shows it, and put it first under "open" as "Made outside the package: <file>: <what and why>", so that the lead can accept or redo it. The frozen files CLAUDE.md names are the exception: never edit those; state the exact change for the lead. A change that is larger, or that you are not sure of, you do not make: describe it exactly under "open".

Verdict: "ready" when the spec is met and everything passes; "ready-with-notes" when what remains is minor and listed in "open"; "not-ready" when a spec item is missing or broken, the tree does not build, or a test fails. A failure whose whole cause is a change outside the package that step 7 did not let you make is not the package's: with nothing else wrong the verdict is "ready-with-notes", with that change first under "open". Packages that build on this one are held back by "not-ready": use it for a package that is not done, never for one that only waits on the lead.

Save the structured result with its final sha in out/port-review.json in the worktree you tested, outside the build cache.
${releaseBuild(p)}
Return the result and include any cleanup note in both copies.`
}

function secondPassPrompt(p, impl, review) {
  return `Work package ${p.id} ${p.name} of the OpenWhisper_CPP port was implemented and reviewed, and the reviewer could not get it ready. Finish it. The current state is the tip of branch wp/${p.id}.

${common(whereOf(impl))}

${setUp(p, impl)}

The package: docs/port/ARCHITECTURE.md (lead amendments, ${p.archSections}) and this spec:
${p.spec}

What the reviewer left open:
${JSON.stringify(review, null, 1)}

Resolve every open item, highest severity first, inside the package's owned files, with tests. Where an item truly cannot be done inside the package (it needs a change elsewhere), say so exactly instead of working around it. ${packageCheck(p)} Then: git add -A; git commit -m "${p.id} ${p.name}: second pass" (no attribution lines); git branch -f wp/${p.id} HEAD.

Save the final review result with its sha in out/port-review.json in the worktree you tested.
${releaseBuild(p)}
Return the result and include any cleanup note in both copies.`
}

// A cap below the runtime's own, so a wave does not saturate the machine: with nine workers
// building at once a merge gate that takes three minutes took over an hour.
const LIMIT = Math.min(jobCount(args.concurrency, 4, 'concurrency'), Math.floor(MAX_JOBS / JOBS))
let active = 0
const waiting = []
async function slot(run, priority = 0) {
  while (active >= LIMIT) await new Promise(resolve => waiting.push({ resolve, priority }))
  active++
  try {
    return await run()
  } finally {
    active--
    // The highest priority waits least; equal priorities go first come, first served.
    let best = -1
    for (let i = 0; i < waiting.length; i++) {
      if (best < 0 || waiting[i].priority > waiting[best].priority) best = i
    }
    if (best >= 0) waiting.splice(best, 1)[0].resolve()
  }
}

// A written package holds its 20 GB build tree until its review ends, so a waiting review takes
// a free slot before any waiting implementer: with 32 packages and reviews queued behind new
// implementers, free space on the system drive fell from 124 GB to 45 GB.
const REVIEW_FIRST = 1000

const LOST_REPORT = { note: 'The implementer committed its work to the branch but its report was lost in a crash. Nothing about the package is known beyond the branch itself.' }

async function attempt(prompt, options, priority = 0) {
  try {
    return (await slot(() => agent(prompt, options), priority)) || null
  } catch (error) {
    log(`${options.label} failed: ${error && error.message ? error.message : error}`)
    return null
  }
}

async function runPackage(p, finished) {
  if (hasDeps(p)) {
    const unknown = p.after.filter(id => !finished[id])
    if (unknown.length) return { id: p.id, impl: null, review: null, skipped: `unknown packages in after: ${unknown.join(', ')}` }
    const deps = await Promise.all(p.after.map(id => finished[id]))
    const bad = deps.filter(d => !d || !d.review || d.review.verdict === 'not-ready').length
    if (bad) return { id: p.id, impl: null, review: null, skipped: `${bad} of the packages it builds on did not get ready` }
  }
  let report
  if (p.reviewOnly) {
    report = LOST_REPORT
  } else {
    report = await attempt(implPrompt(p), {
      label: `impl:${p.id}`, phase: 'Implement', model: p.model || 'opus', effort: p.effort || 'high',
      isolation: 'worktree', schema: IMPL_SCHEMA,
    }, p.priority || 0)
    if (!report) return { id: p.id, impl: null, review: null }
  }
  const review = await attempt(reviewPrompt(p, report), {
    label: `review:${p.id}`, phase: 'Review', model: 'opus', effort: 'high',
    isolation: 'worktree', schema: REVIEW_SCHEMA,
  }, REVIEW_FIRST + (p.priority || 0))
  if (!review || review.verdict !== 'not-ready') return { id: p.id, impl: report, review }
  const second = await attempt(secondPassPrompt(p, report, review), {
    label: `second:${p.id}`, phase: 'Second pass', model: 'opus', effort: 'high',
    isolation: 'worktree', schema: REVIEW_SCHEMA,
  }, REVIEW_FIRST + (p.priority || 0))
  return { id: p.id, impl: report, firstReview: review, review: second || review }
}

// One promise per package, settled with its final result, so a package can wait for the ones
// it builds on.
async function runPackages(wave, packages) {
  if (packages.length) log(`Wave ${wave}: ${packages.map(p => p.id).join(', ')}`)
  const finished = {}
  const settle = {}
  for (const p of packages) finished[p.id] = new Promise(resolve => { settle[p.id] = resolve })
  const results = await Promise.all(packages.map(async p => {
    let result = null
    try {
      result = await runPackage(p, finished)
    } catch (error) {
      log(`${p.id} failed: ${error && error.message ? error.message : error}`)
    } finally {
      settle[p.id](result)
    }
    return result
  }))
  return packages.map((p, i) => results[i] || { id: p.id, impl: null, review: null })
}

const INTEGRATE_SCHEMA = {
  type: 'object',
  properties: {
    gate: { type: 'string', enum: ['green', 'red', 'not-run'] },
    head: { type: 'string', description: 'git rev-parse --short HEAD of port/parity when you finished' },
    testsTotal: { type: 'number', description: 'from the last full gate run' },
    testsFailed: { type: 'number' },
    merged: { type: 'array', items: { type: 'string' }, description: 'package ids merged, in order' },
    notMerged: { type: 'array', items: { type: 'string' }, description: '"<id>: why"' },
    commits: { type: 'array', items: { type: 'string' }, description: '"<short sha> <subject>" of every commit you made' },
    fixes: { type: 'array', items: { type: 'string' }, description: 'what you changed beyond the merges, one line each' },
    flaky: { type: 'array', items: { type: 'string' }, description: 'tests that failed in the gate and passed alone' },
    cleanup: { type: 'string', description: 'worktrees and branches removed, and any you kept and why' },
    forLead: { type: 'array', items: { type: 'string' }, description: 'what needs the lead: decisions, edits to frozen files you made or need, open items of high severity, anything you were unsure about. Most important first, at most 15, one or two sentences each' },
  },
  required: ['gate', 'head', 'merged', 'commits', 'forLead'],
}

const LINUX_SCHEMA = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['green', 'red', 'not-run'] },
    commit: { type: 'string' },
    testsTotal: { type: 'number' },
    testsFailed: { type: 'number' },
    failures: { type: 'array', items: { type: 'string' }, description: 'each failing step or test with the first lines of its error' },
    notes: { type: 'string' },
  },
  required: ['status', 'commit'],
}

function digestOf(p, r) {
  const impl = r.impl || {}
  const review = r.review || {}
  return {
    id: p.id, name: p.name, verdict: review.verdict || 'no review', after: p.after || [],
    implemented: impl.summary, reviewed: review.summary,
    notDone: impl.notDone || [], deviations: impl.deviations || [], blockers: impl.blockers || [],
    open: review.open || [], ownershipViolations: review.ownershipViolations || [],
  }
}

function integratePrompt(w, digests, part = {}) {
  const it = w.integrate
  const jobs = Math.min(jobCount(it.gateJobs, MAX_JOBS, 'gateJobs'), MAX_JOBS)
  const work = part.work || ''
  const source = digests.length
    ? `The packages of this wave, with what their implementers and reviewers reported:
${JSON.stringify(digests, null, 1)}`
    : !it.resultsFile
      ? 'No package is merged in this step: the merges are committed already. Skip the merging and go on with steps 2 to 4 on the tree as it is; the lead names below the branches to delete in step 4.'
      : `The packages, in merge order: ${(it.order || []).join(', ')}. Their reports are in the JSON file ${it.resultsFile} (key "packages": for each id the implementer's report "impl" and the reviewer's "review" with its "verdict" and "open" items); read it with a short Python script, it is large. A package's name is in the subject of the first commit on its branch.`
  return `You are the integrator of wave ${w.wave} of the OpenWhisper_CPP port, a C++23 / Qt 6 rewrite of OpenWhisper. You do the lead's mechanical work after a wave, so that the lead only checks your result. Be exact and conservative: when a step needs a judgment this prompt does not give you, do not guess: leave it and say so under "forLead".

Where you work: the main checkout C:\\coding\\OpenWhisper_CPP, branch port/parity. First run "git -C C:\\coding\\OpenWhisper_CPP branch --show-current" and "git -C C:\\coding\\OpenWhisper_CPP status --short". If the branch is not port/parity or the tree is not clean, change nothing and return gate "not-run" with what you saw. Make that directory your current directory for every command. No other agent is building now.

Rules that have no exception
- Never push, anywhere. Never amend, rebase or revert a commit that exists already. Commit messages carry no attribution or co-author lines.
- The OpenWhisper checkout at ${OW} is read-only: never create, edit or delete anything under it, never run git or any writing command inside it, never start that app, never read the owner's live data in its root.
- Never start the GUI app or a test executable directly: tests run through ctest. Never run a test labelled interactive, nor the windows-interactive preset: they take over the owner's keyboard, microphone and clipboard.
- Install nothing and download nothing.

1. Merge
${source}
${part.stage ? `This is stage ${part.stage} of this run's integration: only the packages listed here are merged now. The wp/ and base/ branches of every other package belong to another stage: neither merge nor delete them.
` : ''}Merge each package whose verdict is "ready" or "ready-with-notes", a package after the ones named in its "after": "git merge --squash wp/<id>", then one commit with the subject "<id> <name>: <one line saying what it adds>" and a body of two to five lines taken from the reports (what exists now, what is knowingly not done). A package that is "not-ready" or has no review is not merged: leave its branch alone and list it under "notMerged".
If a squash conflicts: in files where each package adds lines of its own (tools/lint/qss_unused.txt and other lists and tables) keep both sides; elsewhere read both sides and keep the behavior of both packages. If you cannot do that with confidence, "git reset --hard HEAD", do not merge that package or any package that builds on it, and report it.

2. Lead fixes, in one commit "Lead fixes after ${w.wave}: <what>" (skip it if there is nothing)
- tools/lint/qss_unused.txt: take out the names the merged code now uses (the lint.qss test names them).
- docs/port/open-items.md: read it first and keep its style (one item, what and where). Remove the items these packages closed. Add what they left: each review's "open" items, each implementer's "notDone" and "blockers" that the review did not close. An item a later package must act on goes in the table "For a named later package" with that package's id (the specs are docs/port/packages/M<n>.md); a decision or permission only the owner can give goes under "Waiting on the owner"; the rest under "For the M10 sweep".
- After reconciling the merged PARITY.json entries, run "python -I -B tools/parity/parity_report.py". Resolve any reported ledger errors and include the generated docs/port/parity-report.md when it changes.
- A blocker that states an exact change to a file outside the package: make it if it is small and plainly right, and list it under "forLead"; otherwise only list it.${work ? `
- For this wave, from the lead: ${work}` : ''}
The frozen files named in CLAUDE.md are yours to edit only for the cases above. docs/port/ARCHITECTURE.md is the lead's: what a wave settled that later packages must follow goes under "forLead", not into that file.

3. Gate (PowerShell, from the main checkout)
  . .\\scripts\\vsenv.ps1
  $env:CMAKE_BUILD_PARALLEL_LEVEL = '${jobs}'; $env:CTEST_PARALLEL_LEVEL = '${jobs}'
  $env:OPENWHISPER_CPP_TEST_RUNTIME_DIR = "$env:LOCALAPPDATA\\OpenWhisper\\components\\asr-nvidia-cuda"
  $env:OPENWHISPER_CPP_TEST_MODEL = "$env:LOCALAPPDATA\\OpenWhisper\\speech-models\\parakeet-v3\\parakeet-tdt-0.6b-v3.q8_0.gguf"
  $env:OPENWHISPER_CPP_TEST_WAV = "${OW}\\recorded_audio.wav"
  $env:OPENWHISPER_CPP_TEST_PDFIUM = "${OW}\\.venv\\Lib\\site-packages\\pypdfium2_raw\\pdfium.dll"
  $env:OPENWHISPER_CPP_TEST_ONNXRUNTIME = "${OW}\\.venv\\Lib\\site-packages\\onnxruntime\\capi\\onnxruntime.dll"
  $env:OPENWHISPER_CPP_TEST_SPEAKER_MODEL = "$env:LOCALAPPDATA\\OpenWhisper\\models\\speaker-id\\voxceleb_resnet34_LM.onnx"
  Remove-Item out\\lead-gate.done -ErrorAction SilentlyContinue
  cmake --workflow --preset windows-ci *> out\\lead-gate.log; "exit=$LASTEXITCODE" | Set-Content out\\lead-gate.done
Start those lines as one background command, then wait until out\\lead-gate.done exists: with your Monitor tool if you have one (an until-loop on that file), otherwise with a loop that looks every 15 seconds for at most nine minutes per command, repeated as often as needed (the gate takes two to fifteen minutes on this machine). You have not finished while that file is missing: an integrator that returns while the gate runs leaves the lead with no gate, and that has happened. Then read the .done file and the end of the log, not all of it. For each failing test, run it alone: ctest --test-dir out/build/windows-ci -C RelWithDebInfo -LE interactive -R "<name>" --repeat until-fail:5. One that passes alone fails only under load: list it under "flaky" (and in open-items.md if it is not there) and do not count it against the gate. One that fails alone is real: find the cause. If the fix is small and you are sure of it, make it with the test that proves it, commit "Fix after ${w.wave}: <what>" and run the gate again. Otherwise leave the tree as it is and return gate "red" with the evidence. Never weaken, skip or delete a test to get green. A compile error C1076 or C3859 is the machine running out of memory: wait and retry with ${Math.max(1, Math.floor(jobs / 2))} jobs.
The gate is "green" when the workflow exits 0, or when its only failures pass alone.

${part.debug ? `3b. The Debug configuration, which no worker builds. After a green gate, with the same variables, started and waited for in the same way through out\\lead-debug.done (a first Debug build takes six to twelve minutes): cmake --workflow --preset windows-debug *> out\\lead-debug.log; "exit=$LASTEXITCODE" | Set-Content out\\lead-debug.done. Judge a failure there as you judge one in the gate (alone, under load, real, small fix or report); the gate you report is green only if this run is too.

` : ''}4. Clean up, only after a green gate
- Before deleting a merged package's wp/<id> branch, record its tip. Remove only a worktree under .claude\\worktrees which belongs to a package merged in this stage, has that tip, and has a clean worktree. Keep any worktree whose ownership is uncertain or which has pending edits. Leave the worktrees of unfinished packages for their next run.
- For each merged package, delete its wp/<id> and base/<id> branches when present.
Never change your shell's directory into one of those worktrees.

Return the structured report. "git status --short" must be empty when you finish.`
}

function linuxPrompt() {
  return `Run the Linux core check of the OpenWhisper_CPP port on the owner's Linux machine "jed" (Arch) and report the result. You fix nothing: you run and report.

jed also runs the owner's media server. Rules without exception: log in by key only ("ssh -o BatchMode=yes jed@jed ..."); if anything asks for a password, stop and report; never sudo; install nothing; touch nothing outside ~/openwhisper-cpp-port; start nothing except the commands below (run.sh already runs at the lowest priority with 3 jobs: never raise either); stop nothing you did not start.

1. From C:\\coding\\OpenWhisper_CPP: "git push jed@jed:openwhisper-cpp-port/repo.git port/parity". Then the same branch to the owner's private GitHub repository, as a plain push: git -c credential.helper= -c "credential.helper=!gh auth git-credential" push origin port/parity. The owner approved these two pushes; push nothing else and nowhere else, never with --force, and if one is rejected say so in your report and go on. Change nothing in that checkout.
2. On jed: look at "git -C ~/openwhisper-cpp-port/src remote -v", fetch, and check out the pushed commit detached (git -C ~/openwhisper-cpp-port/src checkout --detach <the commit>). Confirm with rev-parse.
3. For each step of configure, build, test, in that order: remove ~/openwhisper-cpp-port/logs/<step>.done, then start it detached:
     ssh -o BatchMode=yes jed@jed "(setsid nohup ~/openwhisper-cpp-port/run.sh <step> > /dev/null 2>&1 < /dev/null &)"
   and wait for ~/openwhisper-cpp-port/logs/<step>.done to appear, checking once a minute in a loop of at most nine minutes per command, as often as needed (the build can take more than an hour). Then read the .done file and the last 40 lines of ~/openwhisper-cpp-port/logs/<step>.log. Stop at the first step that fails.

Report status "green" only when all three steps succeeded and no test failed; give the test totals from ctest's summary, and for a failure the first lines of each error.`
}

function compact(r) {
  const review = r.review || {}
  return {
    id: r.id,
    verdict: review.verdict || (r.skipped ? `skipped: ${r.skipped}` : 'no review'),
    tests: review.testsTotal ? `${review.testsPassed}/${review.testsTotal}` : undefined,
    high: (review.open || []).filter(o => o.severity === 'high').map(o => `${o.what} (${o.where})`),
    blockers: (r.impl && r.impl.blockers) || [],
  }
}

// args.defaults?: { spec, archSections, effort }: what a package takes when it gives none of its
// own. In spec, {id} and {name} stand for the package's. A package's note is added to its spec.
const DEFAULTS = args.defaults || {}
function withDefaults(p) {
  // {milestone} is the id up to its dot: M3 for M3.4.
  const fill = s => s.replace(/\{id\}/g, p.id).replace(/\{name\}/g, p.name).replace(/\{milestone\}/g, p.id.split('.')[0])
  return {
    ...p,
    spec: (p.spec || fill(DEFAULTS.spec || '')) + (p.note ? `\nA note for this package from the lead: ${p.note}` : ''),
    archSections: p.archSections || DEFAULTS.archSections,
    effort: p.effort || DEFAULTS.effort,
    model: p.model || DEFAULTS.model,
  }
}

const WAVES = Array.isArray(args.waves) ? args.waves : [args]
const done = []
for (const w of WAVES) {
  // { wave, linuxOnly: true } runs only the Linux check, for a run beside a building wave.
  if (w.linuxOnly) {
    const linux = await attempt(linuxPrompt(), {
      label: `linux:${w.wave}`, phase: 'Linux check', model: 'sonnet', effort: 'medium', schema: LINUX_SCHEMA,
    })
    done.push({ wave: w.wave, linux })
    continue
  }
  const packages = (w.packages || []).map(p => ({ ...withDefaults(p), integrating: !!w.integrate }))
  const out = await runPackages(w.wave, packages)
  const ready = out.filter(r => r.review && r.review.verdict !== 'not-ready').map(r => r.id)
  const notReady = out.filter(r => !r.review || r.review.verdict === 'not-ready').map(r => r.id)
  if (packages.length) log(`Wave ${w.wave}: ready ${ready.join(', ') || 'none'}; not ready ${notReady.join(', ') || 'none'}`)
  if (!w.integrate) {
    // Without an integrator the lead merges, so the full reports go back and nothing can follow.
    done.push({ wave: w.wave, ready, notReady, packages: out })
    break
  }
  // integrate.stages: [[ids], [ids]] merges and gates the packages in that many steps, each
  // on top of the one before; stageWork[i] is the lead's note for step i, leadWork the last's.
  const it = w.integrate
  const stages = Array.isArray(it.stages) && it.stages.length ? it.stages : [null]
  const digests = packages.map((p, i) => digestOf(p, out[i]))
  const integrations = []
  for (let s = 0; s < stages.length; s++) {
    const ids = stages[s]
    const last = s === stages.length - 1
    const part = {
      stage: ids ? `${s + 1} of ${stages.length}` : '',
      work: (it.stageWork && it.stageWork[s]) || (last ? it.leadWork : '') || '',
      debug: last && !!it.debug,
    }
    const report = await attempt(integratePrompt(w, ids ? digests.filter(d => ids.includes(d.id)) : digests, part), {
      label: `integrate:${w.wave}${ids ? `:${s + 1}` : ''}`, phase: 'Integrate', model: 'opus', effort: 'high', schema: INTEGRATE_SCHEMA,
    })
    integrations.push(report)
    if (!report || report.gate !== 'green') break
  }
  const integration = integrations[integrations.length - 1]
  const green = integrations.length === stages.length && integrations.every(r => r && r.gate === 'green')
  let linux = null
  if (green && w.integrate.linux) {
    linux = await attempt(linuxPrompt(), {
      label: `linux:${w.wave}`, phase: 'Linux check', model: 'sonnet', effort: 'medium', schema: LINUX_SCHEMA,
    })
  }
  done.push({ wave: w.wave, ready, notReady, integration, earlierStages: integrations.slice(0, -1), linux, packages: out.map(compact) })
  log(`Wave ${w.wave}: gate ${integration ? integration.gate : 'no report'}${linux ? `; Linux ${linux.status}` : ''}`)
  if (!green || notReady.length) {
    if (w !== WAVES[WAVES.length - 1]) log(`The chain stops after wave ${w.wave}`)
    break
  }
}
return WAVES.length === 1 ? done[0] : { waves: done }
