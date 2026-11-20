// Renders the wave script's prompts with a fake agent, to check them before a real run.
import fs from 'fs'
import assert from 'node:assert/strict'

const src = fs.readFileSync('docs/port/workflows/port-wave.js', 'utf8').replace(/^export const meta/m, 'const meta')
const prompts = []
const agent = async (prompt, opts) => {
  prompts.push({ label: opts.label, isolation: opts.isolation, model: opts.model, prompt })
  const kind = opts.label.split(':')[0]
  const id = opts.label.split(':')[1]
  if (kind === 'impl') return { id, branch: `wp/${id}`, sha: '1', worktree: `C:\\coding\\fiorivoce_cpp\\.claude\\worktrees\\wf_x-${id}`, summary: 's', testsTotal: 1, testsPassed: 1, testsFailed: 0, notDone: [], deviations: [], blockers: [] }
  if (kind === 'review') return { id, verdict: id === 'B' ? 'not-ready' : 'ready', sha: '2', summary: 'r', buildClean: true, testsTotal: 1, testsPassed: 1, testsFailed: 0, fixed: [], open: [] }
  if (kind === 'second') return { id, verdict: 'ready', sha: '3', summary: 'r2', buildClean: true, testsTotal: 1, testsPassed: 1, testsFailed: 0, fixed: [], open: [] }
  if (kind === 'integrate') return { gate: 'green', head: 'abc', merged: [], commits: [], forLead: [] }
  if (kind === 'linux') return { status: 'green', commit: 'abc' }
  return null
}
const args = {
  concurrency: 6, jobs: 3,
  defaults: { effort: 'high', model: 'opus', archSections: 'sections 1 and 2', spec: 'Spec of {id} {name}.' },
  wave: 'T',
  packages: [
    { id: 'A', name: 'alpha' },
    { id: 'B', name: 'beta', after: ['A'], model: 'sonnet', note: 'a note' },
    { id: 'C', name: 'gamma', reviewOnly: true },
  ],
  integrate: { stages: [['A', 'B'], ['C']], stageWork: ['work one'], leadWork: 'work last', debug: true, linux: true },
}
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor
const run = new AsyncFunction('args', 'agent', 'log', 'phase', src)
const result = await run(args, agent, () => {}, () => {})

assert.equal(result.integration.gate, 'green')
assert.ok(prompts.every(p => !p.label.startsWith('clean:')))
for (const p of prompts.filter(p => /^(review|second):/.test(p.label))) {
  assert.ok(p.prompt.includes('Keep the cache of an unfinished review'), p.label)
  assert.ok(p.prompt.includes('no junction or symbolic link'), p.label)
}
for (const p of prompts.filter(p => /^(impl|review|second):/.test(p.label))) {
  assert.ok(p.prompt.includes('The integration stage runs the full merge gate'), p.label)
}
for (const p of prompts.filter(p => p.label.startsWith('integrate:'))) {
  assert.ok(p.prompt.includes('cmake --workflow --preset windows-ci'), p.label)
  assert.ok(p.prompt.includes('tools/parity/parity_report.py'), p.label)
}
assert.ok(prompts.find(p => p.label === 'impl:A').prompt.includes('out/port-implementation.json'))
const recovery = prompts.find(p => p.label === 'review:C').prompt
for (const check of ['exact tip of wp/C', 'CMAKE_HOME_DIRECTORY', 'id and sha match', 'out/port-review.json']) {
  assert.ok(recovery.includes(check), check)
}
const standalone = []
await run({ ...args, integrate: undefined, packages: [{ id: 'A', name: 'alpha' }] },
  async (prompt, opts) => {
    standalone.push(opts.label)
    if (opts.label === 'review:A') return null
    assert.ok(prompt.includes('Report the results of this run.'))
    return { id: 'A', worktree: 'example' }
  }, () => {}, () => {})
assert.deepEqual(standalone, ['impl:A', 'review:A'])
const unfinished = []
const one = { ...args, integrate: undefined, packages: [{ id: 'B', name: 'beta', reviewOnly: true }] }
const retry = await run(one, async (prompt, opts) => {
  unfinished.push(opts.label)
  assert.ok(prompt.includes('Report the results of this run.'))
  return { id: 'B', verdict: 'not-ready' }
}, () => {}, () => {})
assert.deepEqual(unfinished, ['review:B', 'second:B'])
assert.deepEqual(retry.notReady, ['B'])

// 6 requested workers at 3 jobs each fit two at a time in the default budget of 8.
let active = 0
let peak = 0
const budgetRun = {
  ...args,
  integrate: undefined,
  packages: ['A', 'B', 'C', 'D'].map(id => ({ id, name: id })),
}
await run(budgetRun, async (_prompt, opts) => {
  if (opts.label.startsWith('impl:')) {
    peak = Math.max(peak, ++active)
    await new Promise(resolve => setImmediate(resolve))
    active--
    return { id: opts.label.slice(5) }
  }
  return null
}, () => {}, () => {})
assert.equal(peak, 2)
await assert.rejects(run({ ...budgetRun, maxJobs: 0 }, agent, () => {}, () => {}), /maxJobs/)
let bad = 0
for (const p of prompts) {
  const flaws = ['undefined', '@@', '[object Object]', 'NaN'].filter(s => p.prompt.includes(s))
  if (flaws.length) bad++
  console.log(p.label.padEnd(16), String(p.isolation).padEnd(9), p.model.padEnd(7), String(p.prompt.length).padStart(6), flaws.join(',') || 'ok')
}
const show = (label, needle, width = 260) => {
  const p = prompts.find(x => x.label === label)
  const i = p.prompt.indexOf(needle)
  assert.notEqual(i, -1, label + ' missing ' + needle)
  console.log(`--- ${label} @ ${needle}: ` + (i < 0 ? 'MISSING' : p.prompt.slice(i, i + width).replace(/\n/g, ' | ')))
}
show('impl:A', 'Where you are', 120)
show('impl:A', 'cmake --preset', 330)
show('impl:A', 'PARALLEL_LEVEL', 70)
show('review:A', 'Where you are', 200)
show('review:A', 'Set up', 200)
show('review:A', '1. Independently', 90)
show('review:C', 'Where you are', 80)
show('review:C', 'Set up', 90)
show('review:C', '1. Independently', 60)
show('second:B', 'Where you are', 110)
show('second:B', 'Configure with', 80)
show('impl:B', 'A note for this package', 60)
show('impl:A', 'The repository and every worktree', 520)
show('integrate:T:1', 'This is stage', 60)
show('integrate:T:2', '4. Clean up', 700)
show('integrate:T:1', 'from the lead', 40)
show('integrate:T:2', 'from the lead', 40)
show('integrate:T:2', '3b.', 200)
console.log('3b in stage 1:', prompts.find(x => x.label === 'integrate:T:1').prompt.includes('3b.'))
assert.equal(bad, 0)
console.log('result keys:', Object.keys(result).join(','), '| stages:', result.earlierStages.length + 1, '| bad prompts:', bad)
