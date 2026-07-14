---
name: add-unit-test
description: Add or update ParaKV unit tests in the repository. Use when Codex needs to create a new C++/CUDA/NPU/MLU unit test, place a test under tests/, wire it into CMake with cc_test, update an existing test target, choose platform gates, or validate test naming and dependencies against current xLLM test conventions.
---

# Add Unit Test

## Workflow

1. Inspect the production code and the nearest existing tests before writing a new test.
   - Match the production path under `parakv/` to `tests/` where possible.
   - Prefer extending an existing nearby `*_test.cpp` and `cc_test` target when the behavior belongs to the same domain.
   - Create a new test source only when it improves isolation, keeps platform setup separate, or follows an existing directory pattern.

2. Read the project style guide before editing production files under `parakv/`, and apply the same C++ style discipline to new test code:
   `.agents/skills/code-review/references/custom-code-style.md`.

3. Follow the current test layout and CMake conventions.
   - Read [parakv-test-patterns.md](references/parakv-test-patterns.md) when adding a new test file, new `cc_test`, platform-specific test, or test directory.
   - Use `*_test.cpp` for C++ test files and `*_test.cu` for CUDA source tests.
   - Do not create nested `test/` or `tests/` directories for new unit tests unless the surrounding tree already requires that structure.

4. Wire tests through CMake with `include(cc_test)` and `cc_test(...)`.
   - Keep source names relative to the current test directory unless an existing target already uses an absolute source path for a production `.cpp`.
   - Use target names ending in `_test`.
   - Put platform-directory gates in the parent `CMakeLists.txt` when the whole child directory is platform-specific.

5. Write tests for observable behavior, not implementation trivia.
   - Cover success, edge, and error paths touched by the change.
   - Prefer deterministic inputs, fixed seeds, and small tensors/data structures.
   - Keep helpers file-local in an anonymous namespace unless shared by multiple test files.
   - Use `TEST`/`TEST_F` names that describe behavior clearly.

6. Validate narrowly before finishing.
   - Always run `git diff --check` for the changed test paths.
   - Search for stale filenames after moving or renaming tests.
   - Run the narrowest build/test command available locally; if not feasible, state the exact reason and what was checked instead.

## Common Commands

```bash
rg --files tests/<area>
rg "old_test_name|old_file_name" tests parakv CMakeLists.txt
git diff --check -- tests/<area>
```

For full remote validation on the development machine, use the repository AGENTS instructions for SSH, container, build, and test commands.
