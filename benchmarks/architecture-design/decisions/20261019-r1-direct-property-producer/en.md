# R1.1 direct property batch producer acceptance

An optional synchronous cell API reuses the authenticated cursor, owner/generation checks, physical page ordering, old snapshots, and error propagation. Existing matrix/cache APIs remain. No format, durability, or isolation change.

DirectPropertyColumns owns budgeted primitive arrays, string offsets/bytes, and complex payload without conversion from a full PropertyValue matrix. Missing, stored NULL, and mixed types remain distinct. Borrows validate owner/incarnation and snapshot; failed construction cannot seal. Gather sorts/deduplicates only source lookup while preserving output order and duplicates.

Five necessary modules initially passed 99/100. The new integration fixture mistakenly opened read-only before creating data; correcting the open mode passed. After strengthening failed-construction sealing, all four affected cases passed again. Preserve the failure and targeted result: 100 distinct cases verified through follow-up, not one 100/100 run. No full suite or performance matrix.

Accept R1.1 under its preregistered foundational capability classification. R1.3/R7 validates performance benefit; interface existence/test elapsed time is not performance admission.
