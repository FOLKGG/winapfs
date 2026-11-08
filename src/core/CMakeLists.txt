add_library(winapfs_core STATIC
	winapfs/Apfs.cpp
	winapfs/Apfs.h
	winapfs/Compression.cpp
	winapfs/DiskLock.cpp
	winapfs/DiskLock.h
	winapfs/Compression.h
	winapfs/Gpt.cpp
	winapfs/Gpt.h
	winapfs/Names.cpp
	winapfs/Names.h
	winapfs/RwVolume.cpp)
target_include_directories(winapfs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(winapfs_core PUBLIC apfslib PRIVATE apfsrw)
target_compile_options(winapfs_core PRIVATE /W4 /wd4100)
