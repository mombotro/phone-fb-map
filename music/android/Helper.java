// Helper.java - audio engine for the Android music player. Runs under app_process (no zygote needed) and plays files with
// the platform MediaPlayer, so every format the phone can decode works and the audio goes to the right output.
// The C player starts it and talks over stdin/stdout, one text line per message.
//   in:  play PATH | loop PATH | pause | resume | seek MS | vol 0-100 | stop | quit
//   out: state playing|paused|stopped|ended|error TEXT      pos MS DURATION_MS
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintStream;

public class Helper {
    static MediaPlayer mp;
    static boolean prepared, started;
    static float vol = 0.7f;
    static PrintStream out = System.out;
    static Handler h;

    static void say(String s) { out.println(s); out.flush(); }

    static void release() {
        if (mp != null) { try { mp.reset(); mp.release(); } catch (Exception e) { } mp = null; }
        prepared = false; started = false;
    }

    static void play(String path) { play(path, false); }

    // Android 8 and newer: the media server may not read files that belong to the shell (the label of /data/local/tmp), so give the
    // file a media label first. Needs root, which this helper has. Failure is harmless.
    static void relabel(String path) {
        if (Build.VERSION.SDK_INT < 26) return;
        try { Runtime.getRuntime().exec(new String[] { "chcon", "u:object_r:media_rw_data_file:s0", path }).waitFor(); } catch (Exception e) { }
    }

    static void play(String path, boolean loop) {
        release();
        relabel(path);
        try {
            mp = new MediaPlayer();
            mp.setAudioStreamType(AudioManager.STREAM_MUSIC);
            mp.setVolume(vol, vol);
            mp.setDataSource(path);
            mp.setLooping(loop);
            mp.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
                public void onPrepared(MediaPlayer m) { prepared = true; m.start(); started = true; say("state playing"); }
            });
            mp.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                public void onCompletion(MediaPlayer m) { started = false; say("state ended"); }
            });
            mp.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                public boolean onError(MediaPlayer m, int what, int extra) { started = false; say("state error " + what + "/" + extra); return true; }
            });
            mp.prepareAsync();
        } catch (Exception e) {
            say("state error " + e);
        }
    }

    static void handle(String line) {
        String[] p = line.split(" ", 2);
        String c = p[0];
        try {
            if (c.equals("play") && p.length > 1) play(p[1]);
            else if (c.equals("loop") && p.length > 1) play(p[1], true);
            else if (c.equals("pause") && mp != null && prepared) { mp.pause(); say("state paused"); }
            else if (c.equals("resume") && mp != null && prepared) { mp.start(); say("state playing"); }
            else if (c.equals("seek") && mp != null && prepared) mp.seekTo(Integer.parseInt(p[1].trim()));
            else if (c.equals("vol") && p.length > 1) { vol = Integer.parseInt(p[1].trim()) / 100f; if (mp != null) mp.setVolume(vol, vol); }
            else if (c.equals("stop")) { release(); say("state stopped"); }
            else if (c.equals("quit")) { release(); System.exit(0); }
        } catch (Exception e) {
            say("state error " + e);
        }
    }

    public static void main(String[] args) throws Exception {
        Shim.install();
        Looper.prepare();                                    // MediaPlayer callbacks arrive on this thread
        h = new Handler(Looper.myLooper());
        say("ready");
        new Thread(new Runnable() {
            public void run() {
                try {
                    BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
                    String line;
                    while ((line = in.readLine()) != null) {
                        final String l = line;
                        h.post(new Runnable() { public void run() { handle(l); } });
                    }
                } catch (Exception e) { }
                System.exit(0);                              // the player closed the pipe or died
            }
        }).start();
        h.post(new Runnable() {
            public void run() {
                if (mp != null && prepared) {
                    try { say("pos " + mp.getCurrentPosition() + " " + mp.getDuration()); } catch (Exception e) { }
                }
                h.postDelayed(this, 400);
            }
        });
        Looper.loop();
    }
}
