# Lossless archive of oversized historical source evidence

Historical source files exceeding 3,000 lines are preserved verbatim in sources.tar.gz. manifest.json records original repository-relative paths, hashes, byte and line counts, checked against archive members. Historical conclusions and measurements are unchanged.

Before replaying an experiment that uses the old paths, run `python3 restore.py /path/to/historical-checkout` in a separate historical workspace, then its original reproduction script. Never add restored historical sources to the current production source set.
