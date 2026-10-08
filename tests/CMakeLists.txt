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

ecm_add_test(ModrinthUrl_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ModrinthUrl)

ecm_add_test(MetaComponentParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME MetaComponentParse)

ecm_add_test(CatPack_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME CatPack)


ecm_add_test(XmlLogs_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME XmlLogs)

add_executable(FakeMinecraftServer server/FakeMinecraftServer.cpp)

# Server test programs. They can start FakeMinecraftServer, and their QtTest text log is
# printed through RunQtTest.cmake because QtTest stdout can be empty on Windows runners, so
# CTest --output-on-failure still shows the failing assertion.
function(add_server_test name)
    add_executable(${name} ${name}_test.cpp ServerTestSupport.h)
    target_link_libraries(${name} Launcher_logic Qt${QT_VERSION_MAJOR}::Test)
    target_compile_definitions(${name} PRIVATE QT_FORCE_ASSERTS
        JLAUNCHER_TEST_JAVA_PATH="${Java_JAVA_EXECUTABLE}")
    ecm_mark_nongui_executable(${name})
    ecm_mark_as_test(${name})
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
        "-DTEST_EXECUTABLE=$<TARGET_FILE:${name}>"
        "-DTEST_LOG=$<TARGET_FILE_DIR:${name}>/${name}-test-output.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunQtTest.cmake")
    add_dependencies(${name} FakeMinecraftServer)
endfunction()

add_server_test(ServerModpackPrep)
add_server_test(ServerModpackDependencies)
add_server_test(ServerManagement)

ecm_add_test(ServerMemory_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerMemory)
set_tests_properties(ServerMemory PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(ServerPackCompatibility_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerPackCompatibility)

add_server_test(ServerBasics)
add_server_test(ServerLaunch)
add_server_test(ServerLoaderInstall)
add_server_test(ServerSoftwareDownload)
add_server_test(ServerContent)

ecm_add_test(ServerSettingsPage_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerSettingsPage)

ecm_add_test(ServerListPage_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerListPage)
ecm_add_test(ServerTools_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerTools)
ecm_add_test(ServerDataStore_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ServerDataStore)
set_tests_properties(ServerListPage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

ecm_add_test(SecretEncryption_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME SecretEncryption)

ecm_add_test(AccountList_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME AccountList)

ecm_add_test(TranslationMetadata_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME TranslationMetadata)
ecm_add_test(ModpackServerSupport_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ModpackServerSupport)
ecm_add_test(LegacyFTBServerPack_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME LegacyFTBServerPack)
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

ecm_add_test(ApiCredentials_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ApiCredentials)

ecm_add_test(GameSettingsSync_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME GameSettingsSync)

ecm_add_test(ApiKeyFields_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ApiKeyFields)
set_tests_properties(ApiKeyFields PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

ecm_add_test(FlameServerReadyFilter_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME FlameServerReadyFilter)

ecm_add_test(GameUpdateTaskGraph_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME GameUpdateTaskGraph)

ecm_add_test(Commandline_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Commandline)
