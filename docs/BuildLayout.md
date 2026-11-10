# Build and resource layout

The repository `build/` contains the module, its platform host and test
compilation outputs. Demonstration applications are disabled by default; set
`IISOCIETYCONTAINER_BUILD_EXAMPLES=ON` to opt in. The loading benchmark remains
an explicit target and is not built by the default target.

Container volumes and other runtime resources are user data. They belong under
`resources/`, never in a disposable build directory. Existing volumes recovered
from older build trees must be retained without opening or mounting them. An
existing consumer may temporarily use a compatibility link to a recovered SDK
package while its configuration still references the old path.

Several legacy container tests currently use the build directory for fixtures
or reports, and some launch a daemon, mount a volume, or execute an installed
consumer. Their isolation remains a separate migration; clean-layout checks
must not run those tests until they are moved to private resource workspaces.
Native directory, file-tree, maintenance and file-repository tests, including
their Qt-enabled counterparts, instead run in disposable `resources/test-workspaces/`
workspaces and clean their fixtures after each test.
