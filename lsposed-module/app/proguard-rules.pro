-dontwarn io.github.libxposed.annotation.**
-adaptresourcefilecontents META-INF/xposed/java_init.list
-keep,allowoptimization public class com.gushu.longvc.ModuleMain extends io.github.libxposed.api.XposedModule {
    public <init>();
    *;
}
-keep class com.gushu.longvc.LongVcApplication { *; }
-keepattributes *Annotation*
