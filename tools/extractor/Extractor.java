// Dumps block/item behaviour data that the vanilla --reports generator doesn't cover.
// (Item components such as max stack size come from the --reports components report instead.)
// Compiled and run against the official server jar by tools/refresh_data.py:
//
//   javac -cp <server jar + libraries> -d out Extractor.java
//   java  -cp out:<server jar + libraries> Extractor data/extracted.json
//
// Only facts about the game (ids, flags, numbers) are written; no Mojang code or assets.
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.io.FileWriter;
import java.lang.reflect.Field;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.StandingAndWallBlockItem;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.EntityBlock;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.Fluids;

public class Extractor {
    static int offGridFaces = 0;
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        JsonObject root = new JsonObject();
        EmptyBlockGetter level = EmptyBlockGetter.INSTANCE;
        BlockPos pos = BlockPos.ZERO;
        var lookup = net.minecraft.data.registries.VanillaRegistries.createWorldLookup();

        // Per block state, indexed by the global state id (Block.BLOCK_STATE_REGISTRY).
        JsonArray states = new JsonArray();
        JsonArray shapes = new JsonArray();
        JsonArray stateShapes = new JsonArray();
        java.util.Map<String, Integer> shapeIndex = new java.util.HashMap<>();
        JsonArray faceMasks = new JsonArray();
        JsonArray stateFaces = new JsonArray();
        java.util.Map<String, Integer> faceIndex = new java.util.HashMap<>();
        for (BlockState state : Block.BLOCK_STATE_REGISTRY) {
            int flags = 0;
            if (state.isAir()) flags |= 1;
            if (state.canBeReplaced()) flags |= 1 << 1;
            var collision = state.getCollisionShape(level, pos);
            if (!collision.isEmpty()) flags |= 1 << 2;
            if (Block.isShapeFullBlock(collision)) flags |= 1 << 3;
            if (state.isRandomlyTicking()) flags |= 1 << 4;
            var fluid = state.getFluidState();
            if (fluid.is(Fluids.WATER) || fluid.is(Fluids.FLOWING_WATER)) flags |= 1 << 5;
            if (fluid.is(Fluids.LAVA) || fluid.is(Fluids.FLOWING_LAVA)) flags |= 1 << 6;
            for (Direction d : Direction.values())
                if (state.isFaceSturdy(level, pos, d)) flags |= 1 << (7 + d.get3DDataValue());
            if (state.isSolid()) flags |= 1 << 13;
            if (state.canOcclude()) flags |= 1 << 14;
            if (fluid.isSource()) flags |= 1 << 15;
            flags |= (state.getLightEmission() & 15) << 16;
            flags |= (state.getLightDampening() & 15) << 20;
            if (state.requiresCorrectToolForDrops()) flags |= 1 << 24;
            // SupportType.CENTER on the top and bottom faces (Block.canSupportCenter).
            if (state.isFaceSturdy(level, pos, Direction.UP, net.minecraft.world.level.block.SupportType.CENTER)) flags |= 1 << 25;
            if (state.isFaceSturdy(level, pos, Direction.DOWN, net.minecraft.world.level.block.SupportType.CENTER)) flags |= 1 << 26;
            if (state.isFaceSturdy(level, pos, Direction.UP, net.minecraft.world.level.block.SupportType.RIGID)) flags |= 1 << 27; // canSupportRigidBlock
            if (state.isSignalSource()) flags |= 1 << 28;
            if (state.isRedstoneConductor(level, pos)) flags |= 1 << 29;
            if (state.useShapeForLightOcclusion()) flags |= 1 << 30;
            if (fluid.isRandomlyTicking()) flags |= 1 << 31;
            states.add(flags);
            // Light occlusion faces (LightEngine.getOcclusionShape) as 16x16 masks, for states whose
            // shape matters (canOcclude and useShapeForLightOcclusion); deduplicated like shapes.
            int faceIdx = -1;
            if (state.canOcclude() && state.useShapeForLightOcclusion()) {
                JsonArray faces = new JsonArray();
                for (Direction d : Direction.values()) {
                    long[] mask = new long[4];
                    for (AABB b : state.getFaceOcclusionShape(d).toAabbs()) {
                        double[] u, v;
                        switch (d.getAxis()) {
                            case Y -> { u = new double[]{b.minX, b.maxX}; v = new double[]{b.minZ, b.maxZ}; }
                            case X -> { u = new double[]{b.minY, b.maxY}; v = new double[]{b.minZ, b.maxZ}; }
                            default -> { u = new double[]{b.minX, b.maxX}; v = new double[]{b.minY, b.maxY}; }
                        }
                        for (double c : new double[]{u[0], u[1], v[0], v[1]})
                            if (Math.abs(c * 16 - Math.rint(c * 16)) > 1e-9) offGridFaces++;
                        for (int j = 0; j < 16; j++)
                            for (int i = 0; i < 16; i++) {
                                double cu = (i + 0.5) / 16, cv = (j + 0.5) / 16;
                                if (cu > u[0] && cu < u[1] && cv > v[0] && cv < v[1]) mask[(i + j * 16) >> 6] |= 1L << ((i + j * 16) & 63);
                            }
                    }
                    for (long m : mask) faces.add(Long.toUnsignedString(m));
                }
                String fkey = faces.toString();
                Integer fi = faceIndex.get(fkey);
                if (fi == null) {
                    fi = faceMasks.size();
                    faceIndex.put(fkey, fi);
                    faceMasks.add(faces);
                }
                faceIdx = fi;
            }
            stateFaces.add(faceIdx);
            // Collision boxes, deduplicated into a shape table.
            JsonArray boxes = new JsonArray();
            for (AABB b : collision.toAabbs()) {
                JsonArray box = new JsonArray();
                box.add(b.minX); box.add(b.minY); box.add(b.minZ); box.add(b.maxX); box.add(b.maxY); box.add(b.maxZ);
                boxes.add(box);
            }
            String key = boxes.toString();
            Integer idx = shapeIndex.get(key);
            if (idx == null) {
                idx = shapes.size();
                shapeIndex.put(key, idx);
                shapes.add(boxes);
            }
            stateShapes.add(idx);
        }
        root.add("state_flags", states);
        root.add("shapes", shapes);
        root.add("state_shape", stateShapes);
        root.add("face_masks", faceMasks);
        root.add("state_faces", stateFaces);
        if (offGridFaces > 0) System.err.println("WARNING: " + offGridFaces + " light face coordinates off the 1/16 grid");

        JsonObject blocks = new JsonObject();
        for (Block block : BuiltInRegistries.BLOCK) {
            JsonObject b = new JsonObject();
            JsonArray classes = new JsonArray();
            for (Class<?> c = block.getClass(); c != null && c != Object.class; c = c.getSuperclass()) classes.add(c.getSimpleName());
            b.add("classes", classes);
            b.addProperty("hardness", block.defaultDestroyTime());
            SoundType sound = block.defaultBlockState().getSoundType();
            JsonObject s = new JsonObject();
            s.addProperty("place", BuiltInRegistries.SOUND_EVENT.getKey(sound.getPlaceSound()).toString());
            s.addProperty("break", BuiltInRegistries.SOUND_EVENT.getKey(sound.getBreakSound()).toString());
            s.addProperty("volume", sound.getVolume());
            s.addProperty("pitch", sound.getPitch());
            b.add("sound", s);
            Item item = block.asItem();
            b.addProperty("item", BuiltInRegistries.ITEM.getKey(item).toString());
            b.addProperty("friction", block.getFriction());
            block.getLootTable().ifPresent(key -> b.addProperty("loot_table", key.identifier().toString()));
            b.addProperty("instrument", block.defaultBlockState().instrument().getSerializedName());
            // Open/close (or click on/off) sounds from the block's BlockSetType / WoodType.
            for (Class<?> c = block.getClass(); c != null && c != Object.class; c = c.getSuperclass()) {
                for (Field f : c.getDeclaredFields()) {
                    if (java.lang.reflect.Modifier.isStatic(f.getModifiers())) continue;
                    f.setAccessible(true);
                    Object v = f.get(block);
                    net.minecraft.sounds.SoundEvent open = null, close = null;
                    if (v instanceof net.minecraft.world.level.block.state.properties.BlockSetType t) {
                        if (block instanceof net.minecraft.world.level.block.DoorBlock) { open = t.doorOpen(); close = t.doorClose(); }
                        else if (block instanceof net.minecraft.world.level.block.TrapDoorBlock) { open = t.trapdoorOpen(); close = t.trapdoorClose(); }
                        else if (block instanceof net.minecraft.world.level.block.ButtonBlock) { open = t.buttonClickOn(); close = t.buttonClickOff(); }
                        else { open = t.pressurePlateClickOn(); close = t.pressurePlateClickOff(); }
                    } else if (v instanceof net.minecraft.world.level.block.state.properties.WoodType w && block instanceof net.minecraft.world.level.block.FenceGateBlock) {
                        open = w.fenceGateOpen(); close = w.fenceGateClose();
                    } else if (c == net.minecraft.world.level.block.ChestBlock.class && f.getName().equals("openSound")) { // chest lids
                        Field cf = c.getDeclaredField("closeSound");
                        cf.setAccessible(true);
                        open = (net.minecraft.sounds.SoundEvent) v;
                        close = (net.minecraft.sounds.SoundEvent) cf.get(block);
                    }
                    if (open != null) {
                        b.addProperty("open_sound", BuiltInRegistries.SOUND_EVENT.getKey(open).toString());
                        b.addProperty("close_sound", BuiltInRegistries.SOUND_EVENT.getKey(close).toString());
                    }
                }
            }
            // FireBlock.setFlammable: ignite and burn odds (0 if the block doesn't burn).
            int ignite = fireOdds("igniteOdds", block), burn = fireOdds("burnOdds", block);
            if (ignite > 0 || burn > 0) {
                b.addProperty("ignite_odds", ignite);
                b.addProperty("burn_odds", burn);
            }
            if (block instanceof net.minecraft.world.level.block.DropExperienceBlock) { // its xpRange IntProvider, as JSON
                Field f = net.minecraft.world.level.block.DropExperienceBlock.class.getDeclaredField("xpRange");
                f.setAccessible(true);
                var range = (net.minecraft.util.valueproviders.IntProvider) f.get(block);
                b.addProperty("xp_range", net.minecraft.util.valueproviders.IntProviders.CODEC
                    .encodeStart(com.mojang.serialization.JsonOps.INSTANCE, range).getOrThrow().toString());
            }
            if (block.defaultBlockState().ignitedByLava()) b.addProperty("ignited_by_lava", true);
            if (block instanceof net.minecraft.world.level.block.WeatheringCopper w) { // WeatheringCopper: age and next stage
                b.addProperty("weather_age", w.getAge().ordinal());
                net.minecraft.world.level.block.WeatheringCopper.getNext(block)
                    .ifPresent(n -> b.addProperty("weathers_to", BuiltInRegistries.BLOCK.getKey(n).toString()));
            }
            if (block instanceof net.minecraft.world.level.block.SaplingBlock) { // its TreeGrower, by name
                Field f = net.minecraft.world.level.block.SaplingBlock.class.getDeclaredField("treeGrower");
                f.setAccessible(true);
                var grower = (net.minecraft.world.level.block.grower.TreeGrower) f.get(block);
                b.addProperty("tree_grower", net.minecraft.world.level.block.grower.TreeGrower.CODEC
                    .encodeStart(com.mojang.serialization.JsonOps.INSTANCE, grower).getOrThrow().getAsString());
            }
            if (block instanceof net.minecraft.world.level.block.MushroomBlock) { // the huge mushroom feature
                Field f = net.minecraft.world.level.block.MushroomBlock.class.getDeclaredField("feature");
                f.setAccessible(true);
                b.addProperty("grows_into", ((net.minecraft.resources.ResourceKey<?>) f.get(block)).identifier().toString());
            }
            if (block instanceof net.minecraft.world.level.block.FlowerPotBlock pot) // the plant in a potted block
                b.addProperty("potted", BuiltInRegistries.BLOCK.getKey(pot.getPotted()).toString());
            if (block instanceof net.minecraft.world.level.block.CandleBlock candle) // CandleCakeBlock.byCandle
                b.addProperty("candle_cake", BuiltInRegistries.BLOCK.getKey(net.minecraft.world.level.block.CandleCakeBlock.byCandle(candle).getBlock()).toString());
            if (block instanceof EntityBlock) {
                // Constructing block entities needs loaded registries; use the types' valid block sets.
                for (var type : BuiltInRegistries.BLOCK_ENTITY_TYPE) {
                    if (type.isValid(block.defaultBlockState())) {
                        b.addProperty("block_entity", BuiltInRegistries.BLOCK_ENTITY_TYPE.getKey(type).toString());
                        // What a fresh one saves (BlockEntity.saveWithoutMetadata), as SNBT.
                        try {
                            var be = ((EntityBlock) block).newBlockEntity(BlockPos.ZERO, block.defaultBlockState());
                            if (be != null) b.addProperty("block_entity_default", be.saveWithoutMetadata(lookup).toString());
                        } catch (Throwable e) { // (vaults need bound item components)
                            System.err.println("no default for " + block + ": " + e);
                        }
                        break;
                    }
                }
            }
            blocks.add(BuiltInRegistries.BLOCK.getKey(block).toString(), b);
        }
        root.add("blocks", blocks);

        Field wallBlock = StandingAndWallBlockItem.class.getDeclaredField("wallBlock");
        Field attachment = StandingAndWallBlockItem.class.getDeclaredField("attachmentDirection");
        wallBlock.setAccessible(true);
        attachment.setAccessible(true);
        JsonObject items = new JsonObject();
        for (Item item : BuiltInRegistries.ITEM) {
            JsonObject i = new JsonObject();
            i.addProperty("class", item.getClass().getSimpleName());
            if (item instanceof BlockItem bi) {
                i.addProperty("block", BuiltInRegistries.BLOCK.getKey(bi.getBlock()).toString());
                if (item instanceof StandingAndWallBlockItem) {
                    i.addProperty("wall_block", BuiltInRegistries.BLOCK.getKey((Block) wallBlock.get(item)).toString());
                    i.addProperty("attachment", ((Direction) attachment.get(item)).getSerializedName());
                }
            }
            if (item.getCraftingRemainder() != null) // Item.getCraftingRemainder (milk bucket -> bucket, ...)
                i.addProperty("craft_remainder", BuiltInRegistries.ITEM.getKey(item.getCraftingRemainder().item().value()).toString());
            if (!item.canFitInsideContainerItems()) i.addProperty("no_container", true); // shulker boxes, bundles
            items.add(BuiltInRegistries.ITEM.getKey(item).toString(), i);
        }
        root.add("items", items);

        // Enums used by item components: serialized name (Codec) and id (StreamCodec idMapper).
        String[] enumClasses = {
            "net.minecraft.world.item.DyeColor", "net.minecraft.world.item.Rarity",
            "net.minecraft.world.item.component.FireworkExplosion$Shape", "net.minecraft.world.entity.EquipmentSlot",
            "net.minecraft.world.entity.EquipmentSlotGroup", "net.minecraft.world.item.ItemUseAnimation",
            "net.minecraft.world.item.SwingAnimationType", "net.minecraft.world.entity.ai.attributes.AttributeModifier$Operation",
            "net.minecraft.world.item.component.ItemAttributeModifiers$Display$Type",
            "net.minecraft.world.item.component.MapPostProcessing", "net.minecraft.core.Direction",
            "net.minecraft.world.entity.animal.fox.Fox$Variant", "net.minecraft.world.entity.animal.fish.Salmon$Variant",
            "net.minecraft.world.entity.animal.parrot.Parrot$Variant", "net.minecraft.world.entity.animal.fish.TropicalFish$Pattern",
            "net.minecraft.world.entity.animal.cow.MushroomCow$Variant", "net.minecraft.world.entity.animal.rabbit.Rabbit$Variant",
            "net.minecraft.world.entity.animal.equine.Variant", "net.minecraft.world.entity.animal.equine.Llama$Variant",
            "net.minecraft.world.entity.animal.axolotl.Axolotl$Variant",
            "net.minecraft.core.component.BlockTransformer$DropStrategy", "net.minecraft.core.component.BlockTransformer$TransformParticle",
            "net.minecraft.core.component.BlockTransformer$TransformType",
        };
        JsonObject enums = new JsonObject();
        for (String name : enumClasses) {
            Class<?> cls = Class.forName(name);
            JsonArray values = new JsonArray();
            for (Object e : cls.getEnumConstants()) {
                JsonArray v = new JsonArray();
                v.add(e instanceof net.minecraft.util.StringRepresentable sr ? sr.getSerializedName() : ((Enum<?>) e).name().toLowerCase());
                v.add(enumId(cls, e));
                values.add(v);
            }
            enums.add(name.substring(name.lastIndexOf('.') + 1), values);
        }
        root.add("enums", enums);

        // NoteBlockInstrument: sound, isTunable, worksAboveNoteBlock (mob heads), hasCustomSound.
        JsonArray instruments = new JsonArray();
        for (var inst : net.minecraft.world.level.block.state.properties.NoteBlockInstrument.values()) {
            JsonObject i = new JsonObject();
            i.addProperty("name", inst.getSerializedName());
            i.addProperty("sound", inst.getSoundEvent().unwrapKey().map(k -> k.identifier().toString()).orElse(""));
            i.addProperty("tunable", inst.isTunable());
            i.addProperty("above", inst.worksAboveNoteBlock());
            i.addProperty("custom", inst.hasCustomSound());
            instruments.add(i);
        }
        root.add("instruments", instruments);

        // Attributes: default, range, whether clients get them, and the player's base values.
        var playerAttributes = net.minecraft.world.entity.ai.attributes.DefaultAttributes.getSupplier(net.minecraft.world.entity.EntityTypes.PLAYER);
        JsonObject attributes = new JsonObject();
        for (var holder : BuiltInRegistries.ATTRIBUTE.asHolderIdMap()) {
            var attr = holder.value();
            JsonObject a = new JsonObject();
            a.addProperty("default", attr.getDefaultValue());
            if (attr instanceof net.minecraft.world.entity.ai.attributes.RangedAttribute r) {
                a.addProperty("min", r.getMinValue());
                a.addProperty("max", r.getMaxValue());
            }
            a.addProperty("syncable", attr.isClientSyncable());
            if (playerAttributes.hasAttribute(holder)) a.addProperty("player_base", playerAttributes.getBaseValue(holder));
            attributes.add(BuiltInRegistries.ATTRIBUTE.getKey(attr).toString(), a);
        }
        root.add("attributes", attributes);

        // Attribute modifiers of mob effects at amplifier 0 (they scale with amplifier + 1).
        JsonObject effects = new JsonObject();
        for (var effect : BuiltInRegistries.MOB_EFFECT) {
            JsonArray mods = new JsonArray();
            effect.createModifiers(0, (attr, mod) -> {
                JsonObject m = new JsonObject();
                m.addProperty("attribute", attr.unwrapKey().orElseThrow().identifier().toString());
                m.addProperty("id", mod.id().toString());
                m.addProperty("amount", mod.amount());
                m.addProperty("operation", mod.operation().getSerializedName());
                mods.add(m);
            });
            effects.add(BuiltInRegistries.MOB_EFFECT.getKey(effect).toString(), mods);
        }
        root.add("effect_modifiers", effects);

        // Environment attributes clients get (EnvironmentAttribute.isSyncable).
        JsonArray syncedAttributes = new JsonArray();
        for (var attribute : BuiltInRegistries.ENVIRONMENT_ATTRIBUTE)
            if (attribute.isSyncable()) syncedAttributes.add(BuiltInRegistries.ENVIRONMENT_ATTRIBUTE.getKey(attribute).toString());
        root.add("synced_environment_attributes", syncedAttributes);

        try (FileWriter out = new FileWriter(args[0])) {
            new GsonBuilder().create().toJson(root, out);
        }
        System.out.println("wrote " + args[0] + ": " + states.size() + " states, " + blocks.size() + " blocks, " + items.size() + " items");
    }

    static int fireOdds(String field, Block block) throws Exception {
        Field f = net.minecraft.world.level.block.FireBlock.class.getDeclaredField(field);
        f.setAccessible(true);
        return ((it.unimi.dsi.fastutil.objects.Object2IntMap<?>) f.get(net.minecraft.world.level.block.Blocks.FIRE)).getInt(block);
    }

    // The int an enum's StreamCodec writes (ByteBufCodecs.idMapper getter), else its ordinal.
    static int enumId(Class<?> cls, Object e) throws Exception {
        for (String m : new String[] {"getId", "id", "getPackedId", "get3DDataValue"}) {
            try {
                var method = cls.getDeclaredMethod(m);
                if (method.getReturnType() == int.class) {
                    method.setAccessible(true);
                    return (int) method.invoke(e);
                }
            } catch (NoSuchMethodException ignored) {
            }
        }
        try {
            Field f = cls.getDeclaredField("id");
            if (f.getType() == int.class) {
                f.setAccessible(true);
                return f.getInt(e);
            }
        } catch (NoSuchFieldException ignored) {
        }
        return ((Enum<?>) e).ordinal();
    }
}
