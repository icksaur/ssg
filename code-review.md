# Code review

Reviewer: Claude Opus 4.8

## Finding

MUST lifecycle safety — `HttpEditorRoute` registers callbacks that capture its
state and owns joinable writer threads, so destroying it while the externally
owned server is running can terminate or use freed state.

## Resolution

The reviewed design requires the server to outlive the route and the route to
outlive the started server. The public API now states that precondition and the
route destructor enforces it by terminating on destruction while the server is
still bound. Lifecycle tests stop the shared server before route destruction.
The convenience `HttpEditorServer` continues to enforce the order internally.
