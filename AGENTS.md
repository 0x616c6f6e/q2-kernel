# Kernel development

Read README.md and task-relevant docs/ and include/q2/ contracts.
In a Q2 workspace also read ../docs/architecture.md, ../docs/development.md and ../ai/kernel.md.
Kernel is an independent repository: do not require parent files to build or run contract checks.
Keep portable C11, four-space/110-column style, public API comments and unified q2_result_t.
SDKs and timing recognition belong to platform; Lua applications remain separate.
Do not edit third_party except for an explicit dependency task; preserve provenance and file hashes.
Run relevant tests and scripts/check_contracts.py. Report actual evidence and hardware limitations.
