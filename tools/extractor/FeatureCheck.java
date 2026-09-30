// Places a world-gen feature with vanilla's own code, the given seeds and an empty superflat world
// (the same layout cmc-feature-check uses) and prints what changed, for tools/feature_check.sh.
// Block and fluid tags come from data/vanilla_config.json (what the C++ server uses too), and the
// feature is decoded from data/features.json (the jar's worldgen/feature JSON), as a running server
// loads it from the vanilla data pack. (That can differ from the values the code bootstraps: the
// cherry foliage codec writes corner_hole_chance from the wide bottom layer chance.)
//
//   java -cp out:<server jar + libraries> FeatureCheck <data dir> <feature> <first seed> <count>
//
// Only facts about the game are printed; no Mojang code or assets are copied.
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.io.FileReader;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Predicate;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.tags.TagKey;
import net.minecraft.tags.TagLoader;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.WorldGenLevel;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.EntityBlock;
import net.minecraft.world.level.block.entity.BeehiveBlockEntity;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.levelgen.feature.Feature;
import net.minecraft.world.level.material.Fluid;
import net.minecraft.world.level.material.FluidState;

public class FeatureCheck {
    static <T> void bindTags(JsonObject tags, net.minecraft.core.Registry<T> registry, ResourceKey<? extends net.minecraft.core.Registry<T>> key) {
        Map<TagKey<T>, List<Holder<T>>> map = new HashMap<>();
        for (var e : tags.entrySet()) {
            List<Holder<T>> values = new ArrayList<>();
            for (var id : e.getValue().getAsJsonArray()) values.add(registry.get(id.getAsInt()).orElseThrow());
            map.put(TagKey.create(key, Identifier.parse(e.getKey())), values);
        }
        registry.prepareTagReload(new TagLoader.LoadResult<>(key, map)).apply();
    }

    static BlockState flat(int y) {
        return y == -64 ? Blocks.BEDROCK.defaultBlockState()
            : y <= -62 ? Blocks.DIRT.defaultBlockState()
            : y == -61 ? Blocks.GRASS_BLOCK.defaultBlockState() : Blocks.AIR.defaultBlockState();
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    static String describe(BlockState state) {
        StringBuilder sb = new StringBuilder(BuiltInRegistries.BLOCK.getKey(state.getBlock()).toString());
        List<Property<?>> props = new ArrayList<>(state.getProperties());
        props.sort(Comparator.comparing(Property::getName));
        if (!props.isEmpty()) {
            sb.append('[');
            for (int i = 0; i < props.size(); i++) {
                Property p = props.get(i);
                if (i > 0) sb.append(',');
                sb.append(p.getName()).append('=').append(p.getName(state.getValue(p)));
            }
            sb.append(']');
        }
        return sb.toString();
    }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        JsonObject config = JsonParser.parseReader(new FileReader(args[0] + "/vanilla_config.json")).getAsJsonObject();
        JsonObject tags = config.getAsJsonObject("tags");
        bindTags(tags.getAsJsonObject("minecraft:block"), BuiltInRegistries.BLOCK, Registries.BLOCK);
        bindTags(tags.getAsJsonObject("minecraft:fluid"), BuiltInRegistries.FLUID, Registries.FLUID);
        var lookup = net.minecraft.data.registries.VanillaRegistries.createWorldLookup();
        if (args[1].equals("--climate")) { // Biome.getHeightAdjustedTemperature on a grid
            var biomes = lookup.lookupOrThrow(Registries.BIOME);
            var method = net.minecraft.world.level.biome.Biome.class.getDeclaredMethod("getHeightAdjustedTemperature", BlockPos.class, int.class);
            method.setAccessible(true);
            for (String name : new String[]{"minecraft:frozen_ocean", "minecraft:snowy_slopes", "minecraft:plains"}) {
                var biome = biomes.getOrThrow(ResourceKey.create(Registries.BIOME, Identifier.parse(name))).value();
                for (int x = -300; x <= 300; x += 37)
                    for (int z = -300; z <= 300; z += 41)
                        for (int y : new int[]{60, 100, 200})
                            System.out.println(name + " " + x + " " + y + " " + z + " " + String.format("%.9g", (float) method.invoke(biome, new BlockPos(x, y, z), 63)));
            }
            return;
        }
        JsonObject features = JsonParser.parseReader(new FileReader(args[0] + "/features.json")).getAsJsonObject();
        Feature feature = Feature.DIRECT_CODEC
            .parse(net.minecraft.resources.RegistryOps.create(com.mojang.serialization.JsonOps.INSTANCE, lookup), features.get(args[1]))
            .getOrThrow();
        long first = Long.parseLong(args[2]);
        int count = Integer.parseInt(args[3]);
        for (long seed = first; seed < first + count; seed++) {
            Map<BlockPos, BlockState> blocks = new HashMap<>();
            Map<BlockPos, BlockEntity> entities = new HashMap<>();
            RandomSource levelRandom = RandomSource.create(12345L);
            InvocationHandler handler = (proxy, method, a) -> {
                switch (method.getName()) {
                    case "getBlockState": return blocks.getOrDefault((BlockPos) a[0], flat(((BlockPos) a[0]).getY()));
                    case "getFluidState": return blocks.getOrDefault((BlockPos) a[0], flat(((BlockPos) a[0]).getY())).getFluidState();
                    case "isStateAtPosition":
                        return ((Predicate<BlockState>) a[1]).test(blocks.getOrDefault((BlockPos) a[0], flat(((BlockPos) a[0]).getY())));
                    case "isFluidAtPosition":
                        return ((Predicate<FluidState>) a[1]).test(blocks.getOrDefault((BlockPos) a[0], flat(((BlockPos) a[0]).getY())).getFluidState());
                    case "setBlock": {
                        BlockPos pos = ((BlockPos) a[0]).immutable();
                        BlockState state = (BlockState) a[1];
                        blocks.put(pos, state);
                        BlockEntity old = entities.get(pos);
                        if (state.hasBlockEntity()) {
                            if (old == null || !old.getType().isValid(state)) entities.put(pos, ((EntityBlock) state.getBlock()).newBlockEntity(pos, state));
                        } else {
                            entities.remove(pos);
                        }
                        return true;
                    }
                    case "getBlockEntity":
                        if (a.length == 1) return entities.get((BlockPos) a[0]);
                        break;
                    case "getMinY": return -64;
                    case "getHeight": if (a == null || a.length == 0) return 384; break;
                    case "getRandom": return levelRandom;
                    case "scheduleTick": return null;
                    case "hashCode": return System.identityHashCode(proxy);
                    case "equals": return proxy == a[0];
                    case "toString": return "FeatureCheckLevel";
                    default: break;
                }
                if (method.isDefault()) return InvocationHandler.invokeDefault(proxy, method, a);
                throw new UnsupportedOperationException(method.toString());
            };
            WorldGenLevel level = (WorldGenLevel) Proxy.newProxyInstance(FeatureCheck.class.getClassLoader(), new Class<?>[]{WorldGenLevel.class}, handler);
            RandomSource random = RandomSource.create(seed);
            boolean placed = feature.place(level, null, random, new BlockPos(0, -60, 0));
            System.out.println("SEED " + seed + " placed=" + (placed ? 1 : 0) + " next=" + random.nextInt());
            for (int x = -24; x <= 24; x++)
                for (int y = -64; y <= 40; y++)
                    for (int z = -24; z <= 24; z++) {
                        BlockPos pos = new BlockPos(x, y, z);
                        BlockState s = blocks.getOrDefault(pos, flat(y));
                        if (s != flat(y)) System.out.println(x + " " + y + " " + z + " " + describe(s));
                        if (s.is(Blocks.BEE_NEST)) {
                            BlockEntity be = entities.get(pos);
                            System.out.println("BEES " + x + " " + y + " " + z + " " + (be instanceof BeehiveBlockEntity hive ? hive.getOccupantCount() : 0));
                        }
                    }
        }
    }
}
