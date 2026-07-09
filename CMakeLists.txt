cmake_minimum_required(VERSION 3.16)
project(plume C)

set(CMAKE_C_STANDARD 11)

# IHSlib as a subproject: no samples, no codegen (uses committed protobuf .c).
set(IHSLIB_SAMPLES OFF CACHE BOOL "" FORCE)
set(IHSLIB_CODEGEN OFF CACHE BOOL "" FORCE)

# Link IHSlib statically into our binary. Buildroot forces -DBUILD_SHARED_LIBS=ON,
# but IHSlib's static sub-libraries aren't -fPIC, so a shared libihslib.so fails
# to link on aarch64. We're its only consumer, so a static lib is simpler and
# self-contained. (Shadows the cache var for this scope + the IHSlib subdir.)
set(BUILD_SHARED_LIBS OFF)

# Prefer a vendored copy (git submodule) so offline builds work — this is the
# path Buildroot/Recalbox takes. Fall back to fetching it for a plain dev clone.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/IHSlib/CMakeLists.txt)
    add_subdirectory(external/IHSlib)
else()
    include(FetchContent)
    FetchContent_Declare(ihslib
            GIT_REPOSITORY https://github.com/mariotaku/IHSlib.git
            GIT_TAG main)   # ponytail: pin a commit for reproducible builds
    FetchContent_MakeAvailable(ihslib)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3 sdl3-ttf)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale libswresample)

add_executable(plume src/main.c src/media.c src/ui.c)
# Keep frame pointers and export symbols so the SIGSEGV handler's backtrace names
# functions instead of bare addresses. Costs nothing measurable here.
target_compile_options(plume PRIVATE -g -fno-omit-frame-pointer)
target_link_options(plume PRIVATE -rdynamic)
target_include_directories(plume PRIVATE ${SDL3_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
# ihslib-hid-sdl carries the gamepad->HID bridge; the host only sees controllers
# through the HID channel.
target_link_libraries(plume PRIVATE ihslib ihslib-hid-sdl ${SDL3_LIBRARIES} ${FFMPEG_LIBRARIES})
target_compile_options(plume PRIVATE ${SDL3_CFLAGS_OTHER} ${FFMPEG_CFLAGS_OTHER})
