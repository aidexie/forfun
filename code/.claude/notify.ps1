# Claude Code notification script
# Reads CLAUDE_TAB from environment variable

param(
    [string]$Message = "Needs input"
)

# Get tab name from environment variable
$TabName = $env:CLAUDE_TAB
if (-not $TabName) {
    $TabName = "Claude"
}

$title = "Claude: $TabName"
$text = "$title - $Message"

# Method 1: BurntToast (if installed) - Best Windows experience
try {
    Import-Module BurntToast -ErrorAction Stop
    New-BurntToastNotification -Text $title, $Message -Sound Default
    exit 0
} catch {
    # BurntToast not installed, try other methods
}

# Method 2: Windows Toast via .NET (Windows 10+)
try {
    [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
    [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null

    $template = @"
<toast>
    <visual>
        <binding template="ToastText02">
            <text id="1">$title</text>
            <text id="2">$Message</text>
        </binding>
    </visual>
    <audio src="ms-winsoundevent:Notification.Default"/>
</toast>
"@

    $xml = New-Object Windows.Data.Xml.Dom.XmlDocument
    $xml.LoadXml($template)
    $toast = [Windows.UI.Notifications.ToastNotification]::new($xml)
    [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("Claude Code").Show($toast)
    exit 0
} catch {
    # .NET toast failed, try fallback
}

# Method 3: Terminal bell + console output (universal fallback)
[Console]::Beep(800, 200)
Write-Host "`a[$title] $Message" -ForegroundColor Yellow
