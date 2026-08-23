if(UNIX)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_search_module(SCDOC scdoc)
        if(SCDOC_FOUND)
            pkg_get_variable(SCDOC_SCDOC scdoc scdoc)
        endif()
    endif()
endif()

set(Launcher_CommonName "JLauncher")
set(Launcher_DisplayName "J Launcher")
set(Launcher_AppID "org.jlauncher.JLauncher")
set(Launcher_Domain "jom3a-j.github.io")
set(Launcher_Git "https://github.com/Jom3a-J/J-Launcher-Minecraft")

set(Launcher_Name "${Launcher_CommonName}" PARENT_SCOPE)
set(Launcher_DisplayName "${Launcher_DisplayName}" PARENT_SCOPE)
set(Launcher_ENVName "JLAUNCHER" PARENT_SCOPE)
set(Launcher_Domain "${Launcher_Domain}" PARENT_SCOPE)
set(Launcher_Git "${Launcher_Git}" PARENT_SCOPE)

set(Launcher_SVGFileName "${Launcher_AppID}.svg")
set(Launcher_Copyright "Copyright (C) 2026 J Launcher Contributors\\nBased on Prism Launcher: Copyright (C) 2022-2026 Prism Launcher Contributors\\nBased on PolyMC: Copyright (C) 2021-2022 PolyMC Contributors\\nBased on MultiMC: Copyright (C) 2012-2021 MultiMC Contributors")
set(Launcher_Copyright_Mac "Copyright (C) 2026 J Launcher Contributors; based on Prism Launcher, PolyMC, and MultiMC contributors" PARENT_SCOPE)
set(Launcher_Copyright "${Launcher_Copyright}" PARENT_SCOPE)
set(Launcher_UserAgent "${Launcher_CommonName}/${Launcher_VERSION_NAME}" PARENT_SCOPE)
set(Launcher_ConfigFile "${Launcher_APP_BINARY_NAME}.cfg" PARENT_SCOPE)
set(Launcher_AppID "${Launcher_AppID}" PARENT_SCOPE)
set(Launcher_SVGFileName "${Launcher_SVGFileName}" PARENT_SCOPE)

set(Launcher_Desktop "program_info/${Launcher_AppID}.desktop" PARENT_SCOPE)
set(Launcher_MIMEInfo "program_info/${Launcher_AppID}.xml" PARENT_SCOPE)
set(Launcher_MetaInfo "program_info/${Launcher_AppID}.metainfo.xml" PARENT_SCOPE)
set(Launcher_PNG_256 "program_info/${Launcher_AppID}_256.png" PARENT_SCOPE)
set(Launcher_SVG "program_info/${Launcher_SVGFileName}" PARENT_SCOPE)
# A liquid-glass Icon Composer source can be added before a macOS release.
# Until then, macOS uses the conventional J Launcher ICNS above.
set(Launcher_Branding_ICNS "program_info/jlauncher.icns" PARENT_SCOPE)
set(Launcher_Branding_MAC_ICON "" PARENT_SCOPE)
# NSIS runs from the install directory and reads this source-tree-relative
# asset. The same image is copied to the build tree under the binary name for
# the Windows resource compiler below.
set(Launcher_Branding_ICO "program_info/jlauncher.ico")
set(Launcher_Branding_ICO "${Launcher_Branding_ICO}" PARENT_SCOPE)
set(Launcher_Branding_WindowsRC "program_info/${Launcher_APP_BINARY_NAME}.rc" PARENT_SCOPE)
set(Launcher_Branding_FileLinkRC "program_info/${Launcher_APP_BINARY_NAME}_filelink.rc" PARENT_SCOPE)
set(Launcher_Branding_UpdaterRC "program_info/${Launcher_APP_BINARY_NAME}_updater.rc" PARENT_SCOPE)
set(Launcher_Branding_LogoQRC "program_info/${Launcher_APP_BINARY_NAME}.qrc" PARENT_SCOPE)
set(Launcher_Authors "J Launcher Contributors")

set(Launcher_Portable_File "program_info/portable.txt" PARENT_SCOPE)

configure_file(org.prismlauncher.PrismLauncher.desktop.in ${Launcher_AppID}.desktop)
configure_file(org.prismlauncher.PrismLauncher.metainfo.xml.in ${Launcher_AppID}.metainfo.xml)
configure_file(prismlauncher.rc.in ${Launcher_APP_BINARY_NAME}.rc @ONLY)
set(Launcher_HelperDescription "J Launcher File Link Helper")
configure_file(jlauncher-helper.rc.in ${Launcher_APP_BINARY_NAME}_filelink.rc @ONLY)
set(Launcher_HelperDescription "J Launcher Updater")
configure_file(jlauncher-helper.rc.in ${Launcher_APP_BINARY_NAME}_updater.rc @ONLY)
configure_file(prismlauncher.qrc.in ${Launcher_APP_BINARY_NAME}.qrc @ONLY)
configure_file(prismlauncher.manifest.in ${Launcher_APP_BINARY_NAME}.manifest @ONLY)
configure_file(jlauncher.ico ${Launcher_APP_BINARY_NAME}.ico COPYONLY)
configure_file(${Launcher_SVGFileName} ${Launcher_SVGFileName} COPYONLY)
configure_file(org.prismlauncher.PrismLauncher.mime.xml ${Launcher_AppID}.xml COPYONLY)

set(Launcher_MSVC_Redist_NSIS_File "")
set(Launcher_MSVC_Redist_NSIS_Section "")
set(Launcher_MSVC_REDISTRIBUTABLE "" CACHE FILEPATH
    "Path to the supported Microsoft Visual C++ x64 redistributable")

if(MSVC)
    if(NOT Launcher_MSVC_REDISTRIBUTABLE)
        file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" _jlauncher_msvc_compiler)
        string(REGEX REPLACE "/Tools/MSVC/[^/]+/bin/.*$" ""
            _jlauncher_vc_root "${_jlauncher_msvc_compiler}")
        if(NOT _jlauncher_vc_root STREQUAL _jlauncher_msvc_compiler)
            file(GLOB _jlauncher_redist_candidates LIST_DIRECTORIES FALSE
                "${_jlauncher_vc_root}/Redist/MSVC/[0-9]*/vc_redist.x64.exe")
            list(SORT _jlauncher_redist_candidates COMPARE NATURAL ORDER DESCENDING)
            if(_jlauncher_redist_candidates)
                list(GET _jlauncher_redist_candidates 0 _jlauncher_redist)
                set(Launcher_MSVC_REDISTRIBUTABLE "${_jlauncher_redist}" CACHE FILEPATH
                    "Path to the supported Microsoft Visual C++ x64 redistributable" FORCE)
            endif()
        endif()
    endif()

    if(EXISTS "${Launcher_MSVC_REDISTRIBUTABLE}")
        install(FILES "${Launcher_MSVC_REDISTRIBUTABLE}" DESTINATION ".")
        set(Launcher_MSVC_Redist_NSIS_File [[File "vc_redist.x64.exe"]])
        set(Launcher_MSVC_Redist_NSIS_Section [=[
Section "-Microsoft Visual C++ Runtime"
  DetailPrint "Installing the Microsoft Visual C++ runtime..."
  ExecWait '"$INSTDIR\vc_redist.x64.exe" /install /quiet /norestart' $0
  ${If} $0 != 0
  ${AndIf} $0 != 1638
  ${AndIf} $0 != 3010
    MessageBox MB_ICONSTOP "The Microsoft Visual C++ runtime installation failed with code $0. J Launcher cannot start without this prerequisite."
    Abort
  ${EndIf}
SectionEnd
]=])
    else()
        message(WARNING
            "Microsoft Visual C++ x64 redistributable was not found; Windows package audits will fail")
    endif()
endif()

if(Launcher_BUILD_UPDATER)
    set(Launcher_NSIS_UPDATER_FILE "File \"${Launcher_APP_BINARY_NAME}_updater.exe\"")
else()
    set(Launcher_NSIS_UPDATER_FILE "")
endif()

configure_file(win_install.nsi.in win_install.nsi @ONLY)

if(SCDOC_FOUND)
    configure_file(prismlauncher.6.scd.in ${Launcher_APP_BINARY_NAME}.6.scd @ONLY)

    set(in_scd "${CMAKE_CURRENT_BINARY_DIR}/${Launcher_APP_BINARY_NAME}.6.scd")
    set(out_man "${CMAKE_CURRENT_BINARY_DIR}/${Launcher_APP_BINARY_NAME}.6")
    add_custom_command(
        DEPENDS "${in_scd}"
        OUTPUT "${out_man}"
        COMMAND ${SCDOC_SCDOC} < "${in_scd}" > "${out_man}"
    )
    add_custom_target(man ALL DEPENDS ${out_man})
    set(Launcher_ManPage "program_info/${Launcher_APP_BINARY_NAME}.6" PARENT_SCOPE)
endif()
