using Microsoft.Win32;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;

namespace WindowCloud;

internal sealed class ExplorerRegistration : IDisposable
{
    private const string NamespaceKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace";
    private const string Classes = @"Software\Classes\CLSID";
    private const string Marker = "WindowCloud.Account";
    private const string Journal = @"Software\WindowCloud\Explorer";
    private readonly string dll = Path.Combine(AppContext.BaseDirectory, "WindowCloud.Explorer.dll");
    private readonly string pipe;
    private readonly Dictionary<Guid, string> visible = [];

    public ExplorerRegistration(string pipe)
    {
        this.pipe = pipe;
        if (!File.Exists(dll))
            throw new FileNotFoundException("WindowCloud.Explorer.dll должна находиться рядом с WindowCloud.exe.", dll);
        using var journal = Registry.CurrentUser.CreateSubKey(Journal);
        foreach (var name in journal.GetValueNames())
        {
            if (Guid.TryParseExact(name, "B", out var id)) Remove(id);
        }
        Notify();
    }

    public void Update(Account[] accounts, IReadOnlySet<Provider> online)
    {
        var desired = accounts.Where(a => online.Contains(a.Provider)).ToDictionary(a => a.Id, a => a.DisplayName);
        var changed = false;
        foreach (var id in visible.Keys.Except(desired.Keys).ToArray())
        {
            Remove(id);
            visible.Remove(id);
            changed = true;
        }
        foreach (var (id, label) in desired)
        {
            if (visible.TryGetValue(id, out var previous) && label == previous) continue;
            Add(id, label);
            visible[id] = label;
            changed = true;
        }
        if (changed) Notify();
    }

    private void Add(Guid id, string label)
    {
        var key = id.ToString("B");
        using (var journal = Registry.CurrentUser.CreateSubKey(Journal)) journal.SetValue(key, 1);
        using var clsid = Registry.CurrentUser.CreateSubKey(Classes + "\\" + key);
        clsid.SetValue(null, label);
        clsid.SetValue(Marker, pipe);
        using (var server = clsid.CreateSubKey("InProcServer32"))
        {
            server.SetValue(null, dll);
            server.SetValue("ThreadingModel", "Apartment");
        }
        using (var icon = clsid.CreateSubKey("DefaultIcon"))
            icon.SetValue(null, "%SystemRoot%\\System32\\imageres.dll,-25", RegistryValueKind.ExpandString);
        using (var shell = clsid.CreateSubKey("ShellFolder"))
            shell.SetValue("Attributes", unchecked((int)0xA0000000), RegistryValueKind.DWord);
        using (var entry = Registry.CurrentUser.CreateSubKey(NamespaceKey + "\\" + key)) entry.SetValue(null, label);
        var pidl = ParseRoot(id);
        if (pidl != 0)
        {
            try { SHChangeNotify(0x8, 0, pidl, 0); }
            finally { Marshal.FreeCoTaskMem(pidl); }
        }
    }

    private static void Remove(Guid id)
    {
        var key = id.ToString("B");
        var pidl = ParseRoot(id);
        using (var clsid = Registry.CurrentUser.OpenSubKey(Classes + "\\" + key))
        {
            if (clsid?.GetValue(Marker) is string)
            {
                Registry.CurrentUser.DeleteSubKeyTree(NamespaceKey + "\\" + key, throwOnMissingSubKey: false);
                Registry.CurrentUser.DeleteSubKeyTree(Classes + "\\" + key, throwOnMissingSubKey: false);
            }
        }
        using var journal = Registry.CurrentUser.CreateSubKey(Journal);
        journal.DeleteValue(key, throwOnMissingValue: false);
        if (pidl != 0)
        {
            try { SHChangeNotify(0x10, 0, pidl, 0); }
            finally { Marshal.FreeCoTaskMem(pidl); }
        }
    }

    private static void Notify()
    {
        if (SHGetSpecialFolderLocation(0, 0x11, out var computer) != 0) return;
        try { SHChangeNotify(0x1000, 0, computer, 0); }
        finally { Marshal.FreeCoTaskMem(computer); }
    }

    public void Dispose()
    {
        foreach (var id in visible.Keys) Remove(id);
        visible.Clear();
        Notify();
    }

    [DllImport("shell32.dll")]
    private static extern int SHGetSpecialFolderLocation(nint window, int folder, out nint pidl);
    [DllImport("shell32.dll")]
    private static extern void SHChangeNotify(uint eventId, uint flags, nint item, nint other);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHParseDisplayName(string name, nint context, out nint pidl, uint mask, nint attributes);

    private static nint ParseRoot(Guid id) =>
        SHParseDisplayName($"::{{20D04FE0-3AEA-1069-A2D8-08002B30309D}}\\::{id:B}", 0, out var pidl, 0, 0) == 0 ? pidl : 0;

    public static async Task<HashSet<Provider>> Check(Provider[] providers, CancellationToken cancellation)
    {
        if (providers.Length == 0 || !NetworkInterface.GetIsNetworkAvailable()) return [];
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(TimeSpan.FromSeconds(5));
        var results = await Task.WhenAll(providers.Select(async provider =>
        {
            var url = provider switch
            {
                Provider.GoogleDrive => "https://www.googleapis.com/drive/v3/about",
                Provider.OneDrive => "https://graph.microsoft.com/v1.0/me/drive",
                _ => "https://api.dropboxapi.com/2/users/get_current_account"
            };
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Head, url);
                using var response = await OAuth.Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, deadline.Token);
                return (Provider: provider, Online: (int)response.StatusCode < 500);
            }
            catch (Exception ex) when (ex is HttpRequestException or OperationCanceledException)
            {
                return (Provider: provider, Online: false);
            }
        }));
        return results.Where(result => result.Online).Select(result => result.Provider).ToHashSet();
    }
}
