// Prints "<slot> <component> <hash>" for every component of every item in a saved player file,
// using vanilla's own Codecs and HashOps.CRC32C_INSTANCE (the hashes a client sends in
// HashedStack). tools/test_components.sh compares this with build/cmc-codec-check.
//
//   javac -cp <server jar + libraries> -d out HashCheck.java
//   java  -cp out:<server jar + libraries> HashCheck world/players/data/<uuid>.dat
import java.nio.file.Path;
import net.minecraft.SharedConstants;
import net.minecraft.core.component.DataComponentType;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.NbtAccounter;
import net.minecraft.nbt.NbtIo;
import net.minecraft.nbt.NbtOps;
import net.minecraft.nbt.Tag;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.RegistryOps;
import net.minecraft.server.Bootstrap;
import net.minecraft.util.HashOps;

public class HashCheck {
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        var lookup = VanillaRegistries.createWorldLookup();
        var nbtOps = RegistryOps.create(NbtOps.INSTANCE, lookup);
        var hashOps = RegistryOps.create(HashOps.CRC32C_INSTANCE, lookup);
        CompoundTag root = NbtIo.readCompressed(Path.of(args[0]), NbtAccounter.unlimitedHeap());
        for (Tag e : root.getListOrEmpty("Inventory")) {
            CompoundTag item = (CompoundTag) e;
            int slot = item.getByteOr("Slot", (byte) 0);
            CompoundTag comps = item.getCompoundOrEmpty("components");
            for (String name : comps.keySet()) {
                if (name.startsWith("!")) continue;
                DataComponentType<?> type = BuiltInRegistries.DATA_COMPONENT_TYPE.getValue(Identifier.parse(name));
                System.out.println(slot + " " + name + " " + hash(type, comps.get(name), nbtOps, hashOps));
            }
        }
    }

    static <T> String hash(DataComponentType<T> type, Tag value, RegistryOps<Tag> nbtOps, RegistryOps<com.google.common.hash.HashCode> hashOps) {
        var codec = type.codecOrThrow();
        return codec.parse(nbtOps, value).flatMap(v -> codec.encodeStart(hashOps, v)).result().map(h -> Integer.toString(h.asInt())).orElse("?");
    }
}
