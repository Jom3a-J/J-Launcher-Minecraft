// SPDX-License-Identifier: GPL-3.0-only

#include <iostream>
#include <fstream>
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
        std::ofstream runScript("run.bat", std::ios::binary);
        runScript << "@echo off\r\n";
        runScript << "echo Synthetic installer output\r\n";
        return runScript.good() ? 0 : 1;
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
