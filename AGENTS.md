# Core agents
- Stick to SOLID principles and patterns as much as possible.
- Run clang-tidy using `ci/run_clang_tidy_diff.sh` and ensure to issues remain.
- Run all tests using `ci/run_all_tests.sh` and ensure all pass.
- Private members and variables should be prefixed with an underscore, i.e. `_value`.