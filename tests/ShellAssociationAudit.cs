using System;
using System.Runtime.InteropServices;
[ComImport, Guid("973810ae-9599-4b88-9e4d-6ee98c9552da"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IEnumAssocHandlers {
 [PreserveSig] int Next(uint c,[MarshalAs(UnmanagedType.Interface)] out IAssocHandler handler,out uint fetched);
}
[ComImport,Guid("f04061ac-1659-4a3f-a954-775aa57fc083"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAssocHandler {
 [PreserveSig] int GetName(out IntPtr name);
 [PreserveSig] int GetUIName(out IntPtr name);
}
public static class VetroShellAudit {
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)] static extern int SHAssocEnumHandlers(string extension,uint filter,out IEnumAssocHandlers handlers);
 public static string List(string extension) {
  IEnumAssocHandlers list;int hr=SHAssocEnumHandlers(extension,0,out list);string result="HR="+hr+"\n";if(hr!=0)return result;
  IAssocHandler handler;uint fetched;while(list.Next(1,out handler,out fetched)==0&&fetched==1){IntPtr n,u;int a=handler.GetName(out n);int b=handler.GetUIName(out u);result+=a+" "+Marshal.PtrToStringUni(n)+" | "+b+" "+Marshal.PtrToStringUni(u)+"\n";Marshal.FreeCoTaskMem(n);Marshal.FreeCoTaskMem(u);Marshal.ReleaseComObject(handler);}Marshal.ReleaseComObject(list);return result;
 }
}
