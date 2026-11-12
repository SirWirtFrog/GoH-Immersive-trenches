#include "../src/TrenchCatalog.h"
#include <cstdio>
#include <filesystem>
#include <string>
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::fprintf(stderr,"Usage: TrenchCatalogRunner <entity-root>\n"); return 2; }
    try {
        trench::Catalog catalog;
        catalog.Load(std::filesystem::path(argv[1]));
        std::printf("registrations=%zu\n",catalog.Modules().size());
        bool failed = false;
        for (const auto& message : catalog.Diagnostics()) {
            std::fprintf(stderr,"%s\n",message.c_str());
            if (message.find(":1: no registered trench variants") == std::string::npos) failed = true;
        }
        for (const auto* module : catalog.Modules()) {
            std::printf("%s default=%u sockets=%u\n",module->entityName,module->socketIds[module->defaultSocket],module->socketCount);
            for (unsigned i=0;i<module->socketCount;++i)
                std::printf("socket=%u x=%.9g y=%.9g yaw=%.9g\n",module->socketIds[i],module->sockets[i].center.x,module->sockets[i].center.y,module->sockets[i].outwardYawDegrees);
        }
        return failed ? 1 : 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
