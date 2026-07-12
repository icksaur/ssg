# settings-model

- Spec: `doc/features/settings.md`
- Depends: `foundation-harness`
- Branch: `settings-model-task`

## Scope

Implement typed setting keys/values, five-scope resolution, versioned
Linux/Windows persistence seams, and reversible set/reset commands.

## Files

`include/ssg/settings.h`, `src/settings.cpp`,
`src/platform/linux_settings.cpp`, `src/platform/windows_settings.cpp`,
`tests/test_settings.cpp`, `tests/test_settings_persistence.cpp`,
`cmake/components/settings-model.cmake`

## Oracle

Hand-authored scope tables, schema/restart round trips, invalid-value
atomicity, and compensating-command restoration.

## Done

The mandatory workflow is complete for settings only.
