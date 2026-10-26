# Minimal completed-image field-update chain

Progress stays 15%; S2 is unaccepted. Fourteen isolated read/query module cases pass. The new case opens and closes a public read-only reader, updates an integer column through the existing private field writer, preserves membership roots/counts and old-pin values, refuses stale scan authority, obtains fresh authority after complete verification, closes all private readers/builder, then reopens publicly and queries the new value. It does not mix writes and reads across uncoordinated private/public services.

This round adds a necessary test, not implementation. Public writable opening remains refused. Atomic node/label maintenance, recovery, real-dataset space/resources and final performance gates remain incomplete. No new benchmark, full unit suite, commit or push. See receipt.json and the existing label-fallback compile/link recipes.
