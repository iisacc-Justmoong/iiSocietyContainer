package com.iisacc.society.storage;

import android.content.Context;
import android.net.Uri;
import android.os.Bundle;
import org.json.JSONObject;

/** Packaged in each signed iisacc client, without registering another drive. */
public final class SocietyClient {
    private SocietyClient() { }
    public static String request(Context context, String action, String section, String path, String identifier) {
        try {
            Bundle request = new Bundle(); request.putString("section", section); request.putString("path", path); request.putString("identifier", identifier);
            Bundle result = context.getContentResolver().call(Uri.parse("content://com.iisacc.society.internal"), action, null, request);
            if (result == null || !result.containsKey("json")) throw new IllegalStateException("Society did not respond");
            return result.getString("json");
        } catch (Exception error) {
            try { return new JSONObject().put("error", error.toString()).toString(); }
            catch (Exception impossible) { return "{\"error\":\"Society is unavailable\"}"; }
        }
    }
}
