package android.hardware.usb;
import com.macapk.loader.HostUsbDirectBridge;import com.macapk.loader.HostUsbCdBridge;import java.util.concurrent.*;import java.util.concurrent.atomic.AtomicInteger;
public class UsbDeviceConnection{
 private final UsbDevice device;private long handle;private final boolean optical;private final boolean virtual;private final BlockingQueue<UsbRequest> completed=new LinkedBlockingQueue<>();private final ExecutorService worker=Executors.newCachedThreadPool();
 private static final AtomicInteger NEXT_VIRTUAL_FD=new AtomicInteger(0x5A20);private final Object botLock=new Object();private byte[] botData=new byte[0],botCsw=new byte[0];private int botDataOffset=0,botCswOffset=0;
 public UsbDeviceConnection(UsbDevice d){this(d,0,false,false);}public UsbDeviceConnection(UsbDevice d,long h){this(d,h,false,false);}UsbDeviceConnection(UsbDevice d,long h,boolean o){this(d,h,o,false);}private UsbDeviceConnection(UsbDevice d,long h,boolean o,boolean v){device=d;handle=h;optical=o;virtual=v;}static UsbDeviceConnection openVirtual(UsbDevice d){int fd=NEXT_VIRTUAL_FD.getAndIncrement();android.util.Log.i("Phase20-B2.3.4.7","openVirtual "+(d==null?"null":d.getDeviceName())+" fd="+fd);return new UsbDeviceConnection(d,fd,false,true);}
 public synchronized boolean isOpen(){return handle!=0;} long nativeHandle(){return handle;} boolean isOptical(){return optical;}
 public boolean claimInterface(UsbInterface i,boolean force){boolean ok=isOpen()&&i!=null&&(virtual||optical||HostUsbDirectBridge.claim(handle,i.getId(),force));if(virtual)android.util.Log.i("Phase20-B1","claimInterface if="+(i==null?-1:i.getId())+" alt="+(i==null?-1:i.getAlternateSetting())+" ok="+ok);return ok;}
 public boolean releaseInterface(UsbInterface i){return isOpen()&&i!=null&&(virtual||optical||HostUsbDirectBridge.release(handle,i.getId()));}
 public boolean setInterface(UsbInterface i){boolean ok=isOpen()&&i!=null&&(virtual?UsbManager.virtualSetInterface(i.getId(),i.getAlternateSetting()):(optical||HostUsbDirectBridge.setInterface(handle,i.getId(),i.getAlternateSetting())));if(virtual)android.util.Log.i("Phase20-B1","setInterface if="+(i==null?-1:i.getId())+" alt="+(i==null?-1:i.getAlternateSetting())+" ok="+ok);return ok;}
 public boolean setConfiguration(UsbConfiguration c){return isOpen()&&c!=null;}
 public int controlTransfer(int rt,int r,int v,int idx,byte[] b,int len,int to){return controlTransfer(rt,r,v,idx,b,0,len,to);}public int controlTransfer(int rt,int r,int v,int idx,byte[] b,int off,int len,int to){if(!isOpen()||optical)return -1;if(virtual)return UsbManager.virtualControl(rt,r,v,idx,b,off,len);return HostUsbDirectBridge.control(handle,rt,r,v,idx,b,off,len,to);}
 public int bulkTransfer(UsbEndpoint ep,byte[] b,int len,int to){return bulkTransfer(ep,b,0,len,to);}public int bulkTransfer(UsbEndpoint ep,byte[] b,int off,int len,int to){if(!isOpen()||ep==null||b==null||off<0||len<0||off+len>b.length)return -1;if(!optical)return HostUsbDirectBridge.transfer(handle,ep.getAddress(),b,off,len,to);synchronized(botLock){return opticalBulkTransfer(ep,b,off,len,to);}}
 private static long le32(byte[] b,int p){return ((long)b[p]&255)|(((long)b[p+1]&255)<<8)|(((long)b[p+2]&255)<<16)|(((long)b[p+3]&255)<<24);}
 private static void putLe32(byte[] b,int p,long v){b[p]=(byte)v;b[p+1]=(byte)(v>>>8);b[p+2]=(byte)(v>>>16);b[p+3]=(byte)(v>>>24);}
 private int opticalBulkTransfer(UsbEndpoint ep,byte[] b,int off,int len,int timeout){
  final boolean in=ep.getDirection()==UsbConstants.USB_DIR_IN;
  if(!in){
   if(len<31||le32(b,off)!=0x43425355L){android.util.Log.e("Phase20-B2.3.3.8","CD BOT invalid CBW length="+len);return -1;}
   final long tag=le32(b,off+4),expected=le32(b,off+8);final boolean dataIn=(b[off+12]&0x80)!=0;final int cdbLen=Math.min(16,b[off+14]&31);
   if(cdbLen<1){android.util.Log.e("Phase20-B2.3.3.8","CD BOT invalid CDB length");return -1;}
   byte[] cdb=new byte[cdbLen];System.arraycopy(b,off+15,cdb,0,cdbLen);
   byte[] result;if(!dataIn&&expected!=0){android.util.Log.e("Phase20-B2.3.3.8","CD BOT data-out command unsupported opcode=0x"+Integer.toHexString(cdb[0]&255));result=null;}else result=HostUsbCdBridge.execute(handle,cdb,dataIn?(int)Math.min(expected,16L*1024L*1024L):0,Math.max(timeout,10000));
   boolean ok=result!=null;botData=ok&&dataIn?result:new byte[0];botDataOffset=0;long transferred=Math.min(expected,(long)botData.length);long residue=Math.max(0,expected-transferred);
   botCsw=new byte[13];putLe32(botCsw,0,0x53425355L);putLe32(botCsw,4,tag);putLe32(botCsw,8,residue);botCsw[12]=(byte)(ok?0:1);botCswOffset=0;
   android.util.Log.i("Phase20-B2.3.3.8","CD BOT CBW opcode=0x"+Integer.toHexString(cdb[0]&255)+" tag="+tag+" expected="+expected+" returned="+botData.length+" status="+(ok?0:1));return len;
  }
  if(botDataOffset<botData.length){int count=Math.min(len,botData.length-botDataOffset);System.arraycopy(botData,botDataOffset,b,off,count);botDataOffset+=count;return count;}
  if(botCswOffset<botCsw.length){int count=Math.min(len,botCsw.length-botCswOffset);System.arraycopy(botCsw,botCswOffset,b,off,count);botCswOffset+=count;if(botCswOffset==botCsw.length){botData=new byte[0];botCsw=new byte[0];botDataOffset=botCswOffset=0;}return count;}
  android.util.Log.w("Phase20-B2.3.3.8","CD BOT IN requested without pending data/CSW");return -1;
 }
 public byte[] getRawDescriptors(){if(!isOpen()||optical)return new byte[0];byte[] out=virtual?UsbManager.virtualDescriptors():HostUsbDirectBridge.descriptors(handle);if(virtual)android.util.Log.i("Phase20-B1","getRawDescriptors bytes="+(out==null?-1:out.length));return out;}
 void submit(final UsbRequest q,final java.nio.ByteBuffer b,final int len){worker.execute(()->{q.runTransfer(b,len);completed.offer(q);});}
 public UsbRequest requestWait(){try{return completed.take();}catch(InterruptedException e){Thread.currentThread().interrupt();return null;}}public UsbRequest requestWait(long timeout)throws TimeoutException{try{UsbRequest r=completed.poll(timeout,TimeUnit.MILLISECONDS);if(r==null)throw new TimeoutException();return r;}catch(InterruptedException e){Thread.currentThread().interrupt();throw new TimeoutException();}}
 boolean cancel(UsbEndpoint ep){return isOpen()&&!optical&&HostUsbDirectBridge.cancel(handle,ep.getAddress());}
 public synchronized void close(){
  if(virtual){
   // The virtual DAC represents the process-wide Windows output route. UCRT may
   // close its source-side USB connection while replacing a MediaPlayer/CD
   // stream. Treat that close as a soft source detach: keep the DAC connection,
   // request worker and native WASAPI/URB ownership alive. CD ownership remains
   // exclusively in HostUsbCdBridge and can never close this route.
   android.util.Log.i("Phase20-B2.3.4.2","virtual DAC close isolated from CD/source lifecycle; preserved handle="+handle);
   return;
  }
  if(handle!=0){
   if(optical){
    android.util.Log.i("Phase20-B2.3.4.2","closing optical connection only handle="+handle);
    HostUsbCdBridge.close(handle);
   }else{
    android.util.Log.i("Phase20-B2.3.4.2","closing physical USB Direct connection only handle="+handle);
    HostUsbDirectBridge.close(handle);
   }
   handle=0;
  }
  worker.shutdownNow();
 }
 public int getFileDescriptor(){int fd=virtual?(int)handle:-1;if(virtual)android.util.Log.i("Phase20-B2.3.4.7","getFileDescriptor virtual fd="+fd);return fd;} public int getSerial(){return 0;}
}
