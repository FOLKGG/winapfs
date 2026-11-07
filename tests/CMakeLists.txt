add_executable(winapfs-verify verify.cpp)
target_link_libraries(winapfs-verify PRIVATE winapfs_core apfsrw bcrypt)
target_include_directories(winapfs-verify PRIVATE ${TP})
target_compile_options(winapfs-verify PRIVATE /W3)
target_link_options(winapfs-verify PRIVATE /ENTRY:wmainCRTStartup)

set(TD ${CMAKE_SOURCE_DIR}/testdata)
set(TEST_PASSWORD winapfs-test)

# image manifest-prefix volume [password]
set(VOLUMES
	"basic.container.img|basic|Basic|"
	"basic.img|basic|Basic|"
	"compressed.img|compressed|Compressed|"
	"links.img|links|Links|"
	"casesensitive.img|casesensitive|CaseSens|"
	"multivol.img|multivol|VolA|"
	"multivol.img|multivol|VolB|"
	"multivol.img|multivol|VolC|"
	"encrypted.img|encrypted|Public|"
	"encrypted.img|encrypted|Secret|${TEST_PASSWORD}")

# Library-level checks: every manifest field (contents, metadata, xattrs) through the core library
foreach(v ${VOLUMES})
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	list(GET f 3 pw)
	set(args ${TD}/${prefix}.${vol}.manifest.json --image ${TD}/${img} --volume ${vol} --fields all)
	if(pw)
		list(APPEND args --password ${pw})
	endif()
	add_test(NAME image.${img}.${vol} COMMAND winapfs-verify ${args})
	if(EXISTS ${TD}/${img})
	else()
		set_tests_properties(image.${img}.${vol} PROPERTIES DISABLED TRUE)
	endif()
endforeach()

# CLI smoke tests
add_test(NAME cli.info COMMAND winapfs-cli info ${TD}/multivol.img)
set_tests_properties(cli.info PROPERTIES PASS_REGULAR_EXPRESSION "VolA.*\n.*VolB.*\n.*VolC")
add_test(NAME cli.cat COMMAND winapfs-cli cat ${TD}/basic.container.img /hello.txt)
set_tests_properties(cli.cat PROPERTIES PASS_REGULAR_EXPRESSION "^Hello, APFS!\n$")
add_test(NAME cli.list COMMAND winapfs-cli list ${TD}/basic.img /names)
set_tests_properties(cli.list PROPERTIES PASS_REGULAR_EXPRESSION "colon:name\\.txt")
add_test(NAME cli.wrong_password COMMAND winapfs-cli list ${TD}/encrypted.img -v Secret -p nope)
set_tests_properties(cli.wrong_password PROPERTIES WILL_FAIL TRUE)

# Milestone 2: mount through WinFsp and check what Windows programs see
# (Win32 directory walk + CreateFile/ReadFile of every file against the manifest).
# MOUNT_FIELDS grows as fidelity features land.
set(MOUNT_FIELDS type,size,sha256)
if(TARGET winapfs)
	foreach(v ${VOLUMES})
		string(REPLACE "|" ";" f "${v}")
		list(GET f 0 img)
		list(GET f 1 prefix)
		list(GET f 2 vol)
		list(GET f 3 pw)
		if(img STREQUAL "links.img")
			continue()  # symlinks/FIFOs need reparse points (milestone 4)
		endif()
		set(args mount ${TD}/${img} * --volume ${vol})
		if(pw)
			list(APPEND args --password ${pw})
		endif()
		list(APPEND args --run "\"$<TARGET_FILE:winapfs-verify>\" \"${TD}/${prefix}.${vol}.manifest.json\" --dir %MOUNT% --fields ${MOUNT_FIELDS}")
		add_test(NAME mount.${img}.${vol} COMMAND winapfs ${args})
		set_tests_properties(mount.${img}.${vol} PROPERTIES RESOURCE_LOCK winfsp_mount)
		if(NOT EXISTS ${TD}/${img})
			set_tests_properties(mount.${img}.${vol} PROPERTIES DISABLED TRUE)
		endif()
	endforeach()
endif()

# Milestone 3: mount-all mounts every volume of every container it finds.
# Images here; tests/physical_test.ps1 does the same against attached VHDs
# (real \.\PhysicalDriveN, needs an elevated prompt).
if(TARGET winapfs AND EXISTS ${TD}/multivol.img AND EXISTS ${TD}/encrypted.img)
	add_test(NAME mount_all.images
		COMMAND winapfs mount-all ${TD}/multivol.img ${TD}/encrypted.img --password ${TEST_PASSWORD}
			--run "powershell -NoProfile -ExecutionPolicy Bypass -File \"${CMAKE_CURRENT_SOURCE_DIR}/verify_mounts.ps1\" -Verify \"$<TARGET_FILE:winapfs-verify>\" -Manifests ${TD}/multivol.VolA.manifest.json,${TD}/multivol.VolB.manifest.json,${TD}/multivol.VolC.manifest.json,${TD}/encrypted.Public.manifest.json,${TD}/encrypted.Secret.manifest.json")
	set_tests_properties(mount_all.images PROPERTIES RESOURCE_LOCK winfsp_mount)
endif()

# The raw-device code path (DeviceWinPhys: aligned reads, chunk cache,
# overlapped I/O) without admin: \.\C:\...\file.img is a device-namespace
# path to a plain file.
file(TO_NATIVE_PATH "${TD}" TD_NATIVE)
set(BS "\\")
foreach(v "basic.img|basic|Basic|" "compressed.img|compressed|Compressed|" "encrypted.img|encrypted|Secret|${TEST_PASSWORD}")
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	list(GET f 3 pw)
	set(args ${TD}/${prefix}.${vol}.manifest.json --image "${BS}${BS}.${BS}${TD_NATIVE}${BS}${img}" --volume ${vol} --fields all)
	if(pw)
		list(APPEND args --password ${pw})
	endif()
	add_test(NAME rawdev.${img}.${vol} COMMAND winapfs-verify ${args})
endforeach()
if(TARGET winapfs)
	add_test(NAME mount_rawdev.basic.img
		COMMAND winapfs mount "${BS}${BS}.${BS}${TD_NATIVE}${BS}basic.img" *
			--run "\"$<TARGET_FILE:winapfs-verify>\" \"${TD}/basic.Basic.manifest.json\" --dir %MOUNT% --fields ${MOUNT_FIELDS}")
	set_tests_properties(mount_rawdev.basic.img PROPERTIES RESOURCE_LOCK winfsp_mount)
endif()

# linux-apfs-rw engine (W1)
add_executable(apfsrw-smoke apfsrw_smoke.cpp)
target_link_libraries(apfsrw-smoke PRIVATE apfsrw)
target_link_options(apfsrw-smoke PRIVATE /ENTRY:wmainCRTStartup)

# Read-path parity of the linux-apfs-rw engine: every manifest field.
# Encrypted volumes are refused (the engine has no encryption yet, W6).
foreach(v ${VOLUMES})
	string(REPLACE "|" ";" f "${v}")
	list(GET f 0 img)
	list(GET f 1 prefix)
	list(GET f 2 vol)
	list(GET f 3 pw)
	add_test(NAME rwengine.${img}.${vol}
		COMMAND winapfs-verify ${TD}/${prefix}.${vol}.manifest.json --image ${TD}/${img} --volume ${vol} --engine rw --fields all)
	if(pw)
		set_tests_properties(rwengine.${img}.${vol} PROPERTIES WILL_FAIL TRUE)
	endif()
endforeach()

# Name matching: case/normalization-insensitive volumes find any spelling,
# case-sensitive ones only the exact one. Both engines, CLI and mount.
add_test(NAME cli.cat_case COMMAND winapfs-cli cat ${TD}/basic.container.img /HELLO.TXT)
set_tests_properties(cli.cat_case PROPERTIES PASS_REGULAR_EXPRESSION "^Hello, APFS!\n$")
if(TARGET winapfs)
	foreach(engine rw lib)
		add_test(NAME mount_case.${engine}.insensitive
			COMMAND winapfs mount ${TD}/basic.container.img * --engine ${engine} --run "cmd /c type %MOUNT%${BS}HELLO.TXT")
		set_tests_properties(mount_case.${engine}.insensitive PROPERTIES PASS_REGULAR_EXPRESSION "Hello, APFS!" RESOURCE_LOCK winfsp_mount)
		add_test(NAME mount_case.${engine}.sensitive
			COMMAND winapfs mount ${TD}/casesensitive.img * --engine ${engine} --run "cmd /c type %MOUNT%${BS}README.TXT")
		set_tests_properties(mount_case.${engine}.sensitive PROPERTIES WILL_FAIL TRUE RESOURCE_LOCK winfsp_mount)
	endforeach()
	# auto picks linux-apfs-rw for unencrypted volumes; keep the apfs-fuse mount path covered
	add_test(NAME mount_lib.basic.img
		COMMAND winapfs mount ${TD}/basic.img * --engine lib
			--run "\"$<TARGET_FILE:winapfs-verify>\" \"${TD}/basic.Basic.manifest.json\" --dir %MOUNT% --fields ${MOUNT_FIELDS}")
	set_tests_properties(mount_lib.basic.img PROPERTIES RESOURCE_LOCK winfsp_mount)
endif()
