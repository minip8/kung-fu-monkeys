# Warnings-as-errors interface target. Every KFM target links kfm::warnings.
add_library(kfm_warnings INTERFACE)
add_library(kfm::warnings ALIAS kfm_warnings)

if(MSVC)
  target_compile_options(kfm_warnings INTERFACE
    /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8
    /wd4324  # structure padded due to alignas: intentional in pools
  )
else()
  target_compile_options(kfm_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wold-style-cast -Wcast-align -Wnon-virtual-dtor -Woverloaded-virtual
    -Wnull-dereference -Wimplicit-fallthrough
  )
endif()

# Flags for the deterministic layers (core, sim): no exceptions, no RTTI.
# Applied per target rather than globally so apps and tests keep the defaults.
function(kfm_no_exceptions_rtti target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /GR-)
  else()
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
  endif()
endfunction()
