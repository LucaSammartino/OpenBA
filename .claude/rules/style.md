# Code style

Applies to new and contributed code. The reference is `.clang-format` at the repo root.

- One statement per line. No `a(); b();` on one line, no `if (x) { a(); b(); }` squeezed onto one line.
- No minified or golfed functions: a body longer than a single `return` / one trivial statement gets
  its own lines. A one-line accessor or `if (x) return;` is fine.
- 4-space indent, no tabs, attached braces, `T* p`, namespaces not indented, about 110 columns.
- Do not sort includes: they carry retail-address comments and their order matters.
- Hand-aligned `a.x   = ...` blocks and trailing-comment columns are kept as written; keep new
  ones tidy.
- Format only the lines you touch (`git clang-format`, or `clang-format --lines=a:b`). Existing files
  are not reformatted wholesale; a formatting-only diff buries the change in review.
- The config is a close fit, not an exact one (about 10-30% of lines in the sampled files differ,
  almost all hand alignment and trailing comments). Trust the surrounding file over the tool.
