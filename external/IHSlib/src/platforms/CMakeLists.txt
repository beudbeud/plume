add_library(ihs-platforms STATIC)
target_include_directories(ihs-platforms PRIVATE ../ ../../include)

if (UNIX)
    # Pure POSIX/pthread backend: no SDL dependency, so ihslib can be linked into
    # an SDL3 application without SDL2/SDL3 symbol clashes.
    target_sources(ihs-platforms PRIVATE ihs_thread_posix.c ihs_ip_posix.c ihs_udp_posix.c)
    find_package(Threads REQUIRED)
    target_link_libraries(ihs-platforms PUBLIC Threads::Threads)
else ()
    # Fallback (e.g. Windows): SDL2 threads + SDL2_net sockets.
    target_sources(ihs-platforms PRIVATE ihs_thread_sdl.c ihs_udp_sdl.c)
    pkg_check_modules(SDL2_NET SDL2_net REQUIRED)
    target_include_directories(ihs-platforms PRIVATE SYSTEM ${SDL2_INCLUDE_DIRS} ${SDL2_NET_INCLUDE_DIRS})
    target_link_libraries(ihs-platforms PUBLIC ${SDL2_LIBRARIES} ${SDL2_NET_LIBRARIES})
endif ()
