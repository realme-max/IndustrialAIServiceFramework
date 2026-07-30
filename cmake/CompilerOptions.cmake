function(iaisf_enable_project_warnings target_name)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(
      "${target_name}"
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
    )
  elseif(MSVC)
    target_compile_options("${target_name}" PRIVATE /W4 /permissive-)
  endif()
endfunction()

