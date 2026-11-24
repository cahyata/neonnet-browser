# Generates ui/tokens.css from design/tokens.json.
# Usage: powershell -ExecutionPolicy Bypass -File tools\gen_tokens.ps1
# build.ps1 runs this first, so ui/tokens.css is always in step with design/tokens.json.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$t = Get-Content (Join-Path $root 'design\tokens.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$nl = "`n"
$o = New-Object System.Text.StringBuilder
[void]$o.Append("/* NeonNet design tokens. Generated from design/tokens.json by tools/gen_tokens.ps1. Do not edit by hand. */$nl")

$themes = @($t.color.themes)
for ($i = 0; $i -lt $themes.Count; $i++) {
    $id = $themes[$i].id
    # NeonNet marks the page with html.light / html.hc; [data-theme] is for the catalog and other projects.
    $app = switch ($id) { 'light' { 'html.light' } 'contrast-dark' { 'html.hc:not(.light)' } 'contrast-light' { 'html.hc.light' } default { $null } }
    $sel = if ($i -eq 0) { ":root,[data-theme=`"$id`"]" } elseif ($app) { "$app,[data-theme=`"$id`"]" } else { "[data-theme=`"$id`"]" }
    [void]$o.Append("$sel{")
    if ($id -like '*light') { [void]$o.Append("color-scheme:light;") } else { [void]$o.Append("color-scheme:dark;") }
    foreach ($c in $t.color.tokens) {
        $v = $c.value.PSObject.Properties[$id]
        if ($null -eq $v) { $v = $c.value.PSObject.Properties[$themes[0].id] }
        [void]$o.Append("--$($c.name):$($v.Value);")
    }
    [void]$o.Append("}$nl")
}

[void]$o.Append(":root{")
foreach ($f in @($t.spacing.tokens + $t.radius.tokens + $t.cut.tokens + $t.zIndex.tokens)) { [void]$o.Append("--$($f.name):$($f.value);") }
foreach ($p in $t.type.families.PSObject.Properties) { [void]$o.Append("--font-$($p.Name):$($p.Value);") }
[void]$o.Append("}$nl")

# Old variable names, kept as aliases of the role names. Declared on every theme scope so a nested
# [data-theme] re-resolves them.
[void]$o.Append("/* legacy names */${nl}:root,[data-theme]{")
foreach ($c in $t.color.tokens) { if ($c.legacy -and $c.legacy -ne $c.name) { [void]$o.Append("--$($c.legacy):var(--$($c.name));") } }
[void]$o.Append("}$nl")
# An incognito window swaps the primary accent for green.
[void]$o.Append(":root.incog{--accent-primary:#35ffa0;--accent-primary-soft:#9dffd0}$nl")

$out = Join-Path $root 'ui\tokens.css'
[System.IO.File]::WriteAllText($out, $o.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote ui\tokens.css ($($t.color.tokens.Count) colours)"
