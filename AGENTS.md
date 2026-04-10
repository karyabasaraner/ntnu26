# Core agents
- Stick to SOLID principles and patterns as much as possible.
- Run clang-tidy using `ci/run_clang_tidy_diff.sh` and ensure no issues remain.
- Run all tests using `ci/run_all_tests.sh` and ensure all pass.
- Private members and variables should be prefixed with an underscore, i.e. `_value`.
- Introduce tests incrementally in small slices. Do not move to the next slice until the current slice is green.
- Default slice size is one tightly coupled header/implementation pair plus its tests.
- When code is too coupled to test directly, extract narrow helpers or adapters instead of widening public APIs broadly.
- Keep new tests deterministic: avoid unbounded sleeps, prefer explicit predicates and tight timeouts.
- Use the `GIVEN`, `WHEN`, `THEN` framework for tests.
