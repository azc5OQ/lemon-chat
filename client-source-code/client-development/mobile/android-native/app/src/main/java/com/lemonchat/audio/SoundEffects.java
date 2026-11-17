package com.lemonchat.audio;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.AudioAttributes;
import android.media.SoundPool;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

/** plays the existing client notification clips with bounded overlap */
public final class SoundEffects implements AutoCloseable
{
    private final Context context;
    private final SoundPool pool;
    private final Map<String, Integer> sounds = new HashMap<>();
    private final Map<Integer, Boolean> ready = new HashMap<>();
    private boolean enabled;
    private boolean closed;

    public SoundEffects(Context context)
    {
        this.context = context.getApplicationContext();
        pool = new SoundPool.Builder().setMaxStreams(4).setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()).build();
        pool.setOnLoadCompleteListener((owner, id, status) ->
        {
            synchronized (SoundEffects.this)
            {
                if (!closed)
                {
                    ready.put(id, status == 0);
                }
            }
        });
        try
        {
            for (String filename : this.context.getAssets().list("sounds"))
            {
                try (AssetFileDescriptor file = this.context.getAssets().openFd("sounds/" + filename))
                {
                    int id = pool.load(file, 1);
                    sounds.put(filename.substring(0, filename.lastIndexOf('.')), id);
                }
            }
        }
        catch (IOException error)
        {
            android.util.Log.w("LemonNative", "Some notification sounds are unavailable", error);
        }
    }

    public synchronized void setEnabled(boolean value)
    {
        enabled = value;
        if (!value && !closed)
        {
            pool.autoPause();
        }
    }

    public synchronized void play(String event)
    {
        Integer sound = sounds.get(event);
        if (!closed && enabled && sound != null && Boolean.TRUE.equals(ready.get(sound)))
        {
            pool.play(sound, 1, 1, 1, 0, 1);
        }
    }

    @Override
    public synchronized void close()
    {
        if (!closed)
        {
            closed = true;
            pool.release();
            sounds.clear();
            ready.clear();
        }
    }
}
