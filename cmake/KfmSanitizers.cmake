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
  endif()
  message(STATUS "KFM: sanitizers enabled: ${KFM_SANITIZE}")
endif()
