# M10-2 optional-subsystem construction audit.
# A low-level facility (linked into every ssg build) that optional subsystems
# call to record their construction, so the fast-startup oracle can assert none
# runs on the first-frame path.

target_sources(ssg_platform PRIVATE
    ${SSG_SOURCE_DIR}/src/startup_audit.cpp
)
