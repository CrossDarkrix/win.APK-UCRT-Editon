package android.hardware.usb;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import com.macapk.loader.HostUsbDirectBridge;
import com.macapk.loader.HostUsbCdBridge;
import java.util.HashMap;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Android USB host facade backed by Windows SetupAPI/WinUSB, plus optical BOT compatibility. */
public class UsbManager {
 public static final String ACTION_USB_DEVICE_ATTACHED="android.hardware.usb.action.USB_DEVICE_ATTACHED";
 public static final String ACTION_USB_DEVICE_DETACHED="android.hardware.usb.action.USB_DEVICE_DETACHED";
 public static final String ACTION_USB_ACCESSORY_ATTACHED="android.hardware.usb.action.USB_ACCESSORY_ATTACHED";
 public static final String ACTION_USB_ACCESSORY_DETACHED="android.hardware.usb.action.USB_ACCESSORY_DETACHED";
 public static final String EXTRA_DEVICE="device",EXTRA_ACCESSORY="accessory",EXTRA_PERMISSION_GRANTED="permission";
 private final Context context;
 private static final Object CD_LOCK=new Object();
 private static final ExecutorService CD_ENUMERATOR=Executors.newSingleThreadExecutor(new ThreadFactory(){public Thread newThread(Runnable r){Thread t=new Thread(r,"winapk-cd-enumerator");t.setDaemon(true);return t;}});
 private static Future<String[]> cdEnumeration;
 private static volatile String[] cdCache=new String[0];
 private static volatile long cdCacheNanos=0L;
 private static final long CD_CACHE_NS=TimeUnit.SECONDS.toNanos(2);
 private static final long CD_WAIT_MS=1200L;
 private static String[] enumerateCdBounded(){
  final long now=System.nanoTime();String[] cached=cdCache;
  if(cached.length>0&&now-cdCacheNanos<CD_CACHE_NS)return cached.clone();
  Future<String[]> future;
  synchronized(CD_LOCK){
   if(cdEnumeration==null||cdEnumeration.isDone())cdEnumeration=CD_ENUMERATOR.submit(new Callable<String[]>(){public String[] call(){String[] found=HostUsbCdBridge.enumerate();return found==null?new String[0]:found;}});
   future=cdEnumeration;
  }
  try{String[] found=future.get(CD_WAIT_MS,TimeUnit.MILLISECONDS);if(found==null)found=new String[0];cdCache=found.clone();cdCacheNanos=System.nanoTime();android.util.Log.i("Phase20-B2.3.3.6","CD enumeration completed entries="+found.length);return found;}
  catch(TimeoutException x){android.util.Log.w("Phase20-B2.3.3.6","CD enumeration still running; returning cache entries="+cached.length);return cached.clone();}
  catch(InterruptedException x){Thread.currentThread().interrupt();android.util.Log.w("Phase20-B2.3.3.6","CD enumeration interrupted; returning cache");return cached.clone();}
  catch(ExecutionException x){synchronized(CD_LOCK){if(cdEnumeration==future)cdEnumeration=null;}android.util.Log.e("Phase20-B2.3.3.6","CD enumeration failed",x.getCause());return cached.clone();}
 }
 public UsbManager(){context=null;} public UsbManager(Context c){context=c;}
 private String pkg(){try{return context==null?"unknown":context.getPackageName();}catch(Throwable t){return "unknown";}}
 public HashMap<String,UsbDevice> getDeviceList(){
  p20log("UsbManager.getDeviceList entered package="+pkg());HashMap<String,UsbDevice> out=new HashMap<>();
  try{UsbDevice d=createVirtualDac();out.put(d.getDeviceName(),d);p20log("virtual device inserted");}catch(Throwable t){p20error("virtual device creation failed",t);}
  p20log("physical HostUsbDirectBridge skipped: native enumerate is optional and unavailable in this build");
  try{String[] cd=enumerateCdBounded();for(int i=0;i+1<cd.length;i+=2){if(cd[i]==null||cd[i].isEmpty())continue;UsbEndpoint bi=new UsbEndpoint(0x81,2,65536,0),bo=new UsbEndpoint(1,2,65536,0);UsbInterface f=new UsbInterface(0,0,"Windows CD-ROM",8,6,0x50);f.setEndpoints(new UsbEndpoint[]{bi,bo});UsbDevice d=new UsbDevice(cd[i],0,0,8,6,0x50,"Windows",cd[i+1]==null?"Windows optical drive":cd[i+1],"1.0",null);d.setInterfaces(new UsbInterface[]{f});out.put(cd[i],d);}p20log("CD devices inserted="+(cd.length/2));}catch(Throwable t){p20error("CD enumeration failed; virtual device preserved",t);}
  p20log("UsbManager.getDeviceList returning size="+out.size()+" containsVirtual="+out.containsKey(P20_NAME));return out;
 }

 private static UsbDevice parse(String r){try{String[] p=r.split("\\t",-1);if(p.length<11)return null;UsbDevice d=new UsbDevice(p[0],Integer.parseInt(p[2],16),Integer.parseInt(p[3],16),Integer.parseInt(p[4]),Integer.parseInt(p[5]),Integer.parseInt(p[6]),p[7],p[8],p[9],p[10]);java.util.ArrayList<UsbInterface> fs=new java.util.ArrayList<>();if(p.length>11&&!p[11].isEmpty())for(String q:p[11].split(";")){String[] x=q.split(",",-1);if(x.length<7)continue;UsbInterface f=new UsbInterface(Integer.parseInt(x[0]),Integer.parseInt(x[1]),x[6],Integer.parseInt(x[2]),Integer.parseInt(x[3]),Integer.parseInt(x[4]));java.util.ArrayList<UsbEndpoint> es=new java.util.ArrayList<>();if(x.length>7&&!x[7].isEmpty())for(String ep:x[7].split("\\|")){String[] e=ep.split(":");if(e.length==4)es.add(new UsbEndpoint(Integer.parseInt(e[0],16),Integer.parseInt(e[1]),Integer.parseInt(e[2]),Integer.parseInt(e[3])));}f.setEndpoints(es.toArray(new UsbEndpoint[0]));fs.add(f);}d.setInterfaces(fs.toArray(new UsbInterface[0]));return d;}catch(Throwable t){return null;}}
 private static boolean isOptical(UsbDevice d){
  if(d==null)return false;
  if(d.getDeviceClass()==8&&d.getDeviceSubclass()==6&&d.getDeviceProtocol()==0x50)return true;
  for(int i=0;i<d.getInterfaceCount();i++){UsbInterface f=d.getInterface(i);if(f!=null&&f.getInterfaceClass()==8&&f.getInterfaceSubclass()==6&&f.getInterfaceProtocol()==0x50)return true;}
  return false;
 }
 public boolean hasPermission(UsbDevice d){
  if(d==null)return false;
  if(isVirtual(d))return true;
  if(isOptical(d)){p20log("CD permission granted by Windows host path="+d.getDeviceName());return true;}
  try{return HostUsbDirectBridge.hasPermission(pkg(),d.getDeviceName());}catch(UnsatisfiedLinkError x){p20error("USB Direct permission native unavailable",x);return false;}
 }
 public void requestPermission(UsbDevice d,PendingIntent pi){
  boolean ok=d!=null;
  if(ok&&!isVirtual(d)&&!isOptical(d))try{HostUsbDirectBridge.setPermission(pkg(),d.getDeviceName(),true);}catch(UnsatisfiedLinkError x){p20error("USB Direct setPermission native unavailable",x);ok=false;}
  if(pi!=null)try{Intent i=new Intent();i.putExtra(EXTRA_DEVICE,d);i.putExtra(EXTRA_PERMISSION_GRANTED,ok);pi.send(context,0,i);}catch(Throwable ignored){}
 }
 public UsbDeviceConnection openDevice(UsbDevice d){
  if(d==null||!hasPermission(d))return null;
  if(isVirtual(d))return UsbDeviceConnection.openVirtual(d);
  if(isOptical(d)){long h=HostUsbCdBridge.open(d.getDeviceName());p20log("CD open path="+d.getDeviceName()+" handle="+h);return h==0?null:new UsbDeviceConnection(d,h,true);}
  long h=HostUsbDirectBridge.open(d.getDeviceName());return h==0?null:new UsbDeviceConnection(d,h,false);
 }
 public UsbAccessory[] getAccessoryList(){return null;} public boolean hasPermission(UsbAccessory a){return false;} public void requestPermission(UsbAccessory a,PendingIntent p){}

 private static final String P20_TAG="Phase20-A3";
 static final String P20_NAME="/dev/bus/usb/virtual/ucrt-dac-fresh-002";
 private static final int[] P20_RATES=new int[]{44100,48000,88200,96000,176400,192000};
 private static volatile int p20SelectedRate=96000;
 private static void p20log(String s){try{android.util.Log.i(P20_TAG,s);}catch(Throwable ignored){try{System.err.println("["+P20_TAG+"] "+s);}catch(Throwable ignored2){}}}
 private static void p20error(String s,Throwable t){try{android.util.Log.e(P20_TAG,s,t);}catch(Throwable ignored){try{System.err.println("["+P20_TAG+"] "+s+" "+t);}catch(Throwable ignored2){}}}
 static boolean isVirtual(UsbDevice d){return d!=null&&P20_NAME.equals(d.getDeviceName());}
 private static UsbDevice createVirtualDac(){
  p20log("createVirtualDac entered");
  UsbInterface ac=new UsbInterface(0,0,"Win.apk Virtual DAC AudioControl",1,1,0);ac.setEndpoints(new UsbEndpoint[0]);
  UsbInterface as0=new UsbInterface(1,0,"Win.apk Virtual DAC Zero Bandwidth",1,2,0);as0.setEndpoints(new UsbEndpoint[0]);
  UsbEndpoint out1=new UsbEndpoint(0x01,0x05,576,1),out2=new UsbEndpoint(0x01,0x05,576,1),out3=new UsbEndpoint(0x01,0x05,576,1),out4=new UsbEndpoint(0x01,0x05,576,1),out5=new UsbEndpoint(0x01,0x05,576,1),out6=new UsbEndpoint(0x01,0x05,576,1);
  UsbInterface as1=new UsbInterface(1,1,"Win.apk Virtual DAC PCM Stereo 24-bit 44.1kHz",1,2,0);as1.setEndpoints(new UsbEndpoint[]{out1});
  UsbInterface as2=new UsbInterface(1,2,"Win.apk Virtual DAC PCM Stereo 24-bit 48kHz",1,2,0);as2.setEndpoints(new UsbEndpoint[]{out2});
  UsbInterface as3=new UsbInterface(1,3,"Win.apk Virtual DAC PCM Stereo 24-bit 88.2kHz",1,2,0);as3.setEndpoints(new UsbEndpoint[]{out3});
  UsbInterface as4=new UsbInterface(1,4,"Win.apk Virtual DAC PCM Stereo 24-bit 96kHz",1,2,0);as4.setEndpoints(new UsbEndpoint[]{out4});
  UsbInterface as5=new UsbInterface(1,5,"Win.apk Virtual DAC PCM Stereo 24-bit 176.4kHz",1,2,0);as5.setEndpoints(new UsbEndpoint[]{out5});
  UsbInterface as6=new UsbInterface(1,6,"Win.apk Virtual DAC PCM Stereo 24-bit 192kHz",1,2,0);as6.setEndpoints(new UsbEndpoint[]{out6});
  UsbConfiguration cfg=new UsbConfiguration(1,"Win.apk Virtual USB Audio Six Rate Diagnostic",0x80,100);cfg.setInterfaces(new UsbInterface[]{ac,as0,as1,as2,as3,as4,as5,as6});
  UsbDevice d=new UsbDevice(P20_NAME,0x1209,0xA20A,0,0,0,"Win.apk","Win.apk Fresh Virtual USB DAC Six Rate","2.02","UCRT-DAC-SIXRATE-003");
  d.setConfigurations(new UsbConfiguration[]{cfg});d.setInterfaces(new UsbInterface[]{ac,as0,as1,as2,as3,as4,as5,as6});
  p20dump(d);return d;
 }
 private static void p20dump(UsbDevice d){
  try{boolean detected=false;p20log("device="+d.getDeviceName()+" interfaces="+d.getInterfaceCount());
   for(int n=0;n<d.getInterfaceCount();n++){UsbInterface f=d.getInterface(n);p20log("if="+f.getId()+" alt="+f.getAlternateSetting()+" class="+f.getInterfaceClass()+" subclass="+f.getInterfaceSubclass()+" endpoints="+f.getEndpointCount());
    for(int e=0;e<f.getEndpointCount();e++){UsbEndpoint ep=f.getEndpoint(e);p20log("ep="+e+" address=0x"+Integer.toHexString(ep.getAddress())+" attributes=0x"+Integer.toHexString(ep.getAttributes())+" type="+ep.getType()+" direction="+ep.getDirection()+" maxPacket="+ep.getMaxPacketSize());if(f.getInterfaceClass()==1&&f.getInterfaceSubclass()==2&&ep.getType()==UsbConstants.USB_ENDPOINT_XFER_ISOC&&ep.getDirection()==UsbConstants.USB_DIR_OUT)detected=true;}}
   p20log("UCRT isDac equivalent="+detected);
  }catch(Throwable t){p20error("p20dump failed",t);}
 }
 static byte[] virtualDescriptors(){return new byte[]{
  18,1,0,2,0,0,0,64,9,18,10,-94,0,1,1,2,3,1,9,2,59,1,2,1,0,-128,50,9,4,0,0,0,1,1,0,0,9,36,1,0,1,30,0,1,1,12,36,2,1,1,1,0,2,3,0,0,0,9,36,3,2,1,3,0,1,0,9,4,1,0,0,1,2,0,0,9,4,1,1,1,1,2,0,0,7,36,1,1,1,1,0,11,36,2,1,2,3,24,1,68,-84,0,9,5,1,5,64,2,1,0,0,7,37,1,1,0,0,0,9,4,1,2,1,1,2,0,0,7,36,1,1,1,1,0,11,36,2,1,2,3,24,1,-128,-69,0,9,5,1,5,64,2,1,0,0,7,37,1,1,0,0,0,9,4,1,3,1,1,2,0,0,7,36,1,1,1,1,0,11,36,2,1,2,3,24,1,-120,88,1,9,5,1,5,64,2,1,0,0,7,37,1,1,0,0,0,9,4,1,4,1,1,2,0,0,7,36,1,1,1,1,0,11,36,2,1,2,3,24,1,0,119,1,9,5,1,5,64,2,1,0,0,7,37,1,1,0,0,0,9,4,1,5,1,1,2,0,0,7,36,1,1,1,1,0,11,36,2,1,2,3,24,1,16,-79,2,9,5,1,5,64,2,1,0,0,7,37,1,1,0,0,0,9,4,1,6,1,1,2,0,0,7,36,1,1,1,1,0,11,36,2,1,2,3,24,1,0,-18,2,9,5,1,5,64,2,1,0,0,7,37,1,1,0,0,0};}
 static boolean virtualSetInterface(int iface,int alt){
  if(iface!=1||alt<0||alt>P20_RATES.length)return false;
  if(alt>0)p20SelectedRate=P20_RATES[alt-1];
  p20log("setInterface selected if="+iface+" alt="+alt+" rate="+(alt==0?0:p20SelectedRate));return true;
 }
 static int virtualControl(int rt,int request,int value,int index,byte[] data,int off,int len){
  if(data==null||off<0||len<0||off+len>data.length)return -1;
  boolean ep=(rt&0x1f)==2&&(value&0xff00)==0x0100;
  if(ep&&(rt&0x80)!=0&&request==0x81&&len>=3){int r=p20SelectedRate;data[off]=(byte)(r&255);data[off+1]=(byte)((r>>>8)&255);data[off+2]=(byte)((r>>>16)&255);return 3;}
  if(ep&&(rt&0x80)==0&&request==0x01&&len>=3){int r=(data[off]&255)|((data[off+1]&255)<<8)|((data[off+2]&255)<<16);for(int x:P20_RATES)if(x==r){p20SelectedRate=r;p20log("SET_CUR rate="+r);return len;}return -1;}
  return 0;
 }
}
