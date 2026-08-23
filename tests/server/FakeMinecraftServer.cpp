// SPDX-License-Identifier: GPL-3.0-only

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <string>

int main(int argc, char **argv)
{
    bool versionProbe = false;
    bool exitBeforeReady = false;
    bool suppressReady = false;
    bool crashWithMemoryError = false;
    bool installServer = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        versionProbe = versionProbe || argument == "-version";
        exitBeforeReady = exitBeforeReady || argument == "-Dfake.exit-before-ready";
        suppressReady = suppressReady || argument == "-Dfake.no-ready";
        crashWithMemoryError = crashWithMemoryError || argument == "-Dfake.crash-memory";
        installServer = installServer || argument == "--installServer";
    }

    if (versionProbe) {
        std::cerr << "openjdk version \"21.0.0\"" << std::endl;
        return 0;
    }
    if (installServer) {
        std::filesystem::create_directories("libraries/synthetic");
#ifdef _WIN32
        std::ofstream runScript("run.bat", std::ios::binary);
        runScript << "@echo off\r\n";
        runScript << "java @libraries/synthetic/win_args.txt %*\r\n";
        std::ofstream arguments("libraries/synthetic/win_args.txt", std::ios::binary);
        arguments << "-jar synthetic-loader.jar\r\n";
#else
        std::ofstream runScript("run.sh", std::ios::binary);
        runScript << "#!/bin/sh\n";
        runScript << "java @libraries/synthetic/unix_args.txt \"$@\"\n";
        std::ofstream arguments("libraries/synthetic/unix_args.txt", std::ios::binary);
        arguments << "-jar synthetic-loader.jar\n";
#endif
        const bool omitServerPayload =
            std::getenv("JLAUNCHER_TEST_OMIT_SERVER_PAYLOAD") != nullptr;
        std::ofstream serverJar;
        if (!omitServerPayload) {
            for (const char* version : { "1.12.2", "1.20.1", "1.21.1", "26.2" }) {
                const std::filesystem::path directory =
                    std::filesystem::path("libraries/net/minecraft/server") / version;
                std::filesystem::create_directories(directory);
                serverJar.open(directory / (std::string("server-") + version + "-bundled.jar"),
                               std::ios::binary);
                serverJar << "synthetic server payload";
                serverJar.close();
            }
        }
        return runScript.good() && arguments.good()
                   && (omitServerPayload || serverJar.good())
            ? 0 : 1;
    }
    if (exitBeforeReady) {
        std::cerr << "Synthetic server startup failure" << std::endl;
        return 0;
    }
    for (int index = 1; index < argc; ++index) {
        std::cout << "ARG:" << argv[index] << std::endl;
    }
    if (!suppressReady) {
        std::cout << "[Server thread/INFO]: Done (0.001s)! For help, type \"help\"" << std::endl;
    }
    if (crashWithMemoryError) {
        std::cerr << "java.lang.OutOfMemoryError: Synthetic controlled crash" << std::endl;
        return 1;
    }

    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "stop") {
            std::cout << "[Server thread/INFO]: Stopping server" << std::endl;
            return 0;
        }
        std::cout << "COMMAND:" << command << std::endl;
    }
    return 0;
}
