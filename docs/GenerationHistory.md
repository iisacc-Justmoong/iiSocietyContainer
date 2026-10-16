# Generation History publication

Generated image originals are PNG files directly below `Generation History/`.
Dreamscapes names them `<job UUID>-0001.png`. Generation does not create an
iiSharedCanvas document. Opening a PNG for editing creates a separate `.iisc`
document in Society `Files/` while retaining the PNG in History. Image attachments
and generation input canvases also belong to Files. Society-local native document
saves and exports must use Files; the editor defaults its save dialog there.

`SharedStorage::publishGeneratedImage(source, fileName, error)` publishes an
already decoded and validated PNG into the pinned Society drive. It accepts one
visible `.png` leaf, checks the PNG signature and direct source path, and reuses
iiFileProvider's no-replacement atomic commit. Decoding, dimensions and image
metadata validation belong to the caller. The source bytes are copied unchanged.
Temporary files and their maintenance lease live in
`.society-runtime/generation-history/`; History observers only see the committed
file. A collision, redirected source/section, invalid filename or unavailable
drive returns an empty path with an error. Existing output is never overwritten.

`iiSocietyHelper::FileSystem::publishGeneratedImage()` delegates to this contract
and reports failures through `errorString`. Presence, delivery and a running
Society daemon are unnecessary. Its selected drive remains pinned until
`open()`/`refresh()`, even when another app changes the shared default.

Society's image gallery and `DashboardFiles::generationHistory` show generated
image originals. `.iisc` documents and editor wrappers do not duplicate PNGs in
the gallery. JSON metadata, hidden files, redirected entries and app subfolders
are excluded. Existing canvases are preserved on disk.

Regression coverage is in `iiSocietyContainer.shared_storage`,
`iiSocietyContainer.dashboard` and `iiSocietyHelper.filesystem`. Dreamscapes
generation tests additionally verify PNG publication, observed History entries,
editor wrapping, native inference and host/replica synchronization.
