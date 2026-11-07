add_executable(winapfs
	main.cpp
	ApfsFs.cpp
	ApfsFs.h)
target_include_directories(winapfs SYSTEM PRIVATE ${WINFSP_INCLUDE_DIR})
target_link_libraries(winapfs PRIVATE winapfs_core ${WINFSP_LIBRARY} delayimp)
target_link_options(winapfs PRIVATE /ENTRY:wmainCRTStartup /DELAYLOAD:winfsp-x64.dll)
target_compile_options(winapfs PRIVATE /W4 /wd4100)
install(TARGETS winapfs RUNTIME DESTINATION bin)
