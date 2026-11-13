using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;

namespace WindowCloud;

internal enum Provider { GoogleDrive, OneDrive, Dropbox }

internal sealed record ClientRegistration(string Id, string? DesktopSecret);
internal sealed record Token(string Access, string Refresh, DateTimeOffset Expires);
internal sealed record Identity(string Id, string Label, string Root, string? Namespace = null);

internal sealed record Account(Guid Id, Provider Provider, Identity Identity, ClientRegistration Client, Token Token)
{
    public string DisplayName => $"{Provider.Label()} ({Identity.Label})";
}

internal static class ProviderNames
{
    public static string Label(this Provider provider) => provider == Provider.GoogleDrive ? "Google Drive" : provider.ToString();
}

internal sealed class Accounts
{
    private readonly object gate = new();
    private readonly string file = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WindowCloud", "accounts.dat");
    private List<Account> accounts;

    public Accounts()
    {
        if (!File.Exists(file))
        {
            accounts = [];
            return;
        }
        var bytes = Protect(File.ReadAllBytes(file), decrypt: true);
        try { accounts = JsonSerializer.Deserialize<List<Account>>(bytes) ?? throw new InvalidDataException(); }
        finally { CryptographicOperations.ZeroMemory(bytes); }
    }

    public Account[] Snapshot() { lock (gate) return [.. accounts]; }

    public Account? Find(Guid id) { lock (gate) return accounts.Find(account => account.Id == id); }

    public Account Save(Provider provider, Identity identity, ClientRegistration client, Token token)
    {
        lock (gate)
        {
            var existing = accounts.Find(a => a.Provider == provider && a.Identity.Id == identity.Id);
            var account = new Account(existing?.Id ?? Guid.NewGuid(), provider, identity, client, token);
            Persist([.. accounts.Where(a => a.Id != account.Id), account]);
            return account;
        }
    }

    public Account UpdateToken(Account previous, Token token)
    {
        lock (gate)
        {
            var current = accounts.Find(a => a.Id == previous.Id) ?? throw new IOException("Аккаунт отключён.");
            if (current != previous) return current;
            var updated = current with { Token = token };
            Persist(accounts.Select(a => a.Id == updated.Id ? updated : a).ToList());
            return updated;
        }
    }

    public void Remove(Guid id)
    {
        lock (gate) Persist(accounts.Where(a => a.Id != id).ToList());
    }

    private void Persist(List<Account> next)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(next);
        byte[] encrypted;
        try { encrypted = Protect(bytes, decrypt: false); }
        finally { CryptographicOperations.ZeroMemory(bytes); }
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        var temporary = file + ".tmp";
        try
        {
            using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
            {
                output.Write(encrypted);
                output.Flush(flushToDisk: true);
            }
            File.Move(temporary, file, overwrite: true);
            accounts = next;
        }
        finally { File.Delete(temporary); }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Blob { public int Size; public IntPtr Data; }

    [DllImport("crypt32.dll", SetLastError = true)]
    private static extern bool CryptProtectData(ref Blob input, IntPtr description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, int flags, out Blob output);
    [DllImport("crypt32.dll", SetLastError = true)]
    private static extern bool CryptUnprotectData(ref Blob input, IntPtr description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, int flags, out Blob output);
    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr data);

    private static byte[] Protect(byte[] data, bool decrypt)
    {
        var handle = GCHandle.Alloc(data, GCHandleType.Pinned);
        Blob output = default;
        try
        {
            var input = new Blob { Size = data.Length, Data = handle.AddrOfPinnedObject() };
            var ok = decrypt
                ? CryptUnprotectData(ref input, 0, 0, 0, 0, 1, out output)
                : CryptProtectData(ref input, 0, 0, 0, 0, 1, out output);
            if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error());
            var result = new byte[output.Size];
            Marshal.Copy(output.Data, result, 0, result.Length);
            return result;
        }
        finally
        {
            if (output.Data != 0)
            {
                for (var i = 0; decrypt && i < output.Size; i++) Marshal.WriteByte(output.Data, i, 0);
                LocalFree(output.Data);
            }
            handle.Free();
        }
    }
}
