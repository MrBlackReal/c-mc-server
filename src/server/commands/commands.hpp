#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "server/commands/brigadier.hpp"
#include "server/commands/command_source.hpp"

namespace mc {

class MinecraftServer;
class ServerPlayer;

namespace commands {

// net.minecraft.commands.Commands: the dispatcher with the vanilla commands this server has, and
// running them the way vanilla does (errors with their context, feedback, the per-player tree).
class Commands {
public:
    explicit Commands(MinecraftServer& server);

    brigadier::CommandDispatcher& dispatcher() { return dispatcher_; }

    // dispatcher.parse(command, source)
    brigadier::ParseResults parse(const std::string& command, const CommandSourceStack& source) const;
    // performPrefixedCommand: strips a leading '/', parses and runs.
    void performPrefixedCommand(const CommandSourceStack& source, std::string command);
    // performCommand: reports parse errors (message + "...context<--[HERE]"), else runs it.
    void performCommand(const brigadier::ParseResults& parse, const std::string& command);
    // sendCommands: the tree of the commands this player may use (ClientboundCommandsPacket).
    void sendCommands(ServerPlayer& player);

    // SignableCommand.of: the minecraft:message arguments in the parse, by name, with their text.
    static std::vector<std::pair<std::string, std::string>> signableArguments(const brigadier::ParseResults& parse);

private:
    MinecraftServer& server_;
    brigadier::CommandDispatcher dispatcher_;
};

} // namespace commands
} // namespace mc
