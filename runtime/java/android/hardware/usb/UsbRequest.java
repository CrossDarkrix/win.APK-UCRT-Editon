package android.hardware.usb;
import java.nio.ByteBuffer;
public class UsbRequest implements AutoCloseable{
 private UsbDeviceConnection connection;private UsbEndpoint endpoint;private Object clientData;private volatile boolean cancelled,closed;
 public UsbRequest(){} public boolean initialize(UsbDeviceConnection c,UsbEndpoint e){if(c==null||e==null||!c.isOpen())return false;connection=c;endpoint=e;return true;}
 public UsbEndpoint getEndpoint(){return endpoint;}public Object getClientData(){return clientData;}public void setClientData(Object x){clientData=x;}
 public boolean queue(ByteBuffer b){return queue(b,b==null?0:b.remaining());}public boolean queue(ByteBuffer b,int length){if(closed||cancelled||connection==null||b==null||length<0||length>b.remaining())return false;connection.submit(this,b,length);return true;}
 void runTransfer(ByteBuffer b,int length){if(cancelled)return;byte[] a=new byte[length];boolean in=endpoint.getDirection()==UsbConstants.USB_DIR_IN;if(!in)b.get(a);int n=connection.bulkTransfer(endpoint,a,0,length,0);if(in&&n>0)b.put(a,0,n);}
 public boolean cancel(){cancelled=true;return connection!=null&&endpoint!=null&&connection.cancel(endpoint);}public void close(){if(!closed){cancel();closed=true;connection=null;endpoint=null;}}
}
