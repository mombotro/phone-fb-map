// Pcm.java - plays raw 16-bit stereo PCM from stdin through AudioTrack. The TIC-80 player on Android sends its sound to it.
// Runs under app_process, like Helper.java. usage: app_process /system/bin Pcm RATE VOLUME_PERCENT
// It prints "ready" when the track is playing. It stops when stdin closes.
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import java.io.InputStream;

public class Pcm {
    public static void main(String[] a) throws Exception {
        Shim.install();
        int rate = a.length > 0 ? Integer.parseInt(a[0]) : 44100;
        float vol = (a.length > 1 ? Integer.parseInt(a[1]) : 100) / 100f;
        int min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        if (min <= 0) min = 8192;
        AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_STEREO,
                                      AudioFormat.ENCODING_PCM_16BIT, min * 2, AudioTrack.MODE_STREAM);
        t.setStereoVolume(vol, vol);
        t.play();
        System.out.println("ready");
        System.out.flush();
        InputStream in = System.in;
        byte[] buf = new byte[2048];
        int n;
        while ((n = in.read(buf)) > 0) {
            n &= ~3;                                           // whole frames only
            if (n > 0) t.write(buf, 0, n);
        }
        t.stop();
        t.release();
    }
}
