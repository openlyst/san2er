cmake_minimum_required(VERSION 3.10.2)
project(pvr_patcher C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED libzip)

add_executable(pvr-patcher
    main.c
    apk_check.c
    elf_symbols.c
    stub_gen.c
)

target_include_directories(pvr-patcher PRIVATE ${LIBZIP_INCLUDE_DIRS})
target_link_libraries(pvr-patcher ${LIBZIP_LIBRARIES})
