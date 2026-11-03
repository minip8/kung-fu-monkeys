# cmake -DKFM_CONTENT_DIR=<content/> -DKFM_DEST=<exe dir>/content -P CopyContent.cmake
# Mirrors content/**/*.toml into an executable's content/ directory: stale
# *.toml files there (renamed or deleted in the source tree) are removed
# first, so the copy always loads the same set as the embedded one. Other
# files there (the test rig's models/) are left alone.
file(GLOB_RECURSE stale "${KFM_DEST}/*.toml")
if(stale)
  file(REMOVE ${stale})
endif()
file(COPY "${KFM_CONTENT_DIR}/" DESTINATION "${KFM_DEST}" FILES_MATCHING PATTERN "*.toml")
