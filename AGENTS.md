# Repository work

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing this repository. It defines the documentation, verification, licensing, and contributor-credit requirements for every PR.

## Find the relevant guidance

- For firmware or hardware work, read [development](docs/development.md), [porting](docs/porting.md), and [hardware verification](docs/hardware-validation.md). Use the project's configured ESP-IDF APIs and the board checklist in CONTRIBUTING.md.
- For CLI, simulator, plugin, or Linux behavior, read the corresponding user guide and existing tests before changing the interface. The contribution guide maps change types to documentation and verification.
- For website or guide changes, inspect `site/package.json` for commands and `site/scripts/docs.mjs` for published pages. A new guide needs working navigation, search, and links.
- For vendor-derived code or register data, follow the licensing section in CONTRIBUTING.md and inspect `NOTICE` and `THIRD_PARTY_NOTICES.md` before adding material.

## Complete the change

1. Inspect the working tree and preserve unrelated user changes. Keep the implementation scoped to the requested behavior.
2. Update the affected documentation in the same PR, or explain why no documentation changes apply.
3. Run the relevant checks from [development](docs/development.md). For a bug fix, verify the observable failure and corrected result where practical. Do not weaken assertions to make a failure disappear.
4. Review the final diff for unintended changes, missing notices, and stale instructions.
5. Report what changed, the checks run, their results, and any remaining limitations. Distinguish hardware measurements, simulations, test doubles, and untested assumptions.

Keep contributor authorship and credit when fixing or salvaging another person's work. Merge only when authorized and the current PR revision passes CI. Do not describe an experimental hardware port as verified without the required physical report.
