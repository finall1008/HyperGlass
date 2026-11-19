import android.app.WallpaperManager;
import android.app.wallpaper.WallpaperDescription;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import java.nio.file.Files;
import java.nio.file.Path;

public final class SetWallpaper {
    public static void main(String[] args) throws Exception {
        // Run as Android's system UID and let WallpaperManagerService crop and
        // rebind the static wallpaper when the image file descriptor closes.
        Class<?> service = Class.forName("android.os.ServiceManager");
        IBinder binder = (IBinder) service.getMethod("getService", String.class).invoke(null, "wallpaper");
        Class<?> manager = Class.forName("android.app.IWallpaperManager");
        Object wallpaper = Class.forName(manager.getName() + "$Stub")
                .getMethod("asInterface", IBinder.class).invoke(null, binder);
        Class<?> callback = Class.forName("android.app.IWallpaperManagerCallback");
        var set = manager.getMethod("setWallpaper", String.class, String.class, WallpaperDescription.class,
                boolean.class, Bundle.class, int.class, callback, int.class);
        int flags = WallpaperManager.FLAG_SYSTEM | WallpaperManager.FLAG_LOCK;
        var description = new WallpaperDescription.Builder().build();
        var file = (ParcelFileDescriptor) set.invoke(wallpaper, "", "android", description, true,
                new Bundle(), flags, null, 0);
        if (file == null) throw new IllegalStateException("Wallpaper service rejected the static image");
        try (var image = new ParcelFileDescriptor.AutoCloseOutputStream(file)) {
            Files.copy(Path.of(args[0]), image);
        }
    }
}
