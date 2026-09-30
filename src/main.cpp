#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/component/data_components.hpp"
#include "core/registries.hpp"
#include "server/minecraft_server.hpp"
#include "util/log.hpp"
#include "world/item/crafting/recipes.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/block/block_state.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace {
mc::MinecraftServer* g_server = nullptr;

void onSignal(int) {
    if (g_server) g_server->halt();
}

// Mirrors the vanilla EULA check in net.minecraft.server.Main.
bool checkEula() {
    std::ifstream in("eula.txt");
    std::string line;
    while (std::getline(in, line))
        if (line.starts_with("eula=")) return line.substr(5).starts_with("true");
    std::ofstream out("eula.txt");
    out << "#By changing the setting below to TRUE you are indicating your agreement to our EULA "
           "(https://aka.ms/MinecraftEULA).\neula=false\n";
    return false;
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--debug")) mc::log::setDebug(true);
        else if (!std::strcmp(argv[i], "--help")) {
            std::printf("usage: %s [--debug]\nRun from the server directory (server.properties, eula.txt).\n", argv[0]);
            return 0;
        }
    }
    if (!checkEula()) {
        mc::log::info("You need to agree to the EULA in order to run the server. Go to eula.txt for more info.");
        return 1;
    }
    mc::log::info("Loading properties");
    auto props = mc::ServerProperties::load("server.properties");
    props.save("server.properties");

    mc::registries::bootstrap();
    mc::components::bootstrap();
    mc::blocks::bootstrap();
    mc::items::bootstrap();
    mc::loot::bootstrap();
    mc::enchantments::bootstrap();
    mc::recipes::bootstrap();

    mc::MinecraftServer server(std::move(props));
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    return server.run();
}
