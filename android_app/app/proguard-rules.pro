# Preserve all upstream nx2android JNI runtime classes and methods (called by libdmhost.so)
-keep class org.nx2a.** { *; }
-keepclassmembers class org.nx2a.** { *; }

# Preserve Hybrid Extractor JNI bridge and progress callback (called by libnxhybrid_extract.so)
-keep class org.nx2a.doraemon.NativeBridge { *; }
-keep class org.nx2a.doraemon.NativeBridge$ProgressCallback { *; }
-keepclassmembers class org.nx2a.doraemon.NativeBridge$ProgressCallback {
    public void onProgress(java.lang.String, long, long, long, long);
}

# Preserve Android components referenced from AndroidManifest.xml
-keep public class * extends android.app.Activity
-keep public class * extends android.app.NativeActivity
-keep public class * extends android.app.Service
-keep public class * extends android.content.BroadcastReceiver
