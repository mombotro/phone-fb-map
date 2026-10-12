// Shim.java - lets AudioTrack and MediaPlayer work while the Android framework is stopped, on Android 8 and newer.
// Since Android 8 these classes register every player with two services that live in system_server (appops and audio), and they crash
// with a NullPointerException when those services are missing, which is the case while FB Tools has the framework stopped. This
// registers two stand-in services that answer every call with "no error, 0 (allowed)". The sound itself still goes through audioserver.
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.lang.reflect.Method;

public class Shim {
    static class Fake extends Binder {
        Fake() { attachInterface(null, "com.fbtools.Fake"); }                 // a Binder with no descriptor crashes queryLocalInterface
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code >= 1 && code <= 0x00FFFFFF && reply != null) { reply.writeNoException(); reply.writeInt(0); return true; }
            return super.onTransact(code, data, reply, flags);
        }
    }

    public static void install() {
        if (Build.VERSION.SDK_INT < 26) return;                   // older Android does not need it
        try {
            Class<?> sm = Class.forName("android.os.ServiceManager");
            Method get = sm.getMethod("getService", String.class);
            Method add = sm.getMethod("addService", String.class, IBinder.class);
            String[] names = { "appops", "audio" };
            for (int i = 0; i < names.length; i++)
                if (get.invoke(null, names[i]) == null) add.invoke(null, names[i], new Fake());
        } catch (Throwable t) {
            System.err.println("shim: " + t);
        }
    }
}
