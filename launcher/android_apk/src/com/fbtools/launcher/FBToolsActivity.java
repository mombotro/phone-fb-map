package com.fbtools.launcher;

import android.app.Activity;
import android.os.Bundle;

// The "FB Tools" icon on an Android home screen. It asks su to run /data/local/tmp/fbrun -d 3600 fblauncher, which detaches the picker
// into its own session (Android's "stop" kills everything in the app's process group) and then this screen closes.
public class FBToolsActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        try {
            Runtime.getRuntime().exec(new String[] { "su", "-c", "/data/local/tmp/fbrun -d 3600 fblauncher" });
        } catch (Exception e) { }
        finish();
    }
}
