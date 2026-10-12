package com.fbtools.launcher;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import java.io.InputStream;

// The "momPKT" icon on an Android home screen. It asks su to run /data/local/tmp/fbrun -d 3600 fblauncher, which detaches the picker
// into its own session (Android's "stop" kills everything in the app's process group). The su call runs in a thread that waits for
// su to finish: on Android 7 the su client must stay alive until the su daemon has started the command, and finishing the screen at
// once let the process die first.
public class FBToolsActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        new Thread(new Runnable() {
            public void run() {
                try {
                    Process p = Runtime.getRuntime().exec(new String[] { "su", "-c", "/data/local/tmp/fbrun -d 3600 fblauncher" });
                    p.getOutputStream().close();
                    InputStream o = p.getInputStream(), e = p.getErrorStream();
                    byte[] buf = new byte[256];
                    StringBuilder out = new StringBuilder();
                    int n;
                    while ((n = o.read(buf)) > 0) out.append(new String(buf, 0, n));        // drain the output so su is not blocked on a full pipe
                    while ((n = e.read(buf)) > 0) out.append(new String(buf, 0, n));
                    int rc = p.waitFor();
                    Log.d("FBTools", "su ended with " + rc + " " + out);
                } catch (Exception e) { Log.e("FBTools", "su failed: " + e); }
            }
        }).start();
        finish();
    }
}
