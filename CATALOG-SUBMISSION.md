# Official open-field catalog submission

Source: https://github.com/LaP0573/open-field/blob/main/docs/catalog.md

`PUBLISH-analogBEAST-field.ps1` submits the entry as a **pinned** release, not as a branch follower. It computes the release pack's SHA-256, size and git commit SHA, verifies the entry, pushes a branch into `dzwerg/open-field` and opens the PR upstream.

The included `catalog/packs/analogbeast.json` is a planning template only; **do not submit the template as-is**. The PowerShell publisher generates the validated, pinned entry.

The `library` category is provisional because the upstream catalog does not currently define an `instruments` category. Ask the maintainer to decide during PR review.
