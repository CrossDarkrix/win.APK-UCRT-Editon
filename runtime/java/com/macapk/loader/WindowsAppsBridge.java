package com.macapk.loader;
import android.content.*;import android.content.pm.*;import android.graphics.Bitmap;import android.graphics.drawable.*;import java.util.*;import java.util.concurrent.*;
public final class WindowsAppsBridge {
 private WindowsAppsBridge(){} private static final String PREFIX="windows.app.";
 public static final class App{public final String id,label,parsing,aumid,pkg,cls;App(String i,String l,String p,String u,String k,String c){id=i;label=l;parsing=p;aumid=u;pkg=k;cls=c;}ComponentName component(){return new ComponentName(pkg,cls);}}
 private static volatile List<App> cache;private static volatile Map<String,App> packages,components;private static volatile long loaded;
 private static final ConcurrentHashMap<String,Drawable> ICONS=new ConcurrentHashMap<>();
 private static final ConcurrentHashMap<String,Long> ICON_MISSES=new ConcurrentHashMap<>();
 private static final Set<String> ICON_LOADING=Collections.newSetFromMap(new ConcurrentHashMap<String,Boolean>());
 private static final ExecutorService ICON_EXEC=Executors.newFixedThreadPool(4,new ThreadFactory(){private int n;public synchronized Thread newThread(Runnable r){Thread t=new Thread(r,"winapk-app-icon-"+(++n));t.setDaemon(true);return t;}});
 private static final long ICON_MISS_RETRY_MS=30000L;
 private static final ExecutorService INIT_EXEC=Executors.newSingleThreadExecutor(new ThreadFactory(){public Thread newThread(Runnable r){Thread t=new Thread(r,"winapk-app-catalog-init");t.setDaemon(true);return t;}});
 private static volatile Future<List<App>> INIT_FUTURE;
 private static volatile int START_APPS_COUNT,EXACT_AUMID_MATCHES,NAME_AUMID_MATCHES;
 private static final java.util.concurrent.atomic.AtomicInteger ICON_OK=new java.util.concurrent.atomic.AtomicInteger(),ICON_DONE=new java.util.concurrent.atomic.AtomicInteger();
 private static String norm(String x){if(x==null)return "";StringBuilder b=new StringBuilder();for(int i=0;i<x.length();i++){char c=Character.toLowerCase(x.charAt(i));if(Character.isLetterOrDigit(c))b.append(c);}return b.toString();}
 private static Map<String,String> startApps(){
  HashMap<String,String> m=new HashMap<>();
  try{
   String[] v=NativeCocoa.windowsStartApps();
   if(v!=null){
    START_APPS_COUNT=v.length/2;
    for(int i=0;i+1<v.length;i+=2){
     String name=v[i],id=v[i+1];
     if(id!=null&&!id.isEmpty()){
      m.put("id:"+id.toLowerCase(java.util.Locale.ROOT),id);
      String k=norm(name);
      if(!k.isEmpty()&&!m.containsKey("name:"+k))m.put("name:"+k,id);
     }
    }
   }
  }catch(Throwable x){
   try{NativeLog.w("Win.apk.AppsFolder","Get-StartApps: "+x);}catch(Throwable ignored){}
  }
  return m;
 }
 private static String matchAumid(Map<String,String>m,String label,String parsing){if(parsing!=null){String exact=m.get("id:"+parsing.toLowerCase(java.util.Locale.ROOT));if(exact!=null){EXACT_AUMID_MATCHES++;return exact;}}String byName=m.get("name:"+norm(label));if(byName!=null){NAME_AUMID_MATCHES++;return byName;}return "";}
 private static List<App> initializeCatalog(){ArrayList<App>o=new ArrayList<>();HashMap<String,App>ps=new HashMap<>(),cs=new HashMap<>();Map<String,String>am=startApps();try{String[]f=NativeCocoa.windowsAppsEnumerate();if(f!=null)for(int i=0;i+4<f.length;i+=5){String aumid=matchAumid(am,f[i+1],f[i+2]);App a=new App(f[i],f[i+1],f[i+2],aumid,f[i+3],f[i+4]);if(a.id!=null&&a.pkg!=null&&a.cls!=null&&a.pkg.startsWith(PREFIX)&&ps.put(a.pkg,a)==null){o.add(a);cs.put(a.pkg+"/"+a.cls,a);}}}catch(Throwable x){try{NativeLog.w("Win.apk.AppsFolder","catalog: "+x);}catch(Throwable ignored){}}
  // Phase 16: publish package metadata immediately. Icon extraction is expensive
  // Shell work and must never delay launcher/package enumeration.
  packages=Collections.unmodifiableMap(ps);components=Collections.unmodifiableMap(cs);final List<App>done=Collections.unmodifiableList(o);
  for(final App a:done){final String k=iconKey(a,64);if(ICONS.containsKey(k))continue;if(!ICON_LOADING.add(k))continue;ICON_EXEC.execute(new Runnable(){public void run(){try{Drawable d=decodeIcon(NativeCocoa.windowsAppIconArgbAumid(a.id,a.aumid,64));if(d!=null){ICONS.put(k,d);ICON_MISSES.remove(k);ICON_OK.incrementAndGet();}else ICON_MISSES.put(k,System.currentTimeMillis());}catch(Throwable x){ICON_MISSES.put(k,System.currentTimeMillis());}finally{ICON_LOADING.remove(k);int n=ICON_DONE.incrementAndGet();if(n==done.size()||n%32==0)try{NativeLog.i("Win.apk.AppsFolder","icon-workers done="+n+"/"+done.size()+" ok="+ICON_OK.get());}catch(Throwable ignored){}}}});}
  try{NativeLog.i("Win.apk.AppsFolder","Get-StartApps source="+START_APPS_COUNT+" exact="+EXACT_AUMID_MATCHES+" name="+NAME_AUMID_MATCHES+" matched="+countMatched(o)+" icon-workers=4 queued="+o.size());}catch(Throwable ignored){}return done;}
 private static int countMatched(List<App>a){int n=0;for(App x:a)if(x.aumid!=null&&!x.aumid.isEmpty())n++;return n;}
 private static String iconKey(App a,int size){return a.id+"@"+size;}
 private static Drawable decodeIcon(int[]v){if(v==null||v.length<3)return null;int w=v[0],h=v[1];if(w<=0||h<=0||v.length!=2+w*h)return null;int[]p=new int[w*h];System.arraycopy(v,2,p,0,p.length);return new BitmapDrawable(Bitmap.createBitmap(p,w,h,Bitmap.Config.ARGB_8888));}
 private static void queueIcon(final App a,final int size){final String k=iconKey(a,size);Long miss=ICON_MISSES.get(k);if(ICONS.containsKey(k)||(miss!=null&&System.currentTimeMillis()-miss<ICON_MISS_RETRY_MS)||!ICON_LOADING.add(k))return;ICON_EXEC.execute(new Runnable(){public void run(){try{Drawable d=decodeIcon(NativeCocoa.windowsAppIconArgbAumid(a.id,a.aumid,size));if(d!=null){ICONS.put(k,d);ICON_MISSES.remove(k);}else ICON_MISSES.put(k,System.currentTimeMillis());}catch(Throwable x){ICON_MISSES.put(k,System.currentTimeMillis());try{NativeLog.w("Win.apk.AppsFolder","async icon "+a.label+": "+x);}catch(Throwable ignored){}}finally{ICON_LOADING.remove(k);}}});}
 private static void prefetchIcons(List<App> list){int limit=Math.min(list.size(),96);for(int i=0;i<limit;i++)queueIcon(list.get(i),64);}
 public static List<App> apps(){List<App>h=cache;if(h!=null)return h;Future<List<App>>f=INIT_FUTURE;if(f==null)synchronized(WindowsAppsBridge.class){f=INIT_FUTURE;if(f==null){f=INIT_EXEC.submit(new Callable<List<App>>(){public List<App> call(){return initializeCatalog();}});INIT_FUTURE=f;}}try{h=f.get();cache=h;loaded=System.currentTimeMillis();return h;}catch(Throwable x){try{NativeLog.w("Win.apk.AppsFolder","initial preload: "+x);}catch(Throwable ignored){}h=Collections.emptyList();cache=h;return h;}}
 public static boolean isWindowsPackage(String p){return p!=null&&p.startsWith(PREFIX);}public static App byPackage(String p){apps();return packages.get(p);}public static App byComponent(ComponentName c){if(c==null)return null;apps();return components.get(c.getPackageName()+"/"+c.getClassName());}
 public static ApplicationInfo applicationInfo(App a){ApplicationInfo i=new ApplicationInfo();i.packageName=a.pkg;i.name=a.cls;i.nonLocalizedLabel=a.label;i.enabled=true;i.sourceDir=a.parsing;i.publicSourceDir=a.parsing;return i;}public static ActivityInfo activityInfo(App a){ActivityInfo i=new ActivityInfo();i.packageName=a.pkg;i.name=a.cls;i.applicationInfo=applicationInfo(a);i.nonLocalizedLabel=a.label;i.enabled=true;i.exported=true;return i;}public static PackageInfo packageInfo(App a){PackageInfo i=new PackageInfo();i.packageName=a.pkg;i.applicationInfo=applicationInfo(a);i.versionName="Windows";i.versionCode=1;i.activities=new ActivityInfo[]{activityInfo(a)};return i;}public static ResolveInfo resolveInfo(App a){ResolveInfo i=new ResolveInfo();i.activityInfo=activityInfo(a);i.nonLocalizedLabel=a.label;i.isDefault=true;i.match=android.content.IntentFilter.MATCH_CATEGORY_EMPTY;return i;}
 public static List<ResolveInfo> query(Intent in){if(in==null)return Collections.emptyList();ComponentName c=in.getComponent();if(c!=null&&isWindowsPackage(c.getPackageName())){App a=byComponent(c);return a==null?Collections.<ResolveInfo>emptyList():Collections.singletonList(resolveInfo(a));}String p=in.getPackage();if(isWindowsPackage(p)){App a=byPackage(p);return a==null?Collections.<ResolveInfo>emptyList():Collections.singletonList(resolveInfo(a));}if(!Intent.ACTION_MAIN.equals(in.getAction())||!in.hasCategory(Intent.CATEGORY_LAUNCHER))return Collections.emptyList();ArrayList<ResolveInfo>o=new ArrayList<>();for(App a:apps())o.add(resolveInfo(a));return o;}public static void appendResolve(List<ResolveInfo>o,Intent i){if(o!=null)o.addAll(query(i));}public static void appendApps(List<ApplicationInfo>o){if(o!=null)for(App a:apps())o.add(applicationInfo(a));}public static void appendPackages(List<PackageInfo>o){if(o!=null)for(App a:apps())o.add(packageInfo(a));}
 public static Drawable icon(App a){if(a==null)return null;String k=iconKey(a,64);Drawable d=ICONS.get(k);if(d!=null)return d;queueIcon(a,64);return null;}
 public static boolean launch(Intent i){if(i==null)return false;App a=byComponent(i.getComponent());if(a==null&&isWindowsPackage(i.getPackage()))a=byPackage(i.getPackage());if(a==null)return false;boolean ok=false;try{ok=NativeCocoa.windowsAppLaunch(a.id);}catch(Throwable x){}try{NativeLog.i("Win.apk.AppsFolder","launch "+a.label+" result="+ok);}catch(Throwable ignored){}return true;}
}
