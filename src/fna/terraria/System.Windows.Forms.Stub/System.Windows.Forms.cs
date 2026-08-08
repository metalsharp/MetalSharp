// Minimal System.Windows.Forms shim for running Windows XNA/FNA games
// (Terraria) under the bundled Mono runtime on macOS.
//
// Terraria's Main.SetBorderlessFormStyle / window-chrome code creates a
// System.Windows.Forms.Form and configures it. Mono's real WinForms on macOS
// uses the Quartz driver, whose native window creation (Form.Handle) blocks
// forever without a proper AppKit main loop — the game never gets to its FNA
// (SDL) window. This stub satisfies the same API surface with inert objects:
// the actual game window is created by FNA, so the WinForms form is only
// configuration. The game dir is FIRST on MONO_PATH, so this assembly wins
// over the GAC copy for the game's process.
//
// Screen bounds come from the real display (Quartz) so borderless-fullscreen
// sizing stays correct; nothing here touches GDI+.
using System;
using System.Runtime.InteropServices;

[assembly: System.Reflection.AssemblyVersion("4.0.0.0")]
[assembly: System.Reflection.AssemblyFileVersion("4.0.0.0")]

namespace System.Windows.Forms
{
    public enum FormBorderStyle { None, FixedSingle, Fixed3D, FixedDialog, Sizable, FixedToolWindow, SizableToolWindow }
    public enum FormWindowState { Normal, Minimized, Maximized }
    public enum FormStartPosition { Manual, CenterScreen, WindowsDefaultLocation, WindowsDefaultBounds, CenterParent }
    public enum DialogResult { None, OK, Cancel, Abort, Retry, Ignore, Yes, No }
    public enum MessageBoxButtons { OK, OKCancel, YesNo, YesNoCancel, AbortRetryIgnore, RetryCancel }
    public enum MessageBoxIcon { None, Error, Hand, Stop, Question, Exclamation, Warning, Asterisk, Information }
    public enum CloseReason { None, WindowsShutDown, MdiFormClosing, UserClosing, TaskManagerClosing, FormOwnerClosing, ApplicationExitCall }
    public enum AutoScaleMode { None, Font, Dpi, Inherit }
    [Flags]
    public enum ControlStyles
    {
        UserPaint = 1, Opaque = 2, ResizeRedraw = 4, FixedWidth = 8, FixedHeight = 16,
        Selectable = 32, UserMouse = 64, SupportsTransparentBackColor = 128, DoubleBuffer = 256,
        OptimizedDoubleBuffer = 512, AllPaintingInWmPaint = 1024, CacheText = 2048, ContainerControl = 4096,
    }

    public delegate void FormClosedEventHandler(object sender, FormClosedEventArgs e);
    public delegate void FormClosingEventHandler(object sender, FormClosingEventArgs e);

    public class FormClosedEventArgs : EventArgs
    {
        public FormClosedEventArgs(CloseReason reason) { CloseReason = reason; }
        public CloseReason CloseReason { get; private set; }
    }

    public class FormClosingEventArgs : EventArgs
    {
        public FormClosingEventArgs(CloseReason reason, bool cancel) { CloseReason = reason; Cancel = cancel; }
        public CloseReason CloseReason { get; private set; }
        public bool Cancel { get; set; }
    }

    public class Control : System.ComponentModel.Component
    {
        // Terraria obtains its WinForms form from the FNA window handle via
        // Control.FromHandle (never `new Form()`). Return a cached inert Form
        // per handle so identity checks and property sets behave.
        private static System.Collections.Generic.Dictionary<IntPtr, Control> formsByHandle =
            new System.Collections.Generic.Dictionary<IntPtr, Control>();
        public static Control FromHandle(IntPtr handle)
        {
            if (handle == IntPtr.Zero) return null;
            Control c;
            if (!formsByHandle.TryGetValue(handle, out c))
            {
                c = new Form();
                formsByHandle[handle] = c;
            }
            return c;
        }
        public static Control FromChildHandle(IntPtr handle) { return FromHandle(handle); }
        public static bool IsKeyLocked(System.Windows.Forms.Keys key) { return false; }
        private string text = "";
        public string Text { get { return text; } set { text = value; } }
        public System.Drawing.Size Size { get; set; }
        public System.Drawing.Size MaximumSize { get; set; }
        public System.Drawing.Size MinimumSize { get; set; }
        public System.Drawing.Point Location { get; set; }
        public System.Drawing.Rectangle Bounds { get; set; }
        public System.Drawing.Color BackColor { get; set; }
        public System.Drawing.Color ForeColor { get; set; }
        public System.Drawing.Font Font { get; set; }
        public object Tag { get; set; }
        public bool Visible { get; set; } = true;
        public bool Enabled { get; set; } = true;
        public bool Focused { get { return true; } }
        public bool IsDisposed { get; private set; }
        public IntPtr Handle { get { return new IntPtr(0x1024); } }
        public IntPtr CreateHandle() { return Handle; }
        public void CreateControl() { }
        public Form FindForm() { return this as Form; }
        public Control GetChildAtPoint(System.Drawing.Point pt) { return null; }
        public Control GetNextControl(Control ctl, bool forward) { return null; }
        public System.Drawing.Point PointToClient(System.Drawing.Point p) { return p; }
        public System.Drawing.Point PointToScreen(System.Drawing.Point p) { return p; }
        public System.Drawing.Rectangle RectangleToClient(System.Drawing.Rectangle r) { return r; }
        public System.Drawing.Rectangle RectangleToScreen(System.Drawing.Rectangle r) { return r; }
        public bool ContainsFocus { get { return true; } }
        public bool Capture { get; set; }
        public Control Parent { get; set; }
        public System.Windows.Forms.Control.ControlCollection Controls { get { return new ControlCollection(); } }
        public System.Windows.Forms.Control.ControlCollection ParentControls { get { return new ControlCollection(); } }
        public int Width { get; set; }
        public int Height { get; set; }
        public int Left { get; set; }
        public int Top { get; set; }
        public int Right { get { return Left + Width; } }
        public int Bottom { get { return Top + Height; } }
        public string Name { get; set; }
        public System.Drawing.Color DefaultBackColor { get { return System.Drawing.Color.Empty; } }
        public System.Drawing.Color DefaultForeColor { get { return System.Drawing.Color.Empty; } }
        public void Select() { }
        public void SelectNextControl(Control ctl, bool forward, bool tabStopOnly, bool nested, bool wrap) { }
        public bool PreProcessMessage(ref System.Windows.Forms.Message msg) { return false; }
        public void SetTopLevel(bool value) { }
        public bool TopLevel { get; set; }
        public void SetStyle(ControlStyles styles, bool value) { }
        public void GetStyle(ControlStyles styles) { }
        public void SuspendLayout() { }
        public void ResumeLayout() { }
        public void ResumeLayout(bool performLayout) { }
        public void PerformLayout() { }
        public void Invalidate() { }
        public void Invalidate(System.Drawing.Rectangle r) { }
        public void Update() { }
        public void Refresh() { }
        public void Focus() { }
        public void BringToFront() { }
        public void SendToBack() { }
        public System.Drawing.Graphics CreateGraphics() { return null; }
        public void Dispose() { IsDisposed = true; }
        protected virtual void OnLoad(EventArgs e) { if (Load != null) Load(this, e); }
        public event EventHandler Load;
        public event EventHandler Shown;
        public event EventHandler Closed;
        public event EventHandler Closing;
        public event FormClosedEventHandler FormClosed;
        public event FormClosingEventHandler FormClosing;
        public event EventHandler HandleCreated;
        public event EventHandler HandleDestroyed;
        public event EventHandler VisibleChanged;
        public event EventHandler EnabledChanged;
        public event EventHandler TextChanged;
        public event EventHandler Resize;

        public class ControlCollection : System.Collections.Generic.List<Control>
        {
            public void AddRange(Control[] controls) { foreach (var c in controls) Add(c); }
        }
    }

    public class FormCollection : System.Collections.Generic.List<Form>
    {
    }

    public class Form : Control
    {
        public FormBorderStyle FormBorderStyle { get; set; }
        public static Form ActiveForm { get { return null; } }
        public static Form ActiveMdiChild { get { return null; } }
        public static FormCollection OpenForms { get { return new FormCollection(); } }
        public FormWindowState WindowState { get; set; }
        public FormStartPosition StartPosition { get; set; }
        public bool ShowInTaskbar { get; set; } = true;
        public bool TopMost { get; set; }
        public bool ControlBox { get; set; } = true;
        public bool MaximizeBox { get; set; } = true;
        public bool MinimizeBox { get; set; } = true;
        public bool ShowIcon { get; set; } = true;
        public System.Drawing.Icon Icon { get; set; }
        public System.Drawing.Size ClientSize { get; set; }
        public System.Drawing.Rectangle DesktopBounds { get; set; }
        public bool IsMdiContainer { get; set; }
        public Form MdiParent { get; set; }
        public bool KeyPreview { get; set; }
        public bool AutoScale { get; set; }
        public AutoScaleMode AutoScaleMode { get; set; }
        public bool AllowTransparency { get; set; }
        public double Opacity { get; set; } = 1.0;
        public System.Drawing.Color TransparencyKey { get; set; }
        public bool AutoSize { get; set; }
        public bool AutoScroll { get; set; }
        public string SiteName { get { return ""; } }
        public bool Modal { get { return false; } }
        public void Show() { Visible = true; }
        public void Hide() { Visible = false; }
        public void Close() { }
        public void Activate() { }
        public void SetBounds(int x, int y, int width, int height) { }
        public void SetDesktopLocation(int x, int y) { }
        public void SetDesktopBounds(int x, int y, int width, int height) { }
        public void AddOwnedForm(Form f) { }
        public void RemoveOwnedForm(Form f) { }
        public void ShowDialog() { }
        public void ShowDialog(System.Windows.Forms.IWin32Window owner) { }
        public void CenterToScreen() { }
        public void CenterToParent() { }
        public void SetVisibleCore(bool value) { Visible = value; }
        public void SetAutoScaleSize(System.Drawing.SizeF size) { }
    }

    public interface IWin32Window
    {
        IntPtr Handle { get; }
    }

    public static class Application
    {
        public static void EnableVisualStyles() { }
        public static void Run() { }
        public static void Run(Form f) { }
        public static void Exit() { }
        public static void ExitThread() { }
        public static void DoEvents() { }
        public static string StartUpPath { get { return ""; } }
        public static string ExecutablePath { get { return ""; } }
        public static string ProductName { get { return ""; } }
        public static string UserAppDataPath { get { return ""; } }
        public static string LocalUserAppDataPath { get { return ""; } }
        public static string CommonAppDataPath { get { return ""; } }
        public static System.Threading.ThreadState ThreadState { get { return 0; } }
        public static bool MessageLoop { get { return true; } }
        public static void SetCompatibleTextRenderingDefault(bool value) { }
        public static void AddMessageFilter(IMessageFilter value) { }
        public static void RemoveMessageFilter(IMessageFilter value) { }
        public static event EventHandler ApplicationExit;
        public static event EventHandler ThreadExit;
        public static event EventHandler Idle;
    }

    public interface IMessageFilter
    {
        bool PreFilterMessage(ref System.Windows.Forms.Message m);
    }

    public struct Message
    {
        public IntPtr HWnd { get; set; }
        public int Msg { get; set; }
        public IntPtr WParam { get; set; }
        public IntPtr LParam { get; set; }
        public IntPtr Result { get; set; }
    }

    public class Screen
    {
        public static Screen PrimaryScreen { get { return new Screen(); } }
        public static Screen FromPoint(System.Drawing.Point point) { return new Screen(); }
        public static Screen FromRectangle(System.Drawing.Rectangle rect) { return new Screen(); }
        public static Screen FromControl(System.Windows.Forms.Control control) { return new Screen(); }
        public static Screen FromHandle(IntPtr handle) { return new Screen(); }
        public static Screen[] AllScreens { get { return new Screen[] { new Screen() }; } }
        public static System.Drawing.Rectangle GetBounds(System.Windows.Forms.Control control) { return PrimaryScreen.Bounds; }
        public static System.Drawing.Rectangle GetBounds(System.Drawing.Rectangle rect) { return PrimaryScreen.Bounds; }
        public static System.Drawing.Rectangle GetBounds(System.Drawing.Point point) { return PrimaryScreen.Bounds; }
        public static System.Drawing.Rectangle GetWorkingArea(System.Windows.Forms.Control control) { return PrimaryScreen.WorkingArea; }
        public static System.Drawing.Rectangle GetWorkingArea(System.Drawing.Rectangle rect) { return PrimaryScreen.WorkingArea; }
        public System.Drawing.Rectangle Bounds
        {
            get
            {
                CGRect r = CGDisplayBounds(CGMainDisplayID());
                return new System.Drawing.Rectangle((int)r.origin.x, (int)r.origin.y, (int)r.size.width, (int)r.size.height);
            }
        }
        public System.Drawing.Rectangle WorkingArea { get { return Bounds; } }
        public string DeviceName { get { return ""; } }
        public int BitsPerPixel { get { return 32; } }

        [StructLayout(LayoutKind.Sequential)]
        private struct CGPoint { public double x, y; }
        [StructLayout(LayoutKind.Sequential)]
        private struct CGSize { public double width, height; }
        [StructLayout(LayoutKind.Sequential)]
        private struct CGRect { public CGPoint origin; public CGSize size; }

        [DllImport("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")]
        private static extern uint CGMainDisplayID();
        [DllImport("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")]
        private static extern CGRect CGDisplayBounds(uint display);
    }

    public class MessageBox
    {
        public static DialogResult Show(string text) { return DialogResult.OK; }
        public static DialogResult Show(string text, string caption) { return DialogResult.OK; }
        public static DialogResult Show(string text, string caption, MessageBoxButtons buttons) { return DialogResult.OK; }
        public static DialogResult Show(string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon) { return DialogResult.OK; }
        public static DialogResult Show(System.Windows.Forms.IWin32Window owner, string text) { return DialogResult.OK; }
        public static DialogResult Show(System.Windows.Forms.IWin32Window owner, string text, string caption) { return DialogResult.OK; }
        public static DialogResult Show(System.Windows.Forms.IWin32Window owner, string text, string caption, MessageBoxButtons buttons) { return DialogResult.OK; }
        public static DialogResult Show(System.Windows.Forms.IWin32Window owner, string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon) { return DialogResult.OK; }
    }

    public static class SystemInformation
    {
        public static System.Drawing.Size PrimaryMonitorSize { get { return new System.Drawing.Size(Screen.PrimaryScreen.Bounds.Width, Screen.PrimaryScreen.Bounds.Height); } }
        public static System.Drawing.Rectangle WorkingArea { get { return Screen.PrimaryScreen.WorkingArea; } }
        public static int ScreenCount { get { return 1; } }
        public static int VirtualScreenWidth { get { return Screen.PrimaryScreen.Bounds.Width; } }
        public static int VirtualScreenHeight { get { return Screen.PrimaryScreen.Bounds.Height; } }
        public static bool TerminalServerSession { get { return false; } }
    }

    public class Timer : System.ComponentModel.Component
    {
        public int Interval { get; set; } = 100;
        public bool Enabled { get; set; }
        public void Start() { }
        public void Stop() { }
        public event EventHandler Tick;
    }

    [Flags]
    public enum Keys
    {
        None = 0, Back = 8, Tab = 9, Enter = 13, ShiftKey = 16, ControlKey = 17, Menu = 18,
        Pause = 19, Capital = 20, Escape = 27, Space = 32, Prior = 33, Next = 34, End = 35,
        Home = 36, Left = 37, Up = 38, Right = 39, Down = 40, Insert = 45, Delete = 46,
        D0 = 48, D1 = 49, D2 = 50, D3 = 51, D4 = 52, D5 = 53, D6 = 54, D7 = 55, D8 = 56, D9 = 57,
        A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72, I = 73, J = 74, K = 75,
        L = 76, M = 77, N = 78, O = 79, P = 80, Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86,
        W = 87, X = 88, Y = 89, Z = 90, LWin = 91, RWin = 92, Apps = 93,
        F1 = 112, F2 = 113, F3 = 114, F4 = 115, F5 = 116, F6 = 117, F7 = 118, F8 = 119,
        F9 = 120, F10 = 121, F11 = 122, F12 = 123,
        Shift = 65536, Control = 131072, Alt = 262144,
    }
}
