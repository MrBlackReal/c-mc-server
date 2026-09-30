#pragma once

#include "nbt/tag.hpp"
#include "server/level/server_player.hpp"

namespace mc::player_data {

// Port of ServerPlayer/Player/LivingEntity/Entity save & load for the fields the server models.
// Everything else in the loaded tag (health, xp, recipe book, ...) is passed through unchanged.
void load(ServerPlayer& player, const nbt::Compound& tag);
nbt::Compound save(const ServerPlayer& player);

} // namespace mc::player_data
