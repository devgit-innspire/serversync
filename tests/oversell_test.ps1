# oversell_test.ps1 - many users check out the SAME product at the same moment: can the shop sell more than its stock?
#
# Talks to the server ONLY over HTTP (curl.exe). It never reads db.conf and runs no SQL.
# It CREATES <Users> new accounts (os_<runid>_<n>@test.com) and, if stock allows, real orders in the database
# the server is connected to, and it lowers that product's stock by the number of orders placed.
#
# Run (server must be running):
#     powershell -ExecutionPolicy Bypass -File tests\oversell_test.ps1
#     powershell -ExecutionPolicy Bypass -File tests\oversell_test.ps1 -ProductId 2 -Users 10
#
# To see 409 (out of stock) answers the product's stock must be LOWER than -Users.
# The script prints the stock before it starts and warns if it is too high.
# Exit code: 0 = nothing oversold, 1 = problem found, 2 = could not run.

param(
    [int]$ProductId = 2,
    [int]$Users = 10,
    [string]$BaseUrl = 'http://localhost:8080'
)

$ErrorActionPreference = 'Stop'
$tmp = Join-Path $PSScriptRoot 'tmp'
$script:created = New-Object System.Collections.ArrayList   # every file this script creates (only these are deleted)
$script:createdDir = $false
$password = '123456'

# one HTTP call with curl.exe; returns "<status code> <redirect url>"; redirects are NOT followed
function Invoke-Http {
    param([string]$Method, [string]$Path, [string]$Body = '', [string]$CookieFile = '', [switch]$SaveCookie)
    $curlArgs = @('-s', '-o', 'NUL', '-m', '60', '-w', '%{http_code} %{redirect_url}')
    if ($CookieFile) {
        if ($SaveCookie) { $curlArgs += @('-c', $CookieFile) }
        if (Test-Path $CookieFile) { $curlArgs += @('-b', $CookieFile) }
    }
    if ($Method -eq 'POST') { $curlArgs += @('--data', $Body) }
    $curlArgs += ($BaseUrl + $Path)
    return ((& curl.exe @curlArgs) -join '').Trim()
}

function Get-Code([string]$result) { return ($result -split ' ')[0] }

# stock shown on the public product page: a number, 0 for "Out of stock", -1 if the page is not found
function Get-Stock {
    $out = @(& curl.exe -s -m 30 -w '\n%{http_code}' ($BaseUrl + '/product?id=' + $ProductId))
    $text = $out -join "`n"
    $lines = $text -split "`n"
    if ($lines[$lines.Count - 1].Trim() -ne '200') { return -1 }
    if ($text -match 'title="(\d+) in stock"') { return [int]$Matches[1] }
    if ($text -match 'Only (\d+) left') { return [int]$Matches[1] }
    if ($text -match 'Out of stock') { return 0 }
    return -1
}

function Invoke-OversellTest {
    if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) {
        Write-Host 'curl.exe not found (it ships with Windows 10 and later).' -ForegroundColor Red
        return 2
    }
    $rootCode = Get-Code (Invoke-Http -Method GET -Path '/')
    if ($rootCode -ne '200') {
        Write-Host "Server not answering at $BaseUrl (GET / gave '$rootCode'). Start server.exe first." -ForegroundColor Red
        return 2
    }
    $stockBefore = Get-Stock
    if ($stockBefore -lt 0) {
        Write-Host "Product $ProductId was not found (GET /product?id=$ProductId)." -ForegroundColor Red
        return 2
    }

    # ---- 1. unique run id: new emails every run ----
    $runId = (Get-Date).ToString('yyyyMMddHHmmss')
    Write-Host ("Run id {0}: product {1}, stock before = {2}, users = {3}" -f $runId, $ProductId, $stockBefore, $Users)
    if ($stockBefore -ge $Users) {
        Write-Host (("NOTE: stock ({0}) is not lower than the number of users ({1}), so every checkout can succeed and " +
                     "no 409 is expected. Lower the stock or use more users to see out-of-stock answers.") -f $stockBefore, $Users) -ForegroundColor Yellow
    }

    if (-not (Test-Path $tmp)) {
        New-Item -ItemType Directory -Path $tmp | Out-Null
        $script:createdDir = $true
    }

    # ---- 2. per user: signup, login (own cookie file), add the product to the cart ----
    Write-Host 'Setting up users (signup, login, add to cart) ...'
    $cookies = @()
    for ($i = 1; $i -le $Users; $i++) {
        $email = "os_${runId}_$i@test.com"
        $cookie = Join-Path $tmp ("cookie_{0}_{1}.txt" -f $runId, $i)
        [void]$script:created.Add($cookie)

        $r = Invoke-Http -Method POST -Path '/signup' -Body "name=OS+User+$i&email=$email&password=$password"
        if ((Get-Code $r) -ne '302') { throw "signup of $email failed: $r" }

        $r = Invoke-Http -Method POST -Path '/login' -Body "email=$email&password=$password" -CookieFile $cookie -SaveCookie
        if ((Get-Code $r) -ne '302' -or -not (Test-Path $cookie)) { throw "login of $email failed: $r" }

        $r = Invoke-Http -Method POST -Path '/cart/add' -Body "product_id=$ProductId&qty=1&return=/products" -CookieFile $cookie
        if ((Get-Code $r) -ne '302') { throw "adding product $ProductId to the cart of $email failed: $r" }
        $cookies += $cookie
    }

    # ---- 3. all checkouts at the same moment ----
    # Each job is a separate process. They all wait for a "go" file, so they really fire together
    # (starting a PowerShell job takes about a second, so a plain loop would NOT be parallel).
    $go = Join-Path $tmp ("go_{0}.flag" -f $runId)
    [void]$script:created.Add($go)
    $jobScript = {
        param($index, $cookie, $url, $go, $ready)
        New-Item -ItemType File -Path $ready -Force | Out-Null
        while (-not (Test-Path $go)) { Start-Sleep -Milliseconds 1 }
        $fired = [DateTime]::UtcNow
        $out = ((& curl.exe -s -o NUL -m 120 -b $cookie -w '%{http_code} %{redirect_url}' --data 'confirm=1' $url) -join '').Trim()
        [pscustomobject]@{ Index = $index; Result = $out; Fired = $fired }
    }

    Write-Host "Starting $Users checkout jobs ..."
    $jobs = @()
    $readyFiles = @()
    for ($i = 1; $i -le $Users; $i++) {
        $ready = Join-Path $tmp ("ready_{0}_{1}.flag" -f $runId, $i)
        [void]$script:created.Add($ready)
        $readyFiles += $ready
        $jobs += Start-Job -ScriptBlock $jobScript -ArgumentList $i, $cookies[$i - 1], ($BaseUrl + '/checkout'), $go, $ready
    }
    $deadline = (Get-Date).AddSeconds(120)
    while (@($readyFiles | Where-Object { -not (Test-Path $_) }).Count -gt 0) {
        if ((Get-Date) -gt $deadline) { throw 'jobs did not become ready in time' }
        Start-Sleep -Milliseconds 50
    }
    Write-Host 'All jobs ready. GO!'
    New-Item -ItemType File -Path $go | Out-Null

    [void](Wait-Job -Job $jobs -Timeout 180)
    $results = @($jobs | Receive-Job)
    $jobs | Remove-Job -Force

    # ---- 4. summary ----
    Write-Host ''
    Write-Host 'Per user (status, redirect target):'
    foreach ($r in ($results | Sort-Object Index)) { Write-Host ("  user {0,2}: {1}" -f $r.Index, $r.Result) }

    $codes = @($results | ForEach-Object { Get-Code $_.Result })
    $placed = @($codes | Where-Object { $_ -eq '302' }).Count
    $soldOut = @($codes | Where-Object { $_ -eq '409' }).Count
    $other = @($codes | Where-Object { $_ -ne '302' -and $_ -ne '409' })
    $missing = $Users - $results.Count
    $spread = -1
    if ($results.Count -gt 0) {
        $times = @($results | ForEach-Object { $_.Fired } | Sort-Object)
        $spread = [int]($times[$times.Count - 1] - $times[0]).TotalMilliseconds
    }

    Write-Host ''
    Write-Host '=== SUMMARY ===' -ForegroundColor Cyan
    Write-Host ("Users                      : {0}   (all fired within {1} ms of each other)" -f $Users, $spread)
    Write-Host ("302 order placed           : {0}" -f $placed)
    Write-Host ("409 out of stock / refused : {0}" -f $soldOut)
    if ($other.Count -gt 0 -or $missing -gt 0) {
        $list = (($other | Sort-Object | Group-Object | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', ')
        Write-Host ("other codes                : {0}  (no answer: {1})" -f $list, $missing) -ForegroundColor Yellow
    } else {
        Write-Host 'other codes                : none'
    }

    # ---- was anything oversold? (stock again from the public product page) ----
    $stockAfter = Get-Stock
    Write-Host ("Stock before / after       : {0} / {1}" -f $stockBefore, $stockAfter)
    $expected = [Math]::Max($stockBefore - $placed, 0)
    $ok = $true
    if ($placed -gt $stockBefore) {
        Write-Host ("FAIL: {0} orders were placed but only {1} units were in stock: OVERSOLD" -f $placed, $stockBefore) -ForegroundColor Red
        $ok = $false
    }
    if ($stockAfter -ge 0 -and $stockAfter -ne $expected) {
        Write-Host ("FAIL: stock should be {0} (before {1} - placed {2}) but the page shows {3}" -f $expected, $stockBefore, $placed, $stockAfter) -ForegroundColor Red
        $ok = $false
    }
    if ($other.Count -gt 0 -or $missing -gt 0) {
        Write-Host 'FAIL: unexpected status codes or missing answers (see above)' -ForegroundColor Red
        $ok = $false
    }
    if ($ok) {
        Write-Host ("PASS: {0} placed <= {1} in stock, stock is now {2}, nothing oversold." -f $placed, $stockBefore, $stockAfter) -ForegroundColor Green
        return 0
    }
    return 1
}

$exitCode = 2
try {
    $exitCode = Invoke-OversellTest
}
catch {
    Write-Host ("ERROR: " + $_.Exception.Message) -ForegroundColor Red
    $exitCode = 2
}
finally {
    # ---- 5. cleanup: only the files this run created; the folder only if this run created it and it is empty ----
    foreach ($f in $script:created) {
        if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
    }
    if ($script:createdDir -and (Test-Path -LiteralPath $tmp) -and @(Get-ChildItem -LiteralPath $tmp -Force).Count -eq 0) {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}
exit $exitCode
