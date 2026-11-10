# KFM_SANITIZE is a comma-separated list, e.g. "address,undefined".
# Applied globally because mixing sanitized and unsanitized objects is unsafe.
if(KFM_SANITIZE)
  if(MSVC)
    if(KFM_SANITIZE MATCHES "address")
      add_compile_options(/fsanitize=address)
    endif()
  else()
    add_compile_options(-fsanitize=${KFM_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${KFM_SANITIZE})
    if(KFM_SANITIZE MATCHES "undefined")
      # The deterministic layers (core, sim, net) are built with -fno-rtti, so
      # the typeinfo UBSan's vptr check wants for ITransport implementations
      # does not exist and sanitized tests fail to link against them.
      add_compile_options(-fno-sanitize=vptr)
      add_link_options(-fno-sanitize=vptr)
    endif()
  endif()
  # A sanitized GCC cannot fold some address comparisons at compile time
  # (kfm/core/assert.hpp, KFM_STATIC_ASSERT_HOOKS).
  add_compile_definitions(KFM_SANITIZE_BUILD=1)
  message(STATUS "KFM: sanitizers enabled: ${KFM_SANITIZE}")
endif()
