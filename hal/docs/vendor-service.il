
//  Microsoft (R) .NET Framework IL Disassembler.  Version 4.8.3928.0




// Metadata version: v4.0.30319
.module extern THP_Service.dll
.assembly extern mscorlib
{
  .publickeytoken = (B7 7A 5C 56 19 34 E0 89 )                         // .z\V.4..
  .ver 4:0:0:0
}
.assembly extern System
{
  .publickeytoken = (B7 7A 5C 56 19 34 E0 89 )                         // .z\V.4..
  .ver 4:0:0:0
}
.assembly extern System.Configuration.Install
{
  .publickeytoken = (B0 3F 5F 7F 11 D5 0A 3A )                         // .?_....:
  .ver 4:0:0:0
}
.assembly extern System.ServiceProcess
{
  .publickeytoken = (B0 3F 5F 7F 11 D5 0A 3A )                         // .?_....:
  .ver 4:0:0:0
}
.assembly extern System.Xml
{
  .publickeytoken = (B7 7A 5C 56 19 34 E0 89 )                         // .z\V.4..
  .ver 4:0:0:0
}
.assembly HuaweiThpService
{
  .custom instance void [mscorlib]System.Runtime.CompilerServices.CompilationRelaxationsAttribute::.ctor(int32) = ( 01 00 08 00 00 00 00 00 ) 
  .custom instance void [mscorlib]System.Runtime.CompilerServices.RuntimeCompatibilityAttribute::.ctor() = ( 01 00 01 00 54 02 16 57 72 61 70 4E 6F 6E 45 78   // ....T..WrapNonEx
                                                                                                             63 65 70 74 69 6F 6E 54 68 72 6F 77 73 01 )       // ceptionThrows.

  // --- 下列自定义特性会自动添加，不要取消注释 -------
  //  .custom instance void [mscorlib]System.Diagnostics.DebuggableAttribute::.ctor(valuetype [mscorlib]System.Diagnostics.DebuggableAttribute/DebuggingModes) = ( 01 00 02 00 00 00 00 00 ) 

  .custom instance void [mscorlib]System.Reflection.AssemblyTitleAttribute::.ctor(string) = ( 01 00 10 48 75 61 77 65 69 54 68 70 53 65 72 76   // ...HuaweiThpServ
                                                                                              69 63 65 00 00 )                                  // ice..
  .custom instance void [mscorlib]System.Reflection.AssemblyDescriptionAttribute::.ctor(string) = ( 01 00 00 00 00 ) 
  .custom instance void [mscorlib]System.Reflection.AssemblyConfigurationAttribute::.ctor(string) = ( 01 00 00 00 00 ) 
  .custom instance void [mscorlib]System.Reflection.AssemblyCompanyAttribute::.ctor(string) = ( 01 00 00 00 00 ) 
  .custom instance void [mscorlib]System.Reflection.AssemblyProductAttribute::.ctor(string) = ( 01 00 10 48 75 61 77 65 69 54 68 70 53 65 72 76   // ...HuaweiThpServ
                                                                                                69 63 65 00 00 )                                  // ice..
  .custom instance void [mscorlib]System.Reflection.AssemblyCopyrightAttribute::.ctor(string) = ( 01 00 12 43 6F 70 79 72 69 67 68 74 20 C2 A9 20   // ...Copyright .. 
                                                                                                  20 32 30 32 32 00 00 )                            //  2022..
  .custom instance void [mscorlib]System.Reflection.AssemblyTrademarkAttribute::.ctor(string) = ( 01 00 00 00 00 ) 
  .custom instance void [mscorlib]System.Runtime.InteropServices.ComVisibleAttribute::.ctor(bool) = ( 01 00 00 00 00 ) 
  .custom instance void [mscorlib]System.Runtime.InteropServices.GuidAttribute::.ctor(string) = ( 01 00 24 36 37 61 65 66 32 31 38 2D 61 39 63 65   // ..$67aef218-a9ce
                                                                                                  2D 34 39 65 65 2D 62 31 39 64 2D 66 32 31 65 35   // -49ee-b19d-f21e5
                                                                                                  31 31 38 63 62 32 31 00 00 )                      // 118cb21..
  .custom instance void [mscorlib]System.Reflection.AssemblyFileVersionAttribute::.ctor(string) = ( 01 00 07 31 2E 30 2E 30 2E 30 00 00 )             // ...1.0.0.0..
  .custom instance void [mscorlib]System.Runtime.Versioning.TargetFrameworkAttribute::.ctor(string) = ( 01 00 1C 2E 4E 45 54 46 72 61 6D 65 77 6F 72 6B   // ....NETFramework
                                                                                                        2C 56 65 72 73 69 6F 6E 3D 76 34 2E 36 2E 31 01   // ,Version=v4.6.1.
                                                                                                        00 54 0E 14 46 72 61 6D 65 77 6F 72 6B 44 69 73   // .T..FrameworkDis
                                                                                                        70 6C 61 79 4E 61 6D 65 14 2E 4E 45 54 20 46 72   // playName..NET Fr
                                                                                                        61 6D 65 77 6F 72 6B 20 34 2E 36 2E 31 )          // amework 4.6.1
  .hash algorithm 0x00008004
  .ver 1:0:0:0
}
.mresource public HuaweiThpService.ProjectInstaller.resources
{
  // Offset: 0x00000000 Length: 0x000000B4
}
.mresource public HuaweiThpService.HuaweiThpService.resources
{
  // Offset: 0x000000B8 Length: 0x000000B4
}
.module HuaweiThpService.exe
// MVID: {35C2CC14-F6F4-4522-96F2-76B991423EAC}
.imagebase 0x0000000140000000
.file alignment 0x00000200
.stackreserve 0x0000000000400000
.subsystem 0x0002       // WINDOWS_GUI
.corflags 0x00000001    //  ILONLY
// Image base: 0x0F230000


// =============== CLASS MEMBERS DECLARATION ===================

.class public auto ansi beforefieldinit HuaweiThpService.ProjectInstaller
       extends [System.Configuration.Install]System.Configuration.Install.Installer
{
  .custom instance void [System]System.ComponentModel.RunInstallerAttribute::.ctor(bool) = ( 01 00 01 00 00 ) 
  .field private class [System]System.ComponentModel.IContainer components
  .field private class [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller serviceProcessInstaller1
  .field private class [System.ServiceProcess]System.ServiceProcess.ServiceInstaller serviceInstaller1
  .method public hidebysig specialname rtspecialname 
          instance void  .ctor() cil managed
  {
    // 代码大小       13 (0xd)
    .maxstack  8
    IL_0000:  ldarg.0
    IL_0001:  call       instance void [System.Configuration.Install]System.Configuration.Install.Installer::.ctor()
    IL_0006:  ldarg.0
    IL_0007:  call       instance void HuaweiThpService.ProjectInstaller::InitializeComponent()
    IL_000c:  ret
  } // end of method ProjectInstaller::.ctor

  .method public hidebysig virtual instance void 
          Commit(class [mscorlib]System.Collections.IDictionary savedState) cil managed
  {
    // 代码大小       53 (0x35)
    .maxstack  2
    .locals init (class [System.ServiceProcess]System.ServiceProcess.ServiceController V_0,
             valuetype [System.ServiceProcess]System.ServiceProcess.ServiceControllerStatus V_1)
    IL_0000:  ldarg.0
    IL_0001:  ldarg.1
    IL_0002:  call       instance void [System.Configuration.Install]System.Configuration.Install.Installer::Commit(class [mscorlib]System.Collections.IDictionary)
    IL_0007:  ldstr      "HuaweiThpService"
    IL_000c:  newobj     instance void [System.ServiceProcess]System.ServiceProcess.ServiceController::.ctor(string)
    IL_0011:  stloc.0
    IL_0012:  ldloc.0
    IL_0013:  callvirt   instance valuetype [System.ServiceProcess]System.ServiceProcess.ServiceControllerStatus [System.ServiceProcess]System.ServiceProcess.ServiceController::get_Status()
    IL_0018:  stloc.1
    IL_0019:  ldloca.s   V_1
    IL_001b:  ldc.i4.1
    IL_001c:  box        [System.ServiceProcess]System.ServiceProcess.ServiceControllerStatus
    IL_0021:  constrained. [System.ServiceProcess]System.ServiceProcess.ServiceControllerStatus
    IL_0027:  callvirt   instance bool [mscorlib]System.Object::Equals(object)
    IL_002c:  brfalse.s  IL_0034

    IL_002e:  ldloc.0
    IL_002f:  callvirt   instance void [System.ServiceProcess]System.ServiceProcess.ServiceController::Start()
    IL_0034:  ret
  } // end of method ProjectInstaller::Commit

  .method family hidebysig virtual instance void 
          Dispose(bool disposing) cil managed
  {
    // 代码大小       30 (0x1e)
    .maxstack  8
    IL_0000:  ldarg.1
    IL_0001:  brfalse.s  IL_0016

    IL_0003:  ldarg.0
    IL_0004:  ldfld      class [System]System.ComponentModel.IContainer HuaweiThpService.ProjectInstaller::components
    IL_0009:  brfalse.s  IL_0016

    IL_000b:  ldarg.0
    IL_000c:  ldfld      class [System]System.ComponentModel.IContainer HuaweiThpService.ProjectInstaller::components
    IL_0011:  callvirt   instance void [mscorlib]System.IDisposable::Dispose()
    IL_0016:  ldarg.0
    IL_0017:  ldarg.1
    IL_0018:  call       instance void [System]System.ComponentModel.Component::Dispose(bool)
    IL_001d:  ret
  } // end of method ProjectInstaller::Dispose

  .method private hidebysig instance void 
          InitializeComponent() cil managed
  {
    // 代码大小       122 (0x7a)
    .maxstack  5
    IL_0000:  ldarg.0
    IL_0001:  newobj     instance void [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller::.ctor()
    IL_0006:  stfld      class [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller HuaweiThpService.ProjectInstaller::serviceProcessInstaller1
    IL_000b:  ldarg.0
    IL_000c:  newobj     instance void [System.ServiceProcess]System.ServiceProcess.ServiceInstaller::.ctor()
    IL_0011:  stfld      class [System.ServiceProcess]System.ServiceProcess.ServiceInstaller HuaweiThpService.ProjectInstaller::serviceInstaller1
    IL_0016:  ldarg.0
    IL_0017:  ldfld      class [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller HuaweiThpService.ProjectInstaller::serviceProcessInstaller1
    IL_001c:  ldc.i4.2
    IL_001d:  callvirt   instance void [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller::set_Account(valuetype [System.ServiceProcess]System.ServiceProcess.ServiceAccount)
    IL_0022:  ldarg.0
    IL_0023:  ldfld      class [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller HuaweiThpService.ProjectInstaller::serviceProcessInstaller1
    IL_0028:  ldnull
    IL_0029:  callvirt   instance void [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller::set_Password(string)
    IL_002e:  ldarg.0
    IL_002f:  ldfld      class [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller HuaweiThpService.ProjectInstaller::serviceProcessInstaller1
    IL_0034:  ldnull
    IL_0035:  callvirt   instance void [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller::set_Username(string)
    IL_003a:  ldarg.0
    IL_003b:  ldfld      class [System.ServiceProcess]System.ServiceProcess.ServiceInstaller HuaweiThpService.ProjectInstaller::serviceInstaller1
    IL_0040:  ldstr      "HuaweiThpService"
    IL_0045:  callvirt   instance void [System.ServiceProcess]System.ServiceProcess.ServiceInstaller::set_ServiceName(string)
    IL_004a:  ldarg.0
    IL_004b:  ldfld      class [System.ServiceProcess]System.ServiceProcess.ServiceInstaller HuaweiThpService.ProjectInstaller::serviceInstaller1
    IL_0050:  ldc.i4.2
    IL_0051:  callvirt   instance void [System.ServiceProcess]System.ServiceProcess.ServiceInstaller::set_StartType(valuetype [System.ServiceProcess]System.ServiceProcess.ServiceStartMode)
    IL_0056:  ldarg.0
    IL_0057:  call       instance class [System.Configuration.Install]System.Configuration.Install.InstallerCollection [System.Configuration.Install]System.Configuration.Install.Installer::get_Installers()
    IL_005c:  ldc.i4.2
    IL_005d:  newarr     [System.Configuration.Install]System.Configuration.Install.Installer
    IL_0062:  dup
    IL_0063:  ldc.i4.0
    IL_0064:  ldarg.0
    IL_0065:  ldfld      class [System.ServiceProcess]System.ServiceProcess.ServiceProcessInstaller HuaweiThpService.ProjectInstaller::serviceProcessInstaller1
    IL_006a:  stelem.ref
    IL_006b:  dup
    IL_006c:  ldc.i4.1
    IL_006d:  ldarg.0
    IL_006e:  ldfld      class [System.ServiceProcess]System.ServiceProcess.ServiceInstaller HuaweiThpService.ProjectInstaller::serviceInstaller1
    IL_0073:  stelem.ref
    IL_0074:  callvirt   instance void [System.Configuration.Install]System.Configuration.Install.InstallerCollection::AddRange(class [System.Configuration.Install]System.Configuration.Install.Installer[])
    IL_0079:  ret
  } // end of method ProjectInstaller::InitializeComponent

} // end of class HuaweiThpService.ProjectInstaller

.class public auto ansi sealed HuaweiThpService.CallBackFunc
       extends [mscorlib]System.MulticastDelegate
{
  .custom instance void [mscorlib]System.Runtime.InteropServices.UnmanagedFunctionPointerAttribute::.ctor(valuetype [mscorlib]System.Runtime.InteropServices.CallingConvention) = ( 01 00 02 00 00 00 00 00 ) 
  .method public hidebysig specialname rtspecialname 
          instance void  .ctor(object 'object',
                               native int 'method') runtime managed
  {
  } // end of method CallBackFunc::.ctor

  .method public hidebysig newslot virtual 
          instance int32  Invoke(int32 arg) runtime managed
  {
  } // end of method CallBackFunc::Invoke

  .method public hidebysig newslot virtual 
          instance class [mscorlib]System.IAsyncResult 
          BeginInvoke(int32 arg,
                      class [mscorlib]System.AsyncCallback callback,
                      object 'object') runtime managed
  {
  } // end of method CallBackFunc::BeginInvoke

  .method public hidebysig newslot virtual 
          instance int32  EndInvoke(class [mscorlib]System.IAsyncResult result) runtime managed
  {
  } // end of method CallBackFunc::EndInvoke

} // end of class HuaweiThpService.CallBackFunc

.class public auto ansi beforefieldinit HuaweiThpService.HuaweiThpService
       extends [System.ServiceProcess]System.ServiceProcess.ServiceBase
{
  .class abstract auto ansi sealed nested assembly beforefieldinit SafeNativeMethods
         extends [mscorlib]System.Object
  {
    .custom instance void [mscorlib]System.Security.SuppressUnmanagedCodeSecurityAttribute::.ctor() = ( 01 00 00 00 ) 
    .method assembly hidebysig static pinvokeimpl("THP_Service.dll" cdecl) 
            void  RegisterPrintEventLog(class HuaweiThpService.CallBackFunc func) cil managed preservesig
    {
    }
    .method assembly hidebysig static pinvokeimpl("THP_Service.dll" cdecl bestfit:off charmaperror:on) 
            void  GetMESSAGE(class [mscorlib]System.Text.StringBuilder  marshal( lpstr) sb,
                             int32& sbLength) cil managed preservesig
    {
    }
    .method assembly hidebysig static pinvokeimpl("THP_Service.dll" cdecl) 
            void  RegisterEventLogStatus(class HuaweiThpService.CallBackFunc func) cil managed preservesig
    {
    }
    .method assembly hidebysig static pinvokeimpl("THP_Service.dll" cdecl) 
            void  RegisterGetPenEleValue(class HuaweiThpService.CallBackFunc func) cil managed preservesig
    {
    }
    .method assembly hidebysig static pinvokeimpl("THP_Service.dll" cdecl) 
            void  RegisterSetPenEleValue(class HuaweiThpService.CallBackFunc func) cil managed preservesig
    {
    }
  } // end of class SafeNativeMethods

  .field private static class HuaweiThpService.CallBackFunc printEventLog
  .field private static int32 eventStatus
  .field private static class HuaweiThpService.CallBackFunc eventLogStatus
  .field private static class HuaweiThpService.CallBackFunc setPenEleValue
  .field private static class HuaweiThpService.CallBackFunc getPenEleValue
  .field private static literal int32 MESSAGE_LENGTH = int32(0x00000040)
  .field public static class [System]System.Diagnostics.EventLog eventLog
  .field private class [System]System.Diagnostics.EventLog log
  .field private static literal int32 SERVICE_CONTROL_PRESHUTDOWN = int32(0x0000000F)
  .field private class [System]System.ComponentModel.IContainer components
  .field private class [System]System.Diagnostics.EventLog eventLog1
  .method private hidebysig static pinvokeimpl("THP_Service.dll" cdecl) 
          int32  ThpFuncStart() cil managed preservesig
  {
  }
  .method private hidebysig static pinvokeimpl("THP_Service.dll" cdecl) 
          int32  ThpFuncStop() cil managed preservesig
  {
  }
  .method public hidebysig specialname rtspecialname 
          instance void  .ctor() cil managed
  {
    // 代码大小       118 (0x76)
    .maxstack  2
    IL_0000:  ldarg.0
    IL_0001:  ldstr      "System"
    IL_0006:  newobj     instance void [System]System.Diagnostics.EventLog::.ctor(string)
    IL_000b:  stfld      class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::log
    IL_0010:  ldarg.0
    IL_0011:  call       instance void [System.ServiceProcess]System.ServiceProcess.ServiceBase::.ctor()
    IL_0016:  ldarg.0
    IL_0017:  call       instance void HuaweiThpService.HuaweiThpService::InitializeComponent()
    IL_001c:  newobj     instance void [System]System.Diagnostics.EventLog::.ctor()
    IL_0021:  stsfld     class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::eventLog
    IL_0026:  ldstr      "THPEvent"
    IL_002b:  call       bool [System]System.Diagnostics.EventLog::SourceExists(string)
    IL_0030:  brtrue.s   IL_0041

    IL_0032:  ldstr      "THPEvent"
    IL_0037:  ldstr      "THPLog"
    IL_003c:  call       void [System]System.Diagnostics.EventLog::CreateEventSource(string,
                                                                                     string)
    IL_0041:  ldsfld     class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::eventLog
    IL_0046:  ldstr      "THPEvent"
    IL_004b:  callvirt   instance void [System]System.Diagnostics.EventLog::set_Source(string)
    IL_0050:  ldsfld     class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::eventLog
    IL_0055:  ldstr      "THPLog"
    IL_005a:  callvirt   instance void [System]System.Diagnostics.EventLog::set_Log(string)
    IL_005f:  ldarg.0
    IL_0060:  ldc.i4.1
    IL_0061:  call       instance void [System.ServiceProcess]System.ServiceProcess.ServiceBase::set_CanHandlePowerEvent(bool)
    IL_0066:  call       class [System]System.Diagnostics.Process [System]System.Diagnostics.Process::GetCurrentProcess()
    IL_006b:  ldc.i4     0x100
    IL_0070:  callvirt   instance void [System]System.Diagnostics.Process::set_PriorityClass(valuetype [System]System.Diagnostics.ProcessPriorityClass)
    IL_0075:  ret
  } // end of method HuaweiThpService::.ctor

  .method family hidebysig virtual instance void 
          OnStart(string[] args) cil managed
  {
    // 代码大小       12 (0xc)
    .maxstack  8
    IL_0000:  call       void HuaweiThpService.HuaweiThpService::FunctionRegister()
    IL_0005:  call       int32 HuaweiThpService.HuaweiThpService::ThpFuncStart()
    IL_000a:  pop
    IL_000b:  ret
  } // end of method HuaweiThpService::OnStart

  .method family hidebysig virtual instance void 
          OnStop() cil managed
  {
    // 代码大小       7 (0x7)
    .maxstack  8
    IL_0000:  call       int32 HuaweiThpService.HuaweiThpService::ThpFuncStop()
    IL_0005:  pop
    IL_0006:  ret
  } // end of method HuaweiThpService::OnStop

  .method family hidebysig virtual instance void 
          OnCustomCommand(int32 command) cil managed
  {
    // 代码大小       13 (0xd)
    .maxstack  8
    IL_0000:  ldarg.1
    IL_0001:  ldc.i4.s   15
    IL_0003:  beq.s      IL_000c

    IL_0005:  ldarg.0
    IL_0006:  ldarg.1
    IL_0007:  call       instance void [System.ServiceProcess]System.ServiceProcess.ServiceBase::OnCustomCommand(int32)
    IL_000c:  ret
  } // end of method HuaweiThpService::OnCustomCommand

  .method private hidebysig static int32 
          PrintEventLog(int32 arg) cil managed
  {
    // 代码大小       18 (0x12)
    .maxstack  2
    .locals init (int32 V_0)
    IL_0000:  ldc.i4.s   64
    IL_0002:  stloc.0
    IL_0003:  ldloc.0
    IL_0004:  newobj     instance void [mscorlib]System.Text.StringBuilder::.ctor(int32)
    IL_0009:  ldloca.s   V_0
    IL_000b:  call       void HuaweiThpService.HuaweiThpService/SafeNativeMethods::GetMESSAGE(class [mscorlib]System.Text.StringBuilder,
                                                                                              int32&)
    IL_0010:  ldc.i4.0
    IL_0011:  ret
  } // end of method HuaweiThpService::PrintEventLog

  .method private hidebysig static void  FunctionRegister() cil managed
  {
    // 代码大小       117 (0x75)
    .maxstack  2
    IL_0000:  ldnull
    IL_0001:  ldftn      int32 HuaweiThpService.HuaweiThpService::PrintEventLog(int32)
    IL_0007:  newobj     instance void HuaweiThpService.CallBackFunc::.ctor(object,
                                                                            native int)
    IL_000c:  stsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::printEventLog
    IL_0011:  ldsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::printEventLog
    IL_0016:  call       void HuaweiThpService.HuaweiThpService/SafeNativeMethods::RegisterPrintEventLog(class HuaweiThpService.CallBackFunc)
    IL_001b:  ldnull
    IL_001c:  ldftn      int32 HuaweiThpService.HuaweiThpService::EventLogStatus(int32)
    IL_0022:  newobj     instance void HuaweiThpService.CallBackFunc::.ctor(object,
                                                                            native int)
    IL_0027:  stsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::eventLogStatus
    IL_002c:  ldsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::eventLogStatus
    IL_0031:  call       void HuaweiThpService.HuaweiThpService/SafeNativeMethods::RegisterEventLogStatus(class HuaweiThpService.CallBackFunc)
    IL_0036:  call       class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::get_Instance()
    IL_003b:  ldftn      instance int32 HuaweiThpService.XmlOperator::SetPenEleValue(int32)
    IL_0041:  newobj     instance void HuaweiThpService.CallBackFunc::.ctor(object,
                                                                            native int)
    IL_0046:  stsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::setPenEleValue
    IL_004b:  ldsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::setPenEleValue
    IL_0050:  call       void HuaweiThpService.HuaweiThpService/SafeNativeMethods::RegisterSetPenEleValue(class HuaweiThpService.CallBackFunc)
    IL_0055:  call       class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::get_Instance()
    IL_005a:  ldftn      instance int32 HuaweiThpService.XmlOperator::GetPenEleValue(int32)
    IL_0060:  newobj     instance void HuaweiThpService.CallBackFunc::.ctor(object,
                                                                            native int)
    IL_0065:  stsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::getPenEleValue
    IL_006a:  ldsfld     class HuaweiThpService.CallBackFunc HuaweiThpService.HuaweiThpService::getPenEleValue
    IL_006f:  call       void HuaweiThpService.HuaweiThpService/SafeNativeMethods::RegisterGetPenEleValue(class HuaweiThpService.CallBackFunc)
    IL_0074:  ret
  } // end of method HuaweiThpService::FunctionRegister

  .method private hidebysig static int32 
          EventLogStatus(int32 arg) cil managed
  {
    // 代码大小       6 (0x6)
    .maxstack  8
    IL_0000:  ldsfld     int32 HuaweiThpService.HuaweiThpService::eventStatus
    IL_0005:  ret
  } // end of method HuaweiThpService::EventLogStatus

  .method public hidebysig static class [System]System.Diagnostics.EventLog 
          GetEventLog() cil managed
  {
    // 代码大小       6 (0x6)
    .maxstack  8
    IL_0000:  ldsfld     class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::eventLog
    IL_0005:  ret
  } // end of method HuaweiThpService::GetEventLog

  .method family hidebysig virtual instance void 
          Dispose(bool disposing) cil managed
  {
    // 代码大小       30 (0x1e)
    .maxstack  8
    IL_0000:  ldarg.1
    IL_0001:  brfalse.s  IL_0016

    IL_0003:  ldarg.0
    IL_0004:  ldfld      class [System]System.ComponentModel.IContainer HuaweiThpService.HuaweiThpService::components
    IL_0009:  brfalse.s  IL_0016

    IL_000b:  ldarg.0
    IL_000c:  ldfld      class [System]System.ComponentModel.IContainer HuaweiThpService.HuaweiThpService::components
    IL_0011:  callvirt   instance void [mscorlib]System.IDisposable::Dispose()
    IL_0016:  ldarg.0
    IL_0017:  ldarg.1
    IL_0018:  call       instance void [System.ServiceProcess]System.ServiceProcess.ServiceBase::Dispose(bool)
    IL_001d:  ret
  } // end of method HuaweiThpService::Dispose

  .method private hidebysig instance void 
          InitializeComponent() cil managed
  {
    // 代码大小       45 (0x2d)
    .maxstack  8
    IL_0000:  ldarg.0
    IL_0001:  newobj     instance void [System]System.Diagnostics.EventLog::.ctor()
    IL_0006:  stfld      class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::eventLog1
    IL_000b:  ldarg.0
    IL_000c:  ldfld      class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::eventLog1
    IL_0011:  callvirt   instance void [System]System.ComponentModel.ISupportInitialize::BeginInit()
    IL_0016:  ldarg.0
    IL_0017:  ldstr      "HUAWEIThpService"
    IL_001c:  call       instance void [System.ServiceProcess]System.ServiceProcess.ServiceBase::set_ServiceName(string)
    IL_0021:  ldarg.0
    IL_0022:  ldfld      class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::eventLog1
    IL_0027:  callvirt   instance void [System]System.ComponentModel.ISupportInitialize::EndInit()
    IL_002c:  ret
  } // end of method HuaweiThpService::InitializeComponent

  .method private hidebysig specialname rtspecialname static 
          void  .cctor() cil managed
  {
    // 代码大小       1 (0x1)
    .maxstack  8
    IL_0000:  ret
  } // end of method HuaweiThpService::.cctor

} // end of class HuaweiThpService.HuaweiThpService

.class private abstract auto ansi sealed beforefieldinit HuaweiThpService.Program
       extends [mscorlib]System.Object
{
  .method private hidebysig static void  Main() cil managed
  {
    .entrypoint
    // 代码大小       20 (0x14)
    .maxstack  8
    IL_0000:  ldc.i4.1
    IL_0001:  newarr     [System.ServiceProcess]System.ServiceProcess.ServiceBase
    IL_0006:  dup
    IL_0007:  ldc.i4.0
    IL_0008:  newobj     instance void HuaweiThpService.HuaweiThpService::.ctor()
    IL_000d:  stelem.ref
    IL_000e:  call       void [System.ServiceProcess]System.ServiceProcess.ServiceBase::Run(class [System.ServiceProcess]System.ServiceProcess.ServiceBase[])
    IL_0013:  ret
  } // end of method Program::Main

} // end of class HuaweiThpService.Program

.class private auto ansi beforefieldinit HuaweiThpService.XmlOperator
       extends [mscorlib]System.Object
{
  .field private static string xmlFile
  .field private static class HuaweiThpService.XmlOperator 'instance'
  .field private static initonly object padlock
  .method private hidebysig specialname rtspecialname 
          instance void  .ctor() cil managed
  {
    // 代码大小       29 (0x1d)
    .maxstack  8
    IL_0000:  ldarg.0
    IL_0001:  call       instance void [mscorlib]System.Object::.ctor()
    IL_0006:  call       class [mscorlib]System.Security.Principal.WindowsIdentity [mscorlib]System.Security.Principal.WindowsIdentity::GetCurrent()
    IL_000b:  pop
    IL_000c:  ldstr      "C:\\Program Files\\Huawei\\HuaweiThpService\\HuaweiTHP"
    + ".config.xml"
    IL_0011:  stsfld     string HuaweiThpService.XmlOperator::xmlFile
    IL_0016:  ldarg.0
    IL_0017:  call       instance void HuaweiThpService.XmlOperator::CreateXmlFile()
    IL_001c:  ret
  } // end of method XmlOperator::.ctor

  .method public hidebysig specialname static 
          class HuaweiThpService.XmlOperator 
          get_Instance() cil managed
  {
    // 代码大小       58 (0x3a)
    .maxstack  2
    .locals init (object V_0,
             bool V_1)
    IL_0000:  ldsfld     class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::'instance'
    IL_0005:  brtrue.s   IL_0034

    IL_0007:  ldsfld     object HuaweiThpService.XmlOperator::padlock
    IL_000c:  stloc.0
    IL_000d:  ldc.i4.0
    IL_000e:  stloc.1
    .try
    {
      IL_000f:  ldloc.0
      IL_0010:  ldloca.s   V_1
      IL_0012:  call       void [mscorlib]System.Threading.Monitor::Enter(object,
                                                                          bool&)
      IL_0017:  ldsfld     class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::'instance'
      IL_001c:  brtrue.s   IL_0028

      IL_001e:  newobj     instance void HuaweiThpService.XmlOperator::.ctor()
      IL_0023:  stsfld     class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::'instance'
      IL_0028:  leave.s    IL_0034

    }  // end .try
    finally
    {
      IL_002a:  ldloc.1
      IL_002b:  brfalse.s  IL_0033

      IL_002d:  ldloc.0
      IL_002e:  call       void [mscorlib]System.Threading.Monitor::Exit(object)
      IL_0033:  endfinally
    }  // end handler
    IL_0034:  ldsfld     class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::'instance'
    IL_0039:  ret
  } // end of method XmlOperator::get_Instance

  .method public hidebysig instance void 
          CreateXmlFile() cil managed
  {
    // 代码大小       161 (0xa1)
    .maxstack  5
    .locals init (class [System.Xml]System.Xml.XmlDeclaration V_0,
             class [System.Xml]System.Xml.XmlElement V_1,
             class [System.Xml]System.Xml.XmlElement V_2,
             class [System.Xml]System.Xml.XmlElement V_3)
    .try
    {
      IL_0000:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
      IL_0005:  call       bool [mscorlib]System.IO.File::Exists(string)
      IL_000a:  brfalse.s  IL_0011

      IL_000c:  leave      IL_00a0

      IL_0011:  newobj     instance void [System.Xml]System.Xml.XmlDocument::.ctor()
      IL_0016:  dup
      IL_0017:  ldstr      "1.0"
      IL_001c:  ldstr      "UTF-8"
      IL_0021:  ldnull
      IL_0022:  callvirt   instance class [System.Xml]System.Xml.XmlDeclaration [System.Xml]System.Xml.XmlDocument::CreateXmlDeclaration(string,
                                                                                                                                         string,
                                                                                                                                         string)
      IL_0027:  stloc.0
      IL_0028:  dup
      IL_0029:  ldloc.0
      IL_002a:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::AppendChild(class [System.Xml]System.Xml.XmlNode)
      IL_002f:  pop
      IL_0030:  dup
      IL_0031:  ldstr      "Config"
      IL_0036:  callvirt   instance class [System.Xml]System.Xml.XmlElement [System.Xml]System.Xml.XmlDocument::CreateElement(string)
      IL_003b:  stloc.1
      IL_003c:  dup
      IL_003d:  ldloc.1
      IL_003e:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::AppendChild(class [System.Xml]System.Xml.XmlNode)
      IL_0043:  pop
      IL_0044:  dup
      IL_0045:  ldstr      "VHFFunction"
      IL_004a:  callvirt   instance class [System.Xml]System.Xml.XmlElement [System.Xml]System.Xml.XmlDocument::CreateElement(string)
      IL_004f:  stloc.2
      IL_0050:  ldloc.2
      IL_0051:  ldstr      "0"
      IL_0056:  callvirt   instance void [System.Xml]System.Xml.XmlNode::set_InnerText(string)
      IL_005b:  dup
      IL_005c:  ldstr      "LogFunction"
      IL_0061:  callvirt   instance class [System.Xml]System.Xml.XmlElement [System.Xml]System.Xml.XmlDocument::CreateElement(string)
      IL_0066:  stloc.3
      IL_0067:  ldloc.3
      IL_0068:  ldstr      "0"
      IL_006d:  callvirt   instance void [System.Xml]System.Xml.XmlNode::set_InnerText(string)
      IL_0072:  ldloc.1
      IL_0073:  ldloc.2
      IL_0074:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::AppendChild(class [System.Xml]System.Xml.XmlNode)
      IL_0079:  pop
      IL_007a:  ldloc.1
      IL_007b:  ldloc.3
      IL_007c:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::AppendChild(class [System.Xml]System.Xml.XmlNode)
      IL_0081:  pop
      IL_0082:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
      IL_0087:  callvirt   instance void [System.Xml]System.Xml.XmlDocument::Save(string)
      IL_008c:  leave.s    IL_00a0

    }  // end .try
    catch [mscorlib]System.Object 
    {
      IL_008e:  pop
      IL_008f:  call       class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::GetEventLog()
      IL_0094:  ldstr      "[XmlOperator] Exception in CreateXmlFile"
      IL_0099:  callvirt   instance void [System]System.Diagnostics.EventLog::WriteEntry(string)
      IL_009e:  leave.s    IL_00a0

    }  // end handler
    IL_00a0:  ret
  } // end of method XmlOperator::CreateXmlFile

  .method private hidebysig instance bool 
          IsXml(string Path) cil managed
  {
    // 代码大小       81 (0x51)
    .maxstack  4
    .locals init (string V_0,
             bool V_1)
    .try
    {
      IL_0000:  ldarg.1
      IL_0001:  ldc.i4.3
      IL_0002:  ldc.i4.1
      IL_0003:  ldc.i4.3
      IL_0004:  newobj     instance void [mscorlib]System.IO.FileStream::.ctor(string,
                                                                               valuetype [mscorlib]System.IO.FileMode,
                                                                               valuetype [mscorlib]System.IO.FileAccess,
                                                                               valuetype [mscorlib]System.IO.FileShare)
      IL_0009:  dup
      IL_000a:  newobj     instance void [mscorlib]System.IO.StreamReader::.ctor(class [mscorlib]System.IO.Stream)
      IL_000f:  dup
      IL_0010:  callvirt   instance string [mscorlib]System.IO.TextReader::ReadToEnd()
      IL_0015:  stloc.0
      IL_0016:  dup
      IL_0017:  callvirt   instance void [mscorlib]System.IO.TextReader::Close()
      IL_001c:  callvirt   instance void [mscorlib]System.IO.TextReader::Dispose()
      IL_0021:  dup
      IL_0022:  callvirt   instance void [mscorlib]System.IO.Stream::Close()
      IL_0027:  callvirt   instance void [mscorlib]System.IO.Stream::Dispose()
      IL_002c:  newobj     instance void [System.Xml]System.Xml.XmlDocument::.ctor()
      IL_0031:  ldloc.0
      IL_0032:  callvirt   instance void [System.Xml]System.Xml.XmlDocument::LoadXml(string)
      IL_0037:  ldc.i4.1
      IL_0038:  stloc.1
      IL_0039:  leave.s    IL_004f

    }  // end .try
    catch [mscorlib]System.Object 
    {
      IL_003b:  pop
      IL_003c:  call       class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::GetEventLog()
      IL_0041:  ldstr      "[XmlOperator] Exception in IsXml"
      IL_0046:  callvirt   instance void [System]System.Diagnostics.EventLog::WriteEntry(string)
      IL_004b:  ldc.i4.0
      IL_004c:  stloc.1
      IL_004d:  leave.s    IL_004f

    }  // end handler
    IL_004f:  ldloc.1
    IL_0050:  ret
  } // end of method XmlOperator::IsXml

  .method public hidebysig instance int32 
          GetPenEleValue(int32 a) cil managed
  {
    // 代码大小       279 (0x117)
    .maxstack  2
    .locals init (int32 V_0,
             object V_1,
             bool V_2,
             class [System.Xml]System.Xml.XmlDocument V_3,
             class [System.Xml]System.Xml.XmlNode V_4,
             int32 V_5,
             class [System.Xml]System.Xml.XmlNode V_6,
             class [System.Xml]System.Xml.XmlNode V_7)
    IL_0000:  ldc.i4.0
    IL_0001:  stloc.0
    IL_0002:  ldsfld     object HuaweiThpService.XmlOperator::padlock
    IL_0007:  stloc.1
    IL_0008:  ldc.i4.0
    IL_0009:  stloc.2
    .try
    {
      IL_000a:  ldloc.1
      IL_000b:  ldloca.s   V_2
      IL_000d:  call       void [mscorlib]System.Threading.Monitor::Enter(object,
                                                                          bool&)
      .try
      {
        IL_0012:  newobj     instance void [System.Xml]System.Xml.XmlDocument::.ctor()
        IL_0017:  stloc.3
        IL_0018:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_001d:  call       bool [mscorlib]System.IO.File::Exists(string)
        IL_0022:  brtrue.s   IL_002a

        IL_0024:  ldarg.0
        IL_0025:  call       instance void HuaweiThpService.XmlOperator::CreateXmlFile()
        IL_002a:  ldarg.0
        IL_002b:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_0030:  call       instance bool HuaweiThpService.XmlOperator::IsXml(string)
        IL_0035:  brfalse.s  IL_0044

        IL_0037:  ldloc.3
        IL_0038:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_003d:  callvirt   instance void [System.Xml]System.Xml.XmlDocument::Load(string)
        IL_0042:  br.s       IL_0056

        IL_0044:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_0049:  call       void [mscorlib]System.IO.File::Delete(string)
        IL_004e:  ldloc.0
        IL_004f:  stloc.s    V_5
        IL_0051:  leave      IL_0114

        IL_0056:  ldloc.3
        IL_0057:  brtrue.s   IL_0061

        IL_0059:  ldloc.0
        IL_005a:  stloc.s    V_5
        IL_005c:  leave      IL_0114

        IL_0061:  ldloc.3
        IL_0062:  ldstr      "Config"
        IL_0067:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::SelectSingleNode(string)
        IL_006c:  stloc.s    V_4
        IL_006e:  ldloc.s    V_4
        IL_0070:  brtrue.s   IL_007a

        IL_0072:  ldloc.0
        IL_0073:  stloc.s    V_5
        IL_0075:  leave      IL_0114

        IL_007a:  ldarg.1
        IL_007b:  brtrue.s   IL_00b6

        IL_007d:  ldloc.s    V_4
        IL_007f:  ldstr      "VHFFunction"
        IL_0084:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::SelectSingleNode(string)
        IL_0089:  stloc.s    V_6
        IL_008b:  ldloc.s    V_6
        IL_008d:  brfalse.s  IL_00a2

        IL_008f:  ldloc.s    V_6
        IL_0091:  callvirt   instance string [System.Xml]System.Xml.XmlNode::get_InnerText()
        IL_0096:  ldstr      ""
        IL_009b:  callvirt   instance bool [mscorlib]System.String::Equals(string)
        IL_00a0:  brfalse.s  IL_00a7

        IL_00a2:  ldloc.0
        IL_00a3:  stloc.s    V_5
        IL_00a5:  leave.s    IL_0114

        IL_00a7:  ldloc.s    V_6
        IL_00a9:  callvirt   instance string [System.Xml]System.Xml.XmlNode::get_InnerText()
        IL_00ae:  call       int32 [mscorlib]System.Int32::Parse(string)
        IL_00b3:  stloc.0
        IL_00b4:  br.s       IL_00f1

        IL_00b6:  ldarg.1
        IL_00b7:  ldc.i4.1
        IL_00b8:  bne.un.s   IL_00f1

        IL_00ba:  ldloc.s    V_4
        IL_00bc:  ldstr      "LogFunction"
        IL_00c1:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::SelectSingleNode(string)
        IL_00c6:  stloc.s    V_7
        IL_00c8:  ldloc.s    V_7
        IL_00ca:  brfalse.s  IL_00df

        IL_00cc:  ldloc.s    V_7
        IL_00ce:  callvirt   instance string [System.Xml]System.Xml.XmlNode::get_InnerText()
        IL_00d3:  ldstr      ""
        IL_00d8:  callvirt   instance bool [mscorlib]System.String::Equals(string)
        IL_00dd:  brfalse.s  IL_00e4

        IL_00df:  ldloc.0
        IL_00e0:  stloc.s    V_5
        IL_00e2:  leave.s    IL_0114

        IL_00e4:  ldloc.s    V_7
        IL_00e6:  callvirt   instance string [System.Xml]System.Xml.XmlNode::get_InnerText()
        IL_00eb:  call       int32 [mscorlib]System.Int32::Parse(string)
        IL_00f0:  stloc.0
        IL_00f1:  leave.s    IL_0112

      }  // end .try
      catch [mscorlib]System.Object 
      {
        IL_00f3:  pop
        IL_00f4:  call       class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::GetEventLog()
        IL_00f9:  ldstr      "[XmlOperator] Exception in GetPenEleValue"
        IL_00fe:  callvirt   instance void [System]System.Diagnostics.EventLog::WriteEntry(string)
        IL_0103:  ldloc.0
        IL_0104:  stloc.s    V_5
        IL_0106:  leave.s    IL_0114

      }  // end handler
    }  // end .try
    finally
    {
      IL_0108:  ldloc.2
      IL_0109:  brfalse.s  IL_0111

      IL_010b:  ldloc.1
      IL_010c:  call       void [mscorlib]System.Threading.Monitor::Exit(object)
      IL_0111:  endfinally
    }  // end handler
    IL_0112:  ldloc.0
    IL_0113:  ret

    IL_0114:  ldloc.s    V_5
    IL_0116:  ret
  } // end of method XmlOperator::GetPenEleValue

  .method public hidebysig instance int32 
          SetPenEleValue(int32 penFunc) cil managed
  {
    // 代码大小       280 (0x118)
    .maxstack  2
    .locals init (object V_0,
             bool V_1,
             class [System.Xml]System.Xml.XmlDocument V_2,
             class [System.Xml]System.Xml.XmlNode V_3,
             class [System.Xml]System.Xml.XmlNode V_4,
             int32 V_5)
    IL_0000:  ldsfld     object HuaweiThpService.XmlOperator::padlock
    IL_0005:  stloc.0
    IL_0006:  ldc.i4.0
    IL_0007:  stloc.1
    .try
    {
      IL_0008:  ldloc.0
      IL_0009:  ldloca.s   V_1
      IL_000b:  call       void [mscorlib]System.Threading.Monitor::Enter(object,
                                                                          bool&)
      .try
      {
        IL_0010:  newobj     instance void [System.Xml]System.Xml.XmlDocument::.ctor()
        IL_0015:  stloc.2
        IL_0016:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_001b:  call       bool [mscorlib]System.IO.File::Exists(string)
        IL_0020:  brtrue.s   IL_0028

        IL_0022:  ldarg.0
        IL_0023:  call       instance void HuaweiThpService.XmlOperator::CreateXmlFile()
        IL_0028:  ldarg.0
        IL_0029:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_002e:  call       instance bool HuaweiThpService.XmlOperator::IsXml(string)
        IL_0033:  brfalse.s  IL_0042

        IL_0035:  ldloc.2
        IL_0036:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_003b:  callvirt   instance void [System.Xml]System.Xml.XmlDocument::Load(string)
        IL_0040:  br.s       IL_0063

        IL_0042:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_0047:  ldc.i4     0x80
        IL_004c:  call       void [mscorlib]System.IO.File::SetAttributes(string,
                                                                          valuetype [mscorlib]System.IO.FileAttributes)
        IL_0051:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_0056:  call       void [mscorlib]System.IO.File::Delete(string)
        IL_005b:  ldc.i4.m1
        IL_005c:  stloc.s    V_5
        IL_005e:  leave      IL_0115

        IL_0063:  ldloc.2
        IL_0064:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
        IL_0069:  callvirt   instance void [System.Xml]System.Xml.XmlDocument::Load(string)
        IL_006e:  ldloc.2
        IL_006f:  brtrue.s   IL_0079

        IL_0071:  ldc.i4.m1
        IL_0072:  stloc.s    V_5
        IL_0074:  leave      IL_0115

        IL_0079:  ldloc.2
        IL_007a:  ldstr      "Config"
        IL_007f:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::SelectSingleNode(string)
        IL_0084:  stloc.3
        IL_0085:  ldloc.3
        IL_0086:  brtrue.s   IL_0090

        IL_0088:  ldc.i4.m1
        IL_0089:  stloc.s    V_5
        IL_008b:  leave      IL_0115

        IL_0090:  ldloc.3
        IL_0091:  ldstr      "VHFFunction"
        IL_0096:  callvirt   instance class [System.Xml]System.Xml.XmlNode [System.Xml]System.Xml.XmlNode::SelectSingleNode(string)
        IL_009b:  stloc.s    V_4
        IL_009d:  ldloc.s    V_4
        IL_009f:  brfalse.s  IL_00b4

        IL_00a1:  ldloc.s    V_4
        IL_00a3:  callvirt   instance string [System.Xml]System.Xml.XmlNode::get_InnerText()
        IL_00a8:  ldstr      ""
        IL_00ad:  callvirt   instance bool [mscorlib]System.String::Equals(string)
        IL_00b2:  brfalse.s  IL_00b9

        IL_00b4:  ldc.i4.m1
        IL_00b5:  stloc.s    V_5
        IL_00b7:  leave.s    IL_0115

        IL_00b9:  ldloc.s    V_4
        IL_00bb:  ldarga.s   penFunc
        IL_00bd:  call       instance string [mscorlib]System.Int32::ToString()
        IL_00c2:  callvirt   instance void [System.Xml]System.Xml.XmlNode::set_InnerText(string)
        .try
        {
          IL_00c7:  ldloc.2
          IL_00c8:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
          IL_00cd:  callvirt   instance void [System.Xml]System.Xml.XmlDocument::Save(string)
          IL_00d2:  ldc.i4.0
          IL_00d3:  stloc.s    V_5
          IL_00d5:  leave.s    IL_0115

        }  // end .try
        catch [mscorlib]System.Object 
        {
          IL_00d7:  pop
          IL_00d8:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
          IL_00dd:  ldc.i4     0x80
          IL_00e2:  call       void [mscorlib]System.IO.File::SetAttributes(string,
                                                                            valuetype [mscorlib]System.IO.FileAttributes)
          IL_00e7:  ldsfld     string HuaweiThpService.XmlOperator::xmlFile
          IL_00ec:  call       void [mscorlib]System.IO.File::Delete(string)
          IL_00f1:  ldc.i4.m1
          IL_00f2:  stloc.s    V_5
          IL_00f4:  leave.s    IL_0115

        }  // end handler
      }  // end .try
      catch [mscorlib]System.Object 
      {
        IL_00f6:  pop
        IL_00f7:  call       class [System]System.Diagnostics.EventLog HuaweiThpService.HuaweiThpService::GetEventLog()
        IL_00fc:  ldstr      "[XmlOperator] Exception in SetPenEleValue"
        IL_0101:  callvirt   instance void [System]System.Diagnostics.EventLog::WriteEntry(string)
        IL_0106:  ldc.i4.m1
        IL_0107:  stloc.s    V_5
        IL_0109:  leave.s    IL_0115

      }  // end handler
    }  // end .try
    finally
    {
      IL_010b:  ldloc.1
      IL_010c:  brfalse.s  IL_0114

      IL_010e:  ldloc.0
      IL_010f:  call       void [mscorlib]System.Threading.Monitor::Exit(object)
      IL_0114:  endfinally
    }  // end handler
    IL_0115:  ldloc.s    V_5
    IL_0117:  ret
  } // end of method XmlOperator::SetPenEleValue

  .method private hidebysig specialname rtspecialname static 
          void  .cctor() cil managed
  {
    // 代码大小       17 (0x11)
    .maxstack  8
    IL_0000:  ldnull
    IL_0001:  stsfld     class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::'instance'
    IL_0006:  newobj     instance void [mscorlib]System.Object::.ctor()
    IL_000b:  stsfld     object HuaweiThpService.XmlOperator::padlock
    IL_0010:  ret
  } // end of method XmlOperator::.cctor

  .property class HuaweiThpService.XmlOperator
          Instance()
  {
    .get class HuaweiThpService.XmlOperator HuaweiThpService.XmlOperator::get_Instance()
  } // end of property XmlOperator::Instance
} // end of class HuaweiThpService.XmlOperator


// =============================================================

// *********** 反汇编完成 ***********************
