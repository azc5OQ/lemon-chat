package com.lemonchat.audio;

import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import java.util.LinkedHashMap;
import java.util.Map;

/** immutable capture and transport preferences */
public final class AudioOptions
{
    public final String microphoneDevice;
    public final String transport;
    public final boolean soundEffects;

    public AudioOptions(String microphoneDevice, String transport, boolean soundEffects)
    {
        this.microphoneDevice = microphoneDevice == null ? "" : microphoneDevice;
        this.transport = "websocket".equals(transport) ? "websocket" : "webrtc";
        this.soundEffects = soundEffects;
    }

    /**
     * lists currently connected microphones with stable preference keys
     *
     * @param context application or settings context
     * @return ordered device keys and readable names
     */
    public static Map<String, String> microphoneChoices(Context context)
    {
        Map<String, String> choices = new LinkedHashMap<>();
        choices.put("", "System default");
        AudioManager manager = context.getSystemService(AudioManager.class);
        if (manager == null)
        {
            return choices;
        }
        for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_INPUTS))
        {
            if (choices.size() >= 64)
            {
                break;
            }
            String label = device.getProductName().toString();
            choices.put(deviceKey(device), label.length() > 96 ? label.substring(0, 96) : label);
        }
        return choices;
    }

    static String deviceKey(AudioDeviceInfo device)
    {
        String address = Build.VERSION.SDK_INT >= 28 ? device.getAddress() : device.getProductName().toString();
        return device.getType() + ":" + address;
    }

    static AudioDeviceInfo findMicrophone(Context context, String key)
    {
        AudioManager manager = context.getSystemService(AudioManager.class);
        if (manager != null && !key.isEmpty())
        {
            for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_INPUTS))
            {
                if (deviceKey(device).equals(key))
                {
                    return device;
                }
            }
        }
        return null;
    }
}
