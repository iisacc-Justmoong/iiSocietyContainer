package com.iisacc.society.storage;

import com.iisacc.iifileprovider.FileStorage;
import java.io.IOException;

/** Runs without an Android device; no Android runtime or JNI method is invoked. */
public final class AndroidPresentationNamesTest {
    public static void main(String[] arguments) throws Exception {
        for (String name : new String[]{".asset.png.iisacc-preview", ".iifile-stage-private", ".iisacc-file-provider",
                ".iisacc-temporary.lock", "asset.iisacc.json", "asset.iisacc-lock", ".ASSET.IISACC-PREVIEW"}) {
            if (!FileStorage.isControlFile(name)) throw new AssertionError("Provider control item is public: " + name);
            try { DriveStore.name(name); throw new AssertionError("Society allows a control item: " + name); }
            catch (IOException expected) { }
        }
        for (String name : new String[]{"문서 #1.txt", "image.png", ".public-hidden", "notes.json", "모델.safetensors"}) {
            if (FileStorage.isControlFile(name)) throw new AssertionError("Public asset is hidden: " + name);
            DriveStore.name(name);
        }
        System.out.println("Android public file names and private presentation boundaries passed without loading JNI");
    }
}
