# winapfs-fsck: apfsprogs' apfsck built with MSVC (see compat/)
set(APFSPROGS ${TP}/apfsprogs)
file(GLOB APFSCK_SRC ${APFSPROGS}/apfsck/*.c)
file(GLOB APFSPROGS_LIB_SRC ${APFSPROGS}/lib/*.c)
set(FSCK_C_SRC compat.c ${APFSCK_SRC} ${APFSPROGS_LIB_SRC})

add_executable(winapfs-fsck main.cpp ${FSCK_C_SRC} ${CMAKE_SOURCE_DIR}/assets/app_icon.rc)
set_target_properties(winapfs-fsck PROPERTIES C_STANDARD 23)
# compat/ shadows POSIX headers. Per-source so the C++ front-end never sees it
# (the VS generator does not support $<COMPILE_LANGUAGE> in include paths).
set_source_files_properties(${FSCK_C_SRC} PROPERTIES
	INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/compat;${APFSPROGS}/include;${APFSFUSE}/3rdparty/lzfse/src"
	COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/compat/kfsck_compat.h;/W3;/wd4244;/wd4267;/wd4018;/wd4146;/wd4996")
set_source_files_properties(${APFSPROGS}/apfsck/apfsck.c PROPERTIES COMPILE_DEFINITIONS main=apfsck_main)
target_link_libraries(winapfs-fsck PRIVATE winapfs_core zlib lzfse)
target_link_options(winapfs-fsck PRIVATE /ENTRY:wmainCRTStartup)
install(TARGETS winapfs-fsck RUNTIME DESTINATION bin)
