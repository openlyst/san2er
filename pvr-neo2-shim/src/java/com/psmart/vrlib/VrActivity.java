package com.psmart.vrlib;

import android.app.Activity;
import android.util.Log;

public class VrActivity {
    private static final String TAG = "VrActivity";

    private static int checkCount = 0;

    public static boolean check6DofAppResume() {
        checkCount++;
        boolean result = (checkCount == 1);
        Log.i(TAG, "check6DofAppResume -> " + result + " (call #" + checkCount + ")");
        return result;
    }

    public static void SetSecure(Activity activity, boolean isOpen) {
        Log.i(TAG, "SetSecure stub");
    }

    public static void BindVerifyService(String gameObjectName) {
        Log.i(TAG, "BindVerifyService stub");
    }

    public static void StartHomeKeyReceiver(String gameObjectName) {
        Log.i(TAG, "StartHomeKeyReceiver stub");
    }

    public static void StopHomeKeyReceiver() {
        Log.i(TAG, "StopHomeKeyReceiver stub");
    }

    public static void initKeyEventManager(Activity activity) {
        Log.i(TAG, "initKeyEventManager stub");
    }

    public static int getColorRes(String name) {
        return 0;
    }

    public static int getConfigInt(String name) {
        return 0;
    }

    public static String getConfigString(String name) {
        return "";
    }

    public static String getDrawableLocation(String name) {
        return "";
    }

    public static int getTextSize(String name) {
        return 0;
    }

    public static String getLangString(String name) {
        return "";
    }

    public static String getStringValue(int id, int type) {
        return "";
    }

    public static int getIntValue(int id, int type) {
        return 0;
    }

    public static float getFloatValue(int id) {
        return 0f;
    }

    public static String getObjectOrArray(int id, int type) {
        return "";
    }

    public static int getCharSpace(int id) {
        return 0;
    }

    public static void Pvr_InitAudioDevice(Activity activity) {
        Log.i(TAG, "Pvr_InitAudioDevice stub");
    }
}
