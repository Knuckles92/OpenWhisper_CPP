export const meta = {
  name: 'plan-milestones',
  description: 'Write the binding work-package specs for one or more milestones from the architecture, the area maps and the tree as built, then check coverage of every mapped feature',
  phases: [
    { title: 'Plan', detail: 'one planner per milestone writes a spec per package', model: 'opus' },
    { title: 'Coverage', detail: 'a critic checks every mapped feature and test file is assigned or explicitly deferred, and fixes the specs', model: 'opus' },
  ],
}

// args: { milestones: [{ key: 'M2', text: '<what the milestone is and any lead notes>' }], prior: ['M0', 'M1'] }
const MILESTONES = args.milestones
const PRIOR = args.prior || []

const OW = 'D:\\coding\\whisper_local'
const FV = 'C:\\coding\\OpenWhisper_CPP'
const PKG = FV + '\\docs\\port\\packages'
const priorFiles = PRIOR.map(k => `${PKG}\\${k}.md`).join(', ')

const CONTEXT = `OpenWhisper_CPP (${FV}) is a C++23 / Qt 6 Widgets rewrite of OpenWhisper (Python 3.12 + PyQt6, read-only checkout at ${OW}) aiming at full feature parity. The port is built by worker agents, each implementing one work package in its own git worktree with a fresh context. A worker cannot ask questions: its package spec, the architecture and the area maps are all it has, plus the Python source it is pointed to.

Read:
- ${FV}\\CLAUDE.md and ${FV}\\docs\\port\\ARCHITECTURE.md in full, starting with the "Lead amendments" at the top: they override the text below them and record what the milestones built so far settled. The architecture's names (targets, directories, classes, headers) are binding; its package tables in section 11 are only a first cut.
- The specs of the earlier milestones: ${priorFiles || '(none)'}. Their overview tables say which feature IDs and test files are already assigned and which were deferred to your milestone. Their cut changes and "stillOpen" notes tell you what moved.
- ${FV}\\docs\\port\\map-summaries.json, and in full the area maps under ${FV}\\docs\\port\\map that your milestone touches (architecture section 11's milestone table says which areas).
- ${FV}\\docs\\port\\areas.json (the Python files of each area), and the Python itself wherever a map is not precise enough to scope a package.
- ${FV}\\tools\\parity\\classification.json: every OpenWhisper test file classified by area and kind.
- The OpenWhisper_CPP tree as it is now under ${FV}\\src and ${FV}\\tests: the headers there are the contracts as built, and they win over anything a document says about their shape. Packages of earlier milestones that are not merged yet exist only as specs; plan against their specs.
Do not modify anything except the one file you are asked to write. Never write under ${OW}, and never run git or any command that can write inside it.`

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
Headers the package implements or provides to later packages (with the members later packages will call), and the headers and fakes it consumes, named as they are in the tree.
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
          estSourceLines: { type: 'number' },
          estTestLines: { type: 'number' },
        },
        required: ['id', 'name', 'wave', 'worker', 'dependsOn', 'estSourceLines'],
      },
    },
    changesToCut: { type: 'array', items: { type: 'string' }, description: 'up to 12: where you departed from architecture section 11 or from what earlier specs assumed, and why' },
    deferred: { type: 'array', items: { type: 'string' }, description: 'groups of features of this milestone\'s areas deferred to later milestones, each as "<area> <what> -> M<n>"' },
    contractGaps: { type: 'array', items: { type: 'string' }, description: 'up to 12: things the contracts in the tree lack that these packages need, and which package adds each' },
    leadDecisions: { type: 'array', items: { type: 'string' }, description: 'up to 8: questions only the lead can settle' },
  },
  required: ['file', 'milestone', 'packages', 'changesToCut', 'deferred', 'contractGaps'],
}

phase('Plan')
const plans = (await parallel(MILESTONES.map(m => () =>
  agent(`You are planning part of a large port: write the package specs for milestone ${m.key}. ${m.text}

${CONTEXT}

Write ${PKG}\\${m.key}.md. Start it with: a short overview of the milestone and its demo (architecture section 11's table); the wave order; a coverage table listing, for each area the milestone touches, every feature ID of that area's inventory that is not already assigned by an earlier milestone's spec, with the package that covers it or the later milestone it is deferred to (no such feature ID may be missing from the table); and a table of the OpenWhisper test files of those areas that earlier milestones left open, with the package that handles each or the milestone they are deferred to. Then one section per package, numbered ${m.key}.1, ${m.key}.2, and so on.

${SPEC_FORMAT}

Rules for a good spec
- Scope by reading, not by guessing: open the maps' feature inventories and module sections, and the Python where needed, so every row names real symbols.
- Packages in the same wave must not depend on each other and must not own the same directory or file. A package may depend only on earlier waves and earlier milestones. A directory that earlier milestones already created can be owned by one package per wave.
- A package must be finishable by one worker: about 1,500 to 4,000 lines of C++ including tests, and no more than about 4,000 lines of Python to read. Split rather than hope.
- The lead-owned frozen files (CLAUDE.md lists them) are edited by no package. If the milestone needs a change to one (a new settings key, a style property, an order or destination constant, a CMake function, a dependency), list it under "leadDecisions" with the exact change, and have the packages assume it is made before their wave.
- Code that takes over the desktop session is proven through fakes; its real-API tests are tagged [interactive] and are written but not run (lead amendment 14). Plan the fakes.
- Do not move behavior out of the milestone just to make packages smaller; defer only what the architecture's milestone table assigns to a later milestone, and name the milestone.
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
    unassignedFound: { type: 'number' },
    doubleAssignedFound: { type: 'number' },
    testFilesUnassignedFound: { type: 'number' },
    waveConflictsFound: { type: 'array', items: { type: 'string' } },
    fixes: { type: 'array', items: { type: 'string' }, description: 'what you changed in the spec files (up to 20)' },
    stillOpen: { type: 'array', items: { type: 'string' }, description: 'problems you could not resolve and the lead must decide (up to 12)' },
    oversized: { type: 'array', items: { type: 'string' } },
  },
  required: ['featureIdsChecked', 'unassignedFound', 'doubleAssignedFound', 'testFilesUnassignedFound', 'waveConflictsFound', 'fixes', 'stillOpen'],
}
const files = plans.map(p => p.file)
const coverage = await agent(`Planners wrote the work-package specs for milestone(s) ${MILESTONES.map(m => m.key).join(' and ')} of a large port: ${files.join(' and ')}. Check them for gaps, overlaps and impossible orderings, and fix the files.

${CONTEXT.replace('Do not modify anything except the one file you are asked to write.', 'You may modify only the spec files named above.')}

Checks
1. Feature coverage. For every area these milestones touch (architecture section 11's milestone table), extract every feature ID from that area map's feature inventory (and its "Added by check" additions) with a script, and confirm each is covered by exactly one package's Scope table across all spec files (the earlier milestones' and these), or explicitly deferred to a named later milestone, or recorded as dropped with a reason. Report the counts. For each unassigned or doubly assigned ID, decide the right owner by reading the map and the architecture, and fix these spec files (do not edit the earlier milestones' files; if one of them is wrong, record it under stillOpen).
2. Test coverage. Every OpenWhisper test file classified under those areas is handled by a package (earlier or these) or deferred to a named milestone.
3. Ownership and order. Within a wave no two packages own the same directory or file and none depends on another; every dependency points to an earlier wave or milestone; every header a package consumes exists in the tree or is provided by an earlier package.
4. Size. Flag packages whose scope clearly exceeds one worker, and split them in the file where the split is clean.
5. The demo. Some package owns every link of each milestone's demo chain (architecture section 11) and its end-to-end test.
6. Frozen files and interactive tests: no package edits a lead-owned frozen file, and code that takes over the desktop session has fakes planned and its real-API tests tagged [interactive].

Fix the spec files in place, keeping their format. Return the structured result.`, {
  label: 'coverage-check', phase: 'Coverage', model: 'opus', effort: 'high', schema: COVERAGE_SCHEMA,
})

return { plans, coverage }
