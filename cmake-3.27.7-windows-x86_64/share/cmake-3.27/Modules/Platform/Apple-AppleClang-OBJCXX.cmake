include(Platform/Apple-Clang-OBJCXX)
if(NOT CMAKE_OBJCXX_COMPILER_VERSION VERSION_LESS 4.2)
  set(CMAKE_OBJCXX_SYSTEM_FRAMEWORK_SEARCH_FLAG "-iframework ")
else()
  unset(CMAKE_OBJCXX_SYSTEM_FRAMEWORK_SEARCH_FLAG)
endif()
