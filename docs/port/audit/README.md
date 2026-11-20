# Behavior audits

`py -3.12 -I -B tools/parity/feature_audit.py` compares the inventory in the area maps with evidence in this directory. Choose areas with `--areas a05,a14`. The `--final` option also rejects handoffs. An unaudited ID indicates that a reviewer still needs to check the behavior. Many already have implementations and tests.

Each JSON document names its `area` and `package`. Its `features` and optional `checklist` objects map inventory IDs to evidence. A `verified` or `fixed` entry names existing implementation and test files in the `cpp` and `tests` arrays. A fix includes a `note`. A `dropped` or `other-platform` entry records its `reason`. A `handoff` names its destination in `to` and describes the work in `note`.

A `difference` entry references an ID in a document's `differences` array. That record identifies the affected `features` and gives `openwhisper`, `openwhisper-cpp`, `reason` and `source`. Run `tools/parity/differences.py` to render the collected differences. The strict parity report checks both the behavior audit and the merged upstream test mappings.
