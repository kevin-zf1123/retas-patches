using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace StylosInk
{
    public static class WinTab
    {
        private const int PacketMessage = 0x7ff0;
        private const int ContextMessages = 4;
        private const int SystemCursor = 1;
        private const int StylosFields = 0x15e2;
        private const int SupportedFields = 0x1fff;
        private const int PressureMaximum = 1024;
        private const int PointerUpdate = 0x245;
        private const int PointerDown = 0x246;
        private const int PointerUp = 0x247;
        private const int PointerEnter = 0x249;
        private const int PointerLeave = 0x24a;
        private const int PointerCaptureChanged = 0x24c;
        private const uint InContact = 0x4;
        private const uint Canceled = 0x8000;
        private const uint PenBarrel = 1;
        private const uint PenInverted = 2;
        private const uint PenEraser = 4;
        private const uint HasPressure = 1;
        private const int TabletGestureQuery = 0x2cc;
        private const int CanvasGestureFlags = 0x190001;
        private const int DestroyWindowMessage = 0x82;
        private const int CancelModeMessage = 0x1f;
        private const int MouseMove = 0x200;
        private const int LeftDown = 0x201;
        private const int LeftUp = 0x202;
        private const int RightDown = 0x204;
        private const int RightUp = 0x205;
        private const int LeftButton = 1;
        private const int RightButton = 2;
        private const uint BridgedMouseFlag = 0x40000000;
        private const string TabletProperty = "MicrosoftTabletPenServiceProperty";

        [StructLayout(LayoutKind.Sequential)]
        internal struct Point
        {
            public int X, Y;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Message
        {
            public IntPtr Window;
            public uint Id;
            public UIntPtr WParam;
            public IntPtr LParam;
            public uint Time;
            public Point Location;
            public uint Private;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PointerInfo
        {
            public uint Type, Id, FrameId, Flags;
            public IntPtr Source, Target;
            public Point Pixel, Himetric, PixelRaw, HimetricRaw;
            public uint Time, HistoryCount;
            public int InputData;
            public uint KeyStates;
            public ulong PerformanceCount;
            public uint ButtonChange;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PenInfo
        {
            public PointerInfo Info;
            public uint Flags, Mask, Pressure, Rotation;
            public int TiltX, TiltY;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        internal struct LogContext
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 40)]
            public string Name;
            public int Options, Status, Locks, MessageBase, Device, Rate;
            public int PacketData, PacketMode, MoveMask, ButtonDownMask, ButtonUpMask;
            public int InX, InY, InZ, InWidth, InHeight, InDepth;
            public int OutX, OutY, OutZ, OutWidth, OutHeight, OutDepth;
            public int SensX, SensY, SensZ;
            public int SysMode, SysX, SysY, SysWidth, SysHeight, SysSensX, SysSensY;
        }

        internal struct Sample
        {
            public int Serial, Status, Time, Cursor, Buttons, X, Y, Pressure, Azimuth, Altitude, Twist;
        }

        private sealed class Context
        {
            public IntPtr Handle, Window;
            public LogContext Settings;
            public bool Enabled;
            public int Capacity = 256;
            public int ActivePointer;
            public IntPtr StrokeWindow;
            public int StrokeButton;
            public bool SystemMouseDrag;
            public Point LastLocation;
            public int StrokeUpdates, StrokeSamples, StrokeReads;
            public int MinimumPressure, MaximumPressure, CursorReads, ButtonReads;
            public Sample LastPacket;
            public readonly List<Sample> Packets = new List<Sample>();
        }

        private delegate IntPtr HookProcedure(int code, UIntPtr wParam, IntPtr lParam);
        private delegate IntPtr SubclassProcedure(IntPtr window, uint message, UIntPtr wParam,
            IntPtr lParam, UIntPtr id, UIntPtr reference);
        private delegate bool WindowEnumerator(IntPtr window, IntPtr parameter);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate short AsyncKeyState(int key);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private delegate bool CursorPosition(out Point point);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate uint MessagePosition();
        private static readonly HookProcedure Hook = ProcessMessage;
        private static readonly SubclassProcedure CanvasHandler = CanvasMessage;
        private static readonly WindowEnumerator CanvasEnumerator = InspectCanvas;
        private static readonly string TracePath = FindTracePath();
        private static readonly AsyncKeyState StylosKeyState = ReadStylosKeyState;
        private static readonly AsyncKeyState StylosQueuedKeyState = ReadStylosQueuedKeyState;
        private static readonly CursorPosition StylosCursorPosition = ReadStylosCursorPosition;
        private static readonly MessagePosition StylosMessagePosition = ReadStylosMessagePosition;
        private static bool mouseQueriesConnected;
        private static bool stylosButtonPollEnabled;
        private static int traceLines;
        [ThreadStatic] private static bool pumpingInk;
        [ThreadStatic] private static bool hasBridgedMouse;
        [ThreadStatic] private static bool bridgedReleasePending;
        [ThreadStatic] private static Point bridgedMousePoint;
        [ThreadStatic] private static uint bridgedMouseButtons;
        [ThreadStatic] private static List<Context> contexts;
        [ThreadStatic] private static Dictionary<IntPtr, IntPtr> canvases;
        [ThreadStatic] private static IntPtr hookHandle;
        [ThreadStatic] private static int serial;
        [ThreadStatic] private static PenInfo[] history;

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWindowsHookExW(int type, HookProcedure callback, IntPtr module, uint thread);
        [DllImport("user32.dll")]
        private static extern bool UnhookWindowsHookEx(IntPtr handle);
        [DllImport("user32.dll")]
        private static extern IntPtr CallNextHookEx(IntPtr hook, int code, UIntPtr wParam, IntPtr lParam);
        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentThreadId();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandle(string name);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool VirtualProtect(IntPtr address, UIntPtr length, uint protection, out uint previous);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, IntPtr process);
        [DllImport("user32.dll")]
        private static extern bool IsChild(IntPtr parent, IntPtr child);
        [DllImport("user32.dll")]
        private static extern IntPtr GetAncestor(IntPtr window, uint flags);
        [DllImport("user32.dll")]
        private static extern bool EnumChildWindows(IntPtr parent, WindowEnumerator callback, IntPtr parameter);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder name, int size);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetProp(IntPtr window, string name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool SetProp(IntPtr window, string name, IntPtr value);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr RemoveProp(IntPtr window, string name);
        [DllImport("comctl32.dll")]
        private static extern bool SetWindowSubclass(IntPtr window, SubclassProcedure callback, UIntPtr id, UIntPtr reference);
        [DllImport("comctl32.dll")]
        private static extern bool RemoveWindowSubclass(IntPtr window, SubclassProcedure callback, UIntPtr id);
        [DllImport("comctl32.dll")]
        private static extern IntPtr DefSubclassProc(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool ConvertPrimaryPointerToMouseDrag();
        [DllImport("user32.dll")]
        private static extern bool ScreenToClient(IntPtr window, ref Point point);
        [DllImport("user32.dll")]
        private static extern bool ClientToScreen(IntPtr window, ref Point point);
        [DllImport("user32.dll")]
        private static extern bool GetCursorPos(out Point point);
        [DllImport("user32.dll")]
        private static extern uint GetMessagePos();
        [DllImport("user32.dll")]
        private static extern short GetKeyState(int key);
        [DllImport("user32.dll")]
        private static extern short GetAsyncKeyState(int key);
        [DllImport("user32.dll")]
        private static extern IntPtr SendMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")]
        private static extern bool PeekMessageW(out Message message, IntPtr window, uint minimum, uint maximum, uint remove);
        [DllImport("user32.dll")]
        private static extern IntPtr DispatchMessageW(ref Message message);
        [DllImport("user32.dll")]
        private static extern void PostQuitMessage(int result);
        [DllImport("user32.dll")]
        private static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll")]
        private static extern bool PostMessageW(IntPtr window, int message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool GetPointerPenInfo(uint id, out PenInfo pen);
        [DllImport("user32.dll")]
        private static extern bool GetPointerPenInfoHistory(uint id, ref uint count, [Out] PenInfo[] entries);
        [DllImport("user32.dll")]
        private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
        [DllImport("user32.dll")]
        private static extern bool PhysicalToLogicalPointForPerMonitorDPI(IntPtr window, ref Point point);

        public static int WTInfoA(int category, int index, IntPtr output)
        {
            if (category == 0 && index == 0)
            {
                Trace("WinTab service queried");
                return 172;
            }

            if ((category == 3 || category == 4 || category == 400 || category == 500) && index == 0)
            {
                if (output != IntPtr.Zero)
                    Marshal.StructureToPtr(DefaultContext(), output, false);
                return 172;
            }

            if (category == 100)
            {
                switch (index)
                {
                    case 1: return WriteText(output, "Windows Ink Pen");
                    case 2: return WriteValues(output, 1);
                    case 3: return WriteValues(output, 3);
                    case 4: return WriteValues(output, 0);
                    case 5: return WriteValues(output, 120);
                    case 6: return WriteValues(output, SupportedFields);
                    case 12: return WriteValues(output, 0, DefaultContext().InWidth, 0, 0);
                    case 13: return WriteValues(output, 0, DefaultContext().InHeight, 0, 0);
                    case 15: return WriteValues(output, 0, PressureMaximum, 0, 0);
                    case 17: return WriteValues(output, 0, 3600, 3, 655360,
                        -900, 900, 3, 655360, 0, 3600, 3, 655360);
                }
            }

            if (category >= 200 && category <= 202 && index == 1)
            {
                string[] names = { "Puck", "Stylus", "Eraser   Stylus" };
                return WriteText(output, names[category - 200]);
            }

            if (category == 1)
            {
                switch (index)
                {
                    case 1: return WriteText(output, "Windows Ink WinTab");
                    case 2:
                    case 3:
                        if (output != IntPtr.Zero) Marshal.WriteInt16(output, 0x104);
                        return 2;
                    case 4: return WriteValues(output, 1);
                    case 5: return WriteValues(output, 3);
                }
            }
            return 0;
        }

        private static int WriteText(IntPtr output, string value)
        {
            byte[] bytes = Encoding.ASCII.GetBytes(value + "\0");
            if (output != IntPtr.Zero)
                Marshal.Copy(bytes, 0, output, bytes.Length);
            return bytes.Length;
        }

        private static int WriteValues(IntPtr output, params int[] values)
        {
            if (output != IntPtr.Zero)
                Marshal.Copy(values, 0, output, values.Length);
            return values.Length * 4;
        }

        private static LogContext DefaultContext()
        {
            int x = GetSystemMetrics(76);
            int y = GetSystemMetrics(77);
            int width = Math.Max(1, GetSystemMetrics(78));
            int height = Math.Max(1, GetSystemMetrics(79));
            return new LogContext
            {
                Name = "Windows Ink",
                Options = SystemCursor | ContextMessages,
                MessageBase = PacketMessage,
                Rate = 120,
                PacketData = StylosFields,
                MoveMask = StylosFields,
                ButtonDownMask = -1,
                ButtonUpMask = -1,
                InWidth = width, InHeight = height, InDepth = 1,
                OutX = x, OutY = y + height, OutWidth = width, OutHeight = -height, OutDepth = 1,
                SensX = 65536, SensY = 65536, SensZ = 65536,
                SysX = x, SysY = y, SysWidth = width, SysHeight = height,
                SysSensX = 65536, SysSensY = 65536
            };
        }

        public static IntPtr WTOpenA(IntPtr window, IntPtr settings, int enable)
        {
            uint thread = GetCurrentThreadId();
            if (window == IntPtr.Zero || GetWindowThreadProcessId(window, IntPtr.Zero) != thread)
                return IntPtr.Zero;
            if (hookHandle == IntPtr.Zero)
                hookHandle = SetWindowsHookExW(3, Hook, IntPtr.Zero, thread);
            if (hookHandle == IntPtr.Zero)
            {
                Trace("WinTab message hook failed " + Marshal.GetLastWin32Error());
                return IntPtr.Zero;
            }

            LogContext initial = settings == IntPtr.Zero ? DefaultContext() : ReadSettings(settings);
            if (contexts == null)
                contexts = new List<Context>();
            var context = new Context { Window = window, Settings = initial, Enabled = enable != 0 };
            context.Handle = new IntPtr(NextSerial());
            contexts.Insert(0, context);
            Trace("WinTab context opened for " + WindowClass(window));
            ConnectStylosMouseQueries();
            InspectCanvas(window, IntPtr.Zero);
            IntPtr root = GetAncestor(window, 2);
            if (root != IntPtr.Zero)
                EnumChildWindows(root, CanvasEnumerator, IntPtr.Zero);
            return context.Handle;
        }

        private static LogContext ReadSettings(IntPtr address)
        {
            var settings = (LogContext)Marshal.PtrToStructure(address, typeof(LogContext));
            settings.PacketData &= SupportedFields;
            return settings;
        }

        private static int NextSerial()
        {
            serial = serial == Int32.MaxValue ? 1 : serial + 1;
            return serial;
        }

        private static Context Find(IntPtr handle)
        {
            if (contexts != null)
                foreach (Context context in contexts)
                    if (context.Handle == handle)
                        return context;
            return null;
        }

        public static int WTClose(IntPtr handle)
        {
            Context context = Find(handle);
            if (context == null)
                return 0;
            contexts.Remove(context);
            if (contexts.Count == 0)
            {
                hasBridgedMouse = false;
                bridgedReleasePending = false;
                if (canvases != null)
                    foreach (IntPtr window in new List<IntPtr>(canvases.Keys))
                        DetachCanvas(window);
                if (UnhookWindowsHookEx(hookHandle))
                    hookHandle = IntPtr.Zero;
            }
            return 1;
        }

        public static int WTEnable(IntPtr handle, int enable)
        {
            Context context = Find(handle);
            if (context == null)
                return 0;
            context.Enabled = enable != 0;
            if (!context.Enabled)
            {
                context.Packets.Clear();
                context.ActivePointer = 0;
            }
            return 1;
        }

        public static int WTOverlap(IntPtr handle, int top)
        {
            Context context = Find(handle);
            if (context == null)
                return 0;
            contexts.Remove(context);
            contexts.Insert(top != 0 ? 0 : contexts.Count, context);
            return 1;
        }

        public static int WTQueueSizeSet(IntPtr handle, int size)
        {
            Context context = Find(handle);
            if (context == null || size < 1 || size > 4096)
                return 0;
            context.Capacity = size;
            if (context.Packets.Count > size)
                context.Packets.RemoveRange(0, context.Packets.Count - size);
            return 1;
        }

        public static int WTGetA(IntPtr handle, IntPtr settings)
        {
            Context context = Find(handle);
            if (context == null || settings == IntPtr.Zero)
                return 0;
            Marshal.StructureToPtr(context.Settings, settings, false);
            return 1;
        }

        public static int WTSetA(IntPtr handle, IntPtr settings)
        {
            Context context = Find(handle);
            if (context == null || settings == IntPtr.Zero)
                return 0;
            context.Packets.Clear();
            context.Settings = ReadSettings(settings);
            return 1;
        }

        public static int WTPacketsGet(IntPtr handle, int limit, IntPtr output)
        {
            Context context = Find(handle);
            if (context == null || limit <= 0)
                return 0;
            PumpStroke(context);
            int count = Math.Min(limit, context.Packets.Count);
            if (context.ActivePointer != 0)
                context.StrokeReads += count;
            if (output != IntPtr.Zero)
                for (int i = 0; i < count; i++)
                    output = WritePacket(output, context.Settings.PacketData, handle, context.Packets[i]);
            context.Packets.RemoveRange(0, count);
            return count;
        }

        public static int WTPacket(IntPtr handle, int packetSerial, IntPtr output)
        {
            Context context = Find(handle);
            if (context == null)
                return 0;
            PumpStroke(context);
            int index = context.Packets.FindIndex(packet => packet.Serial == packetSerial);
            if (index < 0)
                return 0;
            if (output != IntPtr.Zero)
                WritePacket(output, context.Settings.PacketData, handle, context.Packets[index]);
            context.Packets.RemoveRange(0, index + 1);
            return 1;
        }

        internal static IntPtr WritePacket(IntPtr output, int mask, IntPtr handle, Sample packet)
        {
            for (int flag = 1; flag <= 0x1000; flag <<= 1)
            {
                if ((mask & flag) == 0)
                    continue;
                int value;
                switch (flag)
                {
                    case 1: value = handle.ToInt32(); break;
                    case 2: value = packet.Status; break;
                    case 4: value = packet.Time; break;
                    case 8: value = mask; break;
                    case 0x10: value = packet.Serial; break;
                    case 0x20: value = packet.Cursor; break;
                    case 0x40: value = packet.Buttons; break;
                    case 0x80: value = packet.X; break;
                    case 0x100: value = packet.Y; break;
                    case 0x400: value = packet.Pressure; break;
                    case 0x1000:
                        Marshal.WriteInt32(output, 0, packet.Azimuth);
                        Marshal.WriteInt32(output, 4, packet.Altitude);
                        value = packet.Twist;
                        output = IntPtr.Add(output, 8);
                        break;
                    default: value = 0; break;
                }
                Marshal.WriteInt32(output, value);
                output = IntPtr.Add(output, 4);
            }
            return output;
        }

        private static IntPtr ProcessMessage(int code, UIntPtr operation, IntPtr address)
        {
            if (code >= 0 && operation.ToUInt32() == 1 && address != IntPtr.Zero && contexts != null)
            {
                try
                {
                    int messageId = Marshal.ReadInt32(address, 4);
                    if (messageId == 0x0f || messageId == 5 ||
                        (messageId >= PointerUpdate && messageId <= PointerCaptureChanged))
                        InspectCanvas(Marshal.ReadIntPtr(address), IntPtr.Zero);
                    if (!pumpingInk)
                        TrackBridgedMouse(address, messageId);
                    if (!pumpingInk && messageId >= PointerUpdate && messageId <= PointerCaptureChanged)
                    {
                        var message = (Message)Marshal.PtrToStructure(address, typeof(Message));
                        int pointer = (int)(message.WParam.ToUInt32() & 0xffff);
                        Context active = ActiveStroke();
                        if (active != null && active.ActivePointer == pointer &&
                            active.StrokeWindow == message.Window &&
                            ReadPointer(message.Window, message.Id, (uint)pointer, message.LParam))
                            Marshal.WriteInt32(address, 4, 0);
                    }
                }
                catch (Exception error)
                {
                    Trace("Canvas discovery failed " + error.GetType().Name);
                }
            }
            return CallNextHookEx(hookHandle, code, operation, address);
        }

        private static void TrackBridgedMouse(IntPtr address, int messageId)
        {
            if (messageId != MouseMove && messageId != LeftDown && messageId != LeftUp &&
                messageId != RightDown && messageId != RightUp)
            {
                if (!bridgedReleasePending)
                    hasBridgedMouse = false;
                return;
            }
            uint flags = unchecked((uint)Marshal.ReadInt32(address, 8));
            if ((flags & BridgedMouseFlag) == 0 || canvases == null ||
                !canvases.ContainsKey(Marshal.ReadIntPtr(address)))
            {
                hasBridgedMouse = false;
                bridgedReleasePending = false;
                return;
            }
            var message = (Message)Marshal.PtrToStructure(address, typeof(Message));
            uint client = unchecked((uint)message.LParam.ToInt32());
            var point = new Point { X = (short)client, Y = (short)(client >> 16) };
            uint buttons = flags & ~BridgedMouseFlag;
            Marshal.WriteInt32(address, 8, unchecked((int)buttons));
            if (!ClientToScreen(message.Window, ref point))
                return;
            if (!bridgedReleasePending || messageId == LeftUp || messageId == RightUp)
            {
                hasBridgedMouse = true;
                bridgedMousePoint = point;
                bridgedMouseButtons = buttons;
                bridgedReleasePending = false;
            }
            Marshal.WriteInt32(address, 20, point.X);
            Marshal.WriteInt32(address, 24, point.Y);
        }

        private static string WindowClass(IntPtr window)
        {
            var name = new StringBuilder(64);
            GetClassName(window, name, name.Capacity);
            return name.ToString();
        }

        private static bool InspectCanvas(IntPtr window, IntPtr unused)
        {
            if (window == IntPtr.Zero || (canvases != null && canvases.ContainsKey(window)) ||
                WindowClass(window) != "CCelWindow")
                return true;
            if (!SetWindowSubclass(window, CanvasHandler, new UIntPtr(1), UIntPtr.Zero))
            {
                Trace("Stylos canvas callback could not be installed");
                return true;
            }
            if (canvases == null)
                canvases = new Dictionary<IntPtr, IntPtr>();
            IntPtr previous = GetProp(window, TabletProperty);
            canvases.Add(window, previous);
            SetProp(window, TabletProperty, new IntPtr(previous.ToInt32() | CanvasGestureFlags));
            Trace("Stylos canvas drawing input attached");
            return true;
        }

        private static void DetachCanvas(IntPtr window)
        {
            IntPtr previous;
            if (!canvases.TryGetValue(window, out previous))
                return;
            if (previous == IntPtr.Zero)
                RemoveProp(window, TabletProperty);
            else
                SetProp(window, TabletProperty, previous);
            canvases.Remove(window);
            RemoveWindowSubclass(window, CanvasHandler, new UIntPtr(1));
        }

        private static IntPtr CanvasMessage(IntPtr window, uint message, UIntPtr wParam,
            IntPtr lParam, UIntPtr id, UIntPtr reference)
        {
            try
            {
                if (message == TabletGestureQuery)
                    return new IntPtr(CanvasGestureFlags | GetProp(window, TabletProperty).ToInt32());
                if (message == DestroyWindowMessage)
                    DetachCanvas(window);
                if (message >= PointerUpdate && message <= PointerCaptureChanged &&
                    ReadPointer(window, message, wParam.ToUInt32() & 0xffff, lParam))
                    return IntPtr.Zero;
            }
            catch (Exception error)
            {
                Trace("Canvas callback failed " + error.GetType().Name);
            }
            return DefSubclassProc(window, message, wParam, lParam);
        }

        private static bool ReadPointer(IntPtr window, uint message, uint pointer, IntPtr newCapture)
        {
            Context context = TargetContext(window, (int)pointer);
            if (context == null)
                return false;
            if (message == PointerCaptureChanged || message == PointerLeave)
            {
                if (context.ActivePointer != pointer)
                    return false;
                if (message == PointerLeave || newCapture == context.StrokeWindow)
                    return true;
                ReleaseLostPointer(context, window);
                return true;
            }
            if (message != PointerDown && message != PointerUpdate && message != PointerUp && message != PointerEnter)
                return false;

            uint count = ReadPenSamples(pointer, message == PointerUpdate);
            if (count == 0)
            {
                if (message == PointerUp && context.ActivePointer == pointer)
                {
                    ReleaseLostPointer(context, window);
                    return true;
                }
                if (message == PointerDown || message == PointerUp)
                    Trace("No Windows Ink sample for contact " + message);
                return false;
            }
            if (context.ActivePointer != 0 && context.ActivePointer != pointer)
                return true;
            if (message == PointerDown)
                context.Packets.Clear();
            Point location = new Point();
            for (int i = (int)count - 1; i >= 0; i--)
            {
                PenInfo pen = history[i];
                location = pen.Info.Pixel;
                PhysicalToLogicalPointForPerMonitorDPI(window, ref location);
                bool touching = (pen.Info.Flags & (InContact | Canceled)) == InContact && message != PointerUp;
                Sample sample = ConvertPen(context.Settings, pen, location, touching);
                QueuePacket(context, sample);
                if (touching && context.ActivePointer == pointer && TracePath != null)
                {
                    context.MinimumPressure = Math.Min(context.MinimumPressure, sample.Pressure);
                    context.MaximumPressure = Math.Max(context.MaximumPressure, sample.Pressure);
                }
            }
            context.LastLocation = location;
            NotifyPacket(context);
            if (message == PointerDown && context.ActivePointer == 0)
                StartStroke(context, window, pointer, location, history[0]);
            else if (context.ActivePointer == pointer)
            {
                bool canceled = (history[0].Info.Flags & Canceled) != 0;
                if (message == PointerUpdate)
                {
                    context.StrokeUpdates++;
                    context.StrokeSamples += (int)count;
                }
                if (message == PointerUp || canceled)
                    EndStroke(context, window, location, canceled);
                else if (!context.SystemMouseDrag && message == PointerUpdate)
                    DispatchMouse(context.StrokeWindow, MouseMove, context.StrokeButton, location);
            }
            return true;
        }

        private static void StartStroke(Context context, IntPtr window, uint pointer, Point point, PenInfo pen)
        {
            context.ActivePointer = (int)pointer;
            context.StrokeWindow = window;
            context.StrokeButton = (pen.Flags & PenBarrel) != 0 ? RightButton : LeftButton;
            context.LastLocation = point;
            bridgedReleasePending = false;
            context.StrokeUpdates = 0;
            context.StrokeSamples = 0;
            context.StrokeReads = 0;
            context.MinimumPressure = context.LastPacket.Pressure;
            context.MaximumPressure = context.LastPacket.Pressure;
            context.CursorReads = 0;
            context.ButtonReads = 0;
            context.SystemMouseDrag = !stylosButtonPollEnabled &&
                context.StrokeButton == LeftButton && ConvertPrimaryPointerToMouseDrag();
            if (!context.SystemMouseDrag)
                DispatchMouse(window, context.StrokeButton == LeftButton ? LeftDown : RightDown,
                    context.StrokeButton, point);
        }

        private static void EndStroke(Context context, IntPtr window, Point point, bool canceled)
        {
            IntPtr target = context.StrokeWindow == IntPtr.Zero ? window : context.StrokeWindow;
            bool systemDrag = context.SystemMouseDrag;
            int button = context.StrokeButton;
            if (TracePath != null)
                Trace("Stroke ended: updates=" + context.StrokeUpdates + " samples=" + context.StrokeSamples +
                    " read=" + context.StrokeReads + " pressure=" + context.MinimumPressure + ".." +
                    context.MaximumPressure + " cursor=" + context.CursorReads + " buttons=" +
                    context.ButtonReads + " canceled=" + canceled);
            context.ActivePointer = 0;
            context.StrokeWindow = IntPtr.Zero;
            context.StrokeButton = 0;
            context.SystemMouseDrag = false;
            if (!systemDrag)
            {
                hasBridgedMouse = true;
                bridgedMousePoint = point;
                bridgedMouseButtons = 0;
                bridgedReleasePending = true;
                DispatchMouse(target, button == LeftButton ? LeftUp : RightUp, 0, point);
            }
            else if (canceled)
                SendMessageW(target, CancelModeMessage, UIntPtr.Zero, IntPtr.Zero);
        }

        private static void ReleaseLostPointer(Context context, IntPtr window)
        {
            Sample released = context.LastPacket;
            released.Pressure = 0;
            released.Buttons = 0;
            released.Status |= 1;
            QueuePacket(context, released);
            NotifyPacket(context);
            EndStroke(context, window, context.LastLocation, true);
        }

        private static void PumpStroke(Context context)
        {
            if (pumpingInk || context.ActivePointer == 0)
                return;
            pumpingInk = true;
            try
            {
                Message message;
                for (int i = 0; i < 32 && context.ActivePointer != 0 &&
                    PeekMessageW(out message, context.StrokeWindow, PointerUpdate, PointerCaptureChanged, 3); i++)
                {
                    if (message.Id == 0x12)
                    {
                        PostQuitMessage(unchecked((int)message.WParam.ToUInt32()));
                        return;
                    }
                    if (!ReadPointer(message.Window, message.Id, message.WParam.ToUInt32() & 0xffff, message.LParam))
                        DispatchMessageW(ref message);
                }
            }
            finally
            {
                pumpingInk = false;
            }
        }

        private static Context ActiveStroke()
        {
            if (contexts != null)
                foreach (Context context in contexts)
                    if (context.Enabled && context.ActivePointer != 0)
                        return context;
            return null;
        }

        private static short ReadStylosKeyState(int key)
        {
            if (key == LeftButton || key == RightButton)
            {
                Context active = ActiveStroke();
                if (active != null && active.StrokeButton == key)
                {
                    active.ButtonReads++;
                    PollWindowsInk(active);
                    return active.ActivePointer == 0 ? (short)0 : Int16.MinValue;
                }
            }
            return GetAsyncKeyState(key);
        }

        private static short ReadStylosQueuedKeyState(int key)
        {
            if ((key == LeftButton || key == RightButton) && hasBridgedMouse)
                return (bridgedMouseButtons & (uint)key) != 0 ? Int16.MinValue : (short)0;
            Context active = ActiveStroke();
            if (active != null && active.StrokeButton == key)
                return ReadStylosKeyState(key);
            return GetKeyState(key);
        }

        private static bool ReadStylosCursorPosition(out Point point)
        {
            Context active = ActiveStroke();
            if (active != null)
            {
                active.CursorReads++;
                PollWindowsInk(active);
                point = active.LastLocation;
                return true;
            }
            if (hasBridgedMouse)
            {
                point = bridgedMousePoint;
                return true;
            }
            return GetCursorPos(out point);
        }

        private static uint ReadStylosMessagePosition()
        {
            return hasBridgedMouse ? unchecked((uint)PackMousePoint(bridgedMousePoint)) : GetMessagePos();
        }

        private static void PollWindowsInk(Context active)
        {
            try
            {
                PumpStroke(active);
            }
            catch (Exception error)
            {
                Trace("Windows Ink polling failed " + error.GetType().Name);
            }
        }

        private static void ConnectStylosMouseQueries()
        {
            if (mouseQueriesConnected ||
                !String.Equals(Path.GetFileName(Process.GetCurrentProcess().MainModule.FileName),
                "Stylos.exe", StringComparison.OrdinalIgnoreCase))
                return;
            IntPtr image = GetModuleHandle(null);
            IntPtr utility = GetModuleHandle("rtwUtl.dll");
            stylosButtonPollEnabled = RedirectUser32Import(image, "GetAsyncKeyState", StylosKeyState);
            bool appCursor = RedirectUser32Import(image, "GetCursorPos", StylosCursorPosition);
            bool appMessage = RedirectUser32Import(image, "GetMessagePos", StylosMessagePosition);
            bool sharedButton = RedirectUser32Import(utility, "GetAsyncKeyState", StylosKeyState);
            bool sharedQueuedButton = RedirectUser32Import(utility, "GetKeyState", StylosQueuedKeyState);
            bool sharedCursor = RedirectUser32Import(utility, "GetCursorPos", StylosCursorPosition);
            mouseQueriesConnected = true;
            Trace("Mouse queries: app-button=" + stylosButtonPollEnabled + " app-cursor=" + appCursor +
                " app-message=" + appMessage + " shared-button=" + sharedButton + " shared-queued-button=" +
                sharedQueuedButton + " shared-cursor=" + sharedCursor);
        }

        private static bool RedirectUser32Import(IntPtr image, string function, Delegate replacement)
        {
            if (image == IntPtr.Zero)
                return false;
            IntPtr slot = FindImport(image, "USER32.dll", function);
            IntPtr original = GetProcAddress(GetModuleHandle("USER32.dll"), function);
            uint previous;
            UIntPtr size = new UIntPtr((uint)IntPtr.Size);
            if (slot == IntPtr.Zero || original == IntPtr.Zero || Marshal.ReadIntPtr(slot) != original ||
                !VirtualProtect(slot, size, 4, out previous))
                return false;
            try
            {
                Marshal.WriteIntPtr(slot, Marshal.GetFunctionPointerForDelegate(replacement));
            }
            finally
            {
                uint unused;
                VirtualProtect(slot, size, previous, out unused);
            }
            return true;
        }

        private static IntPtr FindImport(IntPtr image, string library, string function)
        {
            if (image == IntPtr.Zero || Marshal.ReadInt16(image) != 0x5a4d)
                return IntPtr.Zero;
            IntPtr header = IntPtr.Add(image, Marshal.ReadInt32(image, 60));
            if (Marshal.ReadInt32(header) != 0x4550 || Marshal.ReadInt16(header, 24) != 0x10b)
                return IntPtr.Zero;
            int imports = Marshal.ReadInt32(header, 128);
            if (imports == 0)
                return IntPtr.Zero;
            for (IntPtr descriptor = IntPtr.Add(image, imports); ; descriptor = IntPtr.Add(descriptor, 20))
            {
                int libraryName = Marshal.ReadInt32(descriptor, 12);
                if (libraryName == 0)
                    return IntPtr.Zero;
                if (!String.Equals(Marshal.PtrToStringAnsi(IntPtr.Add(image, libraryName)), library,
                    StringComparison.OrdinalIgnoreCase))
                    continue;
                int names = Marshal.ReadInt32(descriptor);
                int addresses = Marshal.ReadInt32(descriptor, 16);
                if (names == 0 || addresses == 0)
                    return IntPtr.Zero;
                for (int offset = 0; ; offset += 4)
                {
                    int name = Marshal.ReadInt32(image, names + offset);
                    if (name == 0)
                        return IntPtr.Zero;
                    if (name > 0 && Marshal.PtrToStringAnsi(IntPtr.Add(image, name + 2)) == function)
                        return IntPtr.Add(image, addresses + offset);
                }
            }
        }

        private static void DispatchMouse(IntPtr window, int message, int buttons, Point point)
        {
            if (!ScreenToClient(window, ref point))
                return;
            if (GetKeyState(0x10) < 0) buttons |= 4;
            if (GetKeyState(0x11) < 0) buttons |= 8;
            int flags = buttons | unchecked((int)BridgedMouseFlag);
            PostMessageW(window, message, new IntPtr(flags), new IntPtr(PackMousePoint(point)));
        }

        private static int PackMousePoint(Point point)
        {
            return (point.X & 0xffff) | (point.Y << 16);
        }

        private static Context TargetContext(IntPtr window, int pointer)
        {
            foreach (Context candidate in contexts)
                if (candidate.Enabled && candidate.ActivePointer == pointer)
                    return candidate;
            foreach (Context candidate in contexts)
                if (candidate.Enabled && (candidate.Window == window || IsChild(candidate.Window, window)))
                    return candidate;
            IntPtr root = GetAncestor(window, 2);
            foreach (Context candidate in contexts)
                if (candidate.Enabled && GetAncestor(candidate.Window, 2) == root)
                    return candidate;
            return null;
        }

        private static uint ReadPenSamples(uint pointer, bool includeHistory)
        {
            if (history == null)
                history = new PenInfo[64];
            IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
            try
            {
                PenInfo latest;
                if (!GetPointerPenInfo(pointer, out latest))
                    return 0;
                uint count = Math.Min((uint)history.Length, latest.Info.HistoryCount);
                if (includeHistory && count > 1 &&
                    GetPointerPenInfoHistory(pointer, ref count, history))
                    return Math.Min(count, (uint)history.Length);
                history[0] = latest;
                return 1;
            }
            finally
            {
                if (previous != IntPtr.Zero)
                    SetThreadDpiAwarenessContext(previous);
            }
        }

        private static Sample ConvertPen(LogContext settings, PenInfo pen, Point point, bool touching)
        {
            bool eraser = (pen.Flags & (PenInverted | PenEraser)) != 0;
            int pressure = (pen.Mask & HasPressure) != 0 ? (int)Math.Min(pen.Pressure, (uint)PressureMaximum) : PressureMaximum;
            double tiltX = Math.Tan(pen.TiltX * Math.PI / 180);
            double tiltY = Math.Tan(pen.TiltY * Math.PI / 180);
            int azimuth = (int)Math.Round(Math.Atan2(tiltX, tiltY) * 1800 / Math.PI);
            int altitude = (int)Math.Round(Math.Atan2(1, Math.Sqrt(tiltX * tiltX + tiltY * tiltY)) * 1800 / Math.PI);
            int outputHeight = settings.OutHeight;
            // Stylos asks for tablet coordinates and later reverses Y using the original desktop rectangle.
            if (settings.OutX == settings.InX && settings.OutY == settings.InY &&
                settings.OutWidth == settings.InWidth && outputHeight == -settings.InHeight)
                outputHeight = settings.InHeight;
            return new Sample
            {
                Status = eraser ? 0x10 : 0,
                Time = unchecked((int)pen.Info.Time),
                Cursor = eraser ? 2 : 1,
                Buttons = (touching ? 1 : 0) | ((pen.Flags & PenBarrel) != 0 ? 2 : 0),
                X = MapCoordinate(point.X - settings.SysX, settings.SysWidth, settings.InWidth,
                    settings.InX, settings.OutX, settings.OutWidth),
                Y = MapCoordinate(settings.SysY + settings.SysHeight - point.Y, settings.SysHeight,
                    settings.InHeight, settings.InY, settings.OutY, outputHeight),
                Pressure = touching ? pressure : 0,
                Azimuth = (azimuth + 3600) % 3600,
                Altitude = eraser ? -altitude : altitude,
                Twist = (int)(pen.Rotation % 360) * 10
            };
        }

        internal static int MapCoordinate(int position, int screenExtent, int inputExtent,
            int inputOrigin, int outputOrigin, int outputExtent)
        {
            if (screenExtent == 0 || inputExtent == 0)
                return outputOrigin;
            double input = (double)position * inputExtent / screenExtent;
            return outputOrigin + (int)Math.Round((input - inputOrigin) * outputExtent / inputExtent);
        }

        private static void QueuePacket(Context context, Sample packet)
        {
            packet.Serial = NextSerial();
            if (context.Packets.Count >= context.Capacity)
                context.Packets.RemoveAt(0);
            context.Packets.Add(packet);
            context.LastPacket = packet;
        }

        private static void NotifyPacket(Context context)
        {
            if ((context.Settings.Options & ContextMessages) != 0)
                PostMessageW(context.Window, context.Settings.MessageBase,
                    new IntPtr(context.LastPacket.Serial), context.Handle);
        }

        private static string FindTracePath()
        {
            string folder = Path.GetTempPath();
            return File.Exists(Path.Combine(folder, "StylosInk-trace.enabled"))
                ? Path.Combine(folder, "StylosInk-" + Process.GetCurrentProcess().Id + ".log")
                : null;
        }

        private static void Trace(string message)
        {
            if (TracePath == null || System.Threading.Interlocked.Increment(ref traceLines) > 120)
                return;
            try
            {
                File.AppendAllText(TracePath, DateTime.Now.ToString("HH:mm:ss.fff") + " " + message + Environment.NewLine);
            }
            catch (IOException) {}
            catch (UnauthorizedAccessException) {}
        }
    }
}
