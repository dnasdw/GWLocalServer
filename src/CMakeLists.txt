AUTO_FILES("." "src" "\\.(cpp|h)$")
include_directories(${COMMON_INCLUDE_DIR})
link_directories(${COMMON_LIBRARY_DIR})
if(WIN32)
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
  set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} /MT")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /MT")
endif()
ADD_EXE(GWLocalServer "${src}")
if(WIN32)
  target_link_libraries(GWLocalServer WS2_32
    debug libevent_extras_d optimized libevent_extras
    debug libevent_core_d optimized libevent_core)
else()
  target_link_libraries(GWLocalServer
    event_extra
    event_core)
endif()
install(TARGETS GWLocalServer DESTINATION bin)
