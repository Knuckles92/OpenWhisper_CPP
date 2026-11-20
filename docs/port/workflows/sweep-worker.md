# Sweep worker brief: closing the port's open items from a session of your own

For a worker in a cloud session of its own. Read `CLAUDE.md` and
`docs/port/workflows/cloud-worker.md` first: the second one's "Set up", "Building and testing",
"What already fails on Linux", "Frozen files and scope" and "Handing back" sections apply to you
unchanged (your own container, clone and build tree; commit and push on your branch as you go;
report to the lead session with `send_message`). This page replaces its "The job" and "The
ledger" sections: a sweep package is not about the test ledger.

## Where the work comes from

The upstream test ledger is closed (every OpenWhisper test function is mapped) and every
behaviour id is audited. What is left is `docs/port/open-items.md`: findings of earlier code
reviews that no package picked up. A documentation worker has just checked each one against
the tree and sorted them: `docs/port/workflows/open-items-triage.md` lists every item with its first words (grep
them in `docs/port/open-items.md` to read the item in full: the review's wording is usually
more precise than the triage line), a class and a one-sentence plan.

- **A, parity**: OpenWhisper does something a user could notice and FioriVoce does not, or does
  it differently. OpenWhisper is the behaviour reference: read the Python it concerns
  (`/home/user/knuckles92/openwhisper`, read-only) before you change anything, and make the C++
  do what the Python does, strings verbatim ("OpenWhisper" in copy is `identity().product`).
  Some A items are marked "unsure": establish first whether FioriVoce really differs. If it
  already agrees, the item is closed by a test that proves it (write one if none exists).
- **B, defect**: FioriVoce's own code can crash, hang, race, leak or give a wrong result. Fix
  the cause. Where the Python has the same fault and the item says so, the fix is still wanted,
  and you say in your report that it deliberately differs from OpenWhisper and how.

Your package names its items by class and number, as in the triage file ("A7", "B17").

## For each item

1. Read the open-items row in full, the code it names, and (for A) the Python.
2. Decide what is true now: the tree has moved a great deal since most rows were written, and
   some are already closed or half closed. Do not redo what exists.
3. Fix it, with a test that fails without the fix (say in your report that you checked that,
   and how). A test goes in the `tests/<dir>` of the code it tests; new files are safest
   (`test_sweep_<topic>.cpp`), because other workers are editing the same directories on other branches.
4. If the item turns out to need a decision nobody has written down (two defensible behaviours,
   neither OpenWhisper's), do not choose silently: do the part that is not in doubt, and put
   the question in your report with what each choice would mean.
5. If a deliberate difference results (a B fix where Python has the fault, or a behaviour a
   recorded decision keeps), say so in your report with the one-sentence "OpenWhisper /
   FioriVoce / reason" the lead needs to record it. Do not edit `docs/port/differences.md` or
   `docs/port/open-items.md`. If an audit entry in `docs/port/audit/*.json` describes the
   behaviour you changed and is now wrong, correct its note with the locked helper only:
   `python3 -I -B tools/parity/auditput.py <file>.json features <ID> '<entry as JSON>'` (read the entry
   first; keep its status unless your change makes it untrue).

Never because it is hard: an item you could not finish stays open, and your report says exactly
what is left and what blocks it. Do not weaken, skip, tag away or delete an existing test.

Windows-only code under `win/` cannot be compiled or run here. Where an item's fix belongs
there, keep the logic platform-neutral and tested if the design allows; otherwise leave the
item open and say what the Windows change would be, rather than writing code nobody can build.

## Your report (final message)

1. Per item (by its class and number and its first words): closed (what changed, which test),
   already closed (the evidence), partly closed (what is left), or not done (why).
2. Deliberate differences and open questions for the lead.
3. Files added or changed, by directory.
4. The last run of each test directory you touched (passed / failed, and the cause of any
   failure; `cloud-worker.md` lists what fails on Linux for reasons that are not port defects)
   and of `-L lint`.
5. Anything not compiled or not run here.
6. Your branch name and its last commit.
