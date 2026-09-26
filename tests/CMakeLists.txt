project(tests)

ecm_add_test(FileSystem_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME FileSystem)

ecm_add_test(GZip_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME GZip)

ecm_add_test(GradleSpecifier_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME GradleSpecifier)

ecm_add_test(MojangVersionFormat_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME MojangVersionFormat)

ecm_add_test(Library_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Library)

ecm_add_test(ResourceFolderModel_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ResourceFolderModel)

ecm_add_test(ResourcePackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ResourcePackParse)

ecm_add_test(TexturePackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME TexturePackParse)

ecm_add_test(DataPackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME DataPackParse)

ecm_add_test(ShaderPackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ShaderPackParse)

ecm_add_test(WorldSaveParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME WorldSaveParse)

ecm_add_test(ParseUtils_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ParseUtils)

ecm_add_test(Task_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Task)

ecm_add_test(INIFile_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME INIFile)

ecm_add_test(JavaVersion_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME JavaVersion)

ecm_add_test(Packwiz_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Packwiz)

ecm_add_test(Index_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Index)

ecm_add_test(Version_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Version)

ecm_add_test(MetaComponentParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME MetaComponentParse)

ecm_add_test(CatPack_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME CatPack)


ecm_add_test(XmlLogs_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME XmlLogs)

add_executable(FakeMinecraftServer server/FakeMinecraftServer.cpp)

add_executable(ServerManager ServerManager_test.cpp)
target_link_libraries(ServerManager Launcher_logic Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(ServerManager PRIVATE QT_FORCE_ASSERTS)
ecm_mark_nongui_executable(ServerManager)
ecm_mark_as_test(ServerManager)
add_test(NAME ServerManager COMMAND ${CMAKE_COMMAND}
    "-DTEST_EXECUTABLE=$<TARGET_FILE:ServerManager>"
    "-DTEST_LOG=$<TARGET_FILE_DIR:ServerManager>/ServerManager-test-output.txt"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/RunQtTest.cmake")
add_dependencies(ServerManager FakeMinecraftServer)

ecm_add_test(ServerMemory_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerMemory)
set_tests_properties(ServerMemory PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(ServerPackCompatibility_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerPackCompatibility)

add_executable(ServerInstance ServerInstance_test.cpp)
target_link_libraries(ServerInstance Launcher_logic Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(ServerInstance PRIVATE QT_FORCE_ASSERTS
    JLAUNCHER_TEST_JAVA_PATH="${Java_JAVA_EXECUTABLE}")
ecm_mark_nongui_executable(ServerInstance)
ecm_mark_as_test(ServerInstance)
# QtTest stdout can be empty on Windows runners. Read its explicit text log
# through CMake so CTest --output-on-failure includes the failing assertion.
add_test(NAME ServerInstance COMMAND ${CMAKE_COMMAND}
    "-DTEST_EXECUTABLE=$<TARGET_FILE:ServerInstance>"
    "-DTEST_LOG=$<TARGET_FILE_DIR:ServerInstance>/ServerInstance-test-output.txt"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/RunQtTest.cmake")
add_dependencies(ServerInstance FakeMinecraftServer)

ecm_add_test(ServerSettingsPage_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerSettingsPage)

ecm_add_test(SecretEncryption_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME SecretEncryption)

ecm_add_test(AccountList_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME AccountList)

ecm_add_test(TranslationMetadata_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME TranslationMetadata)
ecm_add_test(TranslationsModel_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME TranslationsModel)

ecm_add_test(LoginCallback_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME LoginCallback)
set_tests_properties(ServerSettingsPage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(ModelessWindow_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ModelessWindow)
set_tests_properties(ModelessWindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(ATLPackManifest_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ATLPackManifest)

ecm_add_test(UniversalResourceDownloader_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME UniversalResourceDownloader)

ecm_add_test(NetRequest_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME NetRequest)
set_tests_properties(NetRequest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(CdnTransportPolicy_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME CdnTransportPolicy)

ecm_add_test(Privacy_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Privacy)

ecm_add_test(HostScheduler_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME HostScheduler)

ecm_add_test(NetJobScheduling_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME NetJobScheduling)
set_tests_properties(NetJobScheduling PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(FlameDownloadCompletion_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME FlameDownloadCompletion)
set_tests_properties(FlameDownloadCompletion PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(PartFile_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME PartFile)
set_tests_properties(PartFile PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(SegmentedDownload_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME SegmentedDownload)
set_tests_properties(SegmentedDownload PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)

ecm_add_test(ModrinthDownloadPolicy_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ModrinthDownloadPolicy)
set_tests_properties(ModrinthDownloadPolicy PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(GameUpdateTaskGraph_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME GameUpdateTaskGraph)
