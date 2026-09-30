# Run from an Administrator PowerShell. Only MQTT on the configured LAN address
# is exposed, and only to peers on the local subnet. HTTP remains loopback-only.
$ErrorActionPreference = 'Stop'
$mqttSettings = Get-Content (Join-Path $PSScriptRoot 'data/connection.json') | ConvertFrom-Json
$ruleName = 'NoteTablet-MQTT-LAN'
$existing = Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
if ($existing) { Remove-NetFirewallRule -Name $ruleName }
New-NetFirewallRule -Name $ruleName -DisplayName 'NoteTablet MQTT home subnet' `
    -Direction Inbound -Protocol TCP -LocalPort $mqttSettings.mqtt_port `
    -LocalAddress $mqttSettings.broker_host -RemoteAddress LocalSubnet `
    -Action Allow -Profile Any | Select-Object Name, Enabled
