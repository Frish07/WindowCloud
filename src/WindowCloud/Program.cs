using System.Net.NetworkInformation;
using System.Text.Json;

namespace WindowCloud;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        using var instance = new Mutex(initiallyOwned: true, @"Local\WindowCloud-Tray", out var first);
        if (!first) return;
        ApplicationConfiguration.Initialize();
        try
        {
            using var iconResource = typeof(Program).Assembly.GetManifestResourceStream("WindowCloud.TrayIcon")
                ?? throw new InvalidOperationException("Не удалось загрузить значок WindowCloud.");
            using var icon = new Icon(iconResource, SystemInformation.SmallIconSize);
            using var tray = new Tray(icon);
            Application.Run(tray);
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "WindowCloud", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}

internal sealed class Tray : ApplicationContext
{
    private readonly Accounts accounts = new();
    private readonly Cloud cloud;
    private readonly ExplorerServer server;
    private readonly ExplorerRegistration registration;
    private readonly NotifyIcon tray;
    private readonly ContextMenuStrip menu = new();
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 15000 };
    private readonly CancellationTokenSource stop = new();
    private readonly Control dispatcher = new();
    private Settings? settings;
    private bool checking;
    private bool pending;

    public Tray(Icon icon)
    {
        cloud = new Cloud(accounts);
        server = new ExplorerServer(accounts, cloud);
        try { registration = new ExplorerRegistration(server.PipeName); }
        catch { server.Dispose(); throw; }
        _ = dispatcher.Handle;
        menu.Items.Add("Настройки", null, (_, _) => ShowSettings());
        menu.Items.Add("Выход", null, (_, _) => ExitThread());
        tray = new NotifyIcon { Text = "WindowCloud", Icon = icon, ContextMenuStrip = menu, Visible = true };
        tray.MouseClick += (_, e) => { if (e.Button == MouseButtons.Left) ShowSettings(); };
        timer.Tick += (_, _) => RefreshExplorer();
        NetworkChange.NetworkAvailabilityChanged += NetworkChanged;
        timer.Start();
        tray.ShowBalloonTip(5000, "WindowCloud",
            "Программа успешно запущена, нажмите на иконку в трее что бы настроить её", ToolTipIcon.Info);
        dispatcher.BeginInvoke(RefreshExplorer);
    }

    private void NetworkChanged(object? sender, NetworkAvailabilityEventArgs args)
    {
        try { dispatcher.BeginInvoke(RefreshExplorer); }
        catch (InvalidOperationException) { }
    }

    private void ShowSettings()
    {
        if (settings is null || settings.IsDisposed) settings = new Settings(accounts, cloud, RefreshExplorer);
        settings.Show();
        if (settings.WindowState == FormWindowState.Minimized) settings.WindowState = FormWindowState.Normal;
        settings.Activate();
    }

    private async void RefreshExplorer()
    {
        if (stop.IsCancellationRequested) return;
        if (!NetworkInterface.GetIsNetworkAvailable())
        {
            try { registration.Update([], new HashSet<Provider>()); }
            catch (Exception error) { Report(error); }
        }
        if (checking) { pending = true; return; }
        checking = true;
        try
        {
            do
            {
                pending = false;
                var providers = accounts.Snapshot().Select(a => a.Provider).Distinct().ToArray();
                var online = await ExplorerRegistration.Check(providers, stop.Token);
                if (!stop.IsCancellationRequested && !pending)
                    registration.Update(accounts.Snapshot(), online);
            } while (pending && !stop.IsCancellationRequested);
        }
        catch (Exception error) { if (!stop.IsCancellationRequested) Report(error); }
        finally { checking = false; }
    }

    private void Report(Exception error) =>
        tray.ShowBalloonTip(4500, "WindowCloud", "Ошибка Проводника. " + error.Message, ToolTipIcon.Error);

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            stop.Cancel();
            timer.Dispose();
            NetworkChange.NetworkAvailabilityChanged -= NetworkChanged;
            dispatcher.Dispose();
            settings?.Dispose();
            tray.Dispose();
            menu.Dispose();
            server.Dispose();
            registration.Dispose();
        }
        base.Dispose(disposing);
    }
}

internal sealed class Settings : Form
{
    private readonly Accounts accounts;
    private readonly Cloud cloud;
    private readonly Action changed;
    private readonly ListBox list = new() { Dock = DockStyle.Fill, DisplayMember = nameof(Account.DisplayName) };
    private readonly ComboBox providers = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 150 };
    private readonly Button add = new() { Text = "Подключить", AutoSize = true };
    private readonly Button remove = new() { Text = "Отключить", AutoSize = true };
    private readonly Button cancel = new() { Text = "Отменить вход", AutoSize = true, Visible = false };
    private readonly Label status = new() { Text = "Подключите аккаунт, чтобы увидеть его в разделе «Этот компьютер».",
        Dock = DockStyle.Fill, AutoSize = true, Padding = new Padding(0, 5, 0, 5) };
    private CancellationTokenSource? signIn;

    public Settings(Accounts accounts, Cloud cloud, Action changed)
    {
        this.accounts = accounts;
        this.cloud = cloud;
        this.changed = changed;
        Text = "WindowCloud";
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(570, 320);
        MinimumSize = new Size(480, 260);
        AutoScaleMode = AutoScaleMode.Dpi;
        ShowInTaskbar = true;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(16), RowCount = 3, ColumnCount = 1 };
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        providers.Items.AddRange(Enum.GetValues<Provider>().Select(p => (object)p.Label()).ToArray());
        providers.SelectedIndex = 0;
        buttons.Controls.AddRange([providers, add, remove, cancel]);
        layout.Controls.Add(status, 0, 0);
        layout.Controls.Add(list, 0, 1);
        layout.Controls.Add(buttons, 0, 2);
        Controls.Add(layout);
        list.SelectedIndexChanged += (_, _) => remove.Enabled = signIn is null && list.SelectedIndex >= 0;
        add.Click += (_, _) => Connect();
        remove.Click += (_, _) => Disconnect();
        cancel.Click += (_, _) => signIn?.Cancel();
        Reload();
    }

    private void Reload()
    {
        list.Items.Clear();
        list.Items.AddRange(accounts.Snapshot().OrderBy(a => a.DisplayName).Cast<object>().ToArray());
        remove.Enabled = false;
    }

    private async void Connect()
    {
        var provider = (Provider)providers.SelectedIndex;
        try
        {
            var client = LoadDefault(provider);
            if (client is null)
            {
                using var dialog = new RegistrationDialog(provider, accounts.Snapshot().FirstOrDefault(a => a.Provider == provider)?.Client);
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                client = dialog.Registration;
            }
            using var cancellation = new CancellationTokenSource();
            signIn = cancellation;
            add.Enabled = remove.Enabled = providers.Enabled = false;
            cancel.Visible = true;
            status.Text = "Завершите вход в браузере.";
            var token = await OAuth.SignIn(provider, client, cancellation.Token);
            using var identityTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
            identityTimeout.CancelAfter(TimeSpan.FromSeconds(15));
            var identity = await cloud.Identify(provider, token, identityTimeout.Token);
            accounts.Save(provider, identity, client, token);
            Reload();
            status.Text = "Аккаунт подключён.";
            changed();
        }
        catch (OperationCanceledException) { if (!IsDisposed) status.Text = "Вход отменён или время ожидания истекло."; }
        catch (Exception error) { if (!IsDisposed) status.Text = error.Message; }
        finally
        {
            signIn = null;
            if (!IsDisposed)
            {
                cancel.Visible = false;
                add.Enabled = providers.Enabled = true;
                remove.Enabled = list.SelectedIndex >= 0;
            }
        }
    }

    private void Disconnect()
    {
        if (list.SelectedItem is not Account account) return;
        try
        {
            accounts.Remove(account.Id);
            Reload();
            status.Text = "Аккаунт отключён.";
            changed();
        }
        catch (Exception error) { status.Text = error.Message; }
    }

    private static ClientRegistration? LoadDefault(Provider provider)
    {
        var path = Path.Combine(AppContext.BaseDirectory, "oauth-clients.json");
        if (!File.Exists(path)) return null;
        using var json = JsonDocument.Parse(File.ReadAllText(path));
        if (!json.RootElement.TryGetProperty(provider.ToString(), out var value)) return null;
        return new ClientRegistration(value.Required("clientId"), value.Text("desktopSecret"));
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) signIn?.Cancel();
        base.Dispose(disposing);
    }
}

internal sealed class RegistrationDialog : Form
{
    private readonly TextBox clientId = new() { Dock = DockStyle.Fill };
    private readonly TextBox secret = new() { Dock = DockStyle.Fill, UseSystemPasswordChar = true };
    public ClientRegistration Registration => new(clientId.Text.Trim(), secret.Text.Trim());

    public RegistrationDialog(Provider provider, ClientRegistration? previous)
    {
        Text = $"Настройка OAuth · {provider.Label()}";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        ClientSize = new Size(520, provider == Provider.GoogleDrive ? 255 : 205);
        MaximizeBox = MinimizeBox = ShowInTaskbar = false;
        AutoScaleMode = AutoScaleMode.Dpi;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(14), ColumnCount = 1 };
        var hint = provider switch
        {
            Provider.GoogleDrive => "В Google Cloud создайте OAuth-клиент типа Desktop app и включите Google Drive API.",
            Provider.OneDrive => "В Microsoft Entra зарегистрируйте публичное приложение и URI http://localhost/callback.",
            _ => $"В Dropbox создайте приложение Full Dropbox и добавьте URI http://127.0.0.1:{OAuth.DropboxPort}/callback."
        };
        layout.Controls.Add(new Label { Text = hint, AutoSize = true, MaximumSize = new Size(485, 0) });
        layout.Controls.Add(new Label { Text = provider == Provider.Dropbox ? "App key" : "Client ID", AutoSize = true });
        layout.Controls.Add(clientId);
        if (provider == Provider.GoogleDrive)
        {
            layout.Controls.Add(new Label { Text = "Desktop client secret", AutoSize = true });
            layout.Controls.Add(secret);
        }
        var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        var ok = new Button { Text = "Войти", DialogResult = DialogResult.OK, AutoSize = true };
        var close = new Button { Text = "Отмена", DialogResult = DialogResult.Cancel, AutoSize = true };
        buttons.Controls.AddRange([ok, close]);
        layout.Controls.Add(buttons);
        Controls.Add(layout);
        AcceptButton = ok;
        CancelButton = close;
        clientId.Text = previous?.Id ?? "";
        secret.Text = previous?.DesktopSecret ?? "";
        ok.Click += (_, _) =>
        {
            if (string.IsNullOrWhiteSpace(clientId.Text) ||
                (provider == Provider.GoogleDrive && string.IsNullOrWhiteSpace(secret.Text)))
            {
                DialogResult = DialogResult.None;
                clientId.Focus();
            }
        };
    }
}
