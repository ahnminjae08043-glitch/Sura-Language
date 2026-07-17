// ================================================================
//  Sura P/Invoke wrapper for Unity / .NET hosts.
//
//  Usage (Unity MonoBehaviour):
//      using var sura = new SuraScript();
//      sura.SetNumber("base_price", 100);
//      sura.SetNumber("demand", 0.8);
//      int rc = sura.Run(@"
//          price is base_price * (1 + demand * 0.5)
//      ");
//      if (rc != SuraScript.OK) Debug.LogError(sura.LastError());
//      double price = sura.GetNumber("price");
//
//  Place SuraEngine.dll next to the game's .exe (or in Unity's Plugins/x64).
// ================================================================
using System;
using System.Runtime.InteropServices;
using System.Text;

public sealed class SuraScript : IDisposable {
    // Return codes — must match sura_ffi.hpp
    public const int OK            =  0;
    public const int ERR_LEX       = -1;
    public const int ERR_PARSE     = -2;
    public const int ERR_RUNTIME   = -3;
    public const int ERR_TYPE      = -4;
    public const int ERR_INTERNAL  = -9;

    private const string DLL = "SuraEngine";

    [DllImport(DLL)] private static extern IntPtr sura_new();
    [DllImport(DLL)] private static extern void   sura_free(IntPtr h);
    [DllImport(DLL)] private static extern void   sura_set_number(IntPtr h, string name, double v);
    [DllImport(DLL)] private static extern void   sura_set_string(IntPtr h, string name, string v);
    [DllImport(DLL)] private static extern void   sura_set_bool  (IntPtr h, string name, int v);
    [DllImport(DLL)] private static extern void   sura_clear_global(IntPtr h, string name);
    [DllImport(DLL)] private static extern void   sura_set_legacy_types(IntPtr h, int enabled);
    [DllImport(DLL)] private static extern int    sura_run(IntPtr h, string source);
    [DllImport(DLL)] private static extern int    sura_has(IntPtr h, string name);
    [DllImport(DLL)] private static extern double sura_get_number(IntPtr h, string name);
    [DllImport(DLL)] private static extern int    sura_get_bool  (IntPtr h, string name);
    [DllImport(DLL)] private static extern IntPtr sura_get_string(IntPtr h, string name);
    [DllImport(DLL)] private static extern IntPtr sura_last_error(IntPtr h);
    [DllImport(DLL)] private static extern IntPtr sura_version();

    private IntPtr _handle;

    public SuraScript() {
        _handle = sura_new();
        if (_handle == IntPtr.Zero)
            throw new InvalidOperationException("sura_new() failed");
    }

    public void Dispose() {
        if (_handle != IntPtr.Zero) {
            sura_free(_handle);
            _handle = IntPtr.Zero;
        }
        GC.SuppressFinalize(this);
    }
    ~SuraScript() { Dispose(); }

    // ── Inputs ─────────────────────────────────────────────
    public void SetNumber(string name, double v) { EnsureOpen(); sura_set_number(_handle, name, v); }
    public void SetString(string name, string v) { EnsureOpen(); sura_set_string(_handle, name, v ?? ""); }
    public void SetBool  (string name, bool v)   { EnsureOpen(); sura_set_bool(_handle, name, v ? 1 : 0); }
    public void Clear    (string name)           { EnsureOpen(); sura_clear_global(_handle, name); }

    // Type errors stop execution by default. Enable this only as a temporary
    // compatibility bridge while migrating older dynamically typed scripts.
    public void SetLegacyTypes(bool enabled) {
        EnsureOpen();
        sura_set_legacy_types(_handle, enabled ? 1 : 0);
    }

    // ── Execute ────────────────────────────────────────────
    // Returns one of the OK/ERR_* constants. On error, LastError() has detail.
    public int Run(string source) {
        EnsureOpen();
        return sura_run(_handle, source ?? "");
    }

    // Convenience: throw on error instead of returning a code.
    public void RunOrThrow(string source) {
        int rc = Run(source);
        if (rc != OK) throw new SuraException(rc, LastError());
    }

    // ── Outputs ────────────────────────────────────────────
    public bool   Has       (string name) { EnsureOpen(); return sura_has(_handle, name) != 0; }
    public double GetNumber (string name) { EnsureOpen(); return sura_get_number(_handle, name); }
    public bool   GetBool   (string name) { EnsureOpen(); return sura_get_bool(_handle, name) != 0; }
    public string GetString (string name) {
        EnsureOpen();
        IntPtr p = sura_get_string(_handle, name);
        return p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
    }

    // ── Diagnostics ────────────────────────────────────────
    public string LastError() {
        EnsureOpen();
        IntPtr p = sura_last_error(_handle);
        return p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
    }

    public static string Version() {
        IntPtr p = sura_version();
        return p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
    }

    private void EnsureOpen() {
        if (_handle == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(SuraScript));
    }
}

public class SuraException : Exception {
    public int Code { get; }
    public SuraException(int code, string message) : base(message) { Code = code; }
}
