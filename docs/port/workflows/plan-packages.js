export const meta = {
  name: 'plan-packages-m0-m1',
  description: 'Write the binding work-package specs for milestones M0 and M1 from the architecture and the area maps, then check coverage of every mapped feature',
  phases: [
    { title: 'Plan', detail: 'one planner per milestone writes a spec per package', model: 'opus' },
    { title: 'Coverage', detail: 'a critic checks every mapped feature and test file is assigned or explicitly deferred, and fixes the specs', model: 'opus' },
  ],
}

const OW = 'D:\\coding\\whisper_local'
const FV = 'C:\\coding\\OpenWhisper_CPP'
const PKG = FV + '\\docs\\port\\packages'

const CONTEXT = `OpenWhisper_CPP (${FV}) is a C++20 / Qt 6 Widgets rewrite of OpenWhisper (Python 3.12 + PyQt6, read-only checkout at ${OW}) aiming at full feature parity. The port is built by worker agents, each implementing one work package in its own git worktree with a fresh context. A worker cannot ask questions: its package spec, the architecture and the area maps are all it has, plus the Python source it is pointed to.

Read:
- ${FV}\\docs\\port\\ARCHITECTURE.md in full, including the "Lead amendments" at the top. Its names (targets, directories, classes, headers) and its package cut in section 11 are binding.
- ${FV}\\docs\\port\\map-summaries.json, and in full the area maps under ${FV}\\docs\\port\\map that your milestone touches (section 11's milestone table says which areas).
- ${FV}\\docs\\port\\areas.json (the Python files of each area), and the Python itself wherever a map is not precise enough to scope a package.
- ${FV}\\out\\parity-scratch\\classification.json and classification.md: every OpenWhisper test file classified by area and kind (pure logic, Qt widget, integration, platform, dropped engine).
- The current OpenWhisper_CPP source under ${FV}\\src and ${FV}\\tests.
Do not modify anything except the one file you are asked to write. Never write under ${OW}.`

const SPEC_FORMAT = `Format of each package section (keep the headings exactly, so workers and tools can find them):

## <ID> <name>
- Owns: every directory and file the package may create or change (source, tests/<dir>, tools/parity cases and builders, tests/golden and tests/reference entries, generators)
- Depends on: package IDs that must be merged first
- Wave: which wave of the milestone it runs in
- Worker: opus or sonnet. Choose sonnet only for packages that are mostly mechanical (straight widget ports from a detailed map, tables, generated data); choose opus for logic, concurrency, platform APIs, protocols and anything other packages build on
- Size: estimated C++ lines, source and tests separately
### Scope
A table with one row per Python module or class ported: Python source (file and symbols, with line ranges where a file is split across packages) -> C++ file and class or function names (following the architecture's naming rules) -> the feature IDs from the area map's inventory that the row covers. Every feature ID the package is responsible for appears here.
### Contracts
Headers the package implements or provides to later packages (with the members later packages will call), and the contract headers and fakes it consumes.
### Tests to port
OpenWhisper test files (from the classification) -> the C++ test file, with the number of test functions and how each file is handled: ported, golden vectors, dropped (engine-specific, with reason) or pending in a named later package.
### Golden vectors
Python modules and functions whose inputs and outputs the package must generate with the harness and replay.
### Reference screenshots
Widgets, dialogs or pages with their variants (theme, scale) and the fixed fake data to use. "None" for packages without widgets.
### Acceptance
Commands and observable results that prove the package is done, including what its part of the milestone demo looks like.
### Not in this package
Behavior a reader might expect here that belongs elsewhere, each with the package or milestone that owns it.
### Traps
The specific things most likely to go wrong, taken from the maps' risks, edge cases and "Added by check" notes, with file:line references into the Python.`

const PLAN_SCHEMA = {
  type: 'object',
  properties: {
    file: { type: 'string' },
    milestone: { type: 'string' },
    packages: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          id: { type: 'string' },
          name: { type: 'string' },
          wave: { type: 'number' },
          worker: { type: 'string', enum: ['opus', 'sonnet'] },
          dependsOn: { type: 'array', items: { type: 'string' } },
          owns: { type: 'array', items: { type: 'string' } },
          estSourceLines: { type: 'number' },
          estTestLines: { type: 'number' },
          featureIds: { type: 'number', description: 'how many feature IDs the package covers' },
          testFiles: { type: 'number', description: 'how many OpenWhisper test files it handles' },
        },
        required: ['id', 'name', 'wave', 'worker', 'dependsOn', 'owns', 'estSourceLines'],
      },
    },
    changesToCut: { type: 'array', items: { type: 'string' }, description: 'every place you changed the package cut of architecture section 11 (split, merged, moved, re-ordered), and why' },
    deferred: { type: 'array', items: { type: 'string' }, description: 'groups of features of this milestone\'s areas deferred to later milestones, each as "<area> <what> -> M<n>"' },
    contractGaps: { type: 'array', items: { type: 'string' }, description: 'things the architecture\'s section 3 contracts lack that these packages will need' },
    risks: { type: 'array', items: { type: 'string' } },
  },
  required: ['file', 'milestone', 'packages', 'changesToCut', 'deferred', 'contractGaps'],
}

const MILESTONES = [
  {
    key: 'M0',
    text: `Write the package specs for milestone M0 (Foundation): packages M0.2 to M0.11 of architecture section 11.1. M0.1 skeleton-and-contracts is already being built with the scope of its row in 11.1 (build skeleton, vcpkg dependencies, the header-only contracts of section 3, lints, docs); list it in your file with a one-paragraph summary and do not re-specify it. You may adjust the cut where the maps show the architecture's first cut is wrong (a package too large for one worker at about 1,500 to 4,000 lines of C++ plus tests, a hidden dependency inside a wave, a directory owned twice in one wave), and you must say so.`,
  },
  {
    key: 'M1',
    text: `Write the package specs for milestone M1 (Dictation): packages M1.1 to M1.16 of architecture section 11.2. Assume M0 is complete as the architecture describes it. You may adjust the cut where the maps show the architecture's first cut is wrong (a package too large for one worker at about 1,500 to 4,000 lines of C++ plus tests, a hidden dependency inside a wave, a directory owned twice in one wave), and you must say so. M1 ends with the demo in section 11's table (press the record key in another app, speak, the overlay reacts, the Parakeet text is pasted, shown in Quick Record and saved to History), so make sure some package owns every link of that chain, including the end-to-end test.`,
  },
]

phase('Plan')
const plans = (await parallel(MILESTONES.map(m => () =>
  agent(`You are planning part of a large port. ${m.text}

${CONTEXT}

Write ${PKG}\\${m.key}.md (create the folder if needed). Start it with: a short overview of the milestone and its demo; the wave order; a coverage table listing, for each area the milestone touches, every feature ID of that area's inventory with the package that covers it or the later milestone it is deferred to (no feature ID of those areas may be missing from the table); and a table of the OpenWhisper test files of those areas with the package that handles each or the milestone they are deferred to. Then one section per package.

${SPEC_FORMAT}

Rules for a good spec
- Scope by reading, not by guessing: open the maps' feature inventories and module sections, and the Python where needed, so every row names real symbols.
- Packages in the same wave must not depend on each other and must not own the same directory. A package may depend only on earlier waves and earlier milestones.
- A package must be finishable by one worker: if a row hides a 3,000-line Python module, split the package rather than hoping.
- Do not move behavior out of M${m.key.slice(1)} just to make packages smaller; defer only what the architecture's milestone table assigns to a later milestone, and name the milestone.
- Prefer exact names and numbers to adjectives.

Then return the structured summary.`, {
    label: `plan:${m.key}`, phase: 'Plan', model: 'opus', effort: 'high', schema: PLAN_SCHEMA,
  })
))).filter(Boolean)

if (!plans.length) return { plans: [], coverage: null }

phase('Coverage')
const COVERAGE_SCHEMA = {
  type: 'object',
  properties: {
    featureIdsChecked: { type: 'number' },
    unassignedFound: { type: 'number', description: 'feature IDs that were neither assigned nor deferred before your fixes' },
    doubleAssignedFound: { type: 'number' },
    testFilesUnassignedFound: { type: 'number' },
    waveConflictsFound: { type: 'array', items: { type: 'string' }, description: 'same-wave dependencies or shared ownership you found' },
    fixes: { type: 'array', items: { type: 'string' }, description: 'what you changed in the spec files (up to 20)' },
    stillOpen: { type: 'array', items: { type: 'string' }, description: 'problems you could not resolve and the lead must decide' },
    oversized: { type: 'array', items: { type: 'string' }, description: 'packages you judge too large for one worker, with the reason' },
  },
  required: ['featureIdsChecked', 'unassignedFound', 'doubleAssignedFound', 'testFilesUnassignedFound', 'waveConflictsFound', 'fixes', 'stillOpen'],
}
const coverage = await agent(`Two planners wrote the work-package specs for milestones M0 and M1 of a large port: ${plans.map(p => p.file).join(' and ')}. Check them for gaps, overlaps and impossible orderings, and fix the files.

${CONTEXT.replace('Do not modify anything except the one file you are asked to write.', 'You may modify only the two spec files.')}

Checks
1. Feature coverage. For every area that M0 or M1 touches (architecture section 11's milestone table), extract every feature ID from that area map's feature inventory (and from its "Added by check" additions) with a script, and confirm each is either covered by exactly one M0 or M1 package's Scope table, or explicitly deferred to a named later milestone in a coverage table. Report the counts. For each unassigned or doubly assigned ID, decide the right owner by reading the map and the architecture, and fix the spec.
2. Test coverage. Every OpenWhisper test file classified under those areas is handled by a package or deferred to a named milestone.
3. Ownership and order. Within a wave no two packages own the same directory or file and none depends on another; every dependency points to an earlier wave or milestone; every contract a package consumes exists in architecture section 3 or is provided by an earlier package (otherwise record a contract gap in the spec file's overview).
4. Size. Flag packages whose scope clearly exceeds one worker (roughly more than 4,500 lines of C++ including tests, or more than about 4,000 lines of Python to read), and split them in the file where the split is clean.
5. The M1 demo chain: runtime and model discovered or adopted, model loaded, global hotkey, recording, overlay, decode, paste into the foreground app, Quick Record display, history row, playback. Some package must own each link and the end-to-end test.

Fix the spec files in place, keeping their format. Return the structured result.`, {
  label: 'coverage-check', phase: 'Coverage', model: 'opus', effort: 'high', schema: COVERAGE_SCHEMA,
})

return { plans, coverage }
