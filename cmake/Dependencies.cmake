include(FetchContent)

if(IAISF_USE_SYSTEM_DEPS)
  find_package(nlohmann_json 3.11.3 REQUIRED)
  if(IAISF_BUILD_TESTS)
    find_package(GTest 1.15.2 REQUIRED)
  endif()
  return()
endif()

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

if(IAISF_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)
endif()

