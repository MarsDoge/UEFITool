CMAKE_MINIMUM_REQUIRED(VERSION 3.22)

PROJECT(UEFITool LANGUAGES C CXX)

SET(CMAKE_CXX_STANDARD 11)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

FIND_PACKAGE(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
FIND_PACKAGE(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

SET(PROJECT_FORMS
 uefitool.ui
 searchdialog.ui
 hexviewdialog.ui
 gotobasedialog.ui
 gotoaddressdialog.ui
 )

SET(PROJECT_HEADERS 
 uefitool.h
 hexspinbox.h
 searchdialog.h
 hexviewdialog.h
 gotobasedialog.h
 gotoaddressdialog.h
)

SET(PROJECT_SOURCES 
 icons/uefitool.icns
 uefitool.rc
 uefitool_main.cpp
 uefitool.cpp
 searchdialog.cpp
 hexviewdialog.cpp
 hexlineedit.cpp
 ffsfinder.cpp
 hexspinbox.cpp
 ../common/fitparser.cpp
 ../common/guiddatabase.cpp
 ../common/nvram.cpp
 ../common/nvramparser.cpp
 ../common/meparser.cpp
 ../common/ffsops.cpp
 ../common/fixedlayout.cpp
 ../common/types.cpp
 ../common/descriptor.cpp
 ../common/ffs.cpp
 ../common/peimage.cpp
 ../common/utility.cpp
 ../common/ffsbuilder.cpp
 ../common/ffsparser.cpp
 ../common/amd_microcode.cpp
 ../common/ffsreport.cpp
 ../common/treeitem.cpp
 ../common/treemodel.cpp
 ../common/brotli/common/constants.c
 ../common/brotli/common/context.c
 ../common/brotli/common/dictionary.c
 ../common/brotli/common/platform.c
 ../common/brotli/common/shared_dictionary.c
 ../common/brotli/common/transform.c
 ../common/brotli/dec/bit_reader.c
 ../common/brotli/dec/decode.c
 ../common/brotli/dec/huffman.c
 ../common/brotli/dec/prefix.c
 ../common/brotli/dec/state.c
 ../common/brotli/dec/static_init.c
 ../common/LZMA/LzmaCompress.c
 ../common/LZMA/LzmaDecompress.c
 ../common/LZMA/SDK/C/CpuArch.c
 ../common/LZMA/SDK/C/Bra.c
 ../common/LZMA/SDK/C/Bra86.c
 ../common/LZMA/SDK/C/LzFind.c
 ../common/LZMA/SDK/C/LzmaDec.c
 ../common/LZMA/SDK/C/LzmaEnc.c
 ../common/Tiano/EfiTianoDecompress.c
 ../common/Tiano/EfiTianoCompress.c
 ../common/Tiano/EfiTianoCompressLegacy.c
 ../common/ustring.cpp
 ../common/digest/sha1.c
 ../common/digest/sha256.c
 ../common/digest/sha512.c
 ../common/digest/sm3.c
 ../common/generated/ami_nvar.cpp
 ../common/generated/apple_sysf.cpp
 ../common/generated/dell_dvar.cpp
 ../common/generated/edk2_vss.cpp
 ../common/generated/edk2_vss2.cpp
 ../common/generated/edk2_ftw.cpp
 ../common/generated/insyde_fdc.cpp
 ../common/generated/insyde_fdm.cpp
 ../common/generated/ms_slic_marker.cpp
 ../common/generated/ms_slic_pubkey.cpp
 ../common/generated/phoenix_evsa.cpp
 ../common/generated/phoenix_flm.cpp
 ../common/generated/intel_acbp_v1.cpp
 ../common/generated/intel_acbp_v2.cpp
 ../common/generated/intel_keym_v1.cpp
 ../common/generated/intel_keym_v2.cpp
 ../common/generated/intel_acm.cpp
 ../common/kaitai/kaitaistream.cpp
 ../common/zlib/adler32.c
 ../common/zlib/compress.c
 ../common/zlib/crc32.c
 ../common/zlib/deflate.c
 ../common/zlib/gzclose.c
 ../common/zlib/gzlib.c
 ../common/zlib/gzread.c
 ../common/zlib/gzwrite.c
 ../common/zlib/inflate.c
 ../common/zlib/infback.c
 ../common/zlib/inftrees.c
 ../common/zlib/inffast.c
 ../common/zlib/trees.c
 ../common/zlib/uncompr.c
 ../common/zlib/zutil.c
)

IF(QT_VERSION_MAJOR EQUAL 6)
 QT6_ADD_RESOURCES(PROJECT_SOURCES uefitool.qrc)
ELSE()
 QT5_ADD_RESOURCES(PROJECT_SOURCES uefitool.qrc)
ENDIF()

ADD_DEFINITIONS(
 -DU_ENABLE_NVRAM_PARSING_SUPPORT
 -DU_ENABLE_ME_PARSING_SUPPORT
 -DU_ENABLE_FIT_PARSING_SUPPORT
 -DU_ENABLE_GUID_DATABASE_SUPPORT
)

SET_SOURCE_FILES_PROPERTIES(icons/uefitool.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

LIST(REMOVE_ITEM PROJECT_SOURCES uefitool_main.cpp icons/uefitool.icns uefitool.rc)
ADD_LIBRARY(UEFIToolEditing OBJECT ${PROJECT_HEADERS} ${PROJECT_FORMS} ${PROJECT_SOURCES})
TARGET_INCLUDE_DIRECTORIES(UEFIToolEditing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/UEFIToolEditing_autogen/include")
TARGET_LINK_LIBRARIES(UEFIToolEditing PUBLIC Qt${QT_VERSION_MAJOR}::Widgets QHexView)
SET_TARGET_PROPERTIES(UEFIToolEditing PROPERTIES AUTOMOC ON AUTOUIC ON)
ADD_EXECUTABLE(UEFITool uefitool_main.cpp icons/uefitool.icns uefitool.rc)
TARGET_LINK_LIBRARIES(UEFITool PRIVATE UEFIToolEditing)

TARGET_INCLUDE_DIRECTORIES(UEFITool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

TARGET_LINK_LIBRARIES(UEFITool PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

ADD_SUBDIRECTORY(QHexView)
TARGET_LINK_LIBRARIES(UEFITool PRIVATE QHexView)

SET_TARGET_PROPERTIES(UEFITool PROPERTIES
 WIN32_EXECUTABLE ON
 MACOSX_BUNDLE ON
 MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
 AUTOMOC ON
 AUTOUIC ON
)

IF(UNIX AND (NOT APPLE) AND (NOT CYGWIN))
 SET_TARGET_PROPERTIES(UEFITool PROPERTIES OUTPUT_NAME uefitool)
 INSTALL(FILES icons/uefitool_16x16.png   DESTINATION share/icons/hicolor/16x16/apps   RENAME uefitool.png)
 INSTALL(FILES icons/uefitool_32x32.png   DESTINATION share/icons/hicolor/32x32/apps   RENAME uefitool.png)
 INSTALL(FILES icons/uefitool_64x64.png   DESTINATION share/icons/hicolor/64x64/apps   RENAME uefitool.png)
 INSTALL(FILES icons/uefitool_128x128.png DESTINATION share/icons/hicolor/128x128/apps RENAME uefitool.png)
 INSTALL(FILES icons/uefitool_256x256.png DESTINATION share/icons/hicolor/256x256/apps RENAME uefitool.png)
 INSTALL(FILES icons/uefitool_512x512.png DESTINATION share/icons/hicolor/512x512/apps RENAME uefitool.png)
 INSTALL(FILES uefitool.desktop DESTINATION share/applications)
ENDIF()

INSTALL(TARGETS UEFITool BUNDLE DESTINATION "/Applications" )

OPTION(UEFITOOL_EDIT_TESTS "Build experimental editor regression tests" OFF)
IF(UEFITOOL_EDIT_TESTS)
 SET_SOURCE_FILES_PROPERTIES(../common/fixedlayout.cpp PROPERTIES COMPILE_DEFINITIONS UEFITOOL_EDIT_TEST_HOOKS)
 ENABLE_TESTING()
 ADD_EXECUTABLE(fixedlayout_tests ../tests/fixedlayout_tests.cpp)
 TARGET_COMPILE_DEFINITIONS(fixedlayout_tests PRIVATE UEFITOOL_EDIT_TEST_HOOKS)
 TARGET_LINK_LIBRARIES(fixedlayout_tests PRIVATE UEFIToolEditing)
 ADD_TEST(NAME fixedlayout COMMAND fixedlayout_tests)
 SET_TESTS_PROPERTIES(fixedlayout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
 SET_TESTS_PROPERTIES(fixedlayout PROPERTIES TIMEOUT 30)
 ADD_EXECUTABLE(uefi_fixed_edit ../tests/fixedlayout_cli.cpp)
 TARGET_LINK_LIBRARIES(uefi_fixed_edit PRIVATE UEFIToolEditing)
ENDIF()
